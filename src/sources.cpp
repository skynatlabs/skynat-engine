// skynat-engine — overlay and PCM source types (see sources.hpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sources.hpp"

#include <graphics/graphics.h>
#include <util/platform.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <vector>

namespace skynat {

// ===========================================================================
// Overlay layer
// ===========================================================================

namespace {

struct OverlaySource {
	obs_source_t *source = nullptr;
	std::mutex mutex;
	uint32_t width = 0;
	uint32_t height = 0;
	bool premultiplied = false;
	std::vector<uint8_t> pixels; // width * height * 4, RGBA
	bool dirty = false;
	bool recreate = false;
	gs_texture_t *texture = nullptr; // graphics thread only
	uint32_t tex_width = 0;
	uint32_t tex_height = 0;
};

const char *overlay_name(void *)
{
	return "Skynat overlay";
}

void overlay_update(void *data, obs_data_t *settings)
{
	auto *o = static_cast<OverlaySource *>(data);
	uint32_t w = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "width"), 1, 8192);
	uint32_t h = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "height"), 1, 8192);
	std::lock_guard<std::mutex> lock(o->mutex);
	o->premultiplied = obs_data_get_bool(settings, "premultiplied");
	if (w != o->width || h != o->height) {
		o->width = w;
		o->height = h;
		o->pixels.assign((size_t)w * h * 4, 0);
		o->recreate = true;
		o->dirty = true;
	}
}

void *overlay_create(obs_data_t *settings, obs_source_t *source)
{
	auto *o = new OverlaySource();
	o->source = source;
	overlay_update(o, settings);
	return o;
}

void overlay_destroy(void *data)
{
	auto *o = static_cast<OverlaySource *>(data);
	if (o->texture) {
		obs_enter_graphics();
		gs_texture_destroy(o->texture);
		obs_leave_graphics();
	}
	delete o;
}

void overlay_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_bool(settings, "premultiplied", false);
}

uint32_t overlay_width(void *data)
{
	return static_cast<OverlaySource *>(data)->width;
}

uint32_t overlay_height(void *data)
{
	return static_cast<OverlaySource *>(data)->height;
}

// Runs on the graphics thread. The upload happens at most once per change,
// however many views draw the layer this frame.
void overlay_render(void *data, gs_effect_t *effect)
{
	auto *o = static_cast<OverlaySource *>(data);
	bool premultiplied;
	{
		std::lock_guard<std::mutex> lock(o->mutex);
		if (o->recreate) {
			if (o->texture)
				gs_texture_destroy(o->texture);
			o->texture = gs_texture_create(o->width, o->height, GS_RGBA, 1, nullptr, GS_DYNAMIC);
			o->tex_width = o->width;
			o->tex_height = o->height;
			o->recreate = false;
			o->dirty = true;
		}
		if (o->dirty && o->texture) {
			gs_texture_set_image(o->texture, o->pixels.data(), o->tex_width * 4, false);
			o->dirty = false;
		}
		premultiplied = o->premultiplied;
	}
	if (!o->texture || !effect)
		return;

	// The layer is drawn like a web page composites it: in sRGB space
	// (this source type is not flagged sRGB-aware, so libobs keeps linear
	// blending off for it), with straight alpha unless the app says the
	// pixels are premultiplied.
	gs_blend_state_push();
	if (premultiplied)
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	else
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, o->texture);
	gs_draw_sprite(o->texture, 0, o->tex_width, o->tex_height);
	gs_blend_state_pop();
}

} // namespace

bool overlay_write(obs_source_t *overlay, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride,
		   bool clear, const uint8_t *data, size_t size, std::string *error)
{
	auto *o = static_cast<OverlaySource *>(obs_obj_get_data(overlay));
	if (!o) {
		*error = "overlay source has no data";
		return false;
	}
	std::lock_guard<std::mutex> lock(o->mutex);
	if (clear)
		std::fill(o->pixels.begin(), o->pixels.end(), (uint8_t)0);
	if (w == 0 || h == 0) {
		o->dirty = o->dirty || clear;
		return true;
	}
	if ((uint64_t)x + w > o->width || (uint64_t)y + h > o->height) {
		*error = "rectangle outside the layer";
		return false;
	}
	if (stride < w * 4 || size < (size_t)(h - 1) * stride + (size_t)w * 4) {
		*error = "pixel data too short for the rectangle";
		return false;
	}
	const size_t row = (size_t)w * 4;
	for (uint32_t r = 0; r < h; r++)
		memcpy(&o->pixels[((size_t)(y + r) * o->width + x) * 4], data + (size_t)r * stride, row);
	o->dirty = true;
	return true;
}

// ===========================================================================
// PCM input
// ===========================================================================

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint64_t kGapResetNs = 400ull * 1000000;   // pause longer than this: start a new timeline
constexpr int64_t kResyncNs = 200ll * 1000000;       // drift beyond this: jump instead of steering
constexpr int64_t kDeadbandNs = 2ll * 1000000;       // closer than this: leave it alone
constexpr uint64_t kWindowNs = 1000ull * 1000000;    // drift is measured over one-second windows

struct PcmSource {
	obs_source_t *source = nullptr;
	std::mutex mutex;
	bool started = false;
	uint64_t base_ts = 0;
	uint64_t samples_out = 0;
	uint64_t last_push = 0;
	uint64_t window_start = 0;
	int64_t window_min = INT64_MAX;
	int64_t pending = 0; // > 0: samples to insert, < 0: samples to drop (one per block)
	std::vector<float> scratch;
	PcmStats stats;
};

const char *pcm_name(void *)
{
	return "Skynat audio input";
}

void *pcm_create(obs_data_t *, obs_source_t *source)
{
	auto *p = new PcmSource();
	p->source = source;
	return p;
}

void pcm_destroy(void *data)
{
	delete static_cast<PcmSource *>(data);
}

inline uint64_t frames_to_ns(uint64_t frames)
{
	return frames * 1000000000ull / kRate;
}

} // namespace

void pcm_push(obs_source_t *pcm, const float *interleaved, uint32_t frames, uint32_t delay_us)
{
	auto *p = static_cast<PcmSource *>(obs_obj_get_data(pcm));
	if (!p || frames == 0)
		return;
	std::lock_guard<std::mutex> lock(p->mutex);

	const uint64_t now = os_gettime_ns();
	const uint64_t arrival_start = now - frames_to_ns(frames) - (uint64_t)delay_us * 1000ull;

	if (!p->started || now - p->last_push > kGapResetNs) {
		p->started = true;
		p->base_ts = arrival_start;
		p->samples_out = 0;
		p->window_start = now;
		p->window_min = INT64_MAX;
		p->pending = 0;
	}
	p->last_push = now;

	// Lateness of this block against the continuous timeline. Pipe jitter
	// only ever adds delay, so the smallest value over a window is the real
	// clock difference between the app's sound card and the engine.
	uint64_t expected = p->base_ts + frames_to_ns(p->samples_out);
	int64_t offset = (int64_t)(arrival_start - expected);
	p->window_min = std::min(p->window_min, offset);

	if (now - p->window_start >= kWindowNs) {
		int64_t drift = p->window_min;
		p->stats.offset_ms = drift / 1e6;
		if (drift > kResyncNs || drift < -kResyncNs) {
			p->base_ts = arrival_start;
			p->samples_out = 0;
			p->pending = 0;
			p->stats.resyncs++;
			expected = arrival_start;
		} else if (drift > kDeadbandNs || drift < -kDeadbandNs) {
			// Steer by at most one sample per block: inaudible.
			p->pending = (int64_t)(drift * (int64_t)kRate / 1000000000ll);
		} else {
			p->pending = 0;
		}
		p->window_start = now;
		p->window_min = INT64_MAX;
	}

	uint32_t out_frames = frames;
	const float *out = interleaved;
	if (p->pending > 0) {
		p->scratch.assign(interleaved, interleaved + (size_t)frames * 2);
		p->scratch.push_back(interleaved[(size_t)(frames - 1) * 2]);
		p->scratch.push_back(interleaved[(size_t)(frames - 1) * 2 + 1]);
		out = p->scratch.data();
		out_frames = frames + 1;
		p->pending--;
		p->stats.samples_inserted++;
	} else if (p->pending < 0 && frames > 1) {
		out_frames = frames - 1;
		p->pending++;
		p->stats.samples_dropped++;
	}

	struct obs_source_audio audio = {};
	audio.data[0] = (const uint8_t *)out;
	audio.frames = out_frames;
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT;
	audio.samples_per_sec = kRate;
	audio.timestamp = expected;
	obs_source_output_audio(p->source, &audio);

	p->samples_out += out_frames;
	p->stats.frames_in += frames;
}

PcmStats pcm_stats(obs_source_t *pcm)
{
	auto *p = static_cast<PcmSource *>(obs_obj_get_data(pcm));
	if (!p)
		return {};
	std::lock_guard<std::mutex> lock(p->mutex);
	return p->stats;
}

// ===========================================================================
// signal as it is: an async video filter that marks each frame of a clip as
// plain (non-HDR) video, so libobs keeps the coded values (its YUV-to-RGB
// with the clip's own matrix still applies) instead of converting HLG or PQ.

static const char *kAsIsName = "skynat-signal-as-is";

static const char *as_is_name(void *)
{
	return "Signal as it is";
}

static void *as_is_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

static void as_is_destroy(void *) {}

static struct obs_source_frame *as_is_filter_video(void *, struct obs_source_frame *frame)
{
	if (frame && (frame->trc == VIDEO_TRC_HLG || frame->trc == VIDEO_TRC_PQ))
		frame->trc = VIDEO_TRC_SRGB;
	return frame;
}

void set_signal_as_is(obs_source_t *source, bool on)
{
	if (!source)
		return;
	obs_source_t *filter = obs_source_get_filter_by_name(source, kAsIsName);
	if (on && !filter) {
		filter = obs_source_create_private("skynat_signal_as_is", kAsIsName, nullptr);
		if (filter) {
			obs_source_filter_add(source, filter);
			obs_source_release(filter);
		}
		return;
	}
	if (!on && filter)
		obs_source_filter_remove(source, filter);
	if (filter)
		obs_source_release(filter);
}

bool signal_as_is(obs_source_t *source)
{
	obs_source_t *filter = source ? obs_source_get_filter_by_name(source, kAsIsName) : nullptr;
	if (filter)
		obs_source_release(filter);
	return filter != nullptr;
}

// ===========================================================================

// ---------------------------------------------------------------------------
// frames: an async video input fed by the app (MSG_FRAME)
// ---------------------------------------------------------------------------

static const char *frames_name(void *)
{
	return "Skynat frames";
}
static void *frames_create(obs_data_t *, obs_source_t *source)
{
	return source;
}
static void frames_destroy(void *) {}

bool frames_push(obs_source_t *src, uint32_t w, uint32_t h, uint32_t format, const uint8_t *data, size_t size,
		 std::string *error)
{
	if (!src || !w || !h || w > 8192 || h > 8192) {
		*error = "bad frame size";
		return false;
	}
	struct obs_source_frame f = {};
	f.width = w;
	f.height = h;
	f.timestamp = os_gettime_ns();
	if (format == 1) {
		if ((w & 1) || (h & 1)) {
			*error = "I420 needs even width and height";
			return false;
		}
		const size_t y = (size_t)w * h, c = y / 4;
		if (size < y + 2 * c) {
			*error = "I420 frame too short";
			return false;
		}
		f.format = VIDEO_FORMAT_I420;
		f.data[0] = const_cast<uint8_t *>(data);
		f.data[1] = const_cast<uint8_t *>(data + y);
		f.data[2] = const_cast<uint8_t *>(data + y + c);
		f.linesize[0] = w;
		f.linesize[1] = w / 2;
		f.linesize[2] = w / 2;
		video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, VIDEO_FORMAT_I420,
						       f.color_matrix, f.color_range_min, f.color_range_max);
		f.full_range = false;
	} else if (format == 2) {
		if (size < (size_t)w * h * 4) {
			*error = "RGBA frame too short";
			return false;
		}
		f.format = VIDEO_FORMAT_RGBA;
		f.data[0] = const_cast<uint8_t *>(data);
		f.linesize[0] = w * 4;
	} else {
		*error = "unknown frame format";
		return false;
	}
	obs_source_output_video(src, &f); // libobs copies the frame
	return true;
}

void register_engine_sources()
{
	struct obs_source_info frames = {};
	frames.id = "skynat_frames";
	frames.type = OBS_SOURCE_TYPE_INPUT;
	frames.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	frames.get_name = frames_name;
	frames.create = frames_create;
	frames.destroy = frames_destroy;
	obs_register_source(&frames);

	struct obs_source_info overlay = {};
	overlay.id = "skynat_overlay";
	overlay.type = OBS_SOURCE_TYPE_INPUT;
	overlay.output_flags = OBS_SOURCE_VIDEO;
	overlay.get_name = overlay_name;
	overlay.create = overlay_create;
	overlay.destroy = overlay_destroy;
	overlay.update = overlay_update;
	overlay.get_defaults = overlay_defaults;
	overlay.get_width = overlay_width;
	overlay.get_height = overlay_height;
	overlay.video_render = overlay_render;
	obs_register_source(&overlay);

	struct obs_source_info pcm = {};
	pcm.id = "skynat_pcm";
	pcm.type = OBS_SOURCE_TYPE_INPUT;
	pcm.output_flags = OBS_SOURCE_AUDIO;
	pcm.get_name = pcm_name;
	pcm.create = pcm_create;
	pcm.destroy = pcm_destroy;
	obs_register_source(&pcm);

	struct obs_source_info as_is = {};
	as_is.id = "skynat_signal_as_is";
	as_is.type = OBS_SOURCE_TYPE_FILTER;
	as_is.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	as_is.get_name = as_is_name;
	as_is.create = as_is_create;
	as_is.destroy = as_is_destroy;
	as_is.filter_video = as_is_filter_video;
	obs_register_source(&as_is);
}

} // namespace skynat
