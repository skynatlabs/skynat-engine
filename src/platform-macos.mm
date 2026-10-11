// skynat-engine — macOS platform layer.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cmath>
#include <cstring>

#include <media-io/video-io.h>

namespace skynat {
namespace platform {

// ---- protocol pipes -------------------------------------------------------

bool take_protocol_fds(intptr_t *in_handle, intptr_t *out_handle)
{
	int in_fd = dup(STDIN_FILENO);
	int out_fd = dup(STDOUT_FILENO);
	if (in_fd < 0 || out_fd < 0)
		return false;
	fcntl(in_fd, F_SETFD, FD_CLOEXEC);
	fcntl(out_fd, F_SETFD, FD_CLOEXEC);

	// Anything that prints to stdout from now on lands on stderr instead.
	dup2(STDERR_FILENO, STDOUT_FILENO);
	int devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		close(devnull);
	}

	// A parent that dies mid-write must not kill us with SIGPIPE; the write
	// fails with EPIPE instead and the engine shuts down cleanly.
	signal(SIGPIPE, SIG_IGN);

	*in_handle = in_fd;
	*out_handle = out_fd;
	return true;
}

bool write_all(intptr_t handle, const void *data, size_t size)
{
	const uint8_t *p = (const uint8_t *)data;
	while (size > 0) {
		ssize_t n = write((int)handle, p, size);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return false;
		}
		p += n;
		size -= (size_t)n;
	}
	return true;
}

long read_some(intptr_t handle, void *data, size_t size)
{
	for (;;) {
		ssize_t n = read((int)handle, data, size);
		if (n < 0 && errno == EINTR)
			continue;
		return (long)n;
	}
}

// ---- main thread ------------------------------------------------------------

static void run_function(void *ctx)
{
	auto *fn = static_cast<std::function<void()> *>(ctx);
	(*fn)();
	delete fn;
}

void post_main(std::function<void()> fn)
{
	dispatch_async_f(dispatch_get_main_queue(), new std::function<void()>(std::move(fn)), run_function);
}

static void keepalive_timer(CFRunLoopTimerRef, void *) {}

void run_main_loop()
{
	// CFRunLoopRun() returns at once when the loop has nothing to wait on;
	// a timer that never fires keeps it alive. The main dispatch queue (where
	// commands run) is serviced by this run loop.
	CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0,
						       0, keepalive_timer, NULL);
	CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
	CFRunLoopRun();
	CFRunLoopRemoveTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
	CFRelease(timer);
}

void quit_main_loop()
{
	dispatch_async(dispatch_get_main_queue(), ^{
		CFRunLoopStop(CFRunLoopGetMain());
	});
}

static std::function<void()> *g_terminate = nullptr;
// Strong references: under ARC a dispatch source nobody holds is cancelled.
static dispatch_source_t g_signal_sources[3];

void on_terminate(std::function<void()> fn)
{
	g_terminate = new std::function<void()>(std::move(fn));
	const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
	for (int i = 0; i < 3; i++) {
		signal(sigs[i], SIG_IGN);
		dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0,
							       dispatch_get_main_queue());
		dispatch_source_set_event_handler(src, ^{
			if (g_terminate)
				(*g_terminate)();
		});
		dispatch_resume(src);
		g_signal_sources[i] = src;
	}
}

void arm_exit_watchdog(int seconds)
{
	// SIGALRM's default action terminates the process.
	signal(SIGALRM, SIG_DFL);
	alarm((unsigned)seconds);
}

// Strong reference (ARC): the activity lasts as long as the process.
static id<NSObject> g_live_activity = nil;

void begin_live_activity()
{
	@autoreleasepool {
		// No App Nap and no timer coalescing for this process (the graphics
		// thread must wake up on time every frame); idle system sleep stays
		// the app's decision.
		g_live_activity = [[NSProcessInfo processInfo]
			beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
					  reason:@"skynat-engine: live video"];
	}
}

// ---- process information -------------------------------------------------

void set_thread_name(const char *name)
{
	pthread_setname_np(name);
}

int thread_count()
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return -1;
	for (mach_msg_type_number_t i = 0; i < count; i++)
		mach_port_deallocate(mach_task_self(), threads[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)threads, sizeof(thread_t) * count);
	return (int)count;
}

double cpu_seconds()
{
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return 0.0;
	return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec +
	       ru.ru_stime.tv_usec / 1e6;
}

uint64_t resident_bytes()
{
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
		return 0;
	return info.phys_footprint;
}

std::string executable_dir()
{
	char buf[PATH_MAX];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) != 0)
		return ".";
	char real[PATH_MAX];
	if (!realpath(buf, real))
		strlcpy(real, buf, sizeof(real));
	return std::string(dirname(real));
}

uint32_t process_id()
{
	return (uint32_t)getpid();
}

// ---- devices and encoders ----------------------------------------------------

static NSArray<AVCaptureDeviceType> *camera_device_types()
{
	NSMutableArray<AVCaptureDeviceType> *types = [NSMutableArray array];
	[types addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
	if (@available(macOS 14.0, *)) {
		[types addObject:AVCaptureDeviceTypeExternal];
		[types addObject:AVCaptureDeviceTypeContinuityCamera];
	} else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		[types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
	}
	if (@available(macOS 13.0, *))
		[types addObject:AVCaptureDeviceTypeDeskViewCamera];
	return types;
}

std::vector<CameraInfo> list_cameras()
{
	std::vector<CameraInfo> out;
	@autoreleasepool {
		NSArray<AVCaptureDeviceType> *types = camera_device_types();
		for (AVMediaType media in @[AVMediaTypeVideo, AVMediaTypeMuxed]) {
			AVCaptureDeviceDiscoverySession *session =
				[AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
										       mediaType:media
											position:AVCaptureDevicePositionUnspecified];
			for (AVCaptureDevice *device in session.devices) {
				CameraInfo info;
				info.id = device.uniqueID.UTF8String ?: "";
				info.name = device.localizedName.UTF8String ?: "";
				bool dup = false;
				for (const auto &c : out)
					dup = dup || c.id == info.id;
				if (!dup)
					out.push_back(info);
			}
		}
	}
	return out;
}

std::string camera_permission()
{
	switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
	case AVAuthorizationStatusAuthorized:
		return "authorized";
	case AVAuthorizationStatusDenied:
		return "denied";
	case AVAuthorizationStatusRestricted:
		return "restricted";
	default:
		return "not_determined";
	}
}

bool pick_camera_format(const std::string &id, int want_w, int want_h, double want_fps, CameraFormat *out)
{
	@autoreleasepool {
		AVCaptureDevice *device = [AVCaptureDevice deviceWithUniqueID:@(id.c_str())];
		if (!device)
			return false;

		const double want_area = (double)want_w * (double)want_h;
		double best_score = 1e18;
		bool found = false;

		for (AVCaptureDeviceFormat *format in device.formats) {
			CMFormatDescriptionRef desc = format.formatDescription;
			CMMediaType media = CMFormatDescriptionGetMediaType(desc);
			if (media != kCMMediaType_Video && media != kCMMediaType_Muxed)
				continue;

			// Only the native formats mac-avcapture's fast path can select
			// (it asks for NV12 in video range, UYVY, YUY2 or BGRA).
			int obs_format;
			double sub_rank;
			switch (CMFormatDescriptionGetMediaSubType(desc)) {
			case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
				obs_format = VIDEO_FORMAT_NV12;
				sub_rank = 0.0;
				break;
			case kCVPixelFormatType_422YpCbCr8:
				obs_format = VIDEO_FORMAT_UYVY;
				sub_rank = 0.01;
				break;
			case kCVPixelFormatType_422YpCbCr8_yuvs:
				obs_format = VIDEO_FORMAT_YUY2;
				sub_rank = 0.02;
				break;
			case kCVPixelFormatType_32BGRA:
				obs_format = VIDEO_FORMAT_BGRA;
				sub_rank = 0.03;
				break;
			default:
				continue;
			}

			CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(desc);
			double area = (double)dims.width * (double)dims.height;
			double size_score;
			if (dims.width == want_w && dims.height == want_h)
				size_score = 0.0;
			else if (area >= want_area)
				size_score = area / want_area - 1.0 + 0.05; // bigger than wanted: fine, a little work
			else
				size_score = 2.0 + (1.0 - area / want_area); // smaller: would be upscaled

			for (AVFrameRateRange *range in format.videoSupportedFrameRateRanges) {
				uint32_t num, den;
				double fps_score;
				if (want_fps >= range.minFrameRate - 0.001 && want_fps <= range.maxFrameRate + 0.001) {
					num = (uint32_t)llround(want_fps * 1000.0);
					den = 1000;
					fps_score = 0.0;
				} else {
					CMTime d = range.minFrameDuration; // the fastest this range allows
					num = (uint32_t)d.timescale;
					den = (uint32_t)d.value;
					double fps = range.maxFrameRate;
					fps_score = fps < want_fps ? 1.0 + (want_fps - fps) / want_fps
								   : 0.25 + (fps - want_fps) / want_fps;
				}
				double score = size_score * 10.0 + fps_score + sub_rank;
				if (score < best_score) {
					best_score = score;
					found = true;
					out->width = dims.width;
					out->height = dims.height;
					out->fps_num = num;
					out->fps_den = den ? den : 1;
					out->obs_video_format = obs_format;
				}
			}
		}
		return found;
	}
}

std::vector<std::string> hardware_h264_encoders()
{
	std::vector<std::string> ids;
	CFArrayRef list = nullptr;
	if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list)
		return ids;
	CFIndex n = CFArrayGetCount(list);
	for (CFIndex i = 0; i < n; i++) {
		CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
		CFNumberRef type_num = (CFNumberRef)CFDictionaryGetValue(dict, kVTVideoEncoderList_CodecType);
		CMVideoCodecType type = 0;
		if (!type_num || !CFNumberGetValue(type_num, kCFNumberSInt32Type, &type) ||
		    type != kCMVideoCodecType_H264)
			continue;
		CFBooleanRef hw = (CFBooleanRef)CFDictionaryGetValue(dict, kVTVideoEncoderList_IsHardwareAccelerated);
		if (!hw || !CFBooleanGetValue(hw))
			continue;
		CFStringRef enc_id = (CFStringRef)CFDictionaryGetValue(dict, kVTVideoEncoderList_EncoderID);
		char buf[256];
		if (!enc_id || !CFStringGetCString(enc_id, buf, sizeof(buf), kCFStringEncodingUTF8))
			continue;
		// A macOS virtual machine lists the host's encoder as
		// "paravirtualized:…". It reorders frames although asked not to (the
		// program stream must have none) and undershoots CBR, so a VM uses
		// the x264 fallback instead. Real Macs never list it.
		if (strncmp(buf, "paravirtualized:", 16) == 0)
			continue;
		ids.emplace_back(buf);
	}
	CFRelease(list);
	return ids;
}

std::vector<std::string> aac_encoders()
{
	return {"CoreAudio_AAC", "ffmpeg_aac"};
}

std::string graphics_module(const std::string &renderer)
{
	if (renderer == "metal")
		return "libobs-metal.dylib";
	return "libobs-opengl.dylib";
}

void module_paths(const std::string &plugin_dir, const std::string &data_dir, const std::string &name,
		  std::string *bin, std::string *data)
{
	(void)data_dir; // data lives inside each plugin bundle on macOS
	*bin = plugin_dir + "/" + name + ".plugin/Contents/MacOS/" + name;
	*data = plugin_dir + "/" + name + ".plugin/Contents/Resources";
}

const char *camera_source_id(bool fast)
{
	return fast ? "macos-avcapture-fast" : "macos-avcapture";
}

} // namespace platform
} // namespace skynat
