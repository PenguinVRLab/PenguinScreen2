// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for text editor detection

layout(std140, binding = 1) uniform cb20
{
	vec2  VertexScale;
	vec2  VertexOffset;

	vec2  TextureScale;
	vec2  TextureOffset;

	vec2  PointSize;

	uint  MaxDepth;
	float LineAA1Width;
	vec2  vr_stereo;
	uint  vr_map_mode;
	uint  vr_band_count;
	vec4  vr_splits;
	vec4  vr_band[4];
};

#ifdef VERTEX_SHADER

#ifndef VS_EXPAND_NONE
#define VS_EXPAND_NONE 0
#define VS_EXPAND_POINT 1
#define VS_EXPAND_LINE 2
#define VS_EXPAND_SPRITE 3
#define VS_EXPAND_LINE_AA1 4
#define VS_EXPAND_TRIANGLE_AA1 5
#endif

out SHADER
{
	vec4 t_float;
	vec4 t_int;
	#if VS_IIP != 0
		vec4 c;
	#else
		flat vec4 c;
	#endif
	float inv_cov;
	flat uint interior;
} VSout;

const float exp_min32 = exp2(-32.0f);

#if VS_EXPAND == VS_EXPAND_NONE

layout(location = 0) in vec2  i_st;
layout(location = 2) in vec4  i_c;
layout(location = 3) in float i_q;
layout(location = 4) in uvec2 i_p;
layout(location = 5) in uint  i_z;
layout(location = 6) in uvec2 i_uv;
layout(location = 7) in vec4  i_f;

void texture_coord()
{
	vec2 uv = vec2(i_uv) - TextureOffset;
	vec2 st = i_st - TextureOffset;

	VSout.t_float.xy = st;
	VSout.t_float.w  = i_q;

	VSout.t_int.xy = uv * TextureScale;
#if VS_FST
	VSout.t_int.zw = uv;
#else
	VSout.t_int.zw = st / TextureScale;
#endif
}

void vs_main()
{
	highp uint z = min(i_z, MaxDepth);

	gl_Position.xy = vec2(i_p) - vec2(0.05f, 0.05f);
	gl_Position.xy = gl_Position.xy * VertexScale - VertexOffset;

	#if HAS_CLIP_CONTROL
		gl_Position.z = float(z) * exp_min32;
	#else
		gl_Position.z = (float(z) * exp_min32) * 2.0f - 1.0f;
	#endif

	gl_Position.w = 1.0f;

	texture_coord();

	VSout.c = i_c;
	VSout.t_float.z = i_f.x;

	#if VS_POINT_SIZE
		gl_PointSize = PointSize.x;
	#endif
}

#else

struct RawVertex
{
	vec2 ST;
	uint RGBA;
	float Q;
	uint XY;
	uint Z;
	uint UV;
	uint FOG;
};

layout(std140, binding = 4) uniform cb22
{
	uint BaseVertex;
	uint BaseIndex;
	uint pad_cb22_0;
	uint pad_cb22_1;
};

layout(std140, binding = 2) readonly buffer VertexBuffer {
	RawVertex vertex_buffer[];
};

layout(std430, binding = 3) readonly buffer IndexBuffer {
	uint index_buffer[];
};

struct ProcessedVertex
{
	vec4 p;
	vec4 t_float;
	vec4 t_int;
	vec4 c;
};

uint load_index(uint _i)
{
	uint i = _i + BaseIndex;
	uint shift = (i & 1u) << 4u;
	return (index_buffer[i >> 1u] >> shift) & 0xFFFFu;
}

ProcessedVertex load_vertex(uint index)
{
	RawVertex rvtx = vertex_buffer[BaseVertex + index];

	vec2 i_st = rvtx.ST;
	vec4 i_c = vec4(uvec4(bitfieldExtract(rvtx.RGBA, 0, 8), bitfieldExtract(rvtx.RGBA, 8, 8),
	                      bitfieldExtract(rvtx.RGBA, 16, 8), bitfieldExtract(rvtx.RGBA, 24, 8)));
	float i_q = rvtx.Q;
	uvec2 i_p = uvec2(bitfieldExtract(rvtx.XY, 0, 16), bitfieldExtract(rvtx.XY, 16, 16));
	uint i_z = rvtx.Z;
	uvec2 i_uv = uvec2(bitfieldExtract(rvtx.UV, 0, 16), bitfieldExtract(rvtx.UV, 16, 16));
	vec4 i_f = unpackUnorm4x8(rvtx.FOG);

	ProcessedVertex vtx;

	uint z = min(i_z, MaxDepth);
	vtx.p.xy = vec2(i_p) - vec2(0.05f, 0.05f);
	vtx.p.xy = vtx.p.xy * VertexScale - VertexOffset;

	#if HAS_CLIP_CONTROL
		vtx.p.z = float(z) * exp_min32;
	#else
		vtx.p.z = (float(z) * exp_min32) * 2.0f - 1.0f;
	#endif

	vtx.p.w = 1.0f;

	vec2 uv = vec2(i_uv) - TextureOffset;
	vec2 st = i_st - TextureOffset;

	vtx.t_float.xy = st;
	vtx.t_float.w  = i_q;

	vtx.t_int.xy = uv * TextureScale;
#if VS_FST
	vtx.t_int.zw = uv;
#else
	vtx.t_int.zw = st / TextureScale;
#endif

	vtx.c = i_c;
	vtx.t_float.z = i_f.x;

	return vtx;
}

vec2 get_xy_unscaled(vec2 xy)
{
	return round(xy / VertexScale) / 16.0f;
}

mat2 get_xy_deltas_unscaled(ProcessedVertex v0, ProcessedVertex v1, ProcessedVertex v2)
{
	vec2 xy0 = get_xy_unscaled(v0.p.xy);
	vec2 xy1 = get_xy_unscaled(v1.p.xy);
	vec2 xy2 = get_xy_unscaled(v2.p.xy);
	return mat2(xy1 - xy0, xy2 - xy0);
}

vec2 get_aa1_triangle_expand_dir(ProcessedVertex v0, ProcessedVertex v1, ProcessedVertex v2)
{
	mat2 xy_deltas = get_xy_deltas_unscaled(v0, v1, v2);
	vec2 line_delta = xy_deltas[0];
	vec2 line_opposite = xy_deltas[1];

	vec2 line_normal = vec2(line_delta.y, -line_delta.x);
	vec2 line_expand = abs(line_delta.x) >= abs(line_delta.y) ? vec2(0.0f, 1.0f) : vec2(1.0f, 0.0f);

	if ((dot(line_expand, line_normal) >= 0.0f) == (dot(line_opposite, line_normal) >= 0.0f))
	{
		line_expand = -line_expand;
	}

	return line_expand;
}

mat2 get_inverse(mat2 mat, float det)
{
	return mat2(mat[1][1], -mat[0][1], -mat[1][0], mat[0][0]) * (1 / det);
}

void extrapolate_aa1_triangle_edge(inout ProcessedVertex v0, ProcessedVertex v1, ProcessedVertex v2, mat2 dp_mat, vec2 dp)
{
	#if VS_TME
		#if VS_FST
			mat2 dt = mat2(v1.t_int.zw - v0.t_int.zw, v2.t_int.zw - v0.t_int.zw);
		#else
			mat2 dt = mat2(v1.t_float.xy - v0.t_float.xy, v2.t_float.xy - v0.t_float.xy);
		#endif
	#endif

	#if VS_IIP
		mat2x4 dc = mat2x4(v1.c - v0.c, v2.c - v0.c);
	#endif

	vec2 dz = vec2(v1.p.z - v0.p.z, v2.p.z - v0.p.z);

	vec2 df = vec2(v1.t_float.z - v0.t_float.z, v2.t_float.z - v0.t_float.z);

	vec2 dq = vec2(v1.t_float.w - v0.t_float.w, v2.t_float.w - v0.t_float.w);

	float dp_det = determinant(dp_mat);
	float len0 = length(dp_mat[0]);
	float len1 = length(dp_mat[1]);
	float len2 = length(dp_mat[1] - dp_mat[0]);
	float min_perp_length = abs(dp_det) / max(max(len0, len1), len2);

	mat2 inv_dp_mat = get_inverse(dp_mat, dp_det);

	vec2 weights = min_perp_length < 2 ? vec2(0) : inv_dp_mat * dp;

	v0.p.xy += dp * PointSize;

	#if VS_TME
		#if VS_FST
			v0.t_int.zw += dt * weights;
			v0.t_int.xy = v0.t_int.zw * TextureScale;
		#else
			v0.t_float.xy += dt * weights;
			v0.t_int.zw = v0.t_float.xy / TextureScale;
			v0.t_float.w += dot(dq, weights);
		#endif
	#endif

	#if VS_IIP
		v0.c += dc * weights;
		v0.c = clamp(v0.c, vec4(0), vec4(255));
	#endif

	v0.p.z += dot(dz, weights);

	v0.t_float.z += dot(df, weights);
}

void main()
{
	ProcessedVertex vtx;

	uint vid = uint(gl_VertexID);

#if VS_EXPAND == VS_EXPAND_POINT

	vtx = load_vertex(vid >> 2);

	vtx.p.x += ((vid & 1u) != 0u) ? PointSize.x : 0.0f;
	vtx.p.y += ((vid & 2u) != 0u) ? PointSize.y : 0.0f;

#elif (VS_EXPAND == VS_EXPAND_LINE) || (VS_EXPAND == VS_EXPAND_LINE_AA1)

	uint vid_base = vid >> 2;
	bool is_bottom = (vid & 2u) != 0u;
	bool is_right = (vid & 1u) != 0u;
	uint vid_other = is_bottom ? vid_base - 1 : vid_base + 1;
	vtx = load_vertex(vid_base);
	ProcessedVertex other = load_vertex(vid_other);

	vec2 line_delta = is_bottom ? (vtx.p.xy - other.p.xy) : (other.p.xy - vtx.p.xy);
	vec2 line_vector = normalize(line_delta / VertexScale);
	vec2 line_expand = vec2(line_vector.y, -line_vector.x);
#if VS_EXPAND == VS_EXPAND_LINE_AA1
	line_expand *= 2.0f * LineAA1Width;
#endif
	vec2 line_width = (line_expand * PointSize) / 2;
	vec2 offset = is_right ? line_width : -line_width;
	vtx.p.xy += offset;

#if VS_EXPAND == VS_EXPAND_LINE_AA1
	VSout.inv_cov = is_right ? 1.0f : -1.0f;
#endif

#elif VS_EXPAND == VS_EXPAND_SPRITE

	uint vid_base = vid >> 1;
	uint vid_lt = vid_base & ~1u;
	uint vid_rb = vid_base | 1u;

	ProcessedVertex lt = load_vertex(vid_lt);
	ProcessedVertex rb = load_vertex(vid_rb);
	vtx = rb;

	bool is_right = ((vid & 1u) != 0u);
	vtx.p.x = is_right ? lt.p.x : vtx.p.x;
	vtx.t_float.x = is_right ? lt.t_float.x : vtx.t_float.x;
	vtx.t_int.xz = is_right ? lt.t_int.xz : vtx.t_int.xz;

	bool is_bottom = ((vid & 2u) != 0u);
	vtx.p.y = is_bottom ? lt.p.y : vtx.p.y;
	vtx.t_float.y = is_bottom ? lt.t_float.y : vtx.t_float.y;
	vtx.t_int.yw = is_bottom ? lt.t_int.yw : vtx.t_int.yw;

#elif VS_EXPAND == VS_EXPAND_TRIANGLE_AA1

	uint prim_id = vid / 39;
	uint prim_offset = vid - 39 * prim_id;
	bool interior = prim_offset < 3;
	bool edge = 3 <= prim_offset && prim_offset < 21;

	if (interior)
	{
		vtx = load_vertex(load_index(3 * prim_id + prim_offset));
		VSout.inv_cov = 0.0f;
		VSout.interior = 1;
	}
	else if (edge)
	{
		uint prim_offset_edges = prim_offset - 3;
		uint i0 = prim_offset_edges / 6;
		uint i1 = (i0 >= 2) ? i0 - 2 : i0 + 1;
		uint i2 = (i0 >= 1) ? i0 - 1 : i0 + 2;
		uint edge_offset = prim_offset_edges - 6 * i0;

		bool is_bottom = (2 <= edge_offset) && (edge_offset <= 4);
		bool is_outside = (edge_offset & 1u) != 0;

		vtx = load_vertex(load_index(3 * prim_id + (is_bottom ? i1 : i0)));
		ProcessedVertex other = load_vertex(load_index(3 * prim_id + (is_bottom ? i0 : i1)));
		ProcessedVertex opposite = load_vertex(load_index(3 * prim_id + i2));

		mat2 pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		vec2 expand_dir = is_outside ? get_aa1_triangle_expand_dir(vtx, other, opposite) : vec2(0);

		extrapolate_aa1_triangle_edge(vtx, other, opposite, pos_deltas, expand_dir);

		VSout.inv_cov = is_outside ? 1.0f : 0.0f;

		VSout.interior = 0;
	}
	else
	{
		uint prim_offset_cap = prim_offset - 21;
		uint i0 = prim_offset_cap / 6;
		uint i1 = (i0 >= 2) ? i0 - 2 : i0 + 1;
		uint i2 = (i0 >= 1) ? i0 - 1 : i0 + 2;
		uint cap_offset = prim_offset_cap - 6 * i0;

		bool is_near_corner = cap_offset == 0 || cap_offset == 3;
		bool is_far_corner = cap_offset == 2 || cap_offset == 5;
		bool is_first_tri = cap_offset < 3;

		vtx = load_vertex(load_index(3 * prim_id + i0));
		ProcessedVertex other = load_vertex(load_index(3 * prim_id + (is_first_tri ? i1 : i2)));
		ProcessedVertex opposite = load_vertex(load_index(3 * prim_id + (is_first_tri ? i2 : i1)));

		mat2 pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		vec2 edge_expand_dir_0 = get_aa1_triangle_expand_dir(vtx, other, opposite);
		vec2 edge_expand_dir_1 = get_aa1_triangle_expand_dir(vtx, opposite, other);

		bool corner_filled = all(equal(edge_expand_dir_0, edge_expand_dir_1));

		vec2 far_corner_dir = corner_filled ? vec2(0) : -normalize((pos_deltas[0] + pos_deltas[1]) / 2);

		vec2 expand_dir = is_near_corner ? vec2(0) :
		                  is_far_corner ? far_corner_dir :
		                  edge_expand_dir_0;

		extrapolate_aa1_triangle_edge(vtx, other, opposite, pos_deltas, expand_dir);

		VSout.inv_cov = is_near_corner ? 0.0f : 1.0f;
	
		VSout.interior = 0;
	}

#endif

	gl_Position = vtx.p;
	VSout.t_float = vtx.t_float;
	VSout.t_int = vtx.t_int;
	VSout.c = vtx.c;
}

#endif

#endif
