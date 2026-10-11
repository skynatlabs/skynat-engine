// skynat-engine — projectors (see projectors.hpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#include "projectors.hpp"

#include "json.hpp"

#include <graphics/vec2.h>

#include <algorithm>
#include <cmath>

namespace skynat {

Projectors::Projectors(obs_view_t *preview_view, std::function<void(uint32_t, const std::string &)> closed)
	: preview_view_(preview_view), closed_(std::move(closed))
{
}

Projectors::~Projectors()
{
	close_all();
}

bool Projectors::parse_content(const std::string &s, Content *out)
{
	if (s.empty() || s == "program")
		*out = Content::Program;
	else if (s == "preview")
		*out = Content::Preview;
	else
		return false;
	return true;
}

bool Projectors::parse_fit(const std::string &s, Fit *out)
{
	if (s.empty() || s == "contain")
		*out = Fit::Contain;
	else if (s == "cover")
		*out = Fit::Cover;
	else if (s == "stretch")
		*out = Fit::Stretch;
	else
		return false;
	return true;
}

// Program: the canvas libobs has just rendered (with every overlay), drawn
// 1:1 where the display is the canvas's size, and with the bicubic filter
// where it is scaled (OBS's own projectors use bilinear; bicubic keeps text
// and edges sharp when a 1080p canvas fills a 4K screen). The same colour
// handling as obs_render_main_texture.
static void render_program(int vw, int vh, float bw, float bh)
{
	gs_texture_t *tex = obs_get_main_texture();
	if (!tex)
		return;
	const bool same = vw == (int)bw && vh == (int)bh;
	gs_effect_t *effect = obs_get_base_effect(same ? OBS_EFFECT_DEFAULT : OBS_EFFECT_BICUBIC);
	if (!effect)
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!effect)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	if (gs_eparam_t *m = gs_effect_get_param_by_name(effect, "multiplier"))
		gs_effect_set_float(m, 1.0f);
	if (!same) {
		struct vec2 d;
		vec2_set(&d, bw, bh);
		if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "base_dimension"))
			gs_effect_set_vec2(p, &d);
		vec2_set(&d, 1.0f / bw, 1.0f / bh);
		if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "base_dimension_i"))
			gs_effect_set_vec2(p, &d);
		if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "undistort_factor"))
			gs_effect_set_float(p, 1.0f);
	}

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_ZERO, GS_BLEND_ONE, GS_BLEND_ZERO);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, 0, 0);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

// Graphics thread, every frame, for each projector.
void Projectors::draw(void *param, uint32_t cx, uint32_t cy)
{
	Item *it = static_cast<Item *>(param);
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height || !cx || !cy)
		return;
	const float bw = (float)ovi.base_width, bh = (float)ovi.base_height;
	const Fit fit = (Fit)it->fit.load();

	int vx = 0, vy = 0, vw = (int)cx, vh = (int)cy;
	if (fit != Fit::Stretch) {
		const float sx = (float)cx / bw, sy = (float)cy / bh;
		const float s = fit == Fit::Cover ? std::max(sx, sy) : std::min(sx, sy);
		vw = (int)std::lround(bw * s);
		vh = (int)std::lround(bh * s);
		vx = ((int)cx - vw) / 2;
		vy = ((int)cy - vh) / 2;
	}

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, bw, 0.0f, bh, -100.0f, 100.0f);
	gs_set_viewport(vx, vy, vw, vh);
	if ((Content)it->content.load() == Content::Preview) {
		// The preview input itself, drawn at the display's size (not a copy
		// of a smaller picture).
		if (it->owner->preview_view_)
			obs_view_render(it->owner->preview_view_);
	} else {
		render_program(vw, vh, bw, bh);
	}
	gs_projection_pop();
	gs_viewport_pop();

	it->frames++;
	it->cx = cx;
	it->cy = cy;
}

static bool same_place(const platform::ProjectorTarget &a, const platform::ProjectorTarget &b)
{
	return a.display_id == b.display_id && a.has_rect == b.has_rect && a.windowed == b.windowed &&
	       (!a.has_rect || (a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height));
}

std::string Projectors::open(uint32_t id, Content content, Fit fit, const platform::ProjectorTarget &target,
			     std::string *error)
{
	auto existing = items_.find(id);
	if (existing != items_.end()) {
		Item &it = *existing->second;
		if (same_place(it.target, target)) {
			it.content = (int)content;
			it.fit = (int)fit;
			return item_json(it);
		}
		destroy(it);
		items_.erase(existing);
	}

	auto item = std::make_unique<Item>();
	Item *raw = item.get();
	raw->owner = this;
	raw->id = id;
	raw->content = (int)content;
	raw->fit = (int)fit;
	raw->target = target;
	raw->window = platform::open_projector_window(
		target, "Skynat Live — Projector", &raw->info,
		[this, id](const std::string &reason) {
			if (close(id) && closed_)
				closed_(id, reason);
		},
		[raw](uint32_t cx, uint32_t cy) {
			if (raw->display && cx && cy)
				obs_display_resize(raw->display, cx, cy);
		},
		error);
	if (!raw->window)
		return "";

	struct gs_init_data gi = {};
	gi.cx = raw->info.cx;
	gi.cy = raw->info.cy;
	gi.format = GS_BGRA;
	gi.zsformat = GS_ZS_NONE;
#ifdef _WIN32
	gi.window.hwnd = raw->info.native;
#elif defined(__APPLE__)
	gi.window.view = (__typeof__(gi.window.view))raw->info.native; // an NSView (the parameter "id" hides the type)
#endif
	raw->display = obs_display_create(&gi, 0x000000);
	if (!raw->display) {
		platform::close_projector_window(raw->window);
		raw->window = nullptr;
		*error = "libobs could not draw into the projector window";
		return "";
	}
	obs_display_add_draw_callback(raw->display, draw, raw);
	blog(LOG_INFO, "[skynat-engine] projector %u open: %ux%u px (%.2fx) on the display at %d,%d %dx%d", id,
	     raw->info.cx, raw->info.cy, raw->info.scale, raw->info.screen_x, raw->info.screen_y,
	     raw->info.screen_width, raw->info.screen_height);
	items_[id] = std::move(item);
	return item_json(*raw);
}

void Projectors::destroy(Item &item)
{
	if (item.display) {
		obs_display_remove_draw_callback(item.display, draw, &item);
		obs_display_destroy(item.display); // waits out a frame being drawn
		item.display = nullptr;
	}
	if (item.window) {
		platform::close_projector_window(item.window);
		item.window = nullptr;
	}
}

bool Projectors::close(uint32_t id)
{
	auto it = items_.find(id);
	if (it == items_.end())
		return false;
	destroy(*it->second);
	items_.erase(it);
	blog(LOG_INFO, "[skynat-engine] projector %u closed", id);
	return true;
}

void Projectors::close_all()
{
	for (auto &kv : items_)
		destroy(*kv.second);
	items_.clear();
}

size_t Projectors::count()
{
	return items_.size();
}

std::string Projectors::item_json(const Item &item)
{
	const char *content = (Content)item.content.load() == Content::Preview ? "preview" : "program";
	const Fit f = (Fit)item.fit.load();
	const char *fit = f == Fit::Cover ? "cover" : f == Fit::Stretch ? "stretch" : "contain";
	const uint32_t cx = item.cx.load() ? item.cx.load() : item.info.cx;
	const uint32_t cy = item.cy.load() ? item.cy.load() : item.info.cy;
	return JsonObj()
		.integer("id", item.id)
		.boolean("on", true)
		.str("content", content)
		.str("fit", fit)
		.integer("width", cx)
		.integer("height", cy)
		.number("scale", item.info.scale)
		.integer("frames", (int64_t)item.frames.load())
		.raw("display", JsonObj()
					.integer("x", item.info.screen_x)
					.integer("y", item.info.screen_y)
					.integer("width", item.info.screen_width)
					.integer("height", item.info.screen_height)
					.dump())
		.dump();
}

std::string Projectors::list_json()
{
	std::string out = "[";
	bool first = true;
	for (auto &kv : items_) {
		if (!first)
			out += ",";
		first = false;
		out += item_json(*kv.second);
	}
	return out + "]";
}

} // namespace skynat
