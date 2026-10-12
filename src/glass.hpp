// glass.hpp — a backdrop blur inside rounded rectangles: what is behind a
// panel drawn on the graphics layer, blurred and a little richer, the way
// CSS's backdrop-filter does it. The panel itself (its tint, edges and words)
// is drawn by the client on the graphics layer above.
#pragma once

#include <obs.h>
#include <vector>

namespace skynat {

struct GlassRect {
	float x, y, w, h, r; // canvas pixels; r = corner radius
};

constexpr size_t kMaxGlassRects = 6;

/** Registers the "skynat_glass_filter" video filter. Once, after startup. */
void register_glass_filter();

/** What the filter blurs: up to kMaxGlassRects rectangles; empty = off. */
void set_glass(obs_source_t *filter, const std::vector<GlassRect> &rects, float blur_px, float saturate, float brightness);

} // namespace skynat
