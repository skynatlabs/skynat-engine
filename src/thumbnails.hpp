// skynat-engine — small pictures of every input for the app's source tiles,
// multiview and screens in "Light" mode (default 256x144, a few per second).
//
// Each input is drawn into a tiny render target right after libobs renders
// the frame (on the graphics thread), copied to a staging surface, and read
// back one frame later so the GPU is never stalled.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.h>

#include <cstdint>
#include <map>
#include <mutex>

namespace skynat {

class Writer;

class Thumbnails {
public:
	explicit Thumbnails(Writer &writer);
	~Thumbnails();

	void enable(uint32_t width, uint32_t height, double fps);
	void disable();
	bool enabled() const { return enabled_; }

	// `scene` is the input's fit-to-canvas wrapper scene (a strong reference is kept).
	void add(uint32_t id, obs_source_t *scene);
	void remove(uint32_t id);
	void clear();

	uint64_t sent() const { return sent_; }
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	double fps() const { return interval_ > 0.0 ? 1.0 / interval_ : 0.0; }

private:
	struct Item {
		obs_source_t *scene = nullptr;
		uint32_t channel = 0;
		gs_texrender_t *texrender = nullptr;
		gs_stagesurf_t *stage = nullptr;
		bool staged = false;
	};

	static void tick_cb(void *param, float seconds);
	static void rendered_cb(void *param);
	void on_tick(float seconds);
	void on_rendered();
	void free_item_gpu(Item &item);
	void attach_view(uint32_t id, Item &item);

	Writer &writer_;
	std::mutex mutex_;
	std::map<uint32_t, Item> items_;
	obs_view_t *view_ = nullptr; // keeps every input "showing" so cameras keep updating
	bool enabled_ = false;
	uint32_t width_ = 256;
	uint32_t height_ = 144;
	double interval_ = 0.5;
	double elapsed_ = 0.0;
	bool due_ = false;       // set by the tick, consumed by the first render callback of the frame
	bool collect_ = false;   // staged pictures are ready to read back
	uint64_t frame_ = 0;
	uint64_t done_frame_ = ~0ull;
	uint64_t sent_ = 0;
};

} // namespace skynat
