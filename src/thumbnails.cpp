// skynat-engine — source thumbnails (see thumbnails.hpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "thumbnails.hpp"
#include "io.hpp"
#include "protocol.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <vector>

namespace skynat {

static constexpr uint32_t kMaxChannels = 64;

Thumbnails::Thumbnails(Writer &writer) : writer_(writer) {}

Thumbnails::~Thumbnails()
{
	disable();
	clear();
}

// libobs calls (which take libobs's own locks) are always made without
// holding mutex_, so the graphics thread can never wait on us in a cycle.
void Thumbnails::attach_view(uint32_t, Item &item)
{
	if (view_ && item.scene)
		obs_view_set_source(view_, item.channel, item.scene);
}

void Thumbnails::add(uint32_t id, obs_source_t *scene)
{
	Item copy;
	bool attach = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (items_.count(id))
			return;
		// First channel not taken by another input.
		uint32_t channel = 0;
		for (; channel < kMaxChannels; channel++) {
			bool used = false;
			for (auto &kv : items_)
				used = used || kv.second.channel == channel;
			if (!used)
				break;
		}
		if (channel >= kMaxChannels)
			return;
		Item item;
		item.scene = obs_source_get_ref(scene);
		item.channel = channel;
		items_[id] = item;
		copy = item;
		attach = enabled_;
	}
	if (attach)
		attach_view(id, copy);
}

void Thumbnails::free_item_gpu(Item &item)
{
	if (item.texrender || item.stage) {
		obs_enter_graphics();
		gs_texrender_destroy(item.texrender);
		gs_stagesurface_destroy(item.stage);
		obs_leave_graphics();
	}
	item.texrender = nullptr;
	item.stage = nullptr;
	item.staged = false;
}

void Thumbnails::remove(uint32_t id)
{
	Item item;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = items_.find(id);
		if (it == items_.end())
			return;
		item = it->second;
		items_.erase(it);
	}
	if (view_)
		obs_view_set_source(view_, item.channel, nullptr);
	free_item_gpu(item);
	obs_source_release(item.scene);
}

void Thumbnails::clear()
{
	std::vector<uint32_t> ids;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : items_)
			ids.push_back(kv.first);
	}
	for (uint32_t id : ids)
		remove(id);
}

void Thumbnails::enable(uint32_t width, uint32_t height, double fps)
{
	if (enabled_)
		disable();
	width_ = width ? width : 256;
	height_ = height ? height : 144;
	interval_ = fps > 0.0 ? 1.0 / fps : 0.5;
	elapsed_ = interval_; // first pictures straight away
	view_ = obs_view_create();
	std::vector<std::pair<uint32_t, Item>> copies;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : items_)
			copies.emplace_back(kv.first, kv.second);
		enabled_ = true;
	}
	for (auto &c : copies)
		attach_view(c.first, c.second);
	obs_add_tick_callback(tick_cb, this);
	obs_add_main_rendered_callback(rendered_cb, this);
}

void Thumbnails::disable()
{
	if (!enabled_)
		return;
	obs_remove_main_rendered_callback(rendered_cb, this);
	obs_remove_tick_callback(tick_cb, this);
	std::vector<Item> gpu;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		enabled_ = false;
		for (auto &kv : items_) {
			gpu.push_back(kv.second);
			kv.second.texrender = nullptr;
			kv.second.stage = nullptr;
			kv.second.staged = false;
		}
	}
	for (auto &item : gpu)
		free_item_gpu(item);
	if (view_) {
		for (uint32_t c = 0; c < kMaxChannels; c++)
			obs_view_set_source(view_, c, nullptr);
		obs_view_destroy(view_);
		view_ = nullptr;
	}
}

void Thumbnails::tick_cb(void *param, float seconds)
{
	static_cast<Thumbnails *>(param)->on_tick(seconds);
}

void Thumbnails::rendered_cb(void *param)
{
	static_cast<Thumbnails *>(param)->on_rendered();
}

void Thumbnails::on_tick(float seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	frame_++;
	elapsed_ += seconds;
	if (elapsed_ >= interval_) {
		elapsed_ = 0.0;
		due_ = true;
	}
}

// Called on the graphics thread after each video mix renders; the work is
// done once per frame.
void Thumbnails::on_rendered()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_ || done_frame_ == frame_)
		return;
	done_frame_ = frame_;

	// 1. Read back what was staged on an earlier frame.
	if (collect_) {
		collect_ = false;
		for (auto &kv : items_) {
			Item &item = kv.second;
			if (!item.staged || !item.stage)
				continue;
			item.staged = false;
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (!gs_stagesurface_map(item.stage, &data, &linesize))
				continue;
			const uint32_t row = width_ * 4;
			Frame f(MSG_THUMBNAIL, 16 + (size_t)row * height_);
			f.u32(kv.first).u16((uint16_t)width_).u16((uint16_t)height_).u8(0).u8(0).u8(0).u8(0).u32(row);
			for (uint32_t y = 0; y < height_; y++)
				f.bytes(data + (size_t)y * linesize, row);
			gs_stagesurface_unmap(item.stage);
			if (writer_.send(f.finish(), Writer::BULK))
				sent_++;
		}
	}

	// 2. Draw and stage the next set when due.
	if (!due_)
		return;
	due_ = false;

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	struct vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	for (auto &kv : items_) {
		Item &item = kv.second;
		if (!item.texrender)
			item.texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!item.stage)
			item.stage = gs_stagesurface_create(width_, height_, GS_RGBA);
		if (!item.texrender || !item.stage)
			continue;
		gs_texrender_reset(item.texrender);
		if (!gs_texrender_begin(item.texrender, width_, height_))
			continue;
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_reset_blend_state();
		obs_source_video_render(item.scene);
		gs_blend_state_pop();
		gs_texrender_end(item.texrender);
		gs_stage_texture(item.stage, gs_texrender_get_texture(item.texrender));
		item.staged = true;
		collect_ = true;
	}
}

} // namespace skynat
