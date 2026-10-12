// skynat-engine — pipe outputs (see output.hpp).
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later

#include "output.hpp"
#include "io.hpp"
#include "protocol.hpp"

#include <obs-avc.h>
#include <util/bmem.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace skynat {

namespace {

Writer *g_writer = nullptr;

struct PipeOutput {
	obs_output_t *output = nullptr;
	bool feed = false;
	uint8_t feed_id = 0;
	std::mutex mutex; // packets arrive on libobs's output thread
	std::vector<uint8_t> video_header; // last avcC sent
	bool audio_header_sent = false;
	bool waiting_keyframe = false;
	PipeOutputStats stats;
};

inline int64_t to_us(int64_t ts, int32_t num, int32_t den)
{
	if (den == 0)
		return 0;
	return ts * 1000000ll * (int64_t)num / (int64_t)den;
}

const char *pipe_name(void *)
{
	return "Skynat pipe output";
}

const char *feed_name(void *)
{
	return "Skynat monitor feed";
}

void *pipe_create(obs_data_t *settings, obs_output_t *output)
{
	auto *o = new PipeOutput();
	o->output = output;
	o->feed = false;
	(void)settings;
	return o;
}

void *feed_create(obs_data_t *settings, obs_output_t *output)
{
	auto *o = new PipeOutput();
	o->output = output;
	o->feed = true;
	o->feed_id = (uint8_t)obs_data_get_int(settings, "feed");
	return o;
}

void pipe_destroy(void *data)
{
	delete static_cast<PipeOutput *>(data);
}

bool pipe_start(void *data)
{
	auto *o = static_cast<PipeOutput *>(data);
	if (!obs_output_can_begin_data_capture(o->output, 0))
		return false;
	if (!obs_output_initialize_encoders(o->output, 0))
		return false;
	{
		std::lock_guard<std::mutex> lock(o->mutex);
		o->video_header.clear();
		o->audio_header_sent = false;
		o->waiting_keyframe = false;
		o->stats = PipeOutputStats();
	}
	return obs_output_begin_data_capture(o->output, 0);
}

void pipe_stop(void *data, uint64_t)
{
	auto *o = static_cast<PipeOutput *>(data);
	obs_output_end_data_capture(o->output);
}

// Sends the avcC for the current encoder parameters when they are new or
// changed (a bitrate change can bring new SPS/PPS on some encoders).
void send_video_header(PipeOutput *o)
{
	obs_encoder_t *enc = obs_output_get_video_encoder(o->output);
	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	if (!enc || !obs_encoder_get_extra_data(enc, &extra, &extra_size) || !extra_size)
		return;
	uint8_t *avcc = nullptr;
	size_t avcc_size = obs_parse_avc_header(&avcc, extra, extra_size);
	if (avcc && avcc_size) {
		if (o->video_header.size() != avcc_size || memcmp(o->video_header.data(), avcc, avcc_size) != 0) {
			o->video_header.assign(avcc, avcc + avcc_size);
			const uint32_t w = obs_encoder_get_width(enc);
			const uint32_t h = obs_encoder_get_height(enc);
			video_t *video = obs_encoder_video(enc);
			const struct video_output_info *voi = video ? video_output_get_info(video) : nullptr;
			// obs_encoder_video() already reports the divided rate.
			uint32_t fps_num = voi ? voi->fps_num : 30;
			uint32_t fps_den = voi ? voi->fps_den : 1;
			obs_data_t *settings = obs_encoder_get_settings(enc);
			uint32_t kbps = (uint32_t)obs_data_get_int(settings, "bitrate");
			obs_data_release(settings);

			Frame f(o->feed ? MSG_FEED_CONFIG : MSG_VIDEO_CONFIG, avcc_size + 16);
			if (o->feed)
				f.u8(o->feed_id);
			f.u16((uint16_t)w).u16((uint16_t)h).u32(fps_num).u32(fps_den).u32(kbps).bytes(avcc, avcc_size);
			g_writer->send(f.finish(), Writer::CONTROL);
		}
	}
	bfree(avcc);
}

void send_audio_header(PipeOutput *o)
{
	obs_encoder_t *enc = obs_output_get_audio_encoder(o->output, 0);
	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	if (!enc || !obs_encoder_get_extra_data(enc, &extra, &extra_size) || !extra_size)
		return;
	obs_data_t *settings = obs_encoder_get_settings(enc);
	uint32_t kbps = (uint32_t)obs_data_get_int(settings, "bitrate");
	obs_data_release(settings);
	Frame f(MSG_AUDIO_CONFIG, extra_size + 12);
	f.u32(obs_encoder_get_sample_rate(enc))
		.u16((uint16_t)audio_output_get_channels(obs_encoder_audio(enc)))
		.u16(0)
		.u32(kbps)
		.bytes(extra, extra_size);
	g_writer->send(f.finish(), Writer::CONTROL);
	o->audio_header_sent = true;
}

void pipe_packet(void *data, struct encoder_packet *packet)
{
	auto *o = static_cast<PipeOutput *>(data);
	if (!packet || !g_writer)
		return;
	std::lock_guard<std::mutex> lock(o->mutex);

	if (packet->type == OBS_ENCODER_VIDEO) {
		if (packet->keyframe || o->video_header.empty())
			send_video_header(o);
		if (o->video_header.empty())
			return; // a decoder can't use packets before the avcC

		if (o->feed && o->waiting_keyframe && !packet->keyframe) {
			o->stats.dropped++;
			return;
		}

		struct encoder_packet avc = {};
		obs_parse_avc_packet(&avc, packet);
		// libobs 31 labels the packets of an encoder with a frame-rate
		// divisor with timebase (divisor x 1/fps) while their pts still
		// count 1/fps steps; undo that so feed timestamps are real time.
		int32_t num = packet->timebase_num;
		const uint32_t divisor = packet->encoder ? obs_encoder_get_frame_rate_divisor(packet->encoder) : 1;
		if (divisor > 1 && num % (int32_t)divisor == 0)
			num /= (int32_t)divisor;
		const int64_t pts = to_us(packet->pts, num, packet->timebase_den);
		const int64_t dts = to_us(packet->dts, num, packet->timebase_den);

		Frame f(o->feed ? MSG_FEED_PACKET : MSG_VIDEO_PACKET, avc.size + 18);
		if (o->feed)
			f.u8(o->feed_id);
		f.i64(pts).i64(dts).u8(packet->keyframe ? 1 : 0).bytes(avc.data, avc.size);
		// Monitor feeds give way first; Program and the stream's own picture are the broadcast.
		bool sent = g_writer->send(f.finish(), o->feed && o->feed_id != FEED_STREAM && o->feed_id != FEED_CLEAN ? Writer::BULK : Writer::MEDIA);
		obs_encoder_packet_release(&avc);

		if (sent) {
			o->stats.video_packets++;
			if (packet->keyframe) {
				o->stats.keyframes++;
				o->waiting_keyframe = false;
			}
		} else {
			o->stats.dropped++;
			if (o->feed)
				o->waiting_keyframe = true; // resume cleanly at the next keyframe
		}
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		if (!o->audio_header_sent)
			send_audio_header(o);
		if (!o->audio_header_sent)
			return;
		const int64_t pts = to_us(packet->pts, packet->timebase_num, packet->timebase_den);
		Frame f(MSG_AUDIO_PACKET, packet->size + 8);
		f.i64(pts).bytes(packet->data, packet->size);
		if (g_writer->send(f.finish(), Writer::MEDIA))
			o->stats.audio_packets++;
		else
			o->stats.dropped++;
	}
}

void feed_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "feed", 0);
}

} // namespace

PipeOutputStats pipe_output_stats(obs_output_t *output)
{
	auto *o = static_cast<PipeOutput *>(obs_obj_get_data(output));
	if (!o)
		return {};
	std::lock_guard<std::mutex> lock(o->mutex);
	return o->stats;
}

void register_engine_outputs(Writer *writer)
{
	g_writer = writer;

	struct obs_output_info pipe = {};
	pipe.id = "skynat_pipe_output";
	pipe.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	pipe.encoded_video_codecs = "h264";
	pipe.encoded_audio_codecs = "aac";
	pipe.get_name = pipe_name;
	pipe.create = pipe_create;
	pipe.destroy = pipe_destroy;
	pipe.start = pipe_start;
	pipe.stop = pipe_stop;
	pipe.encoded_packet = pipe_packet;
	obs_register_output(&pipe);

	struct obs_output_info feed = {};
	feed.id = "skynat_feed_output";
	feed.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	feed.encoded_video_codecs = "h264";
	feed.get_name = feed_name;
	feed.create = feed_create;
	feed.destroy = pipe_destroy;
	feed.start = pipe_start;
	feed.stop = pipe_stop;
	feed.encoded_packet = pipe_packet;
	feed.get_defaults = feed_defaults;
	obs_register_output(&feed);
}

} // namespace skynat
