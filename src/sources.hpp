// skynat-engine — the engine's own libobs source types.
//
//   skynat_overlay  RGBA picture layer fed over the pipe (lower thirds,
//                   scripture, tickers...). Pixels are copied in on the
//                   command thread and uploaded on the graphics thread only
//                   when they changed.
//   skynat_pcm      audio input fed with the app's own mixer output
//                   (float32 interleaved stereo 48 kHz), so the app's audio
//                   chain stays the sound source while the engine owns the clock.
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.h>

#include <cstdint>
#include <string>

namespace skynat {

void register_engine_sources();

// A clip's picture as it is coded: its HDR transfer (HLG or PQ) is not turned
// into the canvas's light by libobs, so the signal reaches the filters after
// it unchanged (the app's look-up table makes the picture of it). Off: libobs
// converts as it always does.
void set_signal_as_is(obs_source_t *source, bool on);
bool signal_as_is(obs_source_t *source);

// ---- overlay -------------------------------------------------------------
// Copies a w x h rectangle (rows `stride` bytes apart) into the layer at
// (x, y). `clear` empties the whole layer first.
bool overlay_write(obs_source_t *overlay, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride,
		   bool clear, const uint8_t *data, size_t size, std::string *error);

// ---- pcm -----------------------------------------------------------------
struct PcmStats {
	uint64_t frames_in = 0;
	uint64_t samples_inserted = 0;
	uint64_t samples_dropped = 0;
	uint64_t resyncs = 0;
	double offset_ms = 0.0; // last measured lateness of the app's audio vs. the engine clock
};

// `interleaved` holds `frames` stereo frames; `delay_us` = how long ago the
// first sample was captured (the app's own buffering), for lip sync.
void pcm_push(obs_source_t *pcm, const float *interleaved, uint32_t frames, uint32_t delay_us);
PcmStats pcm_stats(obs_source_t *pcm);

} // namespace skynat
