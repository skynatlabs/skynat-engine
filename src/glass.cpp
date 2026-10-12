// glass.cpp — the backdrop blur filter (see glass.hpp).
//
// One pass: inside any of the rectangles a pixel is the average of 24 samples
// spread over a disc (a golden-angle spiral, written out in full: the effect
// language is translated to GLSL and HLSL, and plain statements translate
// everywhere), then saturated and brightened a touch. Outside, the picture
// passes through untouched; with no rectangles the filter is skipped.
#include "glass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <graphics/graphics.h>

namespace skynat {

namespace {

struct Params {
	std::vector<GlassRect> rects;
	float blur = 18.0f, saturate = 1.4f, brightness = 1.0f;
};

struct Glass {
	obs_source_t *source = nullptr;
	gs_effect_t *effect = nullptr;
	std::mutex mutex;
	Params params;
};

std::string effect_source()
{
	std::string s = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 size;
uniform float4 rect0; uniform float4 rect1; uniform float4 rect2;
uniform float4 rect3; uniform float4 rect4; uniform float4 rect5;
uniform float4 radius_a;
uniform float2 radius_b;
uniform float rect_count;
uniform float blur_px;
uniform float saturation;
uniform float brightness;

sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };

struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv = v_in.uv;
	return vert_out;
}

float sd_rect(float2 p, float4 r, float rad)
{
	float2 h = r.zw * 0.5;
	float2 d = abs(p - (r.xy + h)) - (h - float2(rad, rad));
	return length(max(d, float2(0.0, 0.0))) + min(max(d.x, d.y), 0.0) - rad;
}

float4 PSGlass(VertData v_in) : TARGET
{
	float4 c = image.Sample(textureSampler, v_in.uv);
	float2 p = v_in.uv * size;
	float s = 1.0;
	if (rect_count > 0.5) s = min(s, sd_rect(p, rect0, radius_a.x));
	if (rect_count > 1.5) s = min(s, sd_rect(p, rect1, radius_a.y));
	if (rect_count > 2.5) s = min(s, sd_rect(p, rect2, radius_a.z));
	if (rect_count > 3.5) s = min(s, sd_rect(p, rect3, radius_a.w));
	if (rect_count > 4.5) s = min(s, sd_rect(p, rect4, radius_b.x));
	if (rect_count > 5.5) s = min(s, sd_rect(p, rect5, radius_b.y));
	if (s > 0.0) return c;
	float2 k = float2(blur_px, blur_px) / size;
	float3 acc = float3(0.0, 0.0, 0.0);
)";
	// 24 taps over a disc: radius ∝ sqrt, angle by the golden angle.
	for (int i = 0; i < 24; i++) {
		const double a = i * 2.39996323, rr = std::sqrt((i + 0.5) / 24.0);
		char line[160];
		std::snprintf(line, sizeof(line), "\tacc += image.Sample(textureSampler, v_in.uv + float2(%.5f, %.5f) * k).rgb;\n",
			      std::cos(a) * rr, std::sin(a) * rr);
		s += line;
	}
	s += R"(
	float3 b = acc / 24.0;
	float l = dot(b, float3(0.2126, 0.7152, 0.0722));
	b = lerp(float3(l, l, l), b, saturation) * brightness;
	return float4(b, c.a);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSGlass(v_in);
	}
}
)";
	return s;
}

const char *glass_name(void *) { return "Skynat glass backdrop"; }

void *glass_create(obs_data_t *, obs_source_t *source)
{
	auto *g = new Glass();
	g->source = source;
	obs_enter_graphics();
	char *errors = nullptr;
	const std::string src = effect_source();
	g->effect = gs_effect_create(src.c_str(), "skynat-glass.effect", &errors);
	if (!g->effect)
		blog(LOG_ERROR, "[skynat-engine] the glass effect did not compile: %s", errors ? errors : "(no message)");
	bfree(errors);
	obs_leave_graphics();
	return g;
}

void glass_destroy(void *data)
{
	auto *g = static_cast<Glass *>(data);
	obs_enter_graphics();
	if (g->effect)
		gs_effect_destroy(g->effect);
	obs_leave_graphics();
	delete g;
}

void glass_render(void *data, gs_effect_t *)
{
	auto *g = static_cast<Glass *>(data);
	Params p;
	{
		std::lock_guard<std::mutex> lock(g->mutex);
		p = g->params;
	}
	obs_source_t *target = obs_filter_get_target(g->source);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (p.rects.empty() || !g->effect || !w || !h) {
		obs_source_skip_video_filter(g->source);
		return;
	}
	if (!obs_source_process_filter_begin(g->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;
	struct vec2 sz;
	vec2_set(&sz, (float)w, (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(g->effect, "size"), &sz);
	float radii[6] = {0, 0, 0, 0, 0, 0};
	for (size_t i = 0; i < kMaxGlassRects; i++) {
		struct vec4 r;
		if (i < p.rects.size()) {
			vec4_set(&r, p.rects[i].x, p.rects[i].y, p.rects[i].w, p.rects[i].h);
			radii[i] = p.rects[i].r;
		} else {
			vec4_zero(&r);
		}
		const std::string name = "rect" + std::to_string(i);
		gs_effect_set_vec4(gs_effect_get_param_by_name(g->effect, name.c_str()), &r);
	}
	struct vec4 ra;
	vec4_set(&ra, radii[0], radii[1], radii[2], radii[3]);
	struct vec2 rb;
	vec2_set(&rb, radii[4], radii[5]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(g->effect, "radius_a"), &ra);
	gs_effect_set_vec2(gs_effect_get_param_by_name(g->effect, "radius_b"), &rb);
	gs_effect_set_float(gs_effect_get_param_by_name(g->effect, "rect_count"), (float)p.rects.size());
	gs_effect_set_float(gs_effect_get_param_by_name(g->effect, "blur_px"), p.blur);
	gs_effect_set_float(gs_effect_get_param_by_name(g->effect, "saturation"), p.saturate);
	gs_effect_set_float(gs_effect_get_param_by_name(g->effect, "brightness"), p.brightness);
	obs_source_process_filter_end(g->source, g->effect, w, h);
}

} // namespace

void register_glass_filter()
{
	static struct obs_source_info info;
	std::memset(&info, 0, sizeof(info));
	info.id = "skynat_glass_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = glass_name;
	info.create = glass_create;
	info.destroy = glass_destroy;
	info.video_render = glass_render;
	obs_register_source(&info);
}

void set_glass(obs_source_t *filter, const std::vector<GlassRect> &rects, float blur_px, float saturate, float brightness)
{
	if (!filter)
		return;
	auto *g = static_cast<Glass *>(obs_obj_get_data(filter));
	if (!g)
		return;
	std::lock_guard<std::mutex> lock(g->mutex);
	g->params.rects.assign(rects.begin(), rects.begin() + std::min(rects.size(), kMaxGlassRects));
	g->params.blur = blur_px;
	g->params.saturate = saturate;
	g->params.brightness = brightness;
}

} // namespace skynat
