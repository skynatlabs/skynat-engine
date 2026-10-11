// skynat-engine — libobs outputs that hand encoded packets to the pipe.
//
//   skynat_pipe_output  program H.264 + AAC (messages 3, 4, 5, 6)
//   skynat_feed_output  a monitor feed, H.264 only (messages 9, 10);
//                       setting "feed" = 0 (Program monitor) or 1 (Preview monitor)
//
// Copyright (C) 2026 Skynat
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <obs.h>

namespace skynat {

class Writer;

void register_engine_outputs(Writer *writer);

struct PipeOutputStats {
	uint64_t video_packets = 0;
	uint64_t audio_packets = 0;
	uint64_t keyframes = 0;
	uint64_t dropped = 0; // packets the pipe could not take (feeds only)
};
PipeOutputStats pipe_output_stats(obs_output_t *output);

} // namespace skynat
