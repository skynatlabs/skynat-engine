// skynat-engine — the libobs side (see engine.hpp and PROTOCOL.md).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "engine.hpp"
#include "json.hpp"
#include "output.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "sources.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>

namespace skynat {

namespace {

struct CommandError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

constexpr uint32_t kOverlayHeader = 28;
constexpr uint32_t kPcmHeader = 16;
constexpr uint32_t kFirstAudioChannel = 2;
constexpr uint32_t kMaxChannels = 64;

bool has(obs_data_t *d, const char *key)
{
	return obs_data_has_user_value(d, key);
}

int64_t get_int(obs_data_t *d, const char *key, int64_t def)
{
	return has(d, key) ? obs_data_get_int(d, key) : def;
}

double get_double(obs_data_t *d, const char *key, double def)
{
	return has(d, key) ? obs_data_get_double(d, key) : def;
}

bool get_bool(obs_data_t *d, const char *key, bool def)
{
	return has(d, key) ? obs_data_get_bool(d, key) : def;
}

std::string get_str(obs_data_t *d, const char *key, const std::string &def = "")
{
	if (!has(d, key))
		return def;
	const char *s = obs_data_get_string(d, key);
	return s ? s : def;
}

uint32_t require_id(obs_data_t *cmd)
{
	if (!has(cmd, "id"))
		throw CommandError("missing \"id\"");
	int64_t id = obs_data_get_int(cmd, "id");
	if (id < 0 || id > 0xffffffffll)
		throw CommandError("\"id\" must be an unsigned 32-bit number");
	return (uint32_t)id;
}

// "#rrggbb" / "#rrggbbaa" -> libobs ABGR.
uint32_t parse_color(const std::string &s)
{
	std::string hex = s;
	if (!hex.empty() && hex[0] == '#')
		hex = hex.substr(1);
	if (hex.size() != 6 && hex.size() != 8)
		throw CommandError("color must be #rrggbb or #rrggbbaa");
	uint32_t v = (uint32_t)std::stoul(hex, nullptr, 16);
	uint32_t r, g, b, a = 0xff;
	if (hex.size() == 6) {
		r = (v >> 16) & 0xff;
		g = (v >> 8) & 0xff;
		b = v & 0xff;
	} else {
		r = (v >> 24) & 0xff;
		g = (v >> 16) & 0xff;
		b = (v >> 8) & 0xff;
		a = v & 0xff;
	}
	return (a << 24) | (b << 16) | (g << 8) | r;
}

bool is_input_type(const std::string &t)
{
	return t == "camera" || t == "media" || t == "image" || t == "color";
}

bool is_number(obs_data_t *d, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(d, key);
	if (!item)
		return false;
	bool number = obs_data_item_gettype(item) == OBS_DATA_NUMBER;
	obs_data_item_release(&item);
	return number;
}

// The request tag echoed in the result: a number or a string, else null.
std::string req_json(obs_data_t *cmd)
{
	obs_data_item_t *item = obs_data_item_byname(cmd, "req");
	if (!item)
		return "null";
	std::string out = "null";
	if (obs_data_item_gettype(item) == OBS_DATA_NUMBER) {
		out = std::to_string(obs_data_item_get_int(item));
	} else if (obs_data_item_gettype(item) == OBS_DATA_STRING) {
		const char *s = obs_data_item_get_string(item);
		out = json_escape(s ? s : "");
	}
	obs_data_item_release(&item);
	return out;
}

} // namespace

Engine::Engine(Writer &writer, Options options) : writer_(writer), opt_(std::move(options)) {}

Engine::~Engine() = default;

// ===========================================================================
// startup
// ===========================================================================

static std::string g_graphics_module; // libobs keeps a pointer to this string

bool Engine::reset_video(uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den, std::string *error)
{
	g_graphics_module = platform::graphics_module(opt_.renderer);
	struct obs_video_info ovi = {};
	ovi.graphics_module = g_graphics_module.c_str();
	ovi.fps_num = fps_num;
	ovi.fps_den = fps_den;
	ovi.base_width = w;
	ovi.base_height = h;
	ovi.output_width = w;
	ovi.output_height = h;
	ovi.output_format = VIDEO_FORMAT_NV12; // what hardware H.264 encoders take
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int rc = obs_reset_video(&ovi);
	if (rc != OBS_VIDEO_SUCCESS) {
		switch (rc) {
		case OBS_VIDEO_MODULE_NOT_FOUND:
			*error = "graphics module not found: " + g_graphics_module;
			break;
		case OBS_VIDEO_NOT_SUPPORTED:
			*error = "graphics not supported on this computer";
			break;
		case OBS_VIDEO_INVALID_PARAM:
			*error = "invalid video parameters";
			break;
		case OBS_VIDEO_CURRENTLY_ACTIVE:
			*error = "video is in use (stop outputs first)";
			break;
		default:
			*error = "obs_reset_video failed (" + std::to_string(rc) + ")";
		}
		return false;
	}
	opt_.width = w;
	opt_.height = h;
	opt_.fps_num = fps_num;
	opt_.fps_den = fps_den;
	return true;
}

bool Engine::startup(std::string *error)
{
	startup_ns_ = os_gettime_ns();
	last_cpu_wall_ = startup_ns_;
	last_cpu_ = platform::cpu_seconds();

	if (!opt_.data_dir.empty())
		obs_add_data_path((opt_.data_dir + "/libobs/").c_str());

	if (!obs_startup("en-US", opt_.config_dir.c_str(), nullptr)) {
		*error = "obs_startup failed";
		return false;
	}

	struct obs_audio_info ai = {};
	ai.samples_per_sec = 48000;
	ai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&ai)) {
		*error = "obs_reset_audio failed";
		return false;
	}

	if (opt_.video && !reset_video(opt_.width, opt_.height, opt_.fps_num, opt_.fps_den, error))
		return false;

	register_engine_sources();
	register_engine_outputs(&writer_);

	// Only what the engine needs — nothing else is loaded.
	std::vector<std::string> modules = {"image-source", "obs-transitions", "obs-filters",
					    "obs-ffmpeg",   "obs-outputs",     "obs-x264"};
#ifdef __APPLE__
	modules.insert(modules.end(), {"mac-avcapture", "mac-videotoolbox", "coreaudio-encoder"});
#elif defined(_WIN32)
	modules.insert(modules.end(), {"win-dshow", "obs-nvenc", "obs-qsv11"});
#endif
	std::vector<std::string> loaded;
	for (const auto &name : modules) {
		std::string bin, data;
		platform::module_paths(opt_.plugins_dir, opt_.data_dir, name, &bin, &data);
		obs_module_t *mod = nullptr;
		int rc = obs_open_module(&mod, bin.c_str(), data.c_str());
		if (rc != MODULE_SUCCESS) {
			blog(LOG_WARNING, "[skynat-engine] module %s not loaded (%d): %s", name.c_str(), rc,
			     bin.c_str());
			continue;
		}
		if (!obs_init_module(mod)) {
			blog(LOG_WARNING, "[skynat-engine] module %s failed to initialise", name.c_str());
			continue;
		}
		loaded.push_back(name);
	}
	obs_post_load_modules();

	if (opt_.video) {
		transition_ = obs_source_create_private("fade_transition", "program", nullptr);
		if (!transition_) {
			*error = "the obs-transitions module is missing (fade_transition)";
			return false;
		}
		obs_transition_set_size(transition_, opt_.width, opt_.height);
		obs_set_output_source(0, transition_);

		overlay_scene_ = obs_scene_create_private("overlays");
		obs_set_output_source(1, obs_scene_get_source(overlay_scene_));

		preview_view_ = obs_view_create();
		thumbs_ = std::make_unique<Thumbnails>(writer_);
		projectors_ = std::make_unique<Projectors>(preview_view_, [this](uint32_t id, const std::string &reason) {
			writer_.event(JsonObj()
					      .str("event", "projector")
					      .integer("id", id)
					      .str("state", "closed")
					      .str("reason", reason)
					      .dump());
		});
	}

	for (const auto &id : platform::hardware_h264_encoders())
		if (obs_get_encoder_codec(id.c_str()))
			hw_encoders_.push_back(id);
	for (const auto &id : platform::aac_encoders())
		if (obs_get_encoder_codec(id.c_str()))
			aac_ids_.push_back(id);

	obs_add_tick_callback(stats_tick, this);
	started_ = true;
	blog(LOG_INFO, "[skynat-engine] started: %ux%u %u/%u fps, renderer %s, %u modules, h264 hw: %s",
	     opt_.width, opt_.height, opt_.fps_num, opt_.fps_den,
	     opt_.video ? g_graphics_module.c_str() : "none (--no-video)", (unsigned)loaded.size(),
	     hw_encoders_.empty() ? "none" : hw_encoders_[0].c_str());
	return true;
}

std::string Engine::capabilities_json()
{
	std::vector<std::string> hw, aac;
	for (auto &id : hw_encoders_)
		hw.push_back(json_escape(id));
	for (auto &id : aac_ids_)
		aac.push_back(json_escape(id));
	JsonObj video;
	video.integer("width", opt_.width)
		.integer("height", opt_.height)
		.integer("fps_num", opt_.fps_num)
		.integer("fps_den", opt_.fps_den)
		.str("format", "nv12")
		.str("colorspace", "709")
		.str("range", "partial");
	JsonObj audio;
	audio.integer("sample_rate", 48000).integer("channels", 2);
	JsonObj enc;
	enc.raw("h264_hardware", json_array(hw))
		.raw("aac", json_array(aac))
		.boolean("x264", obs_get_encoder_codec("obs_x264") != nullptr);
	JsonObj caps;
	caps.str("engine", "skynat-engine")
		.str("version", kEngineVersion)
		.integer("protocol", kProtocolVersion)
		.str("libobs", obs_get_version_string())
		.str("renderer", opt_.video ? opt_.renderer : "none")
		.integer("pid", platform::process_id())
		.raw("video", opt_.video ? video.dump() : "null")
		.raw("audio", audio.dump())
		.raw("encoders", enc.dump())
		.raw("sources", "[\"camera\",\"media\",\"image\",\"color\",\"overlay\",\"pcm\"]");
	return caps.dump();
}

std::string Engine::ready_event()
{
	// {"event":"ready", ...capabilities}
	return "{\"event\":\"ready\"," + capabilities_json().substr(1);
}

// ===========================================================================
// messages
// ===========================================================================

void Engine::on_message(uint8_t type, std::vector<uint8_t> &&payload)
{
	if (shutting_down_.load())
		return;
	switch (type) {
	case MSG_PCM: {
		if (payload.size() < kPcmHeader)
			return;
		const uint8_t *p = payload.data();
		uint32_t id = get_u32(p);
		uint32_t frames = get_u32(p + 4);
		uint32_t delay_us = get_u32(p + 8);
		if ((uint64_t)frames * 8 != payload.size() - kPcmHeader) {
			pcm_unknown_++;
			return;
		}
		obs_source_t *src = nullptr;
		{
			std::lock_guard<std::mutex> lock(pcm_mutex_);
			auto it = pcm_sources_.find(id);
			if (it != pcm_sources_.end())
				src = obs_source_get_ref(it->second);
		}
		if (!src) {
			pcm_unknown_++;
			return;
		}
		pcm_messages_++;
		pcm_push(src, reinterpret_cast<const float *>(p + kPcmHeader), frames, delay_us);
		obs_source_release(src);
		return;
	}
	case MSG_COMMAND: {
		std::string json(payload.begin(), payload.end());
		platform::post_main([this, json]() { handle_command(json); });
		return;
	}
	case MSG_OVERLAY: {
		auto shared = std::make_shared<std::vector<uint8_t>>(std::move(payload));
		platform::post_main([this, shared]() { handle_overlay(*shared); });
		return;
	}
	default:
		writer_.event(JsonObj()
				      .str("event", "error")
				      .str("message", "unknown message type " + std::to_string(type))
				      .dump());
	}
}

void Engine::handle_overlay(const std::vector<uint8_t> &payload)
{
	if (shutting_down_.load())
		return;
	std::string error;
	if (payload.size() < kOverlayHeader) {
		error = "overlay message too short";
	} else {
		const uint8_t *p = payload.data();
		uint32_t id = get_u32(p);
		auto it = sources_.find(id);
		if (it == sources_.end() || it->second.type != "overlay") {
			error = "no overlay source with id " + std::to_string(id);
		} else {
			uint32_t x = get_u32(p + 4), y = get_u32(p + 8), w = get_u32(p + 12), h = get_u32(p + 16);
			uint32_t stride = get_u32(p + 20), flags = get_u32(p + 24);
			if (overlay_write(it->second.source, x, y, w, h, stride, (flags & OVERLAY_CLEAR) != 0,
					  p + kOverlayHeader, payload.size() - kOverlayHeader, &error))
				overlay_updates_++;
		}
	}
	if (!error.empty())
		writer_.event(JsonObj().str("event", "error").str("message", "overlay: " + error).dump());
}

void Engine::handle_command(const std::string &json)
{
	if (shutting_down_.load())
		return;
	obs_data_t *cmd = obs_data_create_from_json(json.c_str());
	if (!cmd) {
		writer_.event(JsonObj()
				      .str("event", "result")
				      .null("id")
				      .boolean("ok", false)
				      .str("error", "command is not valid JSON")
				      .dump());
		return;
	}
	const std::string name = get_str(cmd, "cmd");
	const std::string req = req_json(cmd);
	std::string data;
	std::string error;
	bool quit = false;
	// Commands that need the renderer (all but hello, list_devices, stats and
	// shutdown) are refused when the engine runs with --no-video.
	using Handler = std::string (Engine::*)(obs_data_t *);
	static const struct {
		const char *name;
		Handler fn;
		bool video;
	} kCommands[] = {
		{"hello", &Engine::cmd_hello, false},
		{"configure", &Engine::cmd_configure, true},
		{"list_devices", &Engine::cmd_list_devices, false},
		{"add_source", &Engine::cmd_add_source, true},
		{"update_source", &Engine::cmd_update_source, true},
		{"remove_source", &Engine::cmd_remove_source, true},
		{"set_program", &Engine::cmd_set_program, true},
		{"set_preview", &Engine::cmd_set_preview, true},
		{"overlay_visible", &Engine::cmd_overlay_visible, true},
		{"media", &Engine::cmd_media, true},
		{"lut", &Engine::cmd_lut, true},
		{"start_output", &Engine::cmd_start_output, true},
		{"stop_output", &Engine::cmd_stop_output, true},
		{"set_bitrate", &Engine::cmd_set_bitrate, true},
		{"preview_feed", &Engine::cmd_preview_feed, true},
		{"thumbnails", &Engine::cmd_thumbnails, true},
		{"projector", &Engine::cmd_projector, true},
		{"stats", &Engine::cmd_stats, false},
	};
	try {
		bool known = false;
		if (name == "shutdown") {
			known = true;
			quit = true;
		}
		for (const auto &c : kCommands) {
			if (known || name != c.name)
				continue;
			known = true;
			if (c.video && !opt_.video)
				throw CommandError("video is off (the engine was started with --no-video)");
			data = (this->*c.fn)(cmd);
		}
		if (!known)
			throw CommandError("unknown command \"" + name + "\"");
	} catch (const CommandError &e) {
		error = e.what();
	} catch (const std::exception &e) {
		error = std::string("internal error: ") + e.what();
	}

	obs_data_release(cmd);

	// Commands are matched to results by "req" (see PROTOCOL.md).
	JsonObj res;
	res.str("event", "result").raw("req", req).str("cmd", name).boolean("ok", error.empty());
	if (!error.empty())
		res.str("error", error);
	else if (!data.empty())
		res.raw("data", data);
	writer_.event(res.dump());

	if (quit)
		shutdown("shutdown command");
}

// ===========================================================================
// commands
// ===========================================================================

std::string Engine::cmd_hello(obs_data_t *)
{
	return capabilities_json();
}

bool Engine::any_output_active()
{
	return (output_ && obs_output_active(output_)) || (feed_program_.output && obs_output_active(feed_program_.output)) ||
	       (feed_preview_.output && obs_output_active(feed_preview_.output));
}

// Fitted inside the canvas (letterboxed), turned first when the file says so
// (a phone's portrait video is stored on its side with a rotation to show it
// upright): turned about its centre, the bounds swapped for a quarter turn.
void Engine::fit_to_canvas(obs_sceneitem_t *item, int rotate)
{
	const bool quarter = rotate == 90 || rotate == 270;
	struct vec2 bounds;
	vec2_set(&bounds, (float)(quarter ? opt_.height : opt_.width), (float)(quarter ? opt_.width : opt_.height));
	struct vec2 pos;
	if (rotate) {
		vec2_set(&pos, (float)opt_.width * 0.5f, (float)opt_.height * 0.5f);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
	} else {
		vec2_set(&pos, 0.0f, 0.0f);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	}
	obs_sceneitem_set_rot(item, (float)rotate);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
}

// 0, 90, 180 or 270 from a command's "rotate" (degrees clockwise), else 0.
static int rotation_of(obs_data_t *cmd)
{
	const int64_t r = ((get_int(cmd, "rotate", 0) % 360) + 360) % 360;
	return (r == 90 || r == 180 || r == 270) ? (int)r : 0;
}

// How each camera and clip is scaled to the canvas, from its own size (known
// only once it delivers frames, and it may change): libobs's bicubic whenever
// it is scaled — enlarged (a 720p camera on a 1080p canvas: bilinear left it
// soft) or made smaller by up to half (a portrait phone video fitted to the
// canvas's height: sharper than bilinear; the area filter measured softer
// still, VMAF 89 against bilinear's 94) — below half libobs itself takes its
// low-resolution filter. Not scaled: drawn as it is. libobs then draws the
// source through a texture at its own size and scales that once. Main
// thread, twice a second.
void Engine::update_scale_filters()
{
	for (auto &kv : sources_) {
		Source &s = kv.second;
		if (!s.wrapper_item || !s.source || (s.type != "camera" && s.type != "media"))
			continue;
		uint32_t sw = obs_source_get_width(s.source), sh = obs_source_get_height(s.source);
		if (!sw || !sh)
			continue;
		// Turned a quarter (a phone's portrait video): fitted by its turned size.
		if (s.rotate == 90 || s.rotate == 270)
			std::swap(sw, sh);
		const double k = std::min((double)opt_.width / sw, (double)opt_.height / sh);
		// Enlarged: bicubic. Made smaller: Lanczos (six taps a side — a portrait video at
		// 0.56 kept its detail where bicubic's four softened it).
		const obs_scale_type want = k > 1.01 ? OBS_SCALE_BICUBIC : k < 0.99 ? OBS_SCALE_LANCZOS : OBS_SCALE_DISABLE;
		if (obs_sceneitem_get_scale_filter(s.wrapper_item) != want)
			obs_sceneitem_set_scale_filter(s.wrapper_item, want);
	}
}

std::string Engine::cmd_configure(obs_data_t *cmd)
{
	if (any_output_active() || feed_program_.output || feed_preview_.output)
		throw CommandError("stop the output and monitor feeds before changing the canvas");
	uint32_t w = (uint32_t)get_int(cmd, "width", opt_.width);
	uint32_t h = (uint32_t)get_int(cmd, "height", opt_.height);
	uint32_t num = opt_.fps_num, den = opt_.fps_den;
	if (has(cmd, "fps_num")) {
		num = (uint32_t)get_int(cmd, "fps_num", 30);
		den = (uint32_t)get_int(cmd, "fps_den", 1);
	} else if (has(cmd, "fps")) {
		double fps = get_double(cmd, "fps", 30.0);
		if (std::fabs(fps - std::round(fps)) < 1e-6) {
			num = (uint32_t)std::lround(fps);
			den = 1;
		} else {
			num = (uint32_t)std::lround(fps * 1001.0);
			den = 1001;
		}
	}
	if (w < 64 || h < 64 || w > 7680 || h > 4320 || (w % 2) || (h % 2) || !num || !den)
		throw CommandError("unsupported canvas size or frame rate");
	if (has(cmd, "bitrate"))
		video_kbps_ = (uint32_t)get_int(cmd, "bitrate", video_kbps_);

	const bool thumbs_on = thumbs_->enabled();
	const uint32_t thumb_w = thumbs_->width(), thumb_h = thumbs_->height();
	const double thumb_fps = thumbs_->fps();
	const Thumbnails::Format thumb_format = thumbs_->format();
	const bool thumb_changes = thumbs_->only_changes();
	if (thumbs_on)
		thumbs_->disable();
	std::string error;
	if (!reset_video(w, h, num, den, &error))
		throw CommandError(error);
	obs_transition_set_size(transition_, w, h);
	for (auto &kv : sources_) {
		if (kv.second.wrapper_item)
			fit_to_canvas(kv.second.wrapper_item, kv.second.rotate);
		if (kv.second.type == "color") {
			obs_data_t *s = obs_data_create();
			obs_data_set_int(s, "width", w);
			obs_data_set_int(s, "height", h);
			obs_source_update(kv.second.source, s);
			obs_data_release(s);
		}
	}
	if (thumbs_on)
		thumbs_->enable(thumb_w, thumb_h, thumb_fps, thumb_format, thumb_changes);
	return JsonObj()
		.integer("width", w)
		.integer("height", h)
		.integer("fps_num", num)
		.integer("fps_den", den)
		.dump();
}

std::string Engine::cmd_list_devices(obs_data_t *)
{
	std::vector<std::string> cams;
	const double fps = (double)opt_.fps_num / opt_.fps_den;
	for (const auto &c : platform::list_cameras()) {
		JsonObj cam;
		cam.str("id", c.id).str("name", c.name);
		// The format the zero-copy path would pick for this canvas (macOS).
		platform::CameraFormat f;
		if (platform::pick_camera_format(c.id, (int)opt_.width, (int)opt_.height, fps, &f))
			cam.raw("best_format", JsonObj()
						      .integer("width", f.width)
						      .integer("height", f.height)
						      .number("fps", (double)f.fps_num / f.fps_den)
						      .dump());
		cams.push_back(cam.dump());
	}
	return JsonObj().raw("cameras", json_array(cams)).str("camera_permission", platform::camera_permission()).dump();
}

obs_data_t *Engine::camera_settings(obs_data_t *cmd, const std::string &device_in, bool *fast, std::string *error)
{
	std::string device = device_in;
	std::string name;
	auto cams = platform::list_cameras();
	if (device.empty()) {
		std::string want = get_str(cmd, "device_name");
		for (const auto &c : cams)
			if (c.name == want) {
				device = c.id;
				break;
			}
		if (device.empty() && !want.empty()) {
			std::string lw = want;
			std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
			for (const auto &c : cams) {
				std::string ln = c.name;
				std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
				if (ln.find(lw) != std::string::npos) {
					device = c.id;
					break;
				}
			}
		}
	}
	for (const auto &c : cams)
		if (c.id == device)
			name = c.name;
	if (device.empty() || name.empty()) {
		*error = "camera not found";
		return nullptr;
	}

	const int want_w = (int)get_int(cmd, "width", opt_.width);
	const int want_h = (int)get_int(cmd, "height", opt_.height);
	const double want_fps = get_double(cmd, "fps", (double)opt_.fps_num / opt_.fps_den);

	obs_data_t *s = obs_data_create();
#ifdef __APPLE__
	obs_data_set_string(s, "device", device.c_str());
	obs_data_set_string(s, "device_name", name.c_str());
	obs_data_set_bool(s, "enable_audio", false);
	if (*fast) {
		platform::CameraFormat f;
		if (platform::pick_camera_format(device, want_w, want_h, want_fps, &f)) {
			obs_data_set_bool(s, "use_preset", false);
			std::string res = "{\"width\":" + std::to_string(f.width) + ",\"height\":" + std::to_string(f.height) + "}";
			obs_data_set_string(s, "resolution", res.c_str());
			struct media_frames_per_second fps = {f.fps_num, f.fps_den};
			obs_data_set_frames_per_second(s, "frame_rate", fps, nullptr);
			obs_data_set_int(s, "input_format", f.obs_video_format);
		} else {
			*fast = false;
		}
	}
	if (!*fast) {
		obs_data_set_bool(s, "use_preset", true);
		obs_data_set_string(s, "preset", "AVCaptureSessionPresetHigh");
		obs_data_set_bool(s, "buffering", false);
	}
#elif defined(_WIN32)
	*fast = false;
	obs_data_set_string(s, "video_device_id", device.c_str());
	obs_data_set_int(s, "res_type", 1); // custom
	std::string res = std::to_string(want_w) + "x" + std::to_string(want_h);
	obs_data_set_string(s, "resolution", res.c_str());
	obs_data_set_int(s, "frame_interval", (int64_t)std::llround(10000000.0 / (want_fps > 0 ? want_fps : 30.0)));
	obs_data_set_bool(s, "active", true);
	obs_data_set_int(s, "audio_output_mode", 0);
#endif
	return s;
}

bool Engine::apply_common(Source &s, obs_data_t *cmd)
{
	if (has(cmd, "volume"))
		obs_source_set_volume(s.source, (float)get_double(cmd, "volume", 1.0));
	if (has(cmd, "muted"))
		obs_source_set_muted(s.source, get_bool(cmd, "muted", false));
	return true;
}

void Engine::apply_overlay_layout(Source &s, obs_data_t *cmd)
{
	if (!s.overlay_item)
		return;
	if (has(cmd, "x") || has(cmd, "y")) {
		struct vec2 pos;
		obs_sceneitem_get_pos(s.overlay_item, &pos);
		if (has(cmd, "x"))
			pos.x = (float)get_double(cmd, "x", 0);
		if (has(cmd, "y"))
			pos.y = (float)get_double(cmd, "y", 0);
		obs_sceneitem_set_pos(s.overlay_item, &pos);
	}
	if (has(cmd, "visible"))
		obs_sceneitem_set_visible(s.overlay_item, get_bool(cmd, "visible", true));
}

static void media_ended(void *data, calldata_t *cd)
{
	auto *writer = static_cast<Writer *>(data);
	obs_source_t *src = (obs_source_t *)calldata_ptr(cd, "source");
	obs_data_t *settings = src ? obs_source_get_settings(src) : nullptr;
	int64_t id = settings ? obs_data_get_int(settings, "skynat_id") : -1;
	obs_data_release(settings);
	writer->event(JsonObj().str("event", "media").integer("id", id).str("state", "ended").dump());
}

std::string Engine::cmd_add_source(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	if (sources_.count(id))
		throw CommandError("a source with id " + std::to_string(id) + " already exists");
	const std::string type = get_str(cmd, "type");
	const std::string name = "src-" + std::to_string(id);

	Source s;
	s.id = id;
	s.type = type;
	obs_data_t *settings = nullptr;
	const char *obs_id = nullptr;
	std::string extra;

	if (type == "camera") {
		bool fast = get_bool(cmd, "fast", true);
		std::string error;
		settings = camera_settings(cmd, get_str(cmd, "device"), &fast, &error);
		if (!settings)
			throw CommandError(error);
		obs_id = platform::camera_source_id(fast);
		s.fast_camera = fast;
		extra = JsonObj()
				.str("device", obs_data_get_string(settings, "device"))
				.boolean("fast", fast)
				.str("resolution", obs_data_get_string(settings, "resolution"))
				.dump();
	} else if (type == "media") {
		std::string path = get_str(cmd, "path");
		if (path.empty())
			throw CommandError("media needs \"path\"");
		if (!os_file_exists(path.c_str()))
			throw CommandError("file not found: " + path);
		settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", path.c_str());
		obs_data_set_bool(settings, "looping", get_bool(cmd, "loop", true));
		obs_data_set_bool(settings, "restart_on_activate", get_bool(cmd, "restart_on_activate", false));
		obs_data_set_bool(settings, "hw_decode", get_bool(cmd, "hw_decode", true));
		obs_data_set_bool(settings, "close_when_inactive", false);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		obs_data_set_bool(settings, "log_changes", false);
		obs_id = "ffmpeg_source";
	} else if (type == "image") {
		std::string path = get_str(cmd, "path");
		if (path.empty() || !os_file_exists(path.c_str()))
			throw CommandError("image file not found: " + path);
		settings = obs_data_create();
		obs_data_set_string(settings, "file", path.c_str());
		obs_data_set_bool(settings, "unload", false);
		obs_id = "image_source";
	} else if (type == "color") {
		settings = obs_data_create();
		obs_data_set_int(settings, "color", parse_color(get_str(cmd, "color", "#000000")));
		obs_data_set_int(settings, "width", opt_.width);
		obs_data_set_int(settings, "height", opt_.height);
		obs_id = "color_source";
	} else if (type == "overlay") {
		settings = obs_data_create();
		obs_data_set_int(settings, "width", get_int(cmd, "width", opt_.width));
		obs_data_set_int(settings, "height", get_int(cmd, "height", opt_.height));
		obs_data_set_bool(settings, "premultiplied", get_bool(cmd, "premultiplied", false));
		obs_id = "skynat_overlay";
	} else if (type == "pcm") {
		settings = obs_data_create();
		obs_id = "skynat_pcm";
	} else {
		throw CommandError("unknown source type \"" + type + "\"");
	}
	obs_data_set_int(settings, "skynat_id", id);

	s.source = obs_source_create_private(obs_id, name.c_str(), settings);
	obs_data_release(settings);
	if (!s.source)
		throw CommandError(std::string("could not create source ") + obs_id + " (module missing?)");
	obs_source_set_monitoring_type(s.source, OBS_MONITORING_TYPE_NONE);

	if (is_input_type(type)) {
		s.wrapper = obs_scene_create_private(("in-" + std::to_string(id)).c_str());
		s.wrapper_item = obs_scene_add(s.wrapper, s.source);
		s.rotate = rotation_of(cmd);
		fit_to_canvas(s.wrapper_item, s.rotate);
		thumbs_->add(id, obs_scene_get_source(s.wrapper));
		if (type == "camera" && !get_bool(cmd, "audio", false))
			obs_source_set_muted(s.source, true); // the app's mixer owns sound
		if (type == "media") {
			signal_handler_connect(obs_source_get_signal_handler(s.source), "media_ended", media_ended,
					       &writer_);
			if (get_str(cmd, "hdr") == "as_is")
				set_signal_as_is(s.source, true);
		}
	} else if (type == "overlay") {
		s.overlay_item = obs_scene_add(overlay_scene_, s.source);
		struct vec2 pos;
		vec2_set(&pos, (float)get_double(cmd, "x", 0), (float)get_double(cmd, "y", 0));
		obs_sceneitem_set_pos(s.overlay_item, &pos);
		obs_sceneitem_set_visible(s.overlay_item, get_bool(cmd, "visible", true));
	} else if (type == "pcm") {
		uint32_t channel = kFirstAudioChannel;
		for (; channel < kMaxChannels; channel++) {
			bool used = false;
			for (auto &kv : sources_)
				used = used || kv.second.audio_channel == channel;
			if (!used)
				break;
		}
		if (channel >= kMaxChannels) {
			obs_source_release(s.source);
			throw CommandError("too many audio inputs");
		}
		s.audio_channel = channel;
		obs_set_output_source(channel, s.source);
		std::lock_guard<std::mutex> lock(pcm_mutex_);
		pcm_sources_[id] = s.source;
	}
	apply_common(s, cmd);
	sources_[id] = s;

	JsonObj out;
	out.integer("id", id).str("type", type);
	if (!extra.empty())
		out.raw("camera", extra);
	return out.dump();
}

std::string Engine::cmd_update_source(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	auto it = sources_.find(id);
	if (it == sources_.end())
		throw CommandError("no source with id " + std::to_string(id));
	Source &s = it->second;
	obs_data_t *settings = obs_data_create();
	bool changed = false;
	if (s.type == "camera") {
		if (has(cmd, "device") || has(cmd, "device_name") || has(cmd, "width") || has(cmd, "height") ||
		    has(cmd, "fps")) {
			obs_data_t *cur = obs_source_get_settings(s.source);
			std::string device = has(cmd, "device") || has(cmd, "device_name")
						     ? get_str(cmd, "device")
						     : std::string(obs_data_get_string(cur, "device"));
			obs_data_release(cur);
			bool fast = s.fast_camera;
			std::string error;
			obs_data_t *cs = camera_settings(cmd, device, &fast, &error);
			if (!cs) {
				obs_data_release(settings);
				throw CommandError(error);
			}
			if (fast != s.fast_camera) {
				obs_data_release(cs);
				obs_data_release(settings);
				throw CommandError("this camera mode needs remove_source + add_source");
			}
			obs_data_apply(settings, cs);
			obs_data_release(cs);
			changed = true;
		}
		if (has(cmd, "audio"))
			obs_source_set_muted(s.source, !get_bool(cmd, "audio", false));
	} else if (s.type == "media") {
		if (has(cmd, "path")) {
			std::string path = get_str(cmd, "path");
			if (!os_file_exists(path.c_str())) {
				obs_data_release(settings);
				throw CommandError("file not found: " + path);
			}
			obs_data_set_string(settings, "local_file", path.c_str());
			changed = true;
		}
		if (has(cmd, "loop")) {
			obs_data_set_bool(settings, "looping", get_bool(cmd, "loop", true));
			changed = true;
		}
		if (has(cmd, "hw_decode")) {
			obs_data_set_bool(settings, "hw_decode", get_bool(cmd, "hw_decode", true));
			changed = true;
		}
		if (has(cmd, "restart_on_activate")) {
			obs_data_set_bool(settings, "restart_on_activate", get_bool(cmd, "restart_on_activate", false));
			changed = true;
		}
		if (has(cmd, "hdr"))
			set_signal_as_is(s.source, get_str(cmd, "hdr") == "as_is");
		if (has(cmd, "rotate") && s.wrapper_item) {
			s.rotate = rotation_of(cmd);
			fit_to_canvas(s.wrapper_item, s.rotate);
		}
	} else if (s.type == "image") {
		if (has(cmd, "path")) {
			std::string path = get_str(cmd, "path");
			if (!os_file_exists(path.c_str())) {
				obs_data_release(settings);
				throw CommandError("image file not found: " + path);
			}
			obs_data_set_string(settings, "file", path.c_str());
			changed = true;
		}
	} else if (s.type == "color") {
		if (has(cmd, "color")) {
			obs_data_set_int(settings, "color", parse_color(get_str(cmd, "color")));
			changed = true;
		}
	} else if (s.type == "overlay") {
		if (has(cmd, "width") || has(cmd, "height") || has(cmd, "premultiplied")) {
			obs_data_t *cur = obs_source_get_settings(s.source);
			obs_data_set_int(settings, "width", get_int(cmd, "width", obs_data_get_int(cur, "width")));
			obs_data_set_int(settings, "height", get_int(cmd, "height", obs_data_get_int(cur, "height")));
			obs_data_set_bool(settings, "premultiplied",
					  get_bool(cmd, "premultiplied", obs_data_get_bool(cur, "premultiplied")));
			obs_data_release(cur);
			changed = true;
		}
		apply_overlay_layout(s, cmd);
	}
	if (changed)
		obs_source_update(s.source, settings);
	obs_data_release(settings);
	apply_common(s, cmd);
	return JsonObj().integer("id", id).boolean("changed", changed).dump();
}

void Engine::remove_source_internal(Source &s)
{
	if (program_id_ == (int64_t)s.id) {
		obs_transition_set(transition_, nullptr);
		program_id_ = -1;
	}
	if (preview_id_ == (int64_t)s.id) {
		obs_view_set_source(preview_view_, 0, nullptr);
		preview_id_ = -1;
	}
	if (thumbs_)
		thumbs_->remove(s.id);
	if (s.type == "pcm") {
		{
			std::lock_guard<std::mutex> lock(pcm_mutex_);
			pcm_sources_.erase(s.id);
		}
		obs_set_output_source(s.audio_channel, nullptr);
	}
	if (s.type == "media")
		signal_handler_disconnect(obs_source_get_signal_handler(s.source), "media_ended", media_ended,
					  &writer_);
	if (s.overlay_item)
		obs_sceneitem_remove(s.overlay_item);
	if (s.wrapper)
		obs_scene_release(s.wrapper);
	obs_source_remove(s.source);
	obs_source_release(s.source);
	s = Source();
}

std::string Engine::cmd_remove_source(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	auto it = sources_.find(id);
	if (it == sources_.end())
		throw CommandError("no source with id " + std::to_string(id));
	remove_source_internal(it->second);
	sources_.erase(it);
	return JsonObj().integer("id", id).dump();
}

std::string Engine::cmd_set_program(obs_data_t *cmd)
{
	obs_source_t *target = nullptr;
	int64_t id = -1;
	if (is_number(cmd, "id")) {
		id = obs_data_get_int(cmd, "id");
		auto it = sources_.find((uint32_t)id);
		if (it == sources_.end() || !it->second.wrapper)
			throw CommandError("no input with id " + std::to_string(id));
		target = obs_scene_get_source(it->second.wrapper);
	}
	const std::string tr = get_str(cmd, "transition", "cut");
	const uint32_t ms = (uint32_t)std::clamp<int64_t>(get_int(cmd, "ms", 300), 0, 10000);
	if (tr == "fade" && ms > 0)
		obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, ms, target);
	else if (tr == "cut" || tr == "fade")
		obs_transition_set(transition_, target);
	else
		throw CommandError("transition must be \"cut\" or \"fade\"");
	program_id_ = id;
	return JsonObj().integer("id", id).str("transition", tr).integer("ms", tr == "fade" ? ms : 0).dump();
}

std::string Engine::cmd_set_preview(obs_data_t *cmd)
{
	obs_source_t *target = nullptr;
	int64_t id = -1;
	if (is_number(cmd, "id")) {
		id = obs_data_get_int(cmd, "id");
		auto it = sources_.find((uint32_t)id);
		if (it == sources_.end() || !it->second.wrapper)
			throw CommandError("no input with id " + std::to_string(id));
		target = obs_scene_get_source(it->second.wrapper);
	}
	obs_view_set_source(preview_view_, 0, target);
	preview_id_ = id;
	return JsonObj().integer("id", id).dump();
}

std::string Engine::cmd_overlay_visible(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	auto it = sources_.find(id);
	if (it == sources_.end() || !it->second.overlay_item)
		throw CommandError("no overlay with id " + std::to_string(id));
	bool visible = get_bool(cmd, "visible", true);
	obs_sceneitem_set_visible(it->second.overlay_item, visible);
	return JsonObj().integer("id", id).boolean("visible", visible).dump();
}

// lut {id | target: "program", path, amount}: libobs's own "Apply LUT" filter
// (clut_filter: a .cube or PNG look-up table) on a source, or on Program under
// the overlays. An empty path takes it off. The table is the app's: the
// engine only applies the file it is given.
std::string Engine::cmd_lut(obs_data_t *cmd)
{
	obs_source_t *target = nullptr;
	std::string which;
	if (get_str(cmd, "target") == "program") {
		target = transition_;
		which = "program";
	} else {
		const uint32_t id = require_id(cmd);
		auto it = sources_.find(id);
		if (it == sources_.end() || !it->second.source)
			throw CommandError("no source with id " + std::to_string(id));
		target = it->second.source;
		which = std::to_string(id);
	}
	if (!target)
		throw CommandError("nothing to apply a LUT to");
	const std::string path = get_str(cmd, "path");
	const double amount = std::clamp(get_double(cmd, "amount", 1.0), 0.0, 1.0);
	static const char *kName = "skynat-lut";
	obs_source_t *filter = obs_source_get_filter_by_name(target, kName);
	if (path.empty()) {
		if (filter) {
			obs_source_filter_remove(target, filter);
			obs_source_release(filter);
		}
		return JsonObj().str("target", which).boolean("on", false).dump();
	}
	if (!os_file_exists(path.c_str())) {
		if (filter)
			obs_source_release(filter);
		throw CommandError("LUT file not found: " + path);
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "image_path", path.c_str());
	obs_data_set_double(settings, "clut_amount", amount);
	if (filter) {
		obs_source_update(filter, settings);
		obs_source_release(filter);
	} else {
		filter = obs_source_create_private("clut_filter", kName, settings);
		if (!filter) {
			obs_data_release(settings);
			throw CommandError("libobs's LUT filter is not available");
		}
		obs_source_filter_add(target, filter);
		obs_source_release(filter);
	}
	obs_data_release(settings);
	return JsonObj().str("target", which).boolean("on", true).number("amount", amount).dump();
}

std::string Engine::cmd_media(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	auto it = sources_.find(id);
	if (it == sources_.end() || it->second.type != "media")
		throw CommandError("no media source with id " + std::to_string(id));
	const std::string action = get_str(cmd, "action");
	obs_source_t *src = it->second.source;
	if (action == "play")
		obs_source_media_play_pause(src, false);
	else if (action == "pause")
		obs_source_media_play_pause(src, true);
	else if (action == "restart")
		obs_source_media_restart(src);
	else if (action == "stop")
		obs_source_media_stop(src);
	else
		throw CommandError("action must be play, pause, restart or stop");
	return JsonObj().integer("id", id).str("action", action).dump();
}

// An encoder opened with the engine's generic defaults (constant bitrate at
// kbps, keyframes every keyint_sec; x264 veryfast and zerolatency), then the
// documented libobs settings the app passed for this encoder id, if any
// (`overrides`: {encoder id: {setting: value}}), applied as given — the app
// decides how its encoders are tuned — and then the pipe's own constraints
// again: the bitrate and keyframe interval asked for, no B-frames (the
// protocol's consumers take frames in arrival order), and x264 keyframes only
// at the interval (scenecut=0).
obs_encoder_t *Engine::create_video_encoder(const std::string &name, const std::string &want, uint32_t kbps,
					    uint32_t keyint_sec, const char *profile, obs_data_t *overrides,
					    std::string *chosen, std::string *error)
{
	std::vector<std::string> candidates;
	if (want == "x264")
		candidates = {"obs_x264"};
	else if (want == "hardware")
		candidates = hw_encoders_;
	else if (want.empty() || want == "auto") {
		candidates = hw_encoders_;
		candidates.push_back("obs_x264");
	} else
		candidates = {want};

	for (const auto &id : candidates) {
		if (!obs_get_encoder_codec(id.c_str()))
			continue;
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "rate_control", "CBR");
		obs_data_set_string(s, "profile", profile);
		const bool x264 = id == "obs_x264";
		const bool vt = id.rfind("com.apple.videotoolbox", 0) == 0;
		if (x264) {
			obs_data_set_string(s, "preset", "veryfast");
			obs_data_set_string(s, "tune", "zerolatency");
		} else if (vt) {
			// Intel Macs have no true CBR in VideoToolbox: average bitrate
			// with a hard ceiling instead (ignored where CBR exists).
			obs_data_set_bool(s, "limit_bitrate", true);
			obs_data_set_double(s, "max_bitrate_window", 1.5);
		}
		if (overrides) {
			if (obs_data_t *o = obs_data_get_obj(overrides, id.c_str())) {
				obs_data_apply(s, o);
				obs_data_release(o);
			}
		}
		// The pipe's constraints, whatever was asked for.
		obs_data_set_int(s, "bitrate", kbps);
		obs_data_set_int(s, "keyint_sec", keyint_sec);
		if (vt) {
			obs_data_set_bool(s, "bframes", false);
			obs_data_set_int(s, "max_bitrate", kbps);
		} else {
			// Intel Quick Sync calls it "bframes" (default 3), the others "bf".
			obs_data_set_int(s, "bframes", 0);
			obs_data_set_int(s, "bf", 0);
		}
		if (x264) {
			std::string opts = obs_data_get_string(s, "x264opts");
			if (opts.find("scenecut=") == std::string::npos)
				opts = opts.empty() ? "scenecut=0" : opts + " scenecut=0";
			obs_data_set_string(s, "x264opts", opts.c_str());
		}
		obs_encoder_t *enc = obs_video_encoder_create(id.c_str(), name.c_str(), s, nullptr);
		obs_data_release(s);
		if (enc) {
			*chosen = id;
			return enc;
		}
	}
	*error = want == "hardware" ? "no hardware H.264 encoder on this computer" : "no usable H.264 encoder";
	return nullptr;
}

obs_encoder_t *Engine::create_audio_encoder(uint32_t kbps, std::string *chosen)
{
	for (const auto &id : aac_ids_) {
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "bitrate", kbps);
		obs_data_set_bool(s, "allow he-aac", false); // plain AAC-LC for every platform
		obs_encoder_t *enc = obs_audio_encoder_create(id.c_str(), "program-aac", s, 0, nullptr);
		obs_data_release(s);
		if (enc) {
			*chosen = id;
			return enc;
		}
	}
	return nullptr;
}

void Engine::output_stopped(void *data, calldata_t *cd)
{
	auto *self = static_cast<Engine *>(data);
	obs_output_t *out = (obs_output_t *)calldata_ptr(cd, "output");
	int code = (int)calldata_int(cd, "code");
	const char *last_error = out ? obs_output_get_last_error(out) : nullptr;
	JsonObj ev;
	ev.str("event", "output").str("name", out ? obs_output_get_name(out) : "").str("state", "stopped").integer("code", code);
	if (last_error)
		ev.str("error", last_error);
	self->writer_.event(ev.dump());
}

static void wait_inactive(obs_output_t *out, int timeout_ms)
{
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (obs_output_active(out) && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	if (obs_output_active(out))
		obs_output_force_stop(out);
}

void Engine::stop_program_output(PipeOutputStats *final_stats)
{
	stop_feed(feed_stream_);
	stream_kbps_ = 0;
	if (output_) {
		obs_output_stop(output_);
		wait_inactive(output_, 3000);
		if (final_stats)
			*final_stats = pipe_output_stats(output_); // after the encoders drained
		signal_handler_disconnect(obs_output_get_signal_handler(output_), "stop", output_stopped, this);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (venc_) {
		obs_encoder_release(venc_);
		venc_ = nullptr;
	}
	if (aenc_) {
		obs_encoder_release(aenc_);
		aenc_ = nullptr;
	}
}

std::string Engine::cmd_start_output(obs_data_t *cmd)
{
	if (output_)
		throw CommandError("the output is already running");
	const uint32_t kbps = (uint32_t)std::clamp<int64_t>(get_int(cmd, "video_kbps", get_int(cmd, "bitrate", video_kbps_)), 200, 100000);
	const uint32_t akbps = (uint32_t)std::clamp<int64_t>(get_int(cmd, "audio_kbps", 160), 32, 320);
	const uint32_t keyint = (uint32_t)std::clamp<int64_t>(get_int(cmd, "keyint_sec", 2), 1, 10);
	std::string want = get_str(cmd, "encoder", "auto");
	// {encoder id: {settings}}: how the app tunes each encoder it may get.
	std::unique_ptr<obs_data_t, void (*)(obs_data_t *)> tuning(obs_data_get_obj(cmd, "encoder_settings"), obs_data_release);

	std::string error;
	for (int attempt = 0; attempt < 2; attempt++) {
		venc_ = create_video_encoder("program-h264", want, kbps, keyint, "high", tuning.get(), &venc_id_, &error);
		if (!venc_)
			throw CommandError(error);
		obs_encoder_set_video(venc_, obs_get_video());
		aenc_ = create_audio_encoder(akbps, &aenc_id_);
		if (!aenc_) {
			stop_program_output();
			throw CommandError("no AAC encoder available");
		}
		obs_encoder_set_audio(aenc_, obs_get_audio());

		output_ = obs_output_create("skynat_pipe_output", "program", nullptr, nullptr);
		obs_output_set_video_encoder(output_, venc_);
		obs_output_set_audio_encoder(output_, aenc_, 0);
		signal_handler_connect(obs_output_get_signal_handler(output_), "stop", output_stopped, this);
		if (obs_output_start(output_))
			break;
		const char *e = obs_output_get_last_error(output_);
		error = std::string("output failed to start") + (e ? std::string(": ") + e : "");
		const bool was_hardware = venc_id_ != "obs_x264";
		stop_program_output();
		// A hardware encoder that is present but refuses to open (busy,
		// unsupported size): fall back to x264 once, if allowed.
		if (attempt == 0 && was_hardware && (want == "auto" || want.empty())) {
			blog(LOG_WARNING, "[skynat-engine] %s; retrying with x264", error.c_str());
			want = "x264";
			continue;
		}
		throw CommandError(error);
	}
	video_kbps_ = kbps;
	// The stream's own picture (the app's choice of size, e.g. 960x540 at a low bitrate): a
	// second encoder of the same kind and tuning, scaled by libobs, sent as feed 2.
	JsonObj stream_json;
	bool stream_on = false;
	if (has(cmd, "stream")) {
		std::unique_ptr<obs_data_t, void (*)(obs_data_t *)> st(obs_data_get_obj(cmd, "stream"), obs_data_release);
		struct obs_video_info ovi;
		const bool have_video = obs_get_video_info(&ovi);
		const uint32_t sw = (uint32_t)std::clamp<int64_t>(get_int(st.get(), "width", 0), 0, 7680) & ~1u;
		const uint32_t sh = (uint32_t)std::clamp<int64_t>(get_int(st.get(), "height", 0), 0, 4320) & ~1u;
		const uint32_t skbps = (uint32_t)std::clamp<int64_t>(get_int(st.get(), "kbps", kbps), 200, 100000);
		if (have_video && sw >= 64 && sh >= 64 && sw <= ovi.base_width && sh <= ovi.base_height) {
			std::string sid, serr;
			feed_stream_.encoder = create_video_encoder("stream-h264", venc_id_, skbps, keyint, "high", tuning.get(), &sid, &serr);
			if (feed_stream_.encoder) {
				obs_encoder_set_scaled_size(feed_stream_.encoder, sw, sh);
				obs_encoder_set_gpu_scale_type(feed_stream_.encoder, sw * 2 <= ovi.base_width ? OBS_SCALE_AREA : OBS_SCALE_BICUBIC);
				obs_encoder_set_video(feed_stream_.encoder, obs_get_video());
				obs_data_t *fs = obs_data_create();
				obs_data_set_int(fs, "feed", FEED_STREAM);
				feed_stream_.output = obs_output_create("skynat_feed_output", "stream", fs, nullptr);
				obs_data_release(fs);
				obs_output_set_video_encoder(feed_stream_.output, feed_stream_.encoder);
				signal_handler_connect(obs_output_get_signal_handler(feed_stream_.output), "stop", output_stopped, this);
				stream_on = obs_output_start(feed_stream_.output);
				if (!stream_on) {
					const char *e = obs_output_get_last_error(feed_stream_.output);
					serr = std::string("the stream's encoder did not start") + (e ? std::string(": ") + e : "");
					stop_feed(feed_stream_);
				}
			}
			if (stream_on) {
				stream_kbps_ = skbps;
				stream_json.integer("width", sw).integer("height", sh).integer("kbps", skbps).str("encoder", sid);
			} else {
				blog(LOG_WARNING, "[skynat-engine] %s — the stream shares Program's picture", serr.c_str());
				stream_json.str("error", serr);
			}
		} else {
			stream_json.str("error", "the stream's size must be at least 64x64 and no larger than the canvas");
		}
	}
	writer_.event(JsonObj().str("event", "output").str("name", "program").str("state", "started").dump());
	// The settings the encoder was opened with (the app's, over the defaults).
	obs_data_t *applied = obs_encoder_get_settings(venc_);
	const std::string settings = applied ? obs_data_get_json(applied) : "{}";
	obs_data_release(applied);
	return JsonObj()
		.str("encoder", venc_id_)
		.raw("settings", settings)
		.boolean("hardware", venc_id_ != "obs_x264")
		.str("audio_encoder", aenc_id_)
		.integer("video_kbps", kbps)
		.integer("audio_kbps", akbps)
		.integer("keyint_sec", keyint)
		.raw("stream", has(cmd, "stream") ? stream_json.dump() : std::string("null"))
		.dump();
}

std::string Engine::cmd_stop_output(obs_data_t *)
{
	const bool was = output_ != nullptr;
	PipeOutputStats st;
	stop_program_output(&st);
	return JsonObj()
		.boolean("was_running", was)
		.integer("video_packets", (int64_t)st.video_packets)
		.integer("audio_packets", (int64_t)st.audio_packets)
		.dump();
}

std::string Engine::cmd_set_bitrate(obs_data_t *cmd)
{
	if (!venc_)
		throw CommandError("the output is not running");
	const bool stream = get_str(cmd, "target", "program") == "stream";
	if (stream && !feed_stream_.encoder)
		throw CommandError("the stream has no picture of its own");
	const uint32_t kbps = (uint32_t)std::clamp<int64_t>(get_int(cmd, "kbps", stream ? stream_kbps_ : video_kbps_), 200, 100000);
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", kbps);
	if (venc_id_.rfind("com.apple.videotoolbox", 0) == 0)
		obs_data_set_int(s, "max_bitrate", kbps);
	obs_encoder_update(stream ? feed_stream_.encoder : venc_, s);
	obs_data_release(s);
	if (stream)
		stream_kbps_ = kbps;
	else
		video_kbps_ = kbps;
	return JsonObj().integer("kbps", kbps).str("encoder", venc_id_).str("target", stream ? "stream" : "program").dump();
}

void Engine::stop_feed(Feed &feed)
{
	if (feed.output) {
		obs_output_stop(feed.output);
		wait_inactive(feed.output, 2000);
		signal_handler_disconnect(obs_output_get_signal_handler(feed.output), "stop", output_stopped, this);
		obs_output_release(feed.output);
	}
	if (feed.encoder)
		obs_encoder_release(feed.encoder);
	feed = Feed();
}

// The filter a monitor picture is made smaller with: area below the canvas's
// size, bicubic above it (a monitor bigger than the canvas).
static obs_scale_type monitor_scale(uint32_t w, uint32_t h)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && (w > ovi.base_width || h > ovi.base_height))
		return OBS_SCALE_BICUBIC;
	return OBS_SCALE_AREA;
}

std::string Engine::cmd_preview_feed(obs_data_t *cmd)
{
	const bool on = get_bool(cmd, "on", true);
	if (!on) {
		stop_feed(feed_program_);
		stop_feed(feed_preview_);
		if (preview_video_) {
			obs_view_remove(preview_view_);
			preview_video_ = nullptr;
		}
		return JsonObj().boolean("on", false).dump();
	}
	const bool want_program = get_bool(cmd, "program", true);
	const bool want_preview = get_bool(cmd, "preview", true);
	const uint32_t w = (uint32_t)std::clamp<int64_t>(get_int(cmd, "width", 854), 64, opt_.width) & ~1u;
	const uint32_t h = (uint32_t)std::clamp<int64_t>(get_int(cmd, "height", 480), 64, opt_.height) & ~1u;
	const double fps = std::clamp(get_double(cmd, "fps", 15.0), 1.0, 60.0);
	const uint32_t kbps = (uint32_t)std::clamp<int64_t>(get_int(cmd, "kbps", 1000), 100, 20000);
	const double main_fps = (double)opt_.fps_num / opt_.fps_den;
	const uint32_t divisor = (uint32_t)std::max<long>(1, std::lround(main_fps / fps));
	// Preview may run at a lower rate than Program (a monitor for choosing the
	// next shot: half the canvas rate is what the app's own Preview draws).
	const double preview_fps = std::clamp(get_double(cmd, "preview_fps", fps), 1.0, 60.0);
	const uint32_t preview_divisor = (uint32_t)std::max<long>(1, std::lround(main_fps / preview_fps));
	const std::string want = get_str(cmd, "encoder", "auto");
	const bool gpu_scale = get_bool(cmd, "gpu_scale", false);

	auto start = [&](Feed &feed, uint8_t feed_id, const char *name, video_t *video, bool scale, uint32_t div) {
		std::string chosen, error;
		feed.encoder = create_video_encoder(name, want, kbps, 1, "main", nullptr, &chosen, &error);
		if (!feed.encoder)
			throw CommandError(error);
		if (scale) {
			obs_encoder_set_scaled_size(feed.encoder, w, h);
			// Made smaller with the area filter (every canvas pixel counted by
			// how much of it a monitor pixel covers): OBS's bicubic reads a
			// fixed 4x4 of the canvas, which skips pixels below full size —
			// thin lines and small text shimmered on the monitors.
			if (gpu_scale)
				obs_encoder_set_gpu_scale_type(feed.encoder, monitor_scale(w, h));
		}
		obs_encoder_set_frame_rate_divisor(feed.encoder, div);
		obs_encoder_set_video(feed.encoder, video);
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "feed", feed_id);
		feed.output = obs_output_create("skynat_feed_output", name, s, nullptr);
		obs_data_release(s);
		obs_output_set_video_encoder(feed.output, feed.encoder);
		signal_handler_connect(obs_output_get_signal_handler(feed.output), "stop", output_stopped, this);
		if (!obs_output_start(feed.output)) {
			const char *e = obs_output_get_last_error(feed.output);
			std::string msg = std::string(name) + " failed to start" + (e ? std::string(": ") + e : "");
			stop_feed(feed);
			throw CommandError(msg);
		}
		return chosen;
	};

	// New size, rate, bitrate or encoder for running feeds: restart them.
	const std::string params = std::to_string(w) + "x" + std::to_string(h) + "@" + std::to_string(divisor) + "," +
				   std::to_string(preview_divisor) + "/" + std::to_string(kbps) + "/" + want +
				   (gpu_scale ? "/gpu" : "/cpu");
	if (params != feed_params_ && (feed_program_.output || feed_preview_.output)) {
		stop_feed(feed_program_);
		stop_feed(feed_preview_);
		if (preview_video_) {
			obs_view_remove(preview_view_);
			preview_video_ = nullptr;
		}
	}
	feed_params_ = params;

	std::string program_enc, preview_enc;
	if (want_program && !feed_program_.output)
		program_enc = start(feed_program_, FEED_PROGRAM, "feed-program", obs_get_video(), true, divisor);
	else if (!want_program)
		stop_feed(feed_program_);

	if (want_preview && !feed_preview_.output) {
		if (!preview_video_) {
			struct obs_video_info ovi;
			obs_get_video_info(&ovi);
			ovi.output_width = w;
			ovi.output_height = h;
			// As the Program feed: the area filter when made smaller.
			ovi.scale_type = monitor_scale(w, h);
			ovi.gpu_conversion = true;
			preview_video_ = obs_view_add2(preview_view_, &ovi);
			if (!preview_video_)
				throw CommandError("could not create the preview video mix");
		}
		preview_enc = start(feed_preview_, FEED_PREVIEW, "feed-preview", preview_video_, false, preview_divisor);
	} else if (!want_preview) {
		stop_feed(feed_preview_);
		if (preview_video_) {
			obs_view_remove(preview_view_);
			preview_video_ = nullptr;
		}
	}
	return JsonObj()
		.boolean("on", true)
		.integer("width", w)
		.integer("height", h)
		.number("fps", main_fps / divisor)
		.number("preview_fps", main_fps / preview_divisor)
		.integer("kbps", kbps)
		.boolean("program", feed_program_.output != nullptr)
		.boolean("preview", feed_preview_.output != nullptr)
		.dump();
}

std::string Engine::cmd_thumbnails(obs_data_t *cmd)
{
	const bool on = get_bool(cmd, "on", true);
	if (!on) {
		thumbs_->disable();
		return JsonObj().boolean("on", false).dump();
	}
	const uint32_t w = (uint32_t)std::clamp<int64_t>(get_int(cmd, "width", 256), 16, 1920) & ~1u;
	const uint32_t h = (uint32_t)std::clamp<int64_t>(get_int(cmd, "height", 144), 16, 1080) & ~1u;
	const double fps = std::clamp(get_double(cmd, "fps", 2.0), 0.2, 30.0);
	const std::string format = get_str(cmd, "format", "rgba");
	if (format != "rgba" && format != "i420")
		throw CommandError("unknown thumbnail format \"" + format + "\" (rgba or i420)");
	const bool only_changes = get_bool(cmd, "only_changes", false);
	thumbs_->enable(w, h, fps, format == "i420" ? Thumbnails::I420 : Thumbnails::RGBA, only_changes);
	return JsonObj()
		.boolean("on", true)
		.integer("width", w)
		.integer("height", h)
		.number("fps", fps)
		.str("format", format)
		.boolean("only_changes", only_changes)
		.dump();
}

std::string Engine::stats_json()
{
	const uint64_t now = os_gettime_ns();
	const double cpu = platform::cpu_seconds();
	const double wall = (now - last_cpu_wall_) / 1e9;
	const double cpu_percent = wall > 0.0 ? (cpu - last_cpu_) / wall * 100.0 : 0.0;
	last_cpu_ = cpu;
	last_cpu_wall_ = now;

	video_t *video = opt_.video ? obs_get_video() : nullptr;
	JsonObj out;
	out.boolean("active", output_ && obs_output_active(output_));
	if (output_) {
		PipeOutputStats st = pipe_output_stats(output_);
		out.str("encoder", venc_id_)
			.integer("kbps", video_kbps_)
			.integer("frames", obs_output_get_total_frames(output_))
			.integer("frames_dropped", obs_output_get_frames_dropped(output_))
			.integer("bytes", (int64_t)obs_output_get_total_bytes(output_))
			.integer("video_packets", (int64_t)st.video_packets)
			.integer("audio_packets", (int64_t)st.audio_packets)
			.integer("keyframes", (int64_t)st.keyframes);
	}
	auto feed_json = [](const Feed &f) {
		JsonObj j;
		j.boolean("active", f.output && obs_output_active(f.output));
		if (f.output) {
			PipeOutputStats st = pipe_output_stats(f.output);
			j.integer("packets", (int64_t)st.video_packets).integer("dropped", (int64_t)st.dropped);
		}
		return j.dump();
	};
	std::vector<std::string> pcm;
	{
		std::lock_guard<std::mutex> lock(pcm_mutex_);
		for (auto &kv : pcm_sources_) {
			PcmStats ps = pcm_stats(kv.second);
			pcm.push_back(JsonObj()
					      .integer("id", kv.first)
					      .integer("frames", (int64_t)ps.frames_in)
					      .number("offset_ms", ps.offset_ms)
					      .integer("inserted", (int64_t)ps.samples_inserted)
					      .integer("dropped", (int64_t)ps.samples_dropped)
					      .integer("resyncs", (int64_t)ps.resyncs)
					      .dump());
		}
	}
	JsonObj pipe;
	pipe.integer("queued_bytes", (int64_t)writer_.queued_bytes())
		.integer("bytes_written", (int64_t)writer_.bytes_written())
		.integer("bulk_dropped", (int64_t)writer_.bulk_dropped())
		.integer("media_dropped", (int64_t)writer_.media_dropped());

	// Cameras and clips scaled to the canvas, by filter (update_scale_filters).
	int64_t bicubic = 0, area = 0, lanczos = 0;
	for (const auto &kv : sources_) {
		if (!kv.second.wrapper_item)
			continue;
		const obs_scale_type t = obs_sceneitem_get_scale_filter(kv.second.wrapper_item);
		bicubic += t == OBS_SCALE_BICUBIC;
		area += t == OBS_SCALE_AREA;
		lanczos += t == OBS_SCALE_LANCZOS;
	}
	JsonObj scaled;
	scaled.integer("bicubic", bicubic).integer("area", area).integer("lanczos", lanczos);

	JsonObj s;
	s.number("render_ms", obs_get_average_frame_time_ns() / 1e6)
		.number("fps", (double)opt_.fps_num / opt_.fps_den)
		.integer("frames_rendered", obs_get_total_frames())
		.integer("frames_lagged", obs_get_lagged_frames())
		.integer("frames_output", video ? video_output_get_total_frames(video) : 0)
		.integer("frames_skipped", video ? video_output_get_skipped_frames(video) : 0)
		.raw("output", out.dump())
		.raw("feeds", JsonObj().raw("program", feed_json(feed_program_)).raw("preview", feed_json(feed_preview_)).raw("stream", feed_json(feed_stream_)).dump())
		.raw("pcm", json_array(pcm))
		.integer("pcm_unrouted", (int64_t)pcm_unknown_.load())
		.integer("overlay_updates", (int64_t)overlay_updates_.load())
		.integer("thumbnails_sent", thumbs_ ? (int64_t)thumbs_->sent() : 0)
		.integer("thumbnails_unchanged", thumbs_ ? (int64_t)thumbs_->unchanged() : 0)
		.raw("projectors", projectors_ ? projectors_->list_json() : std::string("[]"))
		.number("cpu_percent", cpu_percent)
		.integer("threads", platform::thread_count())
		.integer("memory_mb", (int64_t)(platform::resident_bytes() / (1024 * 1024)))
		.raw("pipe", pipe.dump())
		.number("uptime_s", (now - startup_ns_) / 1e9)
		.integer("sources", (int64_t)sources_.size())
		.raw("scaled", scaled.dump());
	return s.dump();
}

// projector {id, on, content, fit, display_id, display:{x,y,width,height}, windowed}
std::string Engine::cmd_projector(obs_data_t *cmd)
{
	const uint32_t id = require_id(cmd);
	if (!projectors_)
		throw CommandError("projectors are not available");
	if (!get_bool(cmd, "on", true)) {
		const bool was = projectors_->close(id);
		return JsonObj().integer("id", id).boolean("on", false).boolean("was_open", was).dump();
	}
	Projectors::Content content;
	if (!Projectors::parse_content(get_str(cmd, "content", "program"), &content))
		throw CommandError("content must be \"program\" or \"preview\"");
	Projectors::Fit fit;
	if (!Projectors::parse_fit(get_str(cmd, "fit", "contain"), &fit))
		throw CommandError("fit must be \"contain\", \"cover\" or \"stretch\"");
	platform::ProjectorTarget target;
	target.display_id = get_int(cmd, "display_id", -1);
	target.windowed = get_bool(cmd, "windowed", false);
	obs_data_t *d = obs_data_get_obj(cmd, "display");
	if (d) {
		target.has_rect = true;
		target.x = (int)obs_data_get_int(d, "x");
		target.y = (int)obs_data_get_int(d, "y");
		target.width = (int)obs_data_get_int(d, "width");
		target.height = (int)obs_data_get_int(d, "height");
		obs_data_release(d);
	}
	if (target.windowed && (!target.has_rect || target.width < 16 || target.height < 16))
		throw CommandError("a windowed projector needs \"display\": {x, y, width, height}");
	std::string error;
	std::string json = projectors_->open(id, content, fit, target, &error);
	if (json.empty())
		throw CommandError(error.empty() ? "the projector could not open" : error);
	return json;
}

std::string Engine::cmd_stats(obs_data_t *cmd)
{
	if (has(cmd, "interval_ms"))
		stats_interval_ms_ = (uint32_t)std::clamp<int64_t>(get_int(cmd, "interval_ms", 0), 0, 60000);
	return stats_json();
}

void Engine::stats_tick(void *param, float seconds)
{
	auto *self = static_cast<Engine *>(param);
	self->scale_elapsed_ += seconds;
	if (self->scale_elapsed_ >= 0.5 && !self->shutting_down_.load()) {
		self->scale_elapsed_ = 0.0;
		platform::post_main([self]() {
			if (!self->shutting_down_.load())
				self->update_scale_filters();
		});
	}
	const uint32_t interval = self->stats_interval_ms_.load();
	if (!interval || self->shutting_down_.load()) {
		self->stats_elapsed_ = 0.0;
		return;
	}
	self->stats_elapsed_ += seconds;
	if (self->stats_elapsed_ * 1000.0 < interval)
		return;
	self->stats_elapsed_ = 0.0;
	platform::post_main([self]() {
		if (self->shutting_down_.load())
			return;
		self->writer_.event("{\"event\":\"stats\"," + self->stats_json().substr(1));
	});
}

// ===========================================================================
// shutdown
// ===========================================================================

void Engine::shutdown(const std::string &reason)
{
	if (shutting_down_.exchange(true))
		return;
	platform::arm_exit_watchdog(8);
	blog(LOG_INFO, "[skynat-engine] shutting down: %s", reason.c_str());

	if (started_) {
		obs_remove_tick_callback(stats_tick, this);
		if (projectors_)
			projectors_->close_all();
		if (thumbs_) {
			thumbs_->disable();
			thumbs_->clear();
		}
		stop_feed(feed_program_);
		stop_feed(feed_preview_);
		if (preview_video_) {
			obs_view_remove(preview_view_);
			preview_video_ = nullptr;
		}
		stop_program_output();
		for (uint32_t c = 0; c < kMaxChannels; c++)
			obs_set_output_source(c, nullptr);
		for (auto &kv : sources_)
			remove_source_internal(kv.second);
		sources_.clear();
		if (transition_)
			obs_source_release(transition_);
		transition_ = nullptr;
		if (overlay_scene_)
			obs_scene_release(overlay_scene_);
		overlay_scene_ = nullptr;
		projectors_.reset();
		if (preview_view_)
			obs_view_destroy(preview_view_);
		preview_view_ = nullptr;
		thumbs_.reset();
	}
	writer_.event(JsonObj().str("event", "shutdown").str("reason", reason).dump());
	obs_shutdown();
	writer_.flush_and_stop(1500);
	platform::quit_main_loop();
}

} // namespace skynat
