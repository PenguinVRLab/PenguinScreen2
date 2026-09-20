// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//////////////////////////////////////////////////////////////////////
// Vertex Shader
//////////////////////////////////////////////////////////////////////

#if defined(VERTEX_SHADER)

#if VS_MULTIVIEW
#extension GL_EXT_multiview : require
#endif

#ifndef VS_EXPAND_NONE
#define VS_EXPAND_NONE 0
#define VS_EXPAND_POINT 1
#define VS_EXPAND_LINE 2
#define VS_EXPAND_SPRITE 3
#define VS_EXPAND_LINE_AA1 4
#define VS_EXPAND_TRIANGLE_AA1 5
#endif

layout(std140, set = 0, binding = 0) uniform cb0
{
	vec2 VertexScale;
	vec2 VertexOffset;
	vec2 TextureScale;
	vec2 TextureOffset;
	vec2 PointSize;
	uint MaxDepth;
	float LineAA1Width;
	vec2 vr_stereo;
	uint vr_map_mode;
	uint vr_band_count;
	vec4 vr_splits;
	vec4 vr_band[4];
};

layout(location = 0) out VSOutput
{
	vec4 t;
	vec4 ti;

	#if VS_IIP != 0
		vec4 c;
	#else
		flat vec4 c;
	#endif

	float inv_cov;
	flat uint interior;
} vsOut;

#if !VS_FST
float vr_stereo_disp(float q)
{
	if (vr_map_mode == 0u)
	{
		return vr_stereo.x * max(0.0f, 1.0f - vr_stereo.y * q);
	}

	if (vr_map_mode == 2u)
	{
		float w = 1.0f / max(q, 1e-8f);
		float t = clamp(log(w / vr_band[0].x) / log(vr_band[0].y / vr_band[0].x), 0.0f, 1.0f);
		return (vr_band[0].z * t) * vr_splits.w;
	}

	vec4 band;
	if      (q >= vr_splits.x) band = vr_band[0];
	else if (q >= vr_splits.y) band = vr_band[1];
	else if (q >= vr_splits.z) band = vr_band[2];
	else                       band = vr_band[3];

	float d = band.z + band.y * (1.0f - band.x * q);
	return max(0.0f, d) * vr_splits.w;
}
#endif

#if VS_FST
float vr_collimate_signed()
{
	#if VS_MULTIVIEW
		float vr_coll_sign = (gl_ViewIndex == 0) ? -1.0f : 1.0f;
	#else
		float vr_coll_sign = 1.0f;
	#endif
	return vr_coll_sign * vr_band[0].w;
}
#endif

#if VS_EXPAND == VS_EXPAND_NONE

layout(location = 0) in vec2 a_st;
layout(location = 1) in uvec4 a_c;
layout(location = 2) in float a_q;
layout(location = 3) in uvec2 a_p;
layout(location = 4) in uint a_z;
layout(location = 5) in uvec2 a_uv;
layout(location = 6) in vec4 a_f;

void main()
{
	uint z = min(a_z, MaxDepth);

	gl_Position = vec4(a_p, float(z), 1.0f) - vec4(0.05f, 0.05f, 0, 0);
	gl_Position.xy = gl_Position.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	gl_Position.z *= exp2(-32.0f);
	gl_Position.y = -gl_Position.y;

	#if !VS_FST
		if (vr_stereo.x != 0.0f || vr_map_mode != 0u)
		{
			#if VS_MULTIVIEW
				float vr_eye_sign = (gl_ViewIndex == 0) ? -1.0f : 1.0f;
			#else
				float vr_eye_sign = 1.0f;
			#endif
			gl_Position.x += vr_eye_sign * vr_stereo_disp(a_q);
		}
	#endif

	#if VS_FST
		if (vr_band[0].w != 0.0f)
			gl_Position.x += vr_collimate_signed();
	#endif

	#if VS_TME
		vec2 uv = a_uv - TextureOffset;
		vec2 st = a_st - TextureOffset;

		vsOut.ti.xy = uv * TextureScale;

		#if VS_FST
			vsOut.ti.zw = uv;
		#else
			vsOut.ti.zw = st / TextureScale;
		#endif

		vsOut.t.xy = st;
		vsOut.t.w = a_q;
	#else
		vsOut.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vsOut.ti = vec4(0.0f);
	#endif

	#if VS_POINT_SIZE
		gl_PointSize = PointSize.x;
	#endif

	vsOut.c = vec4(a_c);
	vsOut.t.z = a_f.r;
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

layout(push_constant) uniform cb2
{
	uint BaseVertex;
	uint BaseIndex;
	uint pad_cb2_0;
	uint pad_cb2_1;
};

layout(std140, set = 0, binding = 2) readonly buffer VertexBuffer {
	RawVertex vertex_buffer[];
};

layout(std430, set = 0, binding = 3) readonly buffer IndexBuffer {
	uint index_buffer[];
};

struct ProcessedVertex
{
	vec4 p;
	vec4 t;
	vec4 ti;
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

	vec2 a_st = rvtx.ST;
	uvec4 a_c = uvec4(bitfieldExtract(rvtx.RGBA, 0, 8), bitfieldExtract(rvtx.RGBA, 8, 8),
	                  bitfieldExtract(rvtx.RGBA, 16, 8), bitfieldExtract(rvtx.RGBA, 24, 8));
	float a_q = rvtx.Q;
	uvec2 a_p = uvec2(bitfieldExtract(rvtx.XY, 0, 16), bitfieldExtract(rvtx.XY, 16, 16));
	uint a_z = rvtx.Z;
	uvec2 a_uv = uvec2(bitfieldExtract(rvtx.UV, 0, 16), bitfieldExtract(rvtx.UV, 16, 16));
	vec4 a_f = unpackUnorm4x8(rvtx.FOG);

	ProcessedVertex vtx;

	uint z = min(a_z, MaxDepth);
	vtx.p = vec4(a_p, float(z), 1.0f) - vec4(0.05f, 0.05f, 0, 0);
	vtx.p.xy = vtx.p.xy * vec2(VertexScale.x, -VertexScale.y) - vec2(VertexOffset.x, -VertexOffset.y);
	vtx.p.z *= exp2(-32.0f);
	vtx.p.y = -vtx.p.y;

	#if VS_TME
		vec2 uv = a_uv - TextureOffset;
		vec2 st = a_st - TextureOffset;
		vtx.ti.xy = uv * TextureScale;

		#if VS_FST
			vtx.ti.zw = uv;
		#else
			vtx.ti.zw = st / TextureScale;
		#endif

		vtx.t.xy = st;
		vtx.t.w = a_q;
	#else
		vtx.t = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		vtx.ti = vec4(0.0f);
	#endif

	vtx.c = a_c;
	vtx.t.z = a_f.r;

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
			mat2 dt = mat2(v1.ti.zw - v0.ti.zw, v2.ti.zw - v0.ti.zw);
		#else
			mat2 dt = mat2(v1.t.xy - v0.t.xy, v2.t.xy - v0.t.xy);
		#endif
	#endif

	#if VS_IIP
		mat2x4 dc = mat2x4(v1.c - v0.c, v2.c - v0.c);
	#endif

	vec2 dz = vec2(v1.p.z - v0.p.z, v2.p.z - v0.p.z);

	vec2 df = vec2(v1.t.z - v0.t.z, v2.t.z - v0.t.z);

	vec2 dq = vec2(v1.t.w - v0.t.w, v2.t.w - v0.t.w);

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
			v0.ti.zw += dt * weights;
			v0.ti.xy = v0.ti.zw * TextureScale;
		#else
			v0.t.xy += dt * weights;
			v0.ti.zw = v0.t.xy / TextureScale;
			v0.t.w += dot(dq, weights);
		#endif
	#endif

	#if VS_IIP
		v0.c += dc * weights;
		v0.c = clamp(v0.c, vec4(0), vec4(255));
	#endif

	v0.p.z += dot(dz, weights);

	v0.t.z += dot(df, weights);
}

void main()
{
	ProcessedVertex vtx;
	uint vid = uint(gl_VertexIndex);

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
	vsOut.inv_cov = is_right ? 1.0f : -1.0f;
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
	vtx.t.x = is_right ? lt.t.x : vtx.t.x;
	vtx.ti.xz = is_right ? lt.ti.xz : vtx.ti.xz;

	bool is_bottom = ((vid & 2u) != 0u);
	vtx.p.y = is_bottom ? lt.p.y : vtx.p.y;
	vtx.t.y = is_bottom ? lt.t.y : vtx.t.y;
	vtx.ti.yw = is_bottom ? lt.ti.yw : vtx.ti.yw;

#elif VS_EXPAND == VS_EXPAND_TRIANGLE_AA1

	uint prim_id = vid / 39;
	uint prim_offset = vid - 39 * prim_id;
	bool interior = prim_offset < 3;
	bool edge = 3 <= prim_offset && prim_offset < 21;

	if (interior)
	{
		vtx = load_vertex(load_index(3 * prim_id + prim_offset));
		vsOut.inv_cov = 0.0f;
		vsOut.interior = 1;
	}
	else if (edge)
	{
		uint prim_offset_edges = prim_offset - 3;
		uint i0 = prim_offset_edges / 6;
		uint i1 = (i0 >= 2) ? i0 - 2 : i0 + 1;
		uint i2 = (i0 >= 1) ? i0 - 1 : i0 + 2;
		uint edge_offset = prim_offset_edges - 6 * i0;

		bool is_bottom = (2 <= edge_offset) && (edge_offset <= 4);
		bool is_outside = (edge_offset & 1) != 0;

		vtx = load_vertex(load_index(3 * prim_id + (is_bottom ? i1 : i0)));
		ProcessedVertex other = load_vertex(load_index(3 * prim_id + (is_bottom ? i0 : i1)));
		ProcessedVertex opposite = load_vertex(load_index(3 * prim_id + i2));

		mat2 pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		vec2 expand_dir = is_outside ? get_aa1_triangle_expand_dir(vtx, other, opposite) : vec2(0);

		extrapolate_aa1_triangle_edge(vtx, other, opposite, pos_deltas, expand_dir);

		vsOut.inv_cov = is_outside ? 1.0f : 0.0f;

		vsOut.interior = 0;
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

		vsOut.inv_cov = is_near_corner ? 0.0f : 1.0f;
	
		vsOut.interior = 0;
	}

#endif

	gl_Position = vtx.p;

	#if !VS_FST
		if (vr_stereo.x != 0.0f || vr_map_mode != 0u)
		{
			#if VS_MULTIVIEW
				float vr_eye_sign = (gl_ViewIndex == 0) ? -1.0f : 1.0f;
			#else
				float vr_eye_sign = 1.0f;
			#endif
			gl_Position.x += vr_eye_sign * vr_stereo_disp(vtx.t.w);
		}
	#endif

	#if VS_FST
		if (vr_band[0].w != 0.0f)
			gl_Position.x += vr_collimate_signed();
	#endif

	vsOut.t = vtx.t;
	vsOut.ti = vtx.ti;
	vsOut.c = vtx.c;
}

#endif

#endif

#ifdef FRAGMENT_SHADER

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

#ifndef VS_TME
#define VS_TME 1
#define VS_FST 1
#endif

#ifndef GS_IIP
#define GS_IIP 0
#define GS_PRIM 3
#define GS_POINT 0
#define GS_LINE 0
#endif

#ifndef ZTST_GEQUAL
#define ZTST_GEQUAL 2
#define ZTST_GREATER 3
#endif

#ifndef AFAIL_KEEP
#define AFAIL_KEEP 0
#define AFAIL_FB_ONLY 1
#define AFAIL_ZB_ONLY 2
#define AFAIL_RGB_ONLY 3
#define AFAIL_RGB_ONLY_DSB 4
#define AFAIL_RGB_ONLY_SW_Z 5
#endif

#ifndef PS_ATST_NONE
#define PS_ATST_NONE 0
#define PS_ATST_LEQUAL 1
#define PS_ATST_GEQUAL 2
#define PS_ATST_EQUAL 3
#define PS_ATST_NOTEQUAL 4
#endif

#ifndef PS_AA1_NONE
#define PS_AA1_NONE 0
#define PS_AA1_LINE 1
#define PS_AA1_TRIANGLE 2
#define PS_AA1_TRIANGLE_SW_Z 3
#endif

#ifndef PS_ROV_DEPTH_NONE
#define PS_ROV_DEPTH_NONE 0
#define PS_ROV_DEPTH_READ_WRITE 1
#define PS_ROV_DEPTH_READ_ONLY 2
#endif

#ifndef PS_FST
#define PS_FST 0
#define PS_WMS 0
#define PS_WMT 0
#define PS_ADJS 0
#define PS_ADJT 0
#define PS_FMT FMT_32
#define PS_AEM 0
#define PS_TFX 0
#define PS_TCC 1
#define PS_ATST 1
#define PS_AFAIL 0
#define PS_FOG 0
#define PS_BLEND_HW 0
#define PS_A_MASKED 0
#define PS_FBA 0
#define PS_FBMASK 0
#define PS_LTF 1
#define PS_TCOFFSETHACK 0
#define PS_SHUFFLE 0
#define PS_SHUFFLE_SAME 0
#define PS_PROCESS_BA 0
#define PS_PROCESS_RG 0
#define PS_SHUFFLE_ACROSS 0
#define PS_WRITE_RG 0
#define PS_READ16_SRC 0
#define PS_DST_FMT 0
#define PS_DEPTH_FMT 0
#define PS_PAL_FMT 0
#define PS_CHANNEL_FETCH 0
#define PS_TALES_OF_ABYSS_HLE 0
#define PS_URBAN_CHAOS_HLE 0
#define PS_COLCLIP_HW 0
#define PS_COLCLIP 0
#define PS_BLEND_A 0
#define PS_BLEND_B 0
#define PS_BLEND_C 0
#define PS_BLEND_D 0
#define PS_FIXED_ONE_A 0
#define PS_PABE 0
#define PS_DITHER 0
#define PS_DITHER_ADJUST 0
#define PS_ZCLAMP 0
#define PS_ZFLOOR 0
#define PS_SCANMSK 0
#define PS_AUTOMATIC_LOD 0
#define PS_MANUAL_LOD 0
#define PS_TEX_IS_FB 0
#define PS_NO_COLOR 0
#define PS_NO_COLOR1 0
#define PS_DATE 0
#define PS_TEX_IS_FB 0
#define PS_ROV_COLOR 0
#define PS_ROV_DEPTH 0
#endif

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define SW_BLEND_NEEDS_RT (SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1))
#define SW_AD_TO_HW (PS_BLEND_C == 1 && PS_A_MASKED)
#define AFAIL_NEEDS_RT (PS_AFAIL == AFAIL_ZB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define AFAIL_NEEDS_DEPTH (PS_AFAIL == AFAIL_FB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define ZTST_NEEDS_DEPTH (PS_ZTST == ZTST_GEQUAL || PS_ZTST == ZTST_GREATER)
#define AA1_NEEDS_DEPTH (PS_AA1 == PS_AA1_TRIANGLE_SW_Z)

#define PS_FEEDBACK_LOOP_IS_NEEDED_RT (PS_TEX_IS_FB == 1 || AFAIL_NEEDS_RT || PS_FBMASK || SW_BLEND_NEEDS_RT || SW_AD_TO_HW || (PS_DATE >= 5))
#define PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH (AFAIL_NEEDS_DEPTH || ZTST_NEEDS_DEPTH || AA1_NEEDS_DEPTH)
#define ZWRITE (PS_ZCLAMP || PS_ZFLOOR || SW_DEPTH || PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH)

#define PS_RETURN_COLOR_ROV (!PS_NO_COLOR && PS_ROV_COLOR)
#define PS_RETURN_COLOR (!PS_NO_COLOR && !PS_ROV_COLOR)
#define PS_RETURN_DEPTH_ROV (PS_ROV_DEPTH == PS_ROV_DEPTH_READ_WRITE)
#define PS_RETURN_DEPTH (ZWRITE && !PS_ROV_DEPTH)
#define PS_ROV_EARLYDEPTHSTENCIL (PS_ROV_COLOR && !PS_ROV_DEPTH && !ZWRITE)

#define NEEDS_TEX (PS_TFX != 4)

layout(std140, set = 0, binding = 1) uniform cb1
{
	vec3 FogColor;
	float AREF;
	vec4 WH;
	vec2 TA;
	float MaxDepthPS;
	float Af;
	uvec4 FbMask;
	vec4 HalfTexel;
	vec4 MinMax;
	vec4 LODParams;
	vec4 STRange;
	ivec4 ChannelShuffle;
	vec2 ChannelShuffleOffset;
	vec2 TC_OffsetHack;
	vec2 STScale;
	mat4 DitherMatrix;
	float ScaledScaleFactor;
	float RcpScaleFactor;
	float _pad0_cb1;
	float _pad1_cb1;
	float LineCovScale;
	float _pad2_cb1;
	float _pad3_cb1;
	float _pad4_cb1;
};

layout(location = 0) in VSOutput
{
	vec4 t;
	vec4 ti;
	#if PS_IIP != 0
		vec4 c;
	#else
		flat vec4 c;
	#endif
	float inv_cov;
	flat uint interior;
} vsIn;

#if PS_RETURN_COLOR
	#if !PS_NO_COLOR1
		layout(location = 0, index = 0) out vec4 o_col0;
		layout(location = 0, index = 1) out vec4 o_col1;
	#elif !PS_NO_COLOR
		layout(location = 0) out vec4 o_col0;
	#endif
#elif PS_RETURN_COLOR_ROV
	vec4 o_col0;
#endif

#if PS_ROV_COLOR
	layout(set = 1, binding = 5, rgba8) uniform restrict coherent image2D RtImageRov;
	vec4 rov_rt_value;
	vec4 sample_from_rt() { return rov_rt_value; }
#endif

#if PS_ROV_DEPTH
	layout(set = 1, binding = 6, r32f) uniform restrict coherent image2D DepthImageRov;
	float rov_depth_value;
	float sample_from_depth() { return rov_depth_value; }
#endif

#if NEEDS_TEX
#if PS_TEX_IN_ARRAY
#extension GL_EXT_multiview : require
layout(set = 1, binding = 0) uniform sampler2DArray Texture;
#define TEXC(uv) vec3(uv, float(gl_ViewIndex))
#define ITEXC(uv) ivec3(uv, gl_ViewIndex)
#else
layout(set = 1, binding = 0) uniform sampler2D Texture;
#define TEXC(uv) (uv)
#define ITEXC(uv) (uv)
#endif
layout(set = 1, binding = 1) uniform texture2D Palette;
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_RT || PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH
	#if defined(DISABLE_TEXTURE_BARRIER) || defined(HAS_FEEDBACK_LOOP_LAYOUT)
		#if (PS_RT_IN_ARRAY || PS_DEPTH_IN_ARRAY)
		#extension GL_EXT_multiview : require
		#endif
		#if (PS_FEEDBACK_LOOP_IS_NEEDED_RT && !PS_ROV_COLOR)
			#if PS_RT_IN_ARRAY
				layout(set = 1, binding = 2) uniform texture2DArray RtSampler;
				vec4 sample_from_rt() { return texelFetch(RtSampler, ivec3(ivec2(gl_FragCoord.xy), gl_ViewIndex), 0); }
			#else
				layout(set = 1, binding = 2) uniform texture2D RtSampler;
				vec4 sample_from_rt() { return texelFetch(RtSampler, ivec2(gl_FragCoord.xy), 0); }
			#endif
		#endif
		#if (PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH && !PS_ROV_DEPTH)
			#if PS_DEPTH_IN_ARRAY
				layout(set = 1, binding = 4) uniform texture2DArray DepthSampler;
				float sample_from_depth() { return texelFetch(DepthSampler, ivec3(ivec2(gl_FragCoord.xy), gl_ViewIndex), 0).r; }
			#else
				layout(set = 1, binding = 4) uniform texture2D DepthSampler;
				float sample_from_depth() { return texelFetch(DepthSampler, ivec2(gl_FragCoord.xy), 0).r; }
			#endif
		#endif
	#else
		#if (PS_FEEDBACK_LOOP_IS_NEEDED_RT && !PS_ROV_COLOR) && (PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH && !PS_ROV_DEPTH)
			layout(input_attachment_index = 0, set = 1, binding = 2) uniform subpassInput RtSampler;
			layout(input_attachment_index = 1, set = 1, binding = 4) uniform subpassInput DepthSampler;
			vec4 sample_from_rt() { return subpassLoad(RtSampler); }
			float sample_from_depth() { return subpassLoad(DepthSampler).r; }
		#elif (PS_FEEDBACK_LOOP_IS_NEEDED_RT && !PS_ROV_COLOR)
			layout(input_attachment_index = 0, set = 1, binding = 2) uniform subpassInput RtSampler;
			vec4 sample_from_rt() { return subpassLoad(RtSampler); }
		#elif (PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH && !PS_ROV_DEPTH)
			layout(input_attachment_index = 0, set = 1, binding = 4) uniform subpassInput DepthSampler;
			float sample_from_depth() { return subpassLoad(DepthSampler).r; }
		#endif
	#endif
#endif

#if PS_DATE > 0
layout(set = 1, binding = 3) uniform texture2D PrimMinTexture;
#endif

#if ZWRITE && !PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH
layout(depth_less) out float gl_FragDepth;
#endif

#if NEEDS_TEX

#if (PS_AUTOMATIC_LOD != 1) && (PS_MANUAL_LOD == 1)
float manual_lod(float uv_w)
{
	float K = LODParams.x;
	float L = LODParams.y;
	float bias = LODParams.z;
	float max_lod = LODParams.w;

	float gs_lod = K - log2(abs(uv_w)) * L;
	return min(gs_lod, max_lod) - bias;
}
#endif

#if PS_ANISOTROPIC_FILTERING > 1
vec4 sample_c_af(vec2 uv, float uv_w)
{
	uv = (any(isnan(uv)) || any(isinf(uv))) ? vec2(0.0f, 0.0f) : uv;

	uv = clamp(uv, -8388608.0f, 8388608.0f);

	vec2 sz = vec2(textureSize(Texture, 0).xy);
	vec2 dX = dFdx(uv) * sz;
	vec2 dY = dFdy(uv) * sz;

	float length_x = length(dX);
	float length_y = length(dY);

	bool d_zero = length_x < 0.001f || length_y < 0.001f;
	float f = (dX.x * dY.y - dX.y * dY.x);
	bool d_par = f < 0.001f;
	bool d_per = dot(dX, dY) < 0.001f;
	bool d_inf_nan = any(isinf(dX)) || any(isinf(dY)) || any(isnan(dX)) || any(isnan(dY));

	if (!(d_zero || d_par || d_per || d_inf_nan))
	{
		float A = dX.y * dX.y + dY.y * dY.y;
		float B = -2 * (dX.x * dX.y + dY.x * dY.y);
		float C = dX.x * dX.x + dY.x * dY.x;
		float f = (dX.x * dY.y - dY.x * dX.y);
		float F = f * f;

		float p = A - C;
		float q = A + C;
		float t = sqrt(p * p + B * B);

		float signB = sign(B);
		float denom_plus  = t * (q + t);
		float denom_minus = t * (q - t);

		float sqrtA = sqrt(F * (t + p));
		float sqrtB = sqrt(F * (t - p));

		float inv_sqrt_denom_plus  = inversesqrt(denom_plus);
		float inv_sqrt_denom_minus = inversesqrt(denom_minus);

		vec2 new_dX = vec2(
			sqrtA * inv_sqrt_denom_plus,
			sqrtB * inv_sqrt_denom_plus * signB
		);

		vec2 new_dY = vec2(
			sqrtB * inv_sqrt_denom_minus * -signB,
			sqrtA * inv_sqrt_denom_minus
		);

		d_inf_nan = any(isinf(new_dX)) || any(isinf(new_dY)) || any(isnan(new_dX)) || any(isnan(new_dY));
		if (!d_inf_nan)
		{
			dX = new_dX;
			dY = new_dY;
			length_x = length(dX);
			length_y = length(dY);
		}
	}

	bool is_major_x = length_x > length_y;
	float length_major = is_major_x ? length_x : length_y;
	float length_minor = is_major_x ? length_y : length_x;

	float aniso_ratio;
	float length_lod;
	vec2 aniso_line;
	if (length_major <= 1.0f)
	{
		aniso_ratio = 1.0f;
		length_lod = length_major;
		aniso_line = vec2(0.0f, 0.0f);
	}
	else
	{
		vec2 aniso_line_dir = is_major_x ? dX : dY;

		aniso_ratio = min(length_major / length_minor, PS_ANISOTROPIC_FILTERING);
		length_lod = length_major / aniso_ratio;

		if (length_lod < 1.0f)
			aniso_ratio = max(1.0f, aniso_ratio * length_lod);

		aniso_ratio = round(aniso_ratio);

		aniso_line = aniso_line_dir * 0.5f * (1.0f / sz);
	}

#if PS_AUTOMATIC_LOD == 1
	float lod = log2(length_lod);
#elif PS_MANUAL_LOD == 1
	float lod = manual_lod(uv_w);
#else
	float lod = 0.0f;
#endif

	vec4 colour;
	if (aniso_ratio == 1.0f)
		colour = textureLod(Texture, TEXC(uv), lod);
	else
	{
		vec4 num = vec4(0.0f, 0.0f, 0.0f, 0.0f);
		vec2 segment = (2.0f * aniso_line) / aniso_ratio;
		for (int i = 0; i < aniso_ratio; i++)
		{
			vec2 d = -aniso_line + (0.5f + i) * segment;	
			vec2 uv_sample = uv + d;
			vec4 sample_colour = textureLod(Texture, TEXC(uv_sample), lod);
			num += sample_colour;
		}

		colour = num / aniso_ratio;
	}
	return colour;
}
#endif

vec4 sample_c(vec2 uv)
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#elif PS_REGION_RECT
	return texelFetch(Texture, ITEXC(ivec2(uv)), 0);
#else

#if !PS_ADJS && !PS_ADJT
	uv *= STScale;
#else
	#if PS_ADJS
		uv.x = (uv.x - STRange.x) * STRange.z;
	#else
		uv.x = uv.x * STScale.x;
	#endif
	#if PS_ADJT
		uv.y = (uv.y - STRange.y) * STRange.w;
	#else
		uv.y = uv.y * STScale.y;
	#endif
#endif

#if PS_ANISOTROPIC_FILTERING > 1
	return sample_c_af(uv, vsIn.t.w);
#elif PS_AUTOMATIC_LOD == 1
	return texture(Texture, TEXC(uv));
#elif PS_MANUAL_LOD == 1
	return textureLod(Texture, TEXC(uv), manual_lod(vsIn.t.w));
#else
	return textureLod(Texture, TEXC(uv), 0);
#endif
#endif
}

vec4 sample_p(uint idx)
{
	return texelFetch(Palette, ivec2(int(idx), 0), 0);
}

vec4 sample_p_norm(float u)
{
	return sample_p(uint(u * 255.5f));
}

vec4 clamp_wrap_uv(vec4 uv)
{
	vec4 tex_size = WH.xyxy;

	#if PS_WMS == PS_WMT
	{
		#if PS_REGION_RECT == 1 && PS_WMS == 0
		{
			uv = fract(uv);
		}
		#elif PS_REGION_RECT == 1 && PS_WMS == 1
		{
			uv = clamp(uv, vec4(0.0f), vec4(1.0f));
		}
		#elif PS_WMS == 2
		{
			uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
		}
		#elif PS_WMS == 3
		{
			#if PS_FST == 0
			uv = fract(uv);
			#endif
			uv = vec4((uvec4(uv * tex_size) & floatBitsToUint(MinMax.xyxy)) | floatBitsToUint(MinMax.zwzw)) / tex_size;
		}
		#endif
	}
	#else
	{
		#if PS_REGION_RECT == 1 && PS_WMS == 0
		{
			uv.xz = fract(uv.xz);
		}
		#elif PS_REGION_RECT == 1 && PS_WMS == 1
		{
			uv.xz = clamp(uv.xz, vec2(0.0f), vec2(1.0f));
		}
		#elif PS_WMS == 2
		{
			uv.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);
		}
		#elif PS_WMS == 3
		{
			#if PS_FST == 0
			uv.xz = fract(uv.xz);
			#endif
			uv.xz = vec2((uvec2(uv.xz * tex_size.xx) & floatBitsToUint(MinMax.xx)) | floatBitsToUint(MinMax.zz)) / tex_size.xx;
		}
		#endif
		#if PS_REGION_RECT == 1 && PS_WMT == 0
		{
			uv.yw = fract(uv.yw);
		}
		#elif PS_REGION_RECT == 1 && PS_WMT == 1
		{
			uv.yw = clamp(uv.yw, vec2(0.0f), vec2(1.0f));
		}
		#elif PS_WMT == 2
		{
			uv.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);
		}
		#elif PS_WMT == 3
		{
			#if PS_FST == 0
			uv.yw = fract(uv.yw);
			#endif
			uv.yw = vec2((uvec2(uv.yw * tex_size.yy) & floatBitsToUint(MinMax.yy)) | floatBitsToUint(MinMax.ww)) / tex_size.yy;
		}
		#endif
	}
	#endif

	#if PS_REGION_RECT == 1
		uv = clamp(uv * WH.zwzw + STRange.xyxy, STRange.xyxy, STRange.zwzw);
	#endif

	return uv;
}

mat4 sample_4c(vec4 uv)
{
	mat4 c;

	c[0] = sample_c(uv.xy);
	c[1] = sample_c(uv.zy);
	c[2] = sample_c(uv.xw);
	c[3] = sample_c(uv.zw);

	return c;
}

uvec4 sample_4_index(vec4 uv)
{
	vec4 c;

	c.x = sample_c(uv.xy).a;
	c.y = sample_c(uv.zy).a;
	c.z = sample_c(uv.xw).a;
	c.w = sample_c(uv.zw).a;

#if PS_RTA_SRC_CORRECTION
	uvec4 i = uvec4(round(c * 128.25f));
#else
	uvec4 i = uvec4(c * 255.5f);
#endif

	#if PS_PAL_FMT == 1
		return i & 0xFu;
	#elif PS_PAL_FMT == 2
		return i >> 4u;
	#else
		return i;
	#endif
}

mat4 sample_4p(uvec4 u)
{
	mat4 c;

	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);

	return c;
}

uint fetch_raw_depth(ivec2 xy)
{
#if PS_TEX_IS_FB
	vec4 col = sample_from_rt();
#else
	vec4 col = texelFetch(Texture, ITEXC(xy), 0);
#endif
	return uint(col.r * exp2(32.0f));
}

vec4 fetch_raw_color(ivec2 xy)
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#else
	return texelFetch(Texture, ITEXC(xy), 0);
#endif
}

vec4 fetch_c(ivec2 uv)
{
#if PS_TEX_IS_FB
	return sample_from_rt();
#else
	return texelFetch(Texture, ITEXC(uv), 0);
#endif
}

ivec2 clamp_wrap_uv_depth(ivec2 uv)
{
	ivec4 mask = floatBitsToInt(MinMax) << 4;
	#if (PS_WMS == PS_WMT)
	{
		#if (PS_WMS == 2)
		{
			uv = clamp(uv, mask.xy, mask.zw);
		}
		#elif (PS_WMS == 3)
		{
			uv = (uv & mask.xy) | mask.zw;
		}
		#endif
	}
	#else
	{
		#if (PS_WMS == 2)
		{
			uv.x = clamp(uv.x, mask.x, mask.z);
		}
		#elif (PS_WMS == 3)
		{
			uv.x = (uv.x & mask.x) | mask.z;
		}
		#endif
		#if (PS_WMT == 2)
		{
			uv.y = clamp(uv.y, mask.y, mask.w);
		}
		#elif (PS_WMT == 3)
		{
			uv.y = (uv.y & mask.y) | mask.w;
		}
		#endif
	}
	#endif
	return uv;
}

vec4 sample_depth(vec2 st, ivec2 pos)
{
	vec2 uv_f = vec2(clamp_wrap_uv_depth(ivec2(st))) * vec2(ScaledScaleFactor);

	#if PS_REGION_RECT == 1
		uv_f = clamp(uv_f + STRange.xy, STRange.xy, STRange.zw);
	#endif

	ivec2 uv = ivec2(uv_f);
	vec4 t = vec4(0.0f);

	#if (PS_TALES_OF_ABYSS_HLE == 1)
	{
		uint depth = fetch_raw_depth(pos);

		t = texelFetch(Palette, ivec2((depth >> 8u) & 0xFFu, 0), 0) * 255.0f;
	}
	#elif (PS_URBAN_CHAOS_HLE == 1)
	{

		uint depth = fetch_raw_depth(pos);

		t = texelFetch(Palette, ivec2(depth & 0xFFu, 0), 0) * 255.0f;

		float green = float(((depth >> 8u) & 0xFFu) * 36.0f);
		green = min(green, 255.0f);
		t.g += green;
	}
	#elif (PS_DEPTH_FMT == 1)
	{

		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0xFFu), ((d >> 8) & 0xFFu), ((d >> 16) & 0xFFu), (d >> 24)));
	}
	#elif (PS_DEPTH_FMT == 2)
	{

		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = vec4(uvec4((d & 0x1Fu), ((d >> 5) & 0x1Fu), ((d >> 10) & 0x1Fu), (d >> 15) & 0x01u)) * vec4(8.0f, 8.0f, 8.0f, 128.0f);
	}
	#elif (PS_DEPTH_FMT == 3)
	{
		t = fetch_c(uv) * 255.0f;
	}
	#endif

	#if (PS_AEM_FMT == FMT_24)
	{
		t.a = ((PS_AEM == 0) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	#elif (PS_AEM_FMT == FMT_16)
	{
		t.a = t.a >= 128.0f ? 255.0f * TA.y : ((PS_AEM == 0) || any(bvec3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	#elif PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE
	{
		t = trunc(sample_4p(uvec4(t.aaaa))[0] * 255.0f + 0.05f);
	}
	#endif

	return t;
}

vec4 fetch_red(ivec2 xy)
{
	vec4 rt;

	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		uint depth = (fetch_raw_depth(xy)) & 0xFFu;
		rt = vec4(float(depth) / 255.0f);
	#else
		rt = fetch_raw_color(xy);
	#endif

	return sample_p_norm(rt.r) * 255.0f;
}

vec4 fetch_green(ivec2 xy)
{
	vec4 rt;

	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		uint depth = (fetch_raw_depth(xy) >> 8u) & 0xFFu;
		rt = vec4(float(depth) / 255.0f);
	#else
		rt = fetch_raw_color(xy);
	#endif

	return sample_p_norm(rt.g) * 255.0f;
}

vec4 fetch_blue(ivec2 xy)
{
	vec4 rt;

	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		uint depth = (fetch_raw_depth(xy) >> 16u) & 0xFFu;
		rt = vec4(float(depth) / 255.0f);
	#else
		rt = fetch_raw_color(xy);
	#endif

	return sample_p_norm(rt.b) * 255.0f;
}

vec4 fetch_alpha(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	return sample_p_norm(rt.a) * 255.0f;
}

vec4 fetch_rgb(ivec2 xy)
{
	vec4 rt = fetch_raw_color(xy);
	vec4 c = vec4(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0);
	return c * 255.0f;
}

vec4 fetch_gXbY(ivec2 xy)
{
	#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
		uint depth = fetch_raw_depth(xy);
		uint bg = (depth >> (8u + uint(ChannelShuffle.w))) & 0xFFu;
		return vec4(bg);
	#else
		ivec4 rt = ivec4(fetch_raw_color(xy) * 255.0);
		int green = (rt.g >> ChannelShuffle.w) & ChannelShuffle.z;
		int blue = (rt.b << ChannelShuffle.y) & ChannelShuffle.x;
		return vec4(float(green | blue));
	#endif
}

vec4 sample_color(vec2 st)
{
	#if PS_TCOFFSETHACK
	st += TC_OffsetHack.xy;
	#endif

	vec4 t;
	mat4 c;
	vec2 dd;

	#if PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_REGION_RECT == 0 && PS_WMS < 2 && PS_WMT < 2
	{
		c[0] = sample_c(st);
	}
	#else
	{
		vec4 uv;

		#if PS_LTF
		{
			uv = st.xyxy + HalfTexel;
			dd = fract(uv.xy * WH.zw);

			#if PS_FST == 0
			{
				dd = clamp(dd, vec2(0.0f), vec2(0.9999999f));
			}
			#endif
		}
		#else
		{
			uv = st.xyxy;
		}
		#endif

		uv = clamp_wrap_uv(uv);

#if PS_PAL_FMT != 0
			c = sample_4p(sample_4_index(uv));
#else
			c = sample_4c(uv);
#endif
	}
	#endif

	for (uint i = 0; i < 4; i++)
	{
		#if (PS_AEM_FMT == FMT_24)
			c[i].a = (PS_AEM == 0 || any(bvec3(c[i].rgb))) ? TA.x : 0.0f;
		#elif (PS_AEM_FMT == FMT_16)
			c[i].a = (c[i].a >= 0.5) ? TA.y : ((PS_AEM == 0 || any(bvec3(ivec3(c[i].rgb * 255.0f) & ivec3(0xF8)))) ? TA.x : 0.0f);
		#endif
	}

	#if PS_LTF
	{
		t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
	}
	#else
	{
		t = c[0];
	}
	#endif
#if PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_RTA_SRC_CORRECTION
	t.a = t.a * (128.5f / 255.0f);
#endif
	return trunc(t * 255.0f + 0.05f);
}

#endif

vec4 tfx(vec4 T, vec4 C)
{
	vec4 C_out;
	vec4 FxT = trunc((C * T) / 128.0f);

#if (PS_TFX == 0)
	C_out = FxT;
#elif (PS_TFX == 1)
	C_out = T;
#elif (PS_TFX == 2)
	C_out.rgb = FxT.rgb + C.a;
	C_out.a = T.a + C.a;
#elif (PS_TFX == 3)
	C_out.rgb = FxT.rgb + C.a;
	C_out.a = T.a;
#else
	C_out = C;
#endif

#if (PS_TCC == 0)
	C_out.a = C.a;
#endif

#if (PS_TFX == 0) || (PS_TFX == 2) || (PS_TFX == 3)
	C_out = min(C_out, 255.0f);
#endif

	return C_out;
}

bool atst(vec4 C)
{
	float a = C.a;

#if PS_ATST == PS_ATST_LEQUAL

	return (a <= AREF);

#elif PS_ATST == PS_ATST_GEQUAL

	return (a >= AREF);

#elif PS_ATST == PS_ATST_EQUAL

	return (abs(a - AREF) <= 0.5f);

#elif PS_ATST == PS_ATST_NOTEQUAL

	return (abs(a - AREF) >= 0.5f);

#else

	return true;

#endif
}

vec4 fog(vec4 c, float f)
{
	#if PS_FOG
		c.rgb = trunc(mix(FogColor, c.rgb, (f * 255.0f) / 256.0f));
	#endif

	return c;
}

vec4 ps_color()
{
#if PS_FST == 0
	vec2 st = vsIn.t.xy / vsIn.t.w;
	vec2 st_int = vsIn.ti.zw / vsIn.t.w;
#else
	vec2 st = vsIn.ti.xy;
	vec2 st_int = vsIn.ti.zw;
#endif

#if !NEEDS_TEX
	vec4 T = vec4(0.0f);
#elif PS_CHANNEL_FETCH == 1
	vec4 T = fetch_red(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 2
	vec4 T = fetch_green(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 3
	vec4 T = fetch_blue(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 4
	vec4 T = fetch_alpha(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 5
	vec4 T = fetch_rgb(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 6
	vec4 T = fetch_gXbY(ivec2(gl_FragCoord.xy + ChannelShuffleOffset));
#elif PS_DEPTH_FMT > 0
	vec4 T = sample_depth(st_int, ivec2(gl_FragCoord.xy));
#else
	vec4 T = sample_color(st);
#endif

	#if PS_SHUFFLE && !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
		uvec4 denorm_c_before = uvec4(T);
		#if (PS_PROCESS_BA & SHUFFLE_READ)
			T.r = float((denorm_c_before.b << 3) & 0xF8u);
			T.g = float(((denorm_c_before.b >> 2) & 0x38u) | ((denorm_c_before.a << 6) & 0xC0u));
			T.b = float((denorm_c_before.a << 1) & 0xF8u);
			T.a = float(denorm_c_before.a & 0x80u);
		#else
			T.r = float((denorm_c_before.r << 3) & 0xF8u);
			T.g = float(((denorm_c_before.r >> 2) & 0x38u) | ((denorm_c_before.g << 6) & 0xC0u));
			T.b = float((denorm_c_before.g << 1) & 0xF8u);
			T.a = float(denorm_c_before.g & 0x80u);
		#endif

		T.a = ((T.a >= 127.5f) ? TA.y : ((PS_AEM == 0 || any(bvec3(ivec3(T.rgb) & ivec3(0xF8)))) ? TA.x : 0.0f)) * 255.0f;
	#endif

	vec4 C = tfx(T, vsIn.c);

	C = fog(C, vsIn.t.z);

	return C;
}

void ps_fbmask(inout vec4 C)
{
	#if PS_FBMASK
		#if PS_COLCLIP_HW == 1
			vec4 RT = trunc(sample_from_rt() * 65535.0f);
		#else
			vec4 RT = trunc(sample_from_rt() * 255.0f + 0.1f);
		#endif
		C = vec4((uvec4(C) & ~FbMask) | (uvec4(RT) & FbMask));
	#endif
}

void ps_dither(inout vec3 C, float As)
{
	#if PS_DITHER > 0 && PS_DITHER < 3
		ivec2 fpos;

		#if PS_DITHER == 2
			fpos = ivec2(gl_FragCoord.xy);
		#else
			fpos = ivec2(gl_FragCoord.xy * RcpScaleFactor);
		#endif

		float value = DitherMatrix[fpos.y & 3][fpos.x & 3];

		#if PS_DITHER_ADJUST
			#if PS_BLEND_C == 2
				float Alpha = Af;
			#else
				float Alpha = As;
			#endif

			value *= Alpha > 0.0f ? min(1.0f / Alpha, 1.0f) : 1.0f;
		#endif

		#if PS_ROUND_INV
			C -= value;
		#else
			C += value;
		#endif
	#endif
}

void ps_color_clamp_wrap(inout vec3 C)
{
#if SW_BLEND || (PS_DITHER > 0 && PS_DITHER < 3) || PS_FBMASK

#if PS_DST_FMT == FMT_16 && PS_BLEND_MIX == 0 && PS_ROUND_INV
	C += 7.0f;
#endif

#if PS_COLCLIP == 0 && PS_COLCLIP_HW == 0
	C = clamp(C, vec3(0.0f), vec3(255.0f));
#endif

#if PS_DST_FMT == FMT_16 && PS_DITHER != 3 && (PS_BLEND_MIX == 0 || PS_DITHER > 0)
	C = vec3(ivec3(C) & ivec3(0xF8));
#elif PS_COLCLIP == 1 || PS_COLCLIP_HW == 1
	C = vec3(ivec3(C) & ivec3(0xFF));
#endif

#elif PS_DST_FMT == FMT_16 && PS_DITHER != 3 && PS_BLEND_MIX == 0 && PS_BLEND_HW == 0
	C = vec3(ivec3(C) & ivec3(0xF8));
#endif
}

void ps_blend(inout vec4 Color, inout vec4 As_rgba)
{
	float As = As_rgba.a;

	#if SW_BLEND

		#if PS_PABE
			if (As < 1.0f)
			{
				As_rgba.rgb = vec3(0.0f);
				return;
			}

			As_rgba.rgb = vec3(1.0f);
		#endif

		#if PS_FEEDBACK_LOOP_IS_NEEDED_RT
			vec4 RT = sample_from_rt();
		#else
			vec4 RT = vec4(0.0f);
		#endif

		#if PS_RTA_CORRECTION
			float Ad = trunc(RT.a * 128.0f + 0.1f) / 128.0f;
		#else
			float Ad = trunc(RT.a * 255.0f + 0.1f) / 128.0f;
		#endif

		#if PS_SHUFFLE && PS_FEEDBACK_LOOP_IS_NEEDED_RT
			uvec4 denorm_rt = uvec4(RT);
			#if (PS_PROCESS_BA & SHUFFLE_WRITE)
				RT.r = float((denorm_rt.b << 3) & 0xF8u);
				RT.g = float(((denorm_rt.b >> 2) & 0x38u) | ((denorm_rt.a << 6) & 0xC0u));
				RT.b = float((denorm_rt.a << 1) & 0xF8u);
				RT.a = float(denorm_rt.a & 0x80u);
			#else
				RT.r = float((denorm_rt.r << 3) & 0xF8u);
				RT.g = float(((denorm_rt.r >> 2) & 0x38u) | ((denorm_rt.g << 6) & 0xC0u));
				RT.b = float((denorm_rt.g << 1) & 0xF8u);
				RT.a = float(denorm_rt.g & 0x80u);
			#endif
		#endif

			#if PS_COLCLIP_HW == 1
			vec3 Cd = trunc(RT.rgb * 65535.0f);
			#else
			vec3 Cd = trunc(RT.rgb * 255.0f + 0.1f);
			#endif
			vec3 Cs = Color.rgb;

		#if PS_BLEND_A == 0
			vec3 A = Cs;
		#elif PS_BLEND_A == 1
			vec3 A = Cd;
		#else
			vec3 A = vec3(0.0f);
		#endif

		#if PS_BLEND_B == 0
			vec3 B = Cs;
		#elif PS_BLEND_B == 1
			vec3 B = Cd;
		#else
			vec3 B = vec3(0.0f);
		#endif

		#if PS_BLEND_C == 0
			float C = As;
		#elif PS_BLEND_C == 1
			float C = Ad;
		#else
			float C = Af;
		#endif

		#if PS_BLEND_D == 0
			vec3 D = Cs;
		#elif PS_BLEND_D == 1
			vec3 D = Cd;
		#else
			vec3 D = vec3(0.0f);
		#endif

		float C_clamped = C;
		#if PS_BLEND_MIX > 0 && PS_BLEND_HW != 1 && PS_BLEND_HW != 2
			C_clamped = min(C_clamped, 1.0f);
		#endif

		#if PS_BLEND_A == PS_BLEND_B
			Color.rgb = D;
		#elif PS_BLEND_MIX == 2
			Color.rgb = ((A - B) * C_clamped + D) + (124.0f/256.0f);
		#elif PS_BLEND_MIX == 1
			Color.rgb = ((A - B) * C_clamped + D) - (124.0f/256.0f);
		#else
			Color.rgb = trunc((A - B) * C + D);
		#endif

		#if PS_BLEND_HW == 1
			As_rgba.rgb = vec3(C);
			vec3 alpha_compensate = max(vec3(1.0f), Color.rgb / vec3(255.0f));
			As_rgba.rgb -= alpha_compensate;
		#elif PS_BLEND_HW == 2
			float division_alpha = 1.0f + C;
			Color.rgb /= vec3(division_alpha);
		#elif PS_BLEND_HW == 3
			As_rgba.rgb = vec3(C_clamped);
			vec3 overflow_check = (Color.rgb - vec3(255.0f)) / 255.0f;
			vec3 alpha_compensate = max(vec3(0.0f), overflow_check);
			As_rgba.rgb -= alpha_compensate;
		#endif

	#else

		#if PS_BLEND_C == 2
			vec3 Alpha = vec3(Af);
		#else
			vec3 Alpha = vec3(As);
		#endif

		#if PS_BLEND_HW == 1
			Color.rgb = vec3(255.0f);
		#elif PS_BLEND_HW == 2

			Color.rgb = max(vec3(0.0f), (Alpha - vec3(1.0f)));
			Color.rgb *= vec3(255.0f);
		#elif PS_BLEND_HW == 3 && PS_RTA_CORRECTION == 0
			float max_color = max(max(Color.r, Color.g), Color.b);
			float color_compensate = 255.0f / max(128.0f, max_color);
			Color.rgb *= vec3(color_compensate);
		#elif PS_BLEND_HW == 4

			As_rgba.rgb = Alpha * vec3(128.0f / 255.0f);
			Color.rgb = vec3(127.5f);
		#elif PS_BLEND_HW == 5
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = (Alpha - vec3(0.5f));
			Color.rgb = (Color.rgb * Alpha);
		#elif PS_BLEND_HW == 6
			Alpha *= vec3(128.0f / 255.0f);
			As_rgba.rgb = Alpha;
			Color.rgb *= (Alpha - vec3(0.5f));
		#endif
	#endif
}

#if PS_ROV_COLOR || PS_ROV_DEPTH
layout(pixel_interlock_ordered) in;
#endif

#if PS_ROV_EARLYDEPTHSTENCIL
layout(early_fragment_tests) in;
#endif

#if PS_ROV_COLOR || PS_ROV_DEPTH
	#define DISCARD { rov_discard_color = true; rov_discard_depth = true; }
	#define DISCARD_COLOR rov_discard_color = true
	#define DISCARD_DEPTH rov_discard_depth = true
#else
	#define DISCARD discard
	#define DISCARD_COLOR o_col0 = sample_from_rt()
	#define DISCARD_DEPTH input_z = sample_from_depth()
#endif

void main()
{
	float input_z = gl_FragCoord.z;

#if PS_ZFLOOR
	input_z = floor(input_z * exp2(32.0f)) * exp2(-32.0f);
#endif

#if PS_ROV_COLOR || PS_ROV_DEPTH
	beginInvocationInterlockARB();
#endif

#if PS_ROV_COLOR
	rov_rt_value = imageLoad(RtImageRov, ivec2(gl_FragCoord.xy));
#endif

#if PS_ROV_DEPTH
	rov_depth_value = imageLoad(DepthImageRov, ivec2(gl_FragCoord.xy)).r;
#endif

#if PS_ROV_COLOR || PS_ROV_DEPTH
	bool rov_discard_color = gl_HelperInvocation;
	bool rov_discard_depth = gl_HelperInvocation;
#endif

#if PS_ZTST == ZTST_GEQUAL
	if (input_z < sample_from_depth())
		DISCARD;
#elif PS_ZTST == ZTST_GREATER
	if (input_z <= sample_from_depth())
		DISCARD;
#endif

#if PS_SCANMSK & 2
	if ((int(gl_FragCoord.y) & 1) == (PS_SCANMSK & 1))
		DISCARD;
#endif
#if PS_DATE >= 5

#if PS_WRITE_RG == 1
	float rt_a = sample_from_rt().g;
#else
	float rt_a = sample_from_rt().a;
#endif

#if (PS_DATE & 3) == 1
	#if PS_RTA_CORRECTION
		bool bad = (254.5f / 255.0f) < rt_a;
	#else
		bool bad = (127.5f / 255.0f) < rt_a;
	#endif
#elif (PS_DATE & 3) == 2
	#if PS_RTA_CORRECTION
		bool bad = rt_a < (254.5f / 255.0f);
	#else
		bool bad = rt_a < (127.5f / 255.0f);
	#endif
#endif

	if (bad) {
		DISCARD;
	}

#endif

#if PS_DATE == 3
	int stencil_ceil = int(texelFetch(PrimMinTexture, ivec2(gl_FragCoord.xy), 0).r);

	if (gl_PrimitiveID > stencil_ceil) {
		DISCARD;
	}
#endif

	vec4 C = ps_color();

#if PS_AA1
	#if PS_AA1 == PS_AA1_LINE
		float cov = clamp(LineCovScale * (1.0f - abs(vsIn.inv_cov)), 0.0f, 1.0f);
	#else
		float cov = clamp(1.0f - abs(vsIn.inv_cov), 0.0f, 1.0f);
	#endif
	#if PS_ABE
		if (floor(C.a) == 128.0f)
			C.a = 128.0f * cov;
	#else
		C.a = 128.0f * cov;
	#endif
#elif PS_FIXED_ONE_A
	C.a = 128.0f;
#endif

	bool atst_pass = atst(C);

#if PS_ATST != PS_ATST_NONE && PS_AFAIL == AFAIL_KEEP
	if (!atst_pass) {
		DISCARD;
	}
#endif

#if SW_AD_TO_HW
	#if PS_RTA_CORRECTION
		vec4 RT = trunc(sample_from_rt() * 128.0f + 0.1f);
	#else
		vec4 RT = trunc(sample_from_rt() * 255.0f + 0.1f);
	#endif

	vec4 alpha_blend = vec4(RT.a / 128.0f);
#else
	vec4 alpha_blend = vec4(C.a / 128.0f);
#endif

#if (PS_DST_FMT == FMT_16)
	float A_one = 128.0f;
	C.a = (PS_FBA != 0) ? A_one : step(128.0f, C.a) * A_one;
#elif (PS_DST_FMT == FMT_32) && (PS_FBA != 0)
	if(C.a < 128.0f) C.a += 128.0f;
#endif

#if PS_DATE == 1

	o_col0 = (C.a > 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);

#elif PS_DATE == 2

	o_col0 = (C.a < 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);

#else
	ps_blend(C, alpha_blend);

	#if PS_SHUFFLE
		#if !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
			uvec4 denorm_c_after = uvec4(C);
			#if (PS_PROCESS_BA & SHUFFLE_READ)
				C.b = float(((denorm_c_after.r >> 3) & 0x1Fu) | ((denorm_c_after.g << 2) & 0xE0u));
				C.a = float(((denorm_c_after.g >> 6) & 0x3u) | ((denorm_c_after.b >> 1) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			#else
				C.r = float(((denorm_c_after.r >> 3) & 0x1Fu) | ((denorm_c_after.g << 2) & 0xE0u));
				C.g = float(((denorm_c_after.g >> 6) & 0x3u) | ((denorm_c_after.b >> 1) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			#endif
		#endif

		#if PS_SHUFFLE_SAME
			#if (PS_PROCESS_BA & SHUFFLE_READ)
				uvec4 denorm_c = uvec4(C);
				C = vec4(float((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
			#else
				C.ga = C.rg;
			#endif
		#elif PS_READ16_SRC
			uvec4 denorm_c = uvec4(C);
			uvec2 denorm_TA = uvec2(vec2(TA.xy) * 255.0f + 0.5f);
			C.rb = vec2(float((denorm_c.r >> 3) | (((denorm_c.g >> 3) & 0x7u) << 5)));
			C.ga = vec2(float((denorm_c.g >> 6) | ((denorm_c.b >> 3) << 2) | (denorm_TA.x & 0x80u)));
		#elif PS_SHUFFLE_ACROSS
			#if(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
				C.br = C.rb;
				C.ag = C.ga;
			#elif(PS_PROCESS_BA & SHUFFLE_READ)
				C.rb = C.bb;
				C.ga = C.aa;
			#else
				C.rb = C.rr;
				C.ga = C.gg;
			#endif
		#endif
	#endif

	ps_dither(C.rgb, alpha_blend.a);

	ps_color_clamp_wrap(C.rgb);

	ps_fbmask(C);

	#if (PS_AFAIL == AFAIL_RGB_ONLY_DSB) && !PS_NO_COLOR1
		alpha_blend.a = float(atst_pass);
	#endif

	#if !PS_NO_COLOR
		#if PS_RTA_CORRECTION
			o_col0.a = C.a / 128.0f;
		#else
			o_col0.a = C.a / 255.0f;
		#endif
		#if PS_COLCLIP_HW == 1
			o_col0.rgb = vec3(C.rgb / 65535.0f);
		#else
			o_col0.rgb = C.rgb / 255.0f;
		#endif
		#if !PS_NO_COLOR1
			o_col1 = alpha_blend;
		#endif

		#if PS_AFAIL == AFAIL_FB_ONLY
			if (!atst_pass)
				DISCARD_DEPTH;
		#elif PS_AFAIL == AFAIL_ZB_ONLY
			if (!atst_pass)
				DISCARD_COLOR;
		#elif (PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
			if (!atst_pass)
			{
				o_col0.a = sample_from_rt().a;
			#if PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
				DISCARD_DEPTH;
			#endif
			}
		#endif
	#endif

	#if PS_ZCLAMP
		input_z = min(input_z, MaxDepthPS);
	#endif
	
	#if PS_AA1 == PS_AA1_TRIANGLE_SW_Z
		if (!bool(vsIn.interior))
			DISCARD_DEPTH;
	#endif
	
	#if PS_RETURN_COLOR_ROV
		o_col0 = mix(o_col0, sample_from_rt(), equal(FbMask, uvec4(0xFFu)));

		if (!rov_discard_color)
			imageStore(RtImageRov, ivec2(gl_FragCoord.xy), o_col0);
	#endif
	
	#if PS_RETURN_DEPTH
		gl_FragDepth = input_z;
	#elif PS_RETURN_DEPTH_ROV
		if (!rov_discard_depth)
			imageStore(DepthImageRov, ivec2(gl_FragCoord.xy), vec4(input_z, 0, 0, 1.0f));
	#endif

	#if PS_ROV_COLOR || PS_ROV_DEPTH
		endInvocationInterlockARB();
	#endif
#endif
}

#endif
