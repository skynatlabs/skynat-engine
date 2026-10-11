// skynat-engine — Windows platform layer.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform.hpp"

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include <obs.h>

namespace skynat {
namespace platform {

// ---- protocol pipes -------------------------------------------------------

bool take_protocol_fds(intptr_t *in_handle, intptr_t *out_handle)
{
	HANDLE self = GetCurrentProcess();
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE in_copy = nullptr, out_copy = nullptr;
	if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE)
		return false;
	// Private, non-inheritable copies for the protocol.
	if (!DuplicateHandle(self, in, self, &in_copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return false;
	if (!DuplicateHandle(self, out, self, &out_copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return false;

	// Anything printed to stdout from now on goes to stderr; stdin reads NUL.
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	SetStdHandle(STD_OUTPUT_HANDLE, err);
	_dup2(_fileno(stderr), _fileno(stdout));
	HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
				 nullptr);
	if (nul != INVALID_HANDLE_VALUE)
		SetStdHandle(STD_INPUT_HANDLE, nul);
	int nul_fd = _open("NUL", _O_RDONLY);
	if (nul_fd >= 0) {
		_dup2(nul_fd, _fileno(stdin));
		_close(nul_fd);
	}

	*in_handle = (intptr_t)in_copy;
	*out_handle = (intptr_t)out_copy;
	return true;
}

bool write_all(intptr_t handle, const void *data, size_t size)
{
	const uint8_t *p = (const uint8_t *)data;
	while (size > 0) {
		DWORD chunk = (DWORD)(size > (1u << 30) ? (1u << 30) : size);
		DWORD written = 0;
		if (!WriteFile((HANDLE)handle, p, chunk, &written, nullptr))
			return false; // ERROR_BROKEN_PIPE / ERROR_NO_DATA: the app is gone
		p += written;
		size -= written;
	}
	return true;
}

long read_some(intptr_t handle, void *data, size_t size)
{
	DWORD got = 0;
	DWORD want = (DWORD)(size > (1u << 30) ? (1u << 30) : size);
	if (!ReadFile((HANDLE)handle, data, want, &got, nullptr)) {
		DWORD e = GetLastError();
		return (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) ? 0 : -1;
	}
	return (long)got;
}

// ---- main thread ------------------------------------------------------------

static std::mutex g_mutex;
static std::condition_variable g_cv;
static std::deque<std::function<void()>> g_queue;
static bool g_quit = false;

void post_main(std::function<void()> fn)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_queue.push_back(std::move(fn));
	}
	g_cv.notify_one();
}

void run_main_loop()
{
	for (;;) {
		std::function<void()> fn;
		{
			std::unique_lock<std::mutex> lock(g_mutex);
			g_cv.wait(lock, [] { return g_quit || !g_queue.empty(); });
			if (g_quit)
				return;
			fn = std::move(g_queue.front());
			g_queue.pop_front();
		}
		fn();
	}
}

static std::function<void()> *g_terminate = nullptr;
static HANDLE g_terminated = nullptr;

void quit_main_loop()
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_quit = true;
	}
	g_cv.notify_one();
	if (g_terminated)
		SetEvent(g_terminated); // lets a waiting console close handler return
}

static BOOL WINAPI console_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		if (g_terminate)
			post_main(*g_terminate);
		// Windows ends the process when this handler returns for close /
		// logoff / shutdown; give the clean shutdown a few seconds first.
		if (type >= CTRL_CLOSE_EVENT && g_terminated)
			WaitForSingleObject(g_terminated, 4000);
		return TRUE;
	default:
		return FALSE;
	}
}

void on_terminate(std::function<void()> fn)
{
	g_terminate = new std::function<void()>(std::move(fn));
	g_terminated = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(console_handler, TRUE);
}

void begin_live_activity()
{
	// Nothing to do: libobs raises the timer resolution (timeBeginPeriod(1))
	// when it loads, and Windows does not nap console processes.
}

void arm_exit_watchdog(int seconds)
{
	std::thread([seconds] {
		std::this_thread::sleep_for(std::chrono::seconds(seconds));
		TerminateProcess(GetCurrentProcess(), 1);
	}).detach();
}

// ---- process information -------------------------------------------------

void set_thread_name(const char *name)
{
	// SetThreadDescription exists from Windows 10 1607; look it up at run time.
	using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
	static SetDesc fn = (SetDesc)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
	if (!fn)
		return;
	wchar_t wide[64];
	int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
	if (n > 0)
		fn(GetCurrentThread(), wide);
}

int thread_count()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return -1;
	THREADENTRY32 te;
	te.dwSize = sizeof(te);
	int n = 0;
	const DWORD pid = GetCurrentProcessId();
	if (Thread32First(snap, &te)) {
		do {
			if (te.th32OwnerProcessID == pid)
				n++;
		} while (Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	return n;
}

double cpu_seconds()
{
	FILETIME create, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
		return 0.0;
	auto to_s = [](const FILETIME &ft) {
		ULARGE_INTEGER v;
		v.LowPart = ft.dwLowDateTime;
		v.HighPart = ft.dwHighDateTime;
		return (double)v.QuadPart / 1e7;
	};
	return to_s(kernel) + to_s(user);
}

uint64_t resident_bytes()
{
	PROCESS_MEMORY_COUNTERS_EX pmc;
	pmc.cb = sizeof(pmc);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
		return 0;
	return pmc.PrivateUsage;
}

std::string executable_dir()
{
	wchar_t buf[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
	if (n == 0)
		return ".";
	int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
	std::string path(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, &path[0], len, nullptr, nullptr);
	for (auto &c : path)
		if (c == '\\')
			c = '/';
	size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? "." : path.substr(0, slash);
}

uint32_t process_id()
{
	return (uint32_t)GetCurrentProcessId();
}

// ---- devices and encoders ----------------------------------------------------

std::vector<CameraInfo> list_cameras()
{
	// win-dshow lists the DirectShow video devices in its "video_device_id"
	// property: name = friendly name, value = "name:device path".
	std::vector<CameraInfo> out;
	obs_properties_t *props = obs_get_source_properties("dshow_input");
	if (!props)
		return out;
	obs_property_t *p = obs_properties_get(props, "video_device_id");
	size_t n = p ? obs_property_list_item_count(p) : 0;
	for (size_t i = 0; i < n; i++) {
		const char *name = obs_property_list_item_name(p, i);
		const char *value = obs_property_list_item_string(p, i);
		if (!value || !*value)
			continue;
		out.push_back({value, name ? name : value});
	}
	obs_properties_destroy(props);
	return out;
}

std::string camera_permission()
{
	return "not_required";
}

bool pick_camera_format(const std::string &, int, int, double, CameraFormat *)
{
	return false; // win-dshow picks the format from res_type/resolution/frame_interval
}

std::vector<std::string> hardware_h264_encoders()
{
	// NVIDIA (OBS 31 native NVENC, then its older id), Intel Quick Sync, AMD AMF.
	return {"obs_nvenc_h264_tex", "jim_nvenc", "obs_qsv11_v2", "obs_qsv11", "h264_texture_amf", "amd_amf_h264"};
}

std::vector<std::string> aac_encoders()
{
	return {"ffmpeg_aac", "CoreAudio_AAC"};
}

std::string graphics_module(const std::string &renderer)
{
	return renderer == "opengl" ? "libobs-opengl" : "libobs-d3d11";
}

void module_paths(const std::string &plugin_dir, const std::string &data_dir, const std::string &name,
		  std::string *bin, std::string *data)
{
	*bin = plugin_dir + "/" + name + ".dll";
	*data = data_dir + "/obs-plugins/" + name;
}

const char *camera_source_id(bool)
{
	return "dshow_input";
}

} // namespace platform
} // namespace skynat
