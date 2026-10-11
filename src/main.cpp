// skynat-engine — a silent, headless live-production engine built from
// OBS Studio's libobs. It has no window, no Dock icon and no tray: the
// Skynat Live app starts it as a hidden child process and talks to it only
// over stdin/stdout (see PROTOCOL.md).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "engine.hpp"
#include "io.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "protocol.hpp"

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#define skynat_write_stderr(p, n) _write(2, p, (unsigned)(n))
#else
#include <unistd.h>
#define skynat_write_stderr(p, n) write(2, p, n)
#endif

using namespace skynat;

namespace {

Writer *g_writer = nullptr;
int g_log_level = LOG_INFO;
std::atomic<int> g_log_events{0};
std::atomic<uint64_t> g_log_window{0};

const char *level_name(int lvl)
{
	switch (lvl) {
	case LOG_ERROR:
		return "error";
	case LOG_WARNING:
		return "warning";
	case LOG_INFO:
		return "info";
	default:
		return "debug";
	}
}

// libobs logs go to stderr (the app may save them); warnings and errors are
// also sent as "log" events, at most 20 per second.
void log_handler(int lvl, const char *fmt, va_list args, void *)
{
	if (lvl > g_log_level)
		return;
	char msg[4096];
	vsnprintf(msg, sizeof(msg), fmt, args);
	char line[4200];
	int n = snprintf(line, sizeof(line), "[%s] %s\n", level_name(lvl), msg);
	if (n > 0)
		skynat_write_stderr(line, (size_t)std::min(n, (int)sizeof(line) - 1));

	if (lvl <= LOG_WARNING && g_writer) {
		uint64_t second = os_gettime_ns() / 1000000000ull;
		if (g_log_window.exchange(second) != second)
			g_log_events = 0;
		if (g_log_events++ < 20)
			g_writer->event(
				JsonObj().str("event", "log").str("level", level_name(lvl)).str("message", msg).dump());
	}
}

void usage()
{
	fprintf(stdout,
		"skynat-engine %s — headless live engine built from OBS Studio's libobs (GPL-2.0-or-later)\n"
		"Talks only over stdin/stdout; see PROTOCOL.md.\n\n"
		"  --plugins <dir>     plugin folder (default: next to the executable)\n"
		"  --data <dir>        data folder (Windows; default: <exe>/data)\n"
		"  --config <dir>      module config folder (default: temp folder)\n"
		"  --width <n>         canvas width (default 1920)\n"
		"  --height <n>        canvas height (default 1080)\n"
		"  --fps <n|n/d>       frame rate (default 30)\n"
		"  --renderer <name>   opengl (macOS default) | d3d11 (Windows default)\n"
		"  --log-level <lvl>   error | warning | info (default) | debug\n"
		"  --version           print the version and exit\n",
		kEngineVersion);
}

bool parse_fps(const char *s, uint32_t *num, uint32_t *den)
{
	const char *slash = strchr(s, '/');
	if (slash) {
		*num = (uint32_t)strtoul(s, nullptr, 10);
		*den = (uint32_t)strtoul(slash + 1, nullptr, 10);
	} else {
		double f = strtod(s, nullptr);
		if (f > 0 && f == (double)(uint32_t)f) {
			*num = (uint32_t)f;
			*den = 1;
		} else {
			*num = (uint32_t)(f * 1001.0 + 0.5);
			*den = 1001;
		}
	}
	return *num > 0 && *den > 0;
}

std::string default_temp_dir()
{
#ifdef _WIN32
	const char *tmp = getenv("TEMP");
	std::string base = tmp ? tmp : ".";
	return base + "\\skynat-engine";
#else
	const char *tmp = getenv("TMPDIR");
	std::string base = tmp && *tmp ? tmp : "/tmp";
	if (base.back() == '/')
		base.pop_back();
	return base + "/skynat-engine";
#endif
}

} // namespace

int main(int argc, char **argv)
{
	Options opt;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char *what) -> const char * {
			if (i + 1 >= argc) {
				fprintf(stderr, "missing value for %s\n", what);
				exit(64);
			}
			return argv[++i];
		};
		if (a == "--version") {
			fprintf(stdout, "skynat-engine %s (libobs %s, protocol %u)\n", kEngineVersion,
				obs_get_version_string(), kProtocolVersion);
			return 0;
		} else if (a == "--help" || a == "-h") {
			usage();
			return 0;
		} else if (a == "--plugins") {
			opt.plugins_dir = next("--plugins");
		} else if (a == "--data") {
			opt.data_dir = next("--data");
		} else if (a == "--config") {
			opt.config_dir = next("--config");
		} else if (a == "--width") {
			opt.width = (uint32_t)strtoul(next("--width"), nullptr, 10);
		} else if (a == "--height") {
			opt.height = (uint32_t)strtoul(next("--height"), nullptr, 10);
		} else if (a == "--fps") {
			if (!parse_fps(next("--fps"), &opt.fps_num, &opt.fps_den)) {
				fprintf(stderr, "bad --fps\n");
				return 64;
			}
		} else if (a == "--renderer") {
			opt.renderer = next("--renderer");
		} else if (a == "--log-level") {
			opt.log_level = next("--log-level");
		} else {
			fprintf(stderr, "unknown argument: %s\n", a.c_str());
			return 64;
		}
	}
	if (opt.width < 64 || opt.height < 64 || (opt.width % 2) || (opt.height % 2)) {
		fprintf(stderr, "bad canvas size\n");
		return 64;
	}

	const std::string exe = platform::executable_dir();
#ifdef __APPLE__
	// dist layout: bin/skynat-engine, Frameworks/, PlugIns/
	if (opt.plugins_dir.empty())
		opt.plugins_dir = exe + "/../PlugIns";
#else
	// dist layout: skynat-engine.exe and DLLs, obs-plugins/64bit/, data/
	if (opt.plugins_dir.empty())
		opt.plugins_dir = exe + "/obs-plugins/64bit";
	if (opt.data_dir.empty())
		opt.data_dir = exe + "/data";
#endif
	if (opt.renderer.empty()) {
#ifdef _WIN32
		opt.renderer = "d3d11";
#else
		opt.renderer = "opengl";
#endif
	}
	if (opt.config_dir.empty())
		opt.config_dir = default_temp_dir();
	os_mkdirs(opt.config_dir.c_str());

	if (opt.log_level == "error")
		g_log_level = LOG_ERROR;
	else if (opt.log_level == "warning")
		g_log_level = LOG_WARNING;
	else if (opt.log_level == "debug")
		g_log_level = LOG_DEBUG;
	else
		g_log_level = LOG_INFO;
	base_set_log_handler(log_handler, nullptr);

	intptr_t in_handle = 0, out_handle = 0;
	if (!platform::take_protocol_fds(&in_handle, &out_handle)) {
		fprintf(stderr, "could not take the protocol pipes\n");
		return 70;
	}

	Writer writer(out_handle);
	writer.start();
	g_writer = &writer;

	Engine engine(writer, opt);
	writer.set_on_broken([&engine]() {
		platform::post_main([&engine]() { engine.shutdown("stdout closed"); });
	});
	platform::on_terminate([&engine]() { engine.shutdown("terminated"); });

	std::string error;
	if (!engine.startup(&error)) {
		writer.event(JsonObj().str("event", "error").boolean("fatal", true).str("message", error).dump());
		fprintf(stderr, "[error] startup failed: %s\n", error.c_str());
		g_writer = nullptr;
		writer.flush_and_stop(1000);
		obs_shutdown();
		_exit(3);
	}
	writer.event(engine.ready_event());

	Reader reader(
		in_handle,
		[&engine](uint8_t type, std::vector<uint8_t> &&payload) { engine.on_message(type, std::move(payload)); },
		[&engine](const std::string &why) {
			platform::post_main([&engine, why]() { engine.shutdown(why); });
		});
	reader.start();

	platform::run_main_loop();

	g_writer = nullptr;
	fflush(stderr);
	// The reader thread may still be blocked in read(); skip global
	// destructors and leave straight away.
	_exit(0);
}
