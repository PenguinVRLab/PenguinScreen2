// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for text editor detection

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

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

#ifdef TEX_COORD_DEBUG
#define PS_TFX 1
#define PS_TCC 1
#endif

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define SW_BLEND_NEEDS_RT (SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1))
#define SW_AD_TO_HW (PS_BLEND_C == 1 && PS_A_MASKED)
#define PS_PRIMID_INIT (PS_DATE == 1 || PS_DATE == 2)
#define NEEDS_RT_EARLY (PS_TEX_IS_FB == 1 || PS_DATE >= 5)
#define NEEDS_RT_FOR_AFAIL (PS_AFAIL == AFAIL_ZB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define NEEDS_DEPTH_FOR_AFAIL (PS_AFAIL == AFAIL_FB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define NEEDS_DEPTH_FOR_ZTST (PS_ZTST == ZTST_GEQUAL || PS_ZTST == ZTST_GREATER)
#define NEEDS_DEPTH_FOR_AA1 (PS_AA1 == PS_AA1_TRIANGLE_SW_Z)

#define NEEDS_RT (NEEDS_RT_EARLY || NEEDS_RT_FOR_AFAIL || (!PS_PRIMID_INIT && (PS_FBMASK || SW_BLEND_NEEDS_RT || SW_AD_TO_HW)))
#define NEEDS_TEX (PS_TFX != 4)
#define SW_DEPTH (NEEDS_DEPTH_FOR_AFAIL || NEEDS_DEPTH_FOR_ZTST || NEEDS_DEPTH_FOR_AA1)
#define ZWRITE (SW_DEPTH || PS_ZCLAMP || PS_ZFLOOR)

layout(std140, binding = 0) uniform cb21
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

in SHADER
{
	vec4 t_float;
	vec4 t_int;

	#if PS_IIP != 0
		vec4 c;
	#else
		flat vec4 c;
	#endif

	float inv_cov;
	flat uint interior;
} PSin;

#define TARGET_0_QUALIFIER out

#if HAS_FRAMEBUFFER_FETCH && NEEDS_RT
	#undef PS_NO_COLOR
	#define PS_NO_COLOR 0
	#if defined(GL_EXT_shader_framebuffer_fetch)
		#undef TARGET_0_QUALIFIER
		#define TARGET_0_QUALIFIER inout
		#define LAST_FRAG_COLOR o_col0
	#elif defined(GL_ARM_shader_framebuffer_fetch)
		#define LAST_FRAG_COLOR gl_LastFragColorARM
	#endif
#endif

#if !PS_NO_COLOR && !PS_NO_COLOR1
	layout(location = 0, index = 0) TARGET_0_QUALIFIER vec4 o_col0;
	layout(location = 0, index = 1) out vec4 o_col1;
#elif !PS_NO_COLOR
	layout(location = 0) TARGET_0_QUALIFIER vec4 o_col0;
#endif

#if SW_DEPTH && PS_NO_COLOR1 && (DEPTH_FEEDBACK_SUPPORT == 2)
	#if HAS_FRAMEBUFFER_FETCH
		layout(location = 1) inout float o_col1;
	#else
		layout(location = 1) out float o_col1;
	#endif
#endif

#if NEEDS_TEX
layout(binding = 0) uniform sampler2D TextureSampler;
layout(binding = 1) uniform sampler2D PaletteSampler;
#endif

#if !HAS_FRAMEBUFFER_FETCH && NEEDS_RT
layout(binding = 2) uniform sampler2D RtSampler;
#endif

#if PS_DATE == 3
layout(binding = 3) uniform sampler2D img_prim_min;
#endif

#if (DEPTH_FEEDBACK_SUPPORT == 1 || (DEPTH_FEEDBACK_SUPPORT == 2 && !HAS_FRAMEBUFFER_FETCH)) && SW_DEPTH
layout(binding = 4) uniform sampler2D DepthSampler;
#endif

#if ZWRITE && PS_HAS_CONSERVATIVE_DEPTH && !SW_DEPTH
layout(depth_less) out float gl_FragDepth;
#endif

vec4 sample_from_rt()
{
#if !NEEDS_RT
	return vec4(0.0);
#elif HAS_FRAMEBUFFER_FETCH
	return LAST_FRAG_COLOR;
#else
	return texelFetch(RtSampler, ivec2(gl_FragCoord.xy), 0);
#endif
}

float sample_from_depth()
{
#if !SW_DEPTH
	return 0.0f;
#elif HAS_FRAMEBUFFER_FETCH && (DEPTH_FEEDBACK_SUPPORT == 2)
	return o_col1;
#else
	return texelFetch(DepthSampler, ivec2(gl_FragCoord.xy), 0).r;
#endif
}

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

	vec2 sz = textureSize(TextureSampler, 0);
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
		colour = textureLod(TextureSampler, uv, lod);
	else
	{
		vec4 num = vec4(0.0f, 0.0f, 0.0f, 0.0f);
		vec2 segment = (2.0f * aniso_line) / aniso_ratio;
		for (int i = 0; i < aniso_ratio; i++)
		{
			vec2 d = -aniso_line + (0.5f + i) * segment;	
			vec2 uv_sample = uv + d;
			vec4 sample_colour = textureLod(TextureSampler, uv_sample, lod);
			num += sample_colour;
		}

		colour = num / aniso_ratio;
	}
	return colour;
}
#endif

vec4 sample_c(vec2 uv)
{
#if PS_TEX_IS_FB == 1
	return sample_from_rt();
#elif PS_REGION_RECT
	return texelFetch(TextureSampler, ivec2(uv), 0);
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
	return sample_c_af(uv, PSin.t_float.w);
#elif PS_AUTOMATIC_LOD == 1
	return texture(TextureSampler, uv);
#elif PS_MANUAL_LOD == 1
	return textureLod(TextureSampler, uv, manual_lod(PSin.t_float.w));
#else
	return textureLod(TextureSampler, uv, 0.0f);
#endif

#endif
}

vec4 sample_p(uint idx)
{
	return texelFetch(PaletteSampler, ivec2(int(idx), 0), 0);
}

vec4 sample_p_norm(float u)
{
	return sample_p(uint(u * 255.5f));
}

vec4 clamp_wrap_uv(vec4 uv)
{
	vec4 uv_out = uv;
	vec4 tex_size = WH.xyxy;

#if PS_WMS == PS_WMT

#if PS_REGION_RECT == 1 && PS_WMS == 0
	uv_out = fract(uv);
#elif PS_REGION_RECT == 1 && PS_WMS == 1
	uv_out = clamp(uv, vec4(0.0f), vec4(1.0f));
#elif PS_WMS == 2
	uv_out = clamp(uv, MinMax.xyxy, MinMax.zwzw);
#elif PS_WMS == 3
	#if PS_FST == 0
	uv = fract(uv);
	#endif
	uv_out = vec4((uvec4(uv * tex_size) & floatBitsToUint(MinMax.xyxy)) | floatBitsToUint(MinMax.zwzw)) / tex_size;
#endif

#else

#if PS_REGION_RECT == 1 && PS_WMS == 0
	uv.xz = fract(uv.xz);

#elif PS_REGION_RECT == 1 && PS_WMS == 1
	uv.xz = clamp(uv.xz, vec2(0.0f), vec2(1.0f));

#elif PS_WMS == 2
	uv_out.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);

#elif PS_WMS == 3
	#if PS_FST == 0
		uv.xz = fract(uv.xz);
	#endif
	uv_out.xz = vec2((uvec2(uv.xz * tex_size.xx) & floatBitsToUint(MinMax.xx)) | floatBitsToUint(MinMax.zz)) / tex_size.xx;

#endif

#if PS_REGION_RECT == 1 && PS_WMT == 0
	uv_out.yw = fract(uv.yw);

#elif PS_REGION_RECT == 1 && PS_WMT == 1
	uv_out.yw = clamp(uv.yw, vec2(0.0f), vec2(1.0f));

#elif PS_WMT == 2
	uv_out.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);

#elif PS_WMT == 3
	#if PS_FST == 0
		uv.yw = fract(uv.yw);
	#endif
	uv_out.yw = vec2((uvec2(uv.yw * tex_size.yy) & floatBitsToUint(MinMax.yy)) | floatBitsToUint(MinMax.ww)) / tex_size.yy;
#endif

#endif

#if PS_REGION_RECT == 1
	uv_out = clamp(uv_out * WH.zwzw + STRange.xyxy, STRange.xyxy, STRange.zwzw);
#endif

	return uv_out;
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

uint fetch_raw_depth()
{
	float multiplier = exp2(32.0f);

#if PS_TEX_IS_FB == 1
	return uint(sample_from_rt().r * multiplier);
#else
	return uint(texelFetch(TextureSampler, ivec2(gl_FragCoord.xy + ChannelShuffleOffset), 0).r * multiplier);
#endif
}

vec4 fetch_raw_color()
{
#if PS_TEX_IS_FB == 1
	return sample_from_rt();
#else
	return texelFetch(TextureSampler, ivec2(gl_FragCoord.xy + ChannelShuffleOffset), 0);
#endif
}

vec4 fetch_c(ivec2 uv)
{
#if PS_TEX_IS_FB == 1
	return sample_from_rt();
#else
	return texelFetch(TextureSampler, ivec2(uv), 0);
#endif
}

ivec2 clamp_wrap_uv_depth(ivec2 uv)
{
	ivec2 uv_out = uv;

	ivec4 mask = floatBitsToInt(MinMax) << 4;

#if PS_WMS == PS_WMT

#if PS_WMS == 2
	uv_out = clamp(uv, mask.xy, mask.zw);
#elif PS_WMS == 3
	uv_out = (uv & mask.xy) | mask.zw;
#endif

#else

#if PS_WMS == 2
	uv_out.x = clamp(uv.x, mask.x, mask.z);
#elif PS_WMS == 3
	uv_out.x = (uv.x & mask.x) | mask.z;
#endif

#if PS_WMT == 2
	uv_out.y = clamp(uv.y, mask.y, mask.w);
#elif PS_WMT == 3
	uv_out.y = (uv.y & mask.y) | mask.w;
#endif

#endif

	return uv_out;
}

vec4 sample_depth(vec2 st)
{
	vec2 uv_f = vec2(clamp_wrap_uv_depth(ivec2(st))) * vec2(ScaledScaleFactor);

	#if PS_REGION_RECT == 1
		uv_f = clamp(uv_f + STRange.xy, STRange.xy, STRange.zw);
	#endif

	ivec2 uv = ivec2(uv_f);
	vec4 t = vec4(0.0f);

#if PS_TALES_OF_ABYSS_HLE == 1
	uint depth = fetch_raw_depth();

	t = texelFetch(PaletteSampler, ivec2((depth >> 8u) & 0xFFu, 0), 0) * 255.0f;

#elif PS_URBAN_CHAOS_HLE == 1

	uint depth = fetch_raw_depth();

	t = texelFetch(PaletteSampler, ivec2((depth & 0xFFu), 0), 0) * 255.0f;

	float green = float((depth >> 8u) & 0xFFu) * 36.0f;
	green = min(green, 255.0f);

	t.g += green;


#elif PS_DEPTH_FMT == 1
	uint d = uint(fetch_c(uv).r * exp2(32.0f));
	t = vec4(uvec4((d & 0xFFu), ((d >> 8) & 0xFFu), ((d >> 16) & 0xFFu), (d >> 24)));

#elif PS_DEPTH_FMT == 2
	uint d = uint(fetch_c(uv).r * exp2(32.0f));
	t = vec4(uvec4((d & 0x1Fu), ((d >> 5) & 0x1Fu), ((d >> 10) & 0x1Fu), (d >> 15) & 0x01u)) * vec4(8.0f, 8.0f, 8.0f, 128.0f);

#elif PS_DEPTH_FMT == 3
	t = fetch_c(uv) * 255.0f;

#endif

#if (PS_AEM_FMT == FMT_24)
	t.a = ( (PS_AEM == 0) || any(bvec3(t.rgb))  ) ? 255.0f * TA.x : 0.0f;
#elif (PS_AEM_FMT == FMT_16)
	t.a = t.a >= 128.0f ? 255.0f * TA.y : ( (PS_AEM == 0) || any(bvec3(t.rgb)) ) ? 255.0f * TA.x : 0.0f;
#elif PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE
	t = trunc(sample_4p(uvec4(t.aaaa))[0] * 255.0f + 0.05f);
#endif

	return t;
}

vec4 fetch_red()
{
#if PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2
	uint depth = (fetch_raw_depth()) & 0xFFu;
	vec4 rt = vec4(depth) / 255.0f;
#else
	vec4 rt = fetch_raw_color();
#endif
	return sample_p_norm(rt.r) * 255.0f;
}

vec4 fetch_green()
{
#if PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2
	uint depth = (fetch_raw_depth() >> 8u) & 0xFFu;
	vec4 rt = vec4(depth) / 255.0f;
#else
	vec4 rt = fetch_raw_color();
#endif
	return sample_p_norm(rt.g) * 255.0f;
}

vec4 fetch_blue()
{
#if PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2
	uint depth = (fetch_raw_depth() >> 16u) & 0xFFu;
	vec4 rt = vec4(depth) / 255.0f;
#else
	vec4 rt = fetch_raw_color();
#endif
	return sample_p_norm(rt.b) * 255.0f;
}

vec4 fetch_alpha()
{
	vec4 rt = fetch_raw_color();
	return sample_p_norm(rt.a) * 255.0f;
}

vec4 fetch_rgb()
{
	vec4 rt = fetch_raw_color();
	vec4 c = vec4(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0f);
	return c * 255.0f;
}

vec4 fetch_gXbY()
{
#if PS_DEPTH_FMT == 1 || PS_DEPTH_FMT == 2
	uint depth = fetch_raw_depth();
	uint bg = (depth >> (8u + uint(ChannelShuffle.w))) & 0xFFu;
	return vec4(bg);
#else
	ivec4 rt = ivec4(fetch_raw_color() * 255.0f);
	int green = (rt.g >> ChannelShuffle.w) & ChannelShuffle.z;
	int blue  = (rt.b << ChannelShuffle.y) & ChannelShuffle.x;
	return vec4(green | blue);
#endif
}

vec4 sample_color(vec2 st)
{
#if (PS_TCOFFSETHACK == 1)
	st += TC_OffsetHack.xy;
#endif

	vec4 t;
	mat4 c;
	vec2 dd;

#if (PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_REGION_RECT == 0 && PS_WMS < 2 && PS_WMT < 2)
	c[0] = sample_c(st);
#ifdef TEX_COORD_DEBUG
	c[0].rg = st.xy;
#endif

#else
	vec4 uv;

	if(PS_LTF != 0)
	{
		uv = st.xyxy + HalfTexel;
		dd = fract(uv.xy * WH.zw);
#if (PS_FST == 0)
		dd = clamp(dd, vec2(0.0f), vec2(1.0f));
#endif
	}
	else
	{
		uv = st.xyxy;
	}

	uv = clamp_wrap_uv(uv);

#if PS_PAL_FMT != 0
	c = sample_4p(sample_4_index(uv));
#else
	c = sample_4c(uv);
#endif

#ifdef TEX_COORD_DEBUG
	c[0].rg = uv.xy;
	c[1].rg = uv.xy;
	c[2].rg = uv.xy;
	c[3].rg = uv.xy;
#endif

#endif

	for (int i = 0; i < 4; i++)
	{
#if (PS_AEM_FMT == FMT_24)
		c[i].a = ( (PS_AEM == 0) || any(bvec3(c[i].rgb))  ) ? TA.x : 0.0f;
#elif (PS_AEM_FMT == FMT_16)
		c[i].a = c[i].a >= 0.5 ? TA.y : ( (PS_AEM == 0) || any(bvec3(ivec3(c[i].rgb * 255.0f) & ivec3(0xF8))) ) ? TA.x : 0.0f;
#endif
	}

#if(PS_LTF != 0)
	t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
#else
	t = c[0];
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

void fog(inout vec4 C, float f)
{
#if PS_FOG != 0
	C.rgb = trunc(mix(FogColor, C.rgb, (f * 255.0f) / 256.0f));
#endif
}

vec4 ps_color()
{
#if (PS_FST == 0)
	vec2 st = PSin.t_float.xy / vec2(PSin.t_float.w);
	vec2 st_int = PSin.t_int.zw / vec2(PSin.t_float.w);
#else
	vec2 st = PSin.t_int.xy;
	vec2 st_int = PSin.t_int.zw;
#endif

#if !NEEDS_TEX
	vec4 T = vec4(0.0);
#elif PS_CHANNEL_FETCH == 1
	vec4 T = fetch_red();
#elif PS_CHANNEL_FETCH == 2
	vec4 T = fetch_green();
#elif PS_CHANNEL_FETCH == 3
	vec4 T = fetch_blue();
#elif PS_CHANNEL_FETCH == 4
	vec4 T = fetch_alpha();
#elif PS_CHANNEL_FETCH == 5
	vec4 T = fetch_rgb();
#elif PS_CHANNEL_FETCH == 6
	vec4 T = fetch_gXbY();
#elif PS_DEPTH_FMT > 0
	vec4 T = sample_depth(st_int);
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

	vec4 C = tfx(T, PSin.c);

	fog(C, PSin.t_float.z);

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
	#if PS_DITHER == 2
		ivec2 fpos = ivec2(gl_FragCoord.xy);
	#else
		ivec2 fpos = ivec2(gl_FragCoord.xy * RcpScaleFactor);
	#endif
		float value = DitherMatrix[fpos.y&3][fpos.x&3];

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

#if PS_DST_FMT == FMT_16 && PS_DITHER < 3 && (PS_BLEND_MIX == 0 || PS_DITHER)
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

#if SW_BLEND_NEEDS_RT
	vec4 RT = sample_from_rt();
#else
	vec4 RT = vec4(0.0f);
#endif

	#if PS_RTA_CORRECTION
		float Ad = trunc(RT.a * 128.0f + 0.1f) / 128.0f;
	#else
		float Ad = trunc(RT.a * 255.0f + 0.1f) / 128.0f;
	#endif

	#if PS_SHUFFLE && SW_BLEND_NEEDS_RT
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

void ps_main()
{
	float input_z = gl_FragCoord.z;

#if PS_ZFLOOR
	input_z = floor(input_z * exp2(32.0f)) * exp2(-32.0f);
#endif

#if PS_ZTST == ZTST_GEQUAL
	if (input_z < sample_from_depth())
		discard;
#elif PS_ZTST == ZTST_GREATER
	if (input_z <= sample_from_depth())
		discard;
#endif

#if PS_SCANMSK & 2
	if ((int(gl_FragCoord.y) & 1) == (PS_SCANMSK & 1))
		discard;
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
		discard;
	}

#endif

#if PS_DATE == 3
	int stencil_ceil = int(texelFetch(img_prim_min, ivec2(gl_FragCoord.xy), 0).r);

	if (gl_PrimitiveID > stencil_ceil) {
		discard;
	}
#endif

	vec4 C = ps_color();

#if PS_AA1
	#if PS_AA1 == PS_AA1_LINE
		float cov = clamp(LineCovScale * (1.0f - abs(PSin.inv_cov)), 0.0f, 1.0f);
	#else
		float cov = clamp(1.0f - abs(PSin.inv_cov), 0.0f, 1.0f);
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
	if (!atst_pass)
		discard;
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
	return;
#elif PS_DATE == 2
	o_col0 = (C.a < 127.5f) ? vec4(gl_PrimitiveID) : vec4(0x7FFFFFFF);
	return;
#endif

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
		uvec4 denorm_c = uvec4(C);
	#if (PS_PROCESS_BA & SHUFFLE_READ)
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
		C.a = C.a / 128.0f;
	#else
		C.a = C.a / 255.0f;
	#endif
	#if PS_COLCLIP_HW == 1
		C.rgb = vec3(C.rgb / 65535.0f);
	#else
		C.rgb = C.rgb / 255.0f;
	#endif

	#if PS_AFAIL == AFAIL_FB_ONLY
		if (!atst_pass)
			input_z = sample_from_depth();
	#elif PS_AFAIL == AFAIL_ZB_ONLY
		if (!atst_pass)
			C = sample_from_rt();
	#elif (PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
		if (!atst_pass)
		{
			C.a = sample_from_rt().a;
		#if PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
			input_z = sample_from_depth();
		#endif
		}
	#endif

	o_col0 = C;

	#if !PS_NO_COLOR1
		o_col1 = alpha_blend;
	#endif
#endif

#if PS_ZCLAMP
	input_z = min(input_z, MaxDepthPS);
#endif

#if PS_AA1 == PS_AA1_TRIANGLE_SW_Z
	if (!bool(PSin.interior))
		input_z = sample_from_depth();
#endif

#if ZWRITE
	#if SW_DEPTH && PS_NO_COLOR1 && (DEPTH_FEEDBACK_SUPPORT == 2)
		o_col1 = input_z;
	#endif
	gl_FragDepth = input_z;
#endif
}
