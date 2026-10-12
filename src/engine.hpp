// skynat-engine — the libobs side: startup, scene graph, commands, outputs.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "io.hpp"
#include "output.hpp"
#include "projectors.hpp"
#include "thumbnails.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace skynat {

struct Options {
	std::string plugins_dir;  // folder with the plugin bundles / DLLs
	std::string data_dir;     // Windows: folder holding libobs/ and obs-plugins/ data
	std::string config_dir;   // module config folder (nothing is written there today)
	std::string renderer;     // "" = platform default: opengl (macOS), d3d11 (Windows)
	std::string log_level = "info";
	// false with --no-video: no renderer is started, only hello, list_devices,
	// stats and shutdown work (checks the pipe on a computer without a GPU).
	bool video = true;
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps_num = 30;
	uint32_t fps_den = 1;
};

class Engine {
public:
	Engine(Writer &writer, Options options);
	~Engine();

	bool startup(std::string *error);
	std::string ready_event();

	// Reader thread: PCM is handled at once, everything else is queued to
	// the main thread so commands and overlays keep their order.
	void on_message(uint8_t type, std::vector<uint8_t> &&payload);

	// Main thread.
	void shutdown(const std::string &reason);
	bool shutting_down() const { return shutting_down_.load(); }

private:
	struct Source {
		uint32_t id = 0;
		std::string type;
		obs_source_t *source = nullptr;
		obs_scene_t *wrapper = nullptr;       // inputs: the source fitted to the canvas
		obs_sceneitem_t *wrapper_item = nullptr;
		obs_sceneitem_t *overlay_item = nullptr; // overlays: item in the overlay scene
		uint32_t audio_channel = 0;             // pcm: output channel
		bool fast_camera = false;
		int rotate = 0;                         // inputs: turned clockwise (0, 90, 180, 270) before fitting
		// inputs: a window of the picture (fractions x, y, w, h) shown instead of all of it
		bool windowed = false;
		double win[4] = {0, 0, 1, 1};
		uint32_t win_w = 0, win_h = 0;          // the source size the crop was worked out for
	};

	struct Feed {
		obs_encoder_t *encoder = nullptr;
		obs_output_t *output = nullptr;
	};

	// commands (main thread)
	void handle_command(const std::string &json);
	void handle_overlay(const std::vector<uint8_t> &payload);
	std::string cmd_hello(obs_data_t *cmd);
	std::string cmd_configure(obs_data_t *cmd);
	std::string cmd_list_devices(obs_data_t *cmd);
	std::string cmd_add_source(obs_data_t *cmd);
	std::string cmd_update_source(obs_data_t *cmd);
	std::string cmd_remove_source(obs_data_t *cmd);
	std::string cmd_set_program(obs_data_t *cmd);
	std::string cmd_set_preview(obs_data_t *cmd);
	std::string cmd_overlay_visible(obs_data_t *cmd);
	std::string cmd_media(obs_data_t *cmd);
	std::string cmd_lut(obs_data_t *cmd);
	std::string cmd_start_output(obs_data_t *cmd);
	std::string cmd_stop_output(obs_data_t *cmd);
	std::string cmd_set_bitrate(obs_data_t *cmd);
	std::string cmd_preview_feed(obs_data_t *cmd);
	std::string cmd_thumbnails(obs_data_t *cmd);
	std::string cmd_projector(obs_data_t *cmd);
	std::string cmd_stats(obs_data_t *cmd);

	// helpers
	bool reset_video(uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den, std::string *error);
	void fit_to_canvas(obs_sceneitem_t *item, int rotate = 0);
	void update_scale_filters();
	obs_data_t *camera_settings(obs_data_t *cmd, const std::string &device, bool *fast, std::string *error);
	bool apply_common(Source &s, obs_data_t *cmd);
	void apply_window(Source &s);
	void apply_overlay_layout(Source &s, obs_data_t *cmd);
	obs_encoder_t *create_video_encoder(const std::string &name, const std::string &want, uint32_t kbps,
					    uint32_t keyint_sec, const char *profile, obs_data_t *overrides,
					    std::string *chosen, std::string *error);
	obs_encoder_t *create_audio_encoder(uint32_t kbps, std::string *chosen);
	void stop_feed(Feed &feed);
	void stop_program_output(PipeOutputStats *final_stats = nullptr);
	void remove_source_internal(Source &s);
	std::string stats_json();
	std::string capabilities_json();
	bool any_output_active();
	static void output_stopped(void *data, calldata_t *cd);
	static void stats_tick(void *param, float seconds);

	Writer &writer_;
	Options opt_;
	std::atomic<bool> shutting_down_{false};
	bool started_ = false;

	std::map<uint32_t, Source> sources_;
	std::mutex pcm_mutex_; // the reader thread looks PCM sources up directly
	std::map<uint32_t, obs_source_t *> pcm_sources_;

	obs_source_t *transition_ = nullptr; // program (output channel 0)
	obs_scene_t *overlay_scene_ = nullptr; // overlays on top of program (channel 1)
	obs_view_t *preview_view_ = nullptr;  // studio-mode preview
	video_t *preview_video_ = nullptr;
	int64_t program_id_ = -1;
	int64_t preview_id_ = -1;

	obs_encoder_t *venc_ = nullptr;
	obs_encoder_t *aenc_ = nullptr;
	obs_output_t *output_ = nullptr;
	std::string venc_id_;
	std::string aenc_id_;
	uint32_t video_kbps_ = 6000;

	Feed feed_program_;
	Feed feed_preview_;
	Feed feed_stream_;   // the stream's own smaller picture, beside Program's (start_output "stream")
	uint32_t stream_kbps_ = 0;
	std::string feed_params_;
	std::unique_ptr<Thumbnails> thumbs_;
	std::unique_ptr<Projectors> projectors_;

	std::vector<std::string> hw_encoders_;
	std::vector<std::string> aac_ids_;

	// periodic stats (graphics thread tick -> main thread)
	std::atomic<uint32_t> stats_interval_ms_{0};
	double stats_elapsed_ = 0.0;
	double scale_elapsed_ = 0.0; // see update_scale_filters
	double last_cpu_ = 0.0;
	uint64_t last_cpu_wall_ = 0;
	uint64_t startup_ns_ = 0;
	std::atomic<uint64_t> overlay_updates_{0};
	std::atomic<uint64_t> pcm_messages_{0};
	std::atomic<uint64_t> pcm_unknown_{0};
};

} // namespace skynat
