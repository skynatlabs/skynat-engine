// skynat-engine — the small platform layer (macOS: platform-macos.mm,
// Windows: platform-windows.cpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skynat {
namespace platform {

// ---- protocol pipes -------------------------------------------------------
// Takes private copies of stdin/stdout for the protocol and points the
// process's own stdin at the null device and stdout at stderr, so a stray
// printf() in any library can never corrupt the binary stream.
bool take_protocol_fds(intptr_t *in_handle, intptr_t *out_handle);
bool write_all(intptr_t handle, const void *data, size_t size); // false once the pipe is broken
long read_some(intptr_t handle, void *data, size_t size);       // >0 bytes, 0 = end of file, <0 = error

// ---- main thread ------------------------------------------------------------
// Commands run on the main thread, in order; the stdin reader only parses.
void post_main(std::function<void()> fn);
void run_main_loop();  // returns after quit_main_loop()
void quit_main_loop(); // safe from any thread
// SIGTERM / SIGINT / SIGHUP (macOS) or console close (Windows) -> callback on the main thread.
void on_terminate(std::function<void()> fn);
// Kills the process if a shutdown takes longer than `seconds`.
void arm_exit_watchdog(int seconds);
// The engine is a window-less child process: tells the OS it does live,
// latency-critical work, so it is never napped and its timers are never
// coalesced (macOS App Nap / timer coalescing), even while the app is in the
// background. Windows: libobs already raises the timer resolution itself.
void begin_live_activity();

// ---- process information -------------------------------------------------
void set_thread_name(const char *name); // shows in sample / Instruments / debuggers
int thread_count();
double cpu_seconds(); // user + system time of this process
uint64_t resident_bytes();
std::string executable_dir();
uint32_t process_id();

// ---- devices and encoders ----------------------------------------------------
struct CameraInfo {
	std::string id;   // AVFoundation uniqueID (macOS) / DirectShow device id (Windows)
	std::string name; // localized device name
};
std::vector<CameraInfo> list_cameras();
std::string camera_permission(); // "authorized" | "denied" | "restricted" | "not_determined" | "not_required"

struct CameraFormat {
	int width = 0;
	int height = 0;
	uint32_t fps_num = 0;
	uint32_t fps_den = 1;
	int obs_video_format = 0; // enum video_format value for mac-avcapture's input_format
};
// Picks the device format closest to the wish (exact size and rate first).
bool pick_camera_format(const std::string &id, int want_w, int want_h, double want_fps, CameraFormat *out);

// Hardware H.264 encoder ids in order of preference (libobs encoder ids).
std::vector<std::string> hardware_h264_encoders();
// Default AAC encoder ids in order of preference.
std::vector<std::string> aac_encoders();
// Graphics module file name for obs_video_info.graphics_module.
std::string graphics_module(const std::string &renderer);
// Plugin binary and data paths for a module name inside the plugin folder.
void module_paths(const std::string &plugin_dir, const std::string &data_dir, const std::string &name,
		  std::string *bin, std::string *data);
// Source ids used by the engine for a camera on this platform.
const char *camera_source_id(bool fast);

} // namespace platform
} // namespace skynat
