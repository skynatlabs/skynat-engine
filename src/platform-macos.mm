// skynat-engine — macOS platform layer.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform.hpp"

#import <AppKit/AppKit.h>
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

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <media-io/video-io.h>

// ---- projector windows: AppKit classes (file scope) ---------------------------

// A borderless window over a display. It never takes the focus by itself
// (the studio keeps the keyboard); once clicked, Escape or a double-click
// closes it — the way out when it covers the studio's own screen.
@interface SkynatProjectorWindow : NSWindow
@property (nonatomic, copy) void (^closeHandler)(NSString *reason);
@end

@implementation SkynatProjectorWindow
- (BOOL)canBecomeKeyWindow
{
	return YES;
}
- (BOOL)canBecomeMainWindow
{
	return NO;
}
- (void)keyDown:(NSEvent *)event
{
	if (event.keyCode == 53) { // Escape
		if (self.closeHandler)
			self.closeHandler(@"escape");
		return;
	}
	[super keyDown:event];
}
- (void)cancelOperation:(id)sender
{
	if (self.closeHandler)
		self.closeHandler(@"escape");
}
- (void)mouseDown:(NSEvent *)event
{
	if (event.clickCount >= 2) {
		if (self.closeHandler)
			self.closeHandler(@"double-click");
		return;
	}
	[super mouseDown:event];
}
@end

// The view libobs's OpenGL context draws into, at the display's own pixels.
@interface SkynatProjectorView : NSView
@end

@implementation SkynatProjectorView
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	return YES;
}
- (BOOL)isOpaque
{
	return YES;
}
@end

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

void init_ui()
{
	@autoreleasepool {
		// An application object, so a projector window can be shown and take
		// a key press — but no Dock icon, no menu bar and no windows until a
		// projector opens (activation policy "prohibited" until then).
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	}
}

void run_main_loop()
{
	// The application's own event loop: it services the main dispatch queue
	// (where commands run) and delivers the projector windows' events.
	@autoreleasepool {
		if (!NSApp)
			init_ui();
	}
	[NSApp run];
}

void quit_main_loop()
{
	dispatch_async(dispatch_get_main_queue(), ^{
		[NSApp stop:nil];
		// -stop: takes effect after the next event: post one.
		NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
						   location:NSZeroPoint
					      modifierFlags:0
						  timestamp:0
					       windowNumber:0
						    context:nil
						    subtype:0
						      data1:0
						      data2:0];
		[NSApp postEvent:wake atStart:YES];
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

// ---- projector windows ----------------------------------------------------------

struct ProjectorWindow {
	SkynatProjectorWindow *window = nil;
	SkynatProjectorView *view = nil;
	std::vector<id> observers;
	std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
	bool windowed = false;
	int64_t display_id = -1;
};

static int g_projectors_open = 0;

static NSScreen *projector_screen(const ProjectorTarget &t)
{
	NSArray<NSScreen *> *screens = [NSScreen screens];
	if (t.display_id >= 0) {
		for (NSScreen *s in screens) {
			NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
			if (n && n.longLongValue == t.display_id)
				return s;
		}
	}
	if (t.has_rect && screens.count) {
		// Electron's coordinates: points, top-left origin of the main screen.
		const CGFloat primary_h = screens[0].frame.size.height;
		const NSPoint c = NSMakePoint(t.x + t.width / 2.0, primary_h - (t.y + t.height / 2.0));
		for (NSScreen *s in screens)
			if (NSPointInRect(c, s.frame))
				return s;
	}
	return [NSScreen mainScreen] ?: screens.firstObject;
}

static bool screen_present(NSScreen *screen)
{
	if (!screen)
		return false;
	NSNumber *want = screen.deviceDescription[@"NSScreenNumber"];
	for (NSScreen *s in [NSScreen screens])
		if ([s.deviceDescription[@"NSScreenNumber"] isEqual:want])
			return true;
	return false;
}

static void fill_info(ProjectorWindow *pw, ProjectorWindowInfo *info)
{
	const NSRect px = [pw->view convertRectToBacking:pw->view.bounds];
	info->native = (__bridge void *)pw->view;
	info->cx = (uint32_t)std::lround(px.size.width);
	info->cy = (uint32_t)std::lround(px.size.height);
	info->scale = pw->window.backingScaleFactor;
	NSScreen *screen = pw->window.screen;
	NSArray<NSScreen *> *screens = [NSScreen screens];
	if (screen && screens.count) {
		const NSRect f = screen.frame;
		const CGFloat primary_h = screens[0].frame.size.height;
		info->screen_x = (int)f.origin.x;
		info->screen_y = (int)(primary_h - (f.origin.y + f.size.height));
		info->screen_width = (int)f.size.width;
		info->screen_height = (int)f.size.height;
	}
}

ProjectorWindow *open_projector_window(const ProjectorTarget &target, const std::string &title, ProjectorWindowInfo *info,
				       std::function<void(const std::string &reason)> on_close,
				       std::function<void(uint32_t cx, uint32_t cy)> on_resize, std::string *error)
{
	@autoreleasepool {
		if (!NSApp)
			init_ui();
		NSScreen *screen = projector_screen(target);
		if (!screen) {
			*error = "no display to put the projector on";
			return nullptr;
		}
		NSRect frame = screen.frame;
		if (target.windowed && target.has_rect && target.width > 0 && target.height > 0) {
			const CGFloat primary_h = [NSScreen screens][0].frame.size.height;
			frame = NSMakeRect(target.x, primary_h - (target.y + target.height), target.width, target.height);
		}

		auto *pw = new ProjectorWindow();
		pw->windowed = target.windowed;
		pw->display_id = target.display_id;
		pw->window = [[SkynatProjectorWindow alloc] initWithContentRect:frame
								      styleMask:NSWindowStyleMaskBorderless
									backing:NSBackingStoreBuffered
									  defer:NO];
		pw->window.releasedWhenClosed = NO;
		pw->window.opaque = YES;
		pw->window.hasShadow = NO;
		pw->window.backgroundColor = [NSColor blackColor];
		pw->window.title = [NSString stringWithUTF8String:title.c_str()];
		// Over the menu bar and the Dock; on every Space; not in the window cycle.
		pw->window.level = target.windowed ? NSFloatingWindowLevel : NSMainMenuWindowLevel + 2;
		pw->window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
						NSWindowCollectionBehaviorStationary |
						NSWindowCollectionBehaviorIgnoresCycle |
						NSWindowCollectionBehaviorFullScreenAuxiliary;

		pw->view = [[SkynatProjectorView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		// The display's own pixels on a Retina screen (otherwise OpenGL draws
		// at points and macOS stretches it — soft).
		pw->view.wantsBestResolutionOpenGLSurface = YES;
#pragma clang diagnostic pop
		pw->window.contentView = pw->view;

		auto alive = pw->alive;
		auto close_cb = std::make_shared<std::function<void(const std::string &)>>(std::move(on_close));
		auto resize_cb = std::make_shared<std::function<void(uint32_t, uint32_t)>>(std::move(on_resize));
		// Posted, never run inside the window's own event handling (it is
		// destroyed by the close), and never after it is gone.
		auto post_close = [alive, close_cb](const std::string &reason) {
			post_main([alive, close_cb, reason]() {
				if (alive->load() && *close_cb)
					(*close_cb)(reason);
			});
		};
		pw->window.closeHandler = ^(NSString *reason) {
			post_close(reason.UTF8String);
		};

		if (g_projectors_open++ == 0 && NSApp.activationPolicy == NSApplicationActivationPolicyProhibited)
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
		[pw->window orderFrontRegardless];

		NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
		void (^resized)(NSNotification *) = ^(NSNotification *) {
			const NSRect px = [pw->view convertRectToBacking:pw->view.bounds];
			const uint32_t cx = (uint32_t)std::lround(px.size.width), cy = (uint32_t)std::lround(px.size.height);
			post_main([alive, resize_cb, cx, cy]() {
				if (alive->load() && *resize_cb)
					(*resize_cb)(cx, cy);
			});
		};
		pw->observers.push_back([nc addObserverForName:NSWindowDidResizeNotification
							object:pw->window
							 queue:nil
						    usingBlock:resized]);
		pw->observers.push_back([nc addObserverForName:NSWindowDidChangeBackingPropertiesNotification
							object:pw->window
							 queue:nil
						    usingBlock:resized]);
		// A display unplugged (or its arrangement changed): a full-screen
		// projector follows its display's new size, and closes if it is gone
		// — it must never land on top of the studio by itself.
		pw->observers.push_back([nc addObserverForName:NSApplicationDidChangeScreenParametersNotification
							object:nil
							 queue:nil
						    usingBlock:^(NSNotification *) {
							    if (pw->windowed)
								    return;
							    NSScreen *now = nil;
							    for (NSScreen *sc in [NSScreen screens]) {
								    NSNumber *n = sc.deviceDescription[@"NSScreenNumber"];
								    if (pw->display_id >= 0 ? (n && n.longLongValue == pw->display_id)
											     : screen_present(pw->window.screen)) {
									    now = pw->display_id >= 0 ? sc : pw->window.screen;
									    break;
								    }
							    }
							    if (!now) {
								    post_close("display-gone");
								    return;
							    }
							    if (!NSEqualRects(pw->window.frame, now.frame))
								    [pw->window setFrame:now.frame display:YES];
						    }]);

		fill_info(pw, info);
		return pw;
	}
}

void close_projector_window(ProjectorWindow *pw)
{
	if (!pw)
		return;
	@autoreleasepool {
		pw->alive->store(false);
		for (id o : pw->observers)
			[[NSNotificationCenter defaultCenter] removeObserver:o];
		pw->observers.clear();
		pw->window.closeHandler = nil;
		[pw->window orderOut:nil];
		[pw->window close];
		pw->window.contentView = nil;
		pw->view = nil;
		pw->window = nil;
		if (--g_projectors_open == 0)
			[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
	}
	delete pw;
}

} // namespace platform
} // namespace skynat
