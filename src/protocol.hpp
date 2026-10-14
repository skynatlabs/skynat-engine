// skynat-engine — pipe protocol constants and little-endian helpers.
// See PROTOCOL.md for the full description of every message.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace skynat {

constexpr uint32_t kProtocolVersion = 1;
constexpr const char *kEngineVersion = "0.4.2";

// Every message on both pipes: [u32 LE payload length][u8 type][payload].
constexpr size_t kHeaderSize = 5;
constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;

enum MsgType : uint8_t {
	MSG_COMMAND = 1,      // app -> engine: UTF-8 JSON command
	MSG_EVENT = 2,        // engine -> app: UTF-8 JSON event
	MSG_VIDEO_CONFIG = 3, // engine -> app: program H.264 avcC
	MSG_VIDEO_PACKET = 4, // engine -> app: program H.264 access unit (AVCC)
	MSG_AUDIO_CONFIG = 5, // engine -> app: AAC AudioSpecificConfig
	MSG_AUDIO_PACKET = 6, // engine -> app: raw AAC frame
	MSG_OVERLAY = 7,      // app -> engine: RGBA pixels for an overlay layer
	MSG_PCM = 8,          // app -> engine: float32 interleaved stereo 48 kHz
	MSG_FEED_CONFIG = 9,  // engine -> app: monitor feed avcC (feed byte first)
	MSG_FEED_PACKET = 10, // engine -> app: monitor feed access unit (feed byte first)
	MSG_THUMBNAIL = 11,   // engine -> app: small RGBA picture of one source
	MSG_FRAME = 12,       // app -> engine: one video frame for a "frames" source (I420, RGBA or NV12)
};

enum FeedId : uint8_t {
	FEED_PROGRAM = 0, // the Program monitor (scaled copy of the output)
	FEED_PREVIEW = 1, // the Preview monitor (studio mode)
	FEED_STREAM = 2,  // the stream's own picture (start_output "stream"): a broadcast, never dropped
	FEED_CLEAN = 3,   // Program without the graphics layer (start_output "clean"): a recording, never dropped
};

// Overlay message flags (u32 at offset 24 of the overlay header).
enum OverlayFlags : uint32_t {
	OVERLAY_CLEAR = 1u << 0, // clear the whole layer to transparent before applying the rectangle
};

inline void put_u8(std::vector<uint8_t> &b, uint8_t v)
{
	b.push_back(v);
}

inline void put_u16(std::vector<uint8_t> &b, uint16_t v)
{
	b.push_back((uint8_t)(v & 0xff));
	b.push_back((uint8_t)(v >> 8));
}

inline void put_u32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b.push_back((uint8_t)(v >> (8 * i)));
}

inline void put_i64(std::vector<uint8_t> &b, int64_t sv)
{
	uint64_t v = (uint64_t)sv;
	for (int i = 0; i < 8; i++)
		b.push_back((uint8_t)(v >> (8 * i)));
}

inline void put_bytes(std::vector<uint8_t> &b, const void *p, size_t n)
{
	const uint8_t *s = (const uint8_t *)p;
	b.insert(b.end(), s, s + n);
}

inline uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline int64_t get_i64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return (int64_t)v;
}

// Builds one complete frame (header + payload) in a single buffer so the
// writer thread can hand it to write() without another copy.
class Frame {
public:
	explicit Frame(uint8_t type, size_t reserve = 64)
	{
		buf.reserve(kHeaderSize + reserve);
		put_u32(buf, 0);
		put_u8(buf, type);
	}

	Frame &u8(uint8_t v)
	{
		put_u8(buf, v);
		return *this;
	}
	Frame &u16(uint16_t v)
	{
		put_u16(buf, v);
		return *this;
	}
	Frame &u32(uint32_t v)
	{
		put_u32(buf, v);
		return *this;
	}
	Frame &i64(int64_t v)
	{
		put_i64(buf, v);
		return *this;
	}
	Frame &bytes(const void *p, size_t n)
	{
		put_bytes(buf, p, n);
		return *this;
	}
	Frame &text(const std::string &s)
	{
		put_bytes(buf, s.data(), s.size());
		return *this;
	}

	// Patches the length field; the frame is ready to send afterwards.
	std::vector<uint8_t> finish()
	{
		uint32_t len = (uint32_t)(buf.size() - kHeaderSize);
		for (int i = 0; i < 4; i++)
			buf[i] = (uint8_t)(len >> (8 * i));
		return std::move(buf);
	}

private:
	std::vector<uint8_t> buf;
};

} // namespace skynat
