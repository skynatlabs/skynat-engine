// skynat-engine — projectors: Program or Preview drawn by libobs straight into
// a borderless window of the engine's own over a whole display, at that
// display's own pixels. Nothing is encoded, sent or decoded between the
// picture and the screen (the way vMix and OBS Studio drive a projector).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace skynat {

class Projectors {
public:
	enum class Content { Program = 0, Preview = 1 };
	enum class Fit { Contain = 0, Cover = 1, Stretch = 2 };

	// `closed` hears a projector the operator closed (Escape, a double-click)
	// or that lost its display — on the main thread, already gone.
	Projectors(obs_view_t *preview_view, std::function<void(uint32_t id, const std::string &reason)> closed);
	~Projectors();

	// Main thread. Opens projector `id` (or changes what an open one shows
	// and how — on the same display it is not opened again). Returns the
	// result's JSON object, or "" with *error.
	std::string open(uint32_t id, Content content, Fit fit, const platform::ProjectorTarget &target,
			 std::string *error);
	bool close(uint32_t id);
	void close_all();
	std::string list_json();
	size_t count();

	static bool parse_content(const std::string &s, Content *out);
	static bool parse_fit(const std::string &s, Fit *out);

private:
	struct Item {
		Projectors *owner = nullptr;
		uint32_t id = 0;
		std::atomic<int> content{0};
		std::atomic<int> fit{0};
		obs_display_t *display = nullptr;
		platform::ProjectorWindow *window = nullptr;
		platform::ProjectorWindowInfo info;
		platform::ProjectorTarget target;
		std::atomic<uint64_t> frames{0};
		std::atomic<uint32_t> cx{0}, cy{0};
	};

	static void draw(void *param, uint32_t cx, uint32_t cy);
	void destroy(Item &item);
	std::string item_json(const Item &item);

	obs_view_t *preview_view_;
	std::function<void(uint32_t, const std::string &)> closed_;
	std::map<uint32_t, std::unique_ptr<Item>> items_;
};

} // namespace skynat
