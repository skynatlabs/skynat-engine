// skynat-engine — source thumbnails (see thumbnails.hpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "thumbnails.hpp"
#include "io.hpp"
#include "protocol.hpp"

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cstring>
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
	if (item.texrender || item.big || item.stage) {
		obs_enter_graphics();
		gs_texrender_destroy(item.texrender);
		gs_texrender_destroy(item.big);
		gs_stagesurface_destroy(item.stage);
		obs_leave_graphics();
	}
	item.texrender = nullptr;
	item.big = nullptr;
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

void Thumbnails::enable(uint32_t width, uint32_t height, double fps, Format format, bool only_changes)
{
	if (enabled_)
		disable();
	width_ = width ? width : 256;
	height_ = height ? height : 144;
	interval_ = fps > 0.0 ? 1.0 / fps : 0.5;
	format_ = format;
	only_changes_ = only_changes;
	elapsed_ = interval_; // first pictures straight away
	view_ = obs_view_create();
	std::vector<std::pair<uint32_t, Item>> copies;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &kv : items_) {
			kv.second.last.clear(); // the first picture after (re)enabling always goes
			copies.emplace_back(kv.first, kv.second);
		}
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
			kv.second.big = nullptr;
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

// The staged picture, packed: RGBA rows, or I420 — full-range BT.709 (the
// standard coefficients), each chroma sample from the 2x2 pixels it covers.
void Thumbnails::pack(const uint8_t *data, uint32_t linesize)
{
	const uint32_t w = width_, h = height_;
	if (format_ != I420) {
		pixels_.resize((size_t)w * h * 4);
		for (uint32_t y = 0; y < h; y++)
			std::memcpy(pixels_.data() + (size_t)y * w * 4, data + (size_t)y * linesize, (size_t)w * 4);
		return;
	}
	const uint32_t cw = w / 2, ch = h / 2;
	pixels_.resize((size_t)w * h + 2 * (size_t)cw * ch);
	uint8_t *Y = pixels_.data(), *U = Y + (size_t)w * h, *V = U + (size_t)cw * ch;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *p = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < w; x++, p += 4)
			Y[(size_t)y * w + x] = (uint8_t)((54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8);
	}
	for (uint32_t y = 0; y < ch; y++) {
		const uint8_t *a = data + (size_t)(2 * y) * linesize, *b = a + linesize;
		for (uint32_t x = 0; x < cw; x++, a += 8, b += 8) {
			const int r = a[0] + a[4] + b[0] + b[4], g = a[1] + a[5] + b[1] + b[5], bl = a[2] + a[6] + b[2] + b[6];
			const int u = ((-29 * r - 99 * g + 128 * bl) / 4 + 128) >> 8, v = ((128 * r - 116 * g - 12 * bl) / 4 + 128) >> 8;
			U[(size_t)y * cw + x] = (uint8_t)std::clamp(u + 128, 0, 255);
			V[(size_t)y * cw + x] = (uint8_t)std::clamp(v + 128, 0, 255);
		}
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
			const uint32_t row = format_ == I420 ? width_ : width_ * 4;
			pack(data, linesize);
			gs_stagesurface_unmap(item.stage);
			if (only_changes_ && item.last == pixels_) {
				unchanged_++;
				continue;
			}
			Frame f(MSG_THUMBNAIL, 16 + pixels_.size());
			f.u32(kv.first).u16((uint16_t)width_).u16((uint16_t)height_).u8((uint8_t)format_).u8(0).u8(0).u8(0).u32(row);
			f.bytes(pixels_.data(), pixels_.size());
			if (writer_.send(f.finish(), Writer::BULK)) {
				sent_++;
				if (only_changes_)
					item.last = pixels_;
			}
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
	// Each source's scene is drawn at up to four times the thumbnail's size,
	// then made smaller with the area filter (every source pixel counted by
	// how much of it a thumbnail pixel covers). Drawn straight at 256x144, a
	// 1080p picture is sampled one pixel in seven: small text and fine
	// patterns shimmer on the tiles.
	const uint32_t bw = std::min<uint32_t>(ovi.base_width, width_ * 4);
	const uint32_t bh = std::min<uint32_t>(ovi.base_height, height_ * 4);
	gs_effect_t *area = obs_get_base_effect(OBS_EFFECT_AREA);
	for (auto &kv : items_) {
		Item &item = kv.second;
		if (!item.texrender)
			item.texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!item.big && area)
			item.big = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!item.stage)
			item.stage = gs_stagesurface_create(width_, height_, GS_RGBA);
		if (!item.texrender || !item.stage)
			continue;
		const bool two_steps = area && item.big && (bw > width_ || bh > height_);
		gs_texrender_t *first = two_steps ? item.big : item.texrender;
		gs_texrender_reset(first);
		if (!gs_texrender_begin(first, two_steps ? bw : width_, two_steps ? bh : height_))
			continue;
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_reset_blend_state();
		obs_source_video_render(item.scene);
		gs_blend_state_pop();
		gs_texrender_end(first);
		if (two_steps) {
			gs_texture_t *tex = gs_texrender_get_texture(item.big);
			gs_texrender_reset(item.texrender);
			if (!tex || !gs_texrender_begin(item.texrender, width_, height_))
				continue;
			gs_ortho(0.0f, (float)bw, 0.0f, (float)bh, -100.0f, 100.0f);
			const bool srgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(false);
			gs_effect_set_texture(gs_effect_get_param_by_name(area, "image"), tex);
			struct vec2 d;
			vec2_set(&d, (float)bw, (float)bh);
			gs_effect_set_vec2(gs_effect_get_param_by_name(area, "base_dimension"), &d);
			vec2_set(&d, 1.0f / (float)bw, 1.0f / (float)bh);
			gs_effect_set_vec2(gs_effect_get_param_by_name(area, "base_dimension_i"), &d);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			while (gs_effect_loop(area, "Draw"))
				gs_draw_sprite(tex, 0, bw, bh);
			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(srgb);
			gs_texrender_end(item.texrender);
		}
		gs_stage_texture(item.stage, gs_texrender_get_texture(item.texrender));
		item.staged = true;
		collect_ = true;
	}
}

} // namespace skynat
