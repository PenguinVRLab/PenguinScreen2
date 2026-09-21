// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

#ifndef VS_TME
#define VS_IIP 0
#define VS_TME 1
#define VS_FST 1
#endif

#ifndef GS_IIP
#define GS_IIP 0
#define GS_PRIM 3
#define GS_FORWARD_PRIMID 0
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
#define PS_IIP 0
#define PS_FST 0
#define PS_WMS 0
#define PS_WMT 0
#define PS_ADJS 0
#define PS_ADJT 0
#define PS_AEM_FMT FMT_32
#define PS_AEM 0
#define PS_TFX 0
#define PS_TCC 1
#define PS_ATST 1
#define PS_FOG 0
#define PS_IIP 0
#define PS_BLEND_HW 0
#define PS_A_MASKED 0
#define PS_FBA 0
#define PS_FBMASK 0
#define PS_LTF 1
#define PS_TCOFFSETHACK 0
#define PS_POINT_SAMPLER 0
#define PS_REGION_RECT 0
#define PS_SHUFFLE 0
#define PS_SHUFFLE_SAME 0
#define PS_PROCESS_BA 0
#define PS_PROCESS_RG 0
#define PS_SHUFFLE_ACROSS 0
#define PS_READ16_SRC 0
#define PS_WRITE_RG 0
#define PS_DST_FMT 0
#define PS_DEPTH_FMT 0
#define PS_PAL_FMT 0
#define PS_CHANNEL_FETCH 0
#define PS_TALES_OF_ABYSS_HLE 0
#define PS_URBAN_CHAOS_HLE 0
#define PS_COLCLIP_HW 0
#define PS_RTA_CORRECTION 0
#define PS_RTA_SRC_CORRECTION 0
#define PS_COLCLIP 0
#define PS_BLEND_A 0
#define PS_BLEND_B 0
#define PS_BLEND_C 0
#define PS_BLEND_D 0
#define PS_BLEND_MIX 0
#define PS_ROUND_INV 0
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
#define PS_AA1 0
#define PS_ABE 0
#define PS_ROV_COLOR 0
#define PS_ROV_DEPTH 0
#endif

#ifndef VS_EXPAND_NONE
#define VS_EXPAND_NONE 0
#define VS_EXPAND_POINT 1
#define VS_EXPAND_LINE 2
#define VS_EXPAND_SPRITE 3
#define VS_EXPAND_LINE_AA1 4
#define VS_EXPAND_TRIANGLE_AA1 5
#endif

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define SW_BLEND_NEEDS_RT (SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1))
#define SW_AD_TO_HW (PS_BLEND_C == 1 && PS_A_MASKED)
#define NEEDS_RT_FOR_AFAIL (PS_AFAIL == AFAIL_ZB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define NEEDS_DEPTH_FOR_AFAIL (PS_AFAIL == AFAIL_FB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define NEEDS_DEPTH_FOR_ZTST (PS_ZTST == ZTST_GEQUAL || PS_ZTST == ZTST_GREATER)
#define NEEDS_DEPTH_FOR_AA1 (PS_AA1 == PS_AA1_TRIANGLE_SW_Z)
#define SW_DEPTH (NEEDS_DEPTH_FOR_AFAIL || NEEDS_DEPTH_FOR_ZTST || NEEDS_DEPTH_FOR_AA1)
#define ZWRITE (PS_ZFLOOR || PS_ZCLAMP || SW_DEPTH)

#define PS_RETURN_COLOR_ROV (!PS_NO_COLOR && PS_ROV_COLOR)
#define PS_RETURN_COLOR (!PS_NO_COLOR && !PS_ROV_COLOR)
#define PS_RETURN_DEPTH_ROV (PS_ROV_DEPTH == PS_ROV_DEPTH_READ_WRITE)
#define PS_RETURN_DEPTH (ZWRITE && !PS_ROV_DEPTH)
#define PS_ROV_EARLYDEPTHSTENCIL (PS_ROV_COLOR && !PS_ROV_DEPTH && !ZWRITE)

struct VS_INPUT
{
	float2 st : TEXCOORD0;
	uint4 c : COLOR0;
	float q : TEXCOORD1;
	uint2 p : POSITION0;
	uint z : POSITION1;
	uint2 uv : TEXCOORD2;
	float4 f : COLOR1;
};

struct VS_OUTPUT
{
	float4 p : SV_Position;
	float4 t : TEXCOORD0;
	float4 ti : TEXCOORD2;

#if VS_IIP != 0 || GS_IIP != 0 || PS_IIP != 0
	float4 c : COLOR0;
#else
	nointerpolation float4 c : COLOR0;
#endif

	float inv_cov : COLOR1;
	nointerpolation uint interior : COLOR2;
};

struct PS_INPUT
{
	noperspective centroid float4 p : SV_Position;
	float4 t : TEXCOORD0;
	float4 ti : TEXCOORD2;
#if VS_IIP != 0 || GS_IIP != 0 || PS_IIP != 0
	float4 c : COLOR0;
#else
	nointerpolation float4 c : COLOR0;
#endif
	float inv_cov : COLOR1;
	nointerpolation uint interior : COLOR2;
#if (PS_DATE >= 1 && PS_DATE <= 3) || GS_FORWARD_PRIMID
	uint primid : SV_PrimitiveID;
#endif
};

#ifdef PIXEL_SHADER

struct PS_OUTPUT
{
#define NUM_RTS 0

#if PS_RETURN_COLOR
	#if PS_DATE == 1 || PS_DATE == 2
		float c0 : SV_Target;
	#else
		
		float4 c0 : SV_Target0;

		#undef NUM_RTS
		#define NUM_RTS 1
		
		#if !PS_NO_COLOR1
			float4 c1 : SV_Target1;
		#endif
	#endif
#endif

#if PS_RETURN_DEPTH
	#if SW_DEPTH && PS_NO_COLOR1 && PS_DEPTH_FEEDBACK_SUPPORT == 2
		#if NUM_RTS > 0
			float depth_color : SV_Target1;
		#else
			float depth_color : SV_Target0;
		#endif
	#endif
	#if PS_HAS_CONSERVATIVE_DEPTH && !SW_DEPTH
		float depth : SV_DepthLessEqual;
	#else
		float depth : SV_Depth;
	#endif
#endif

#undef NUM_RTS
};

Texture2D<float4> Texture : register(t0);
Texture2D<float4> Palette : register(t1);
#if !PS_ROV_COLOR
Texture2D<float4> RtTexture : register(t2);
#endif
Texture2D<float> PrimMinTexture : register(t3);
#if !PS_ROV_DEPTH
Texture2D<float> DepthTexture : register(t4);
#endif
SamplerState TextureSampler : register(s0);

#if PS_ROV_COLOR
RasterizerOrderedTexture2D<unorm float4> RtTextureRov : register(u0);
static float4 rov_rt_value;
#endif

#if PS_ROV_DEPTH
RasterizerOrderedTexture2D<float> DepthTextureRov : register(u1);
static float rov_depth_value;
#endif

#ifdef DX12
cbuffer cb1 : register(b1)
#else
cbuffer cb1
#endif
{
	float3 FogColor;
	float AREF;
	float4 WH;
	float2 TA;
	float MaxDepthPS;
	float Af;
	uint4 FbMask;
	float4 HalfTexel;
	float4 MinMax;
	float4 LODParams;
	float4 STRange;
	int4 ChannelShuffle;
	float2 ChannelShuffleOffset;
	float2 TC_OffsetHack;
	float2 STScale;
	float4x4 DitherMatrix;
	float ScaledScaleFactor;
	float RcpScaleFactor;
	float _pad0_cb1;
	float _pad1_cb1;
	float LineCovScale;
	float _pad2_cb1;
	float _pad3_cb1;
	float _pad4_cb1;
};

float4 RtLoad(int2 xy)
{
#if PS_ROV_COLOR
	return rov_rt_value;
#else
	return RtTexture.Load(int3(int2(xy), 0));
#endif
}

float DepthLoad(int2 xy)
{
#if PS_ROV_DEPTH
	return rov_depth_value;
#else
	return DepthTexture.Load(int3(int2(xy), 0));
#endif
}

void RtWrite(int2 xy, float4 c)
{
#if PS_ROV_COLOR
	RtTextureRov[xy] = c;
#endif
}

void DepthWrite(int2 xy, float d)
{
#if PS_ROV_DEPTH
	DepthTextureRov[xy] = d;
#endif
}

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
bool2 nan_or_inf(float2 xy)
{
#ifdef __hlsl_dx_compiler
	return isinf(xy) | isnan(xy);
#else
	return (asuint(xy) & 0x7f800000) == 0x7f800000;
#endif
}

float4 sample_c_af(float2 uv, float uv_w)
{
	uv = any(nan_or_inf(uv)) ? float2(0.0f, 0.0f) : uv;

	uv = clamp(uv, -8388608.0f, 8388608.0f);

	float2 sz;
	Texture.GetDimensions(sz.x, sz.y);
	float2 dX = ddx(uv) * sz;
	float2 dY = ddy(uv) * sz;

	float length_x = length(dX);
	float length_y = length(dY);

	bool d_zero = length_x < 0.001f || length_y < 0.001f;
	float f = (dX.x * dY.y - dX.y * dY.x);
	bool d_par = f < 0.001f;
	bool d_per = dot(dX, dY) < 0.001f;
	bool d_inf_nan = any(nan_or_inf(dX) | nan_or_inf(dY));

	if (!(d_zero || d_par || d_per || d_inf_nan))
	{
		float A = dX.y * dX.y + dY.y * dY.y;
		float B = -2 * (dX.x * dX.y + dY.x * dY.y);
		float C = dX.x * dX.x + dY.x * dY.x;
		float F = f * f;

		float p = A - C;
		float q = A + C;
		float t = sqrt(p * p + B * B);

		float sqrt_num_plus  = sqrt(F * (t + p));
		float sqrt_num_minus = sqrt(F * (t - p));

		float inv_sqrt_denom_plus  = rsqrt(t * (q + t));
		float inv_sqrt_denom_minus = rsqrt(t * (q - t));

		float signB = sign(B);

		float2 new_dX = float2(
			sqrt_num_plus  * inv_sqrt_denom_plus,
			sqrt_num_minus * inv_sqrt_denom_plus * signB
		);

		float2 new_dY = float2(
			sqrt_num_minus * inv_sqrt_denom_minus * -signB,
			sqrt_num_plus  * inv_sqrt_denom_minus
		);

		d_inf_nan = any(nan_or_inf(new_dX) | nan_or_inf(new_dY));
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
	float2 aniso_line;

	if (length_major <= 1.0f)
	{
		aniso_ratio = 1.0f;
		length_lod = length_major;
		aniso_line = float2(0.0f, 0.0f);
	}
	else
	{
		float2 aniso_line_dir = is_major_x ? dX : dY;

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

	float4 colour;
	if (aniso_ratio == 1.0f)
		colour = Texture.SampleLevel(TextureSampler, uv, lod);
	else
	{
		float4 num = float4(0.0f, 0.0f, 0.0f, 0.0f);
		float2 segment = (2.0f * aniso_line) / aniso_ratio;

		int aniso_ratio_i = (int)aniso_ratio;
		for (int i = 0; i < aniso_ratio_i; i++)
		{
			float2 d = -aniso_line + (0.5f + i) * segment;	
			float2 uv_sample = uv + d;
			float4 sample_colour = Texture.SampleLevel(TextureSampler, uv_sample, lod);
			num += sample_colour;
		}

		colour = num / aniso_ratio;
	}
	return colour;
}
#endif

float4 sample_c(float2 uv, float uv_w, int2 xy)
{
#if PS_TEX_IS_FB == 1
	return RtLoad(xy);
#elif PS_REGION_RECT == 1
	return Texture.Load(int3(int2(uv), 0));
#else
	if (PS_POINT_SAMPLER)
	{
		uv = (trunc(uv * WH.zw) + float2(0.5, 0.5)) / WH.zw;
	}
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
	return sample_c_af(uv, uv_w);
#elif PS_AUTOMATIC_LOD == 1
	return Texture.Sample(TextureSampler, uv);
#elif PS_MANUAL_LOD == 1
	return Texture.SampleLevel(TextureSampler, uv, manual_lod(uv_w));
#else
	return Texture.SampleLevel(TextureSampler, uv, 0);
#endif
#endif
}

float4 sample_p(uint u)
{
	return Palette.Load(int3(int(u), 0, 0));
}

float4 sample_p_norm(float u)
{
	return sample_p(uint(u * 255.5f));
}

float4 clamp_wrap_uv(float4 uv)
{
	float4 tex_size = WH.xyxy;

	if(PS_WMS == PS_WMT)
	{
		if(PS_REGION_RECT != 0 && PS_WMS == 0)
		{
			uv = frac(uv);
		}
		else if(PS_REGION_RECT != 0 && PS_WMS == 1)
		{
			uv = saturate(uv);
		}
		else if(PS_WMS == 2)
		{
			uv = clamp(uv, MinMax.xyxy, MinMax.zwzw);
		}
		else if(PS_WMS == 3)
		{
			#if PS_FST == 0
			uv = frac(uv);
			#endif
			uv = (float4)(((uint4)(uv * tex_size) & asuint(MinMax.xyxy)) | asuint(MinMax.zwzw)) / tex_size;
		}
	}
	else
	{
		if(PS_REGION_RECT != 0 && PS_WMS == 0)
		{
			uv.xz = frac(uv.xz);
		}
		else if(PS_REGION_RECT != 0 && PS_WMS == 1)
		{
			uv.xz = saturate(uv.xz);
		}
		else if(PS_WMS == 2)
		{
			uv.xz = clamp(uv.xz, MinMax.xx, MinMax.zz);
		}
		else if(PS_WMS == 3)
		{
			#if PS_FST == 0
			uv.xz = frac(uv.xz);
			#endif
			uv.xz = (float2)(((uint2)(uv.xz * tex_size.xx) & asuint(MinMax.xx)) | asuint(MinMax.zz)) / tex_size.xx;
		}
		if(PS_REGION_RECT != 0 && PS_WMT == 0)
		{
			uv.yw = frac(uv.yw);
		}
		else if(PS_REGION_RECT != 0 && PS_WMT == 1)
		{
			uv.yw = saturate(uv.yw);
		}
		else if(PS_WMT == 2)
		{
			uv.yw = clamp(uv.yw, MinMax.yy, MinMax.ww);
		}
		else if(PS_WMT == 3)
		{
			#if PS_FST == 0
			uv.yw = frac(uv.yw);
			#endif
			uv.yw = (float2)(((uint2)(uv.yw * tex_size.yy) & asuint(MinMax.yy)) | asuint(MinMax.ww)) / tex_size.yy;
		}
	}

	if(PS_REGION_RECT != 0)
	{
		uv = clamp(uv * WH.zwzw + STRange.xyxy, STRange.xyxy, STRange.zwzw);
	}

	return uv;
}

float4x4 sample_4c(float4 uv, float uv_w, int2 xy)
{
	float4x4 c;

	c[0] = sample_c(uv.xy, uv_w, xy);
	c[1] = sample_c(uv.zy, uv_w, xy);
	c[2] = sample_c(uv.xw, uv_w, xy);
	c[3] = sample_c(uv.zw, uv_w, xy);

	return c;
}

uint4 sample_4_index(float4 uv, float uv_w, int2 xy)
{
	float4 c;

	c.x = sample_c(uv.xy, uv_w, xy).a;
	c.y = sample_c(uv.zy, uv_w, xy).a;
	c.z = sample_c(uv.xw, uv_w, xy).a;
	c.w = sample_c(uv.zw, uv_w, xy).a;

	uint4 i;
		
	if (PS_RTA_SRC_CORRECTION)
	{
		i = uint4(round(c * 128.25f));
	}
	else
	{
		i = uint4(c * 255.5f);
	}

	if (PS_PAL_FMT == 1)
	{
		return i & 0xFu;
	}
	else if (PS_PAL_FMT == 2)
	{
		return i >> 4u;
	}
	else
	{
		return i;
	}
}

float4x4 sample_4p(uint4 u)
{
	float4x4 c;

	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);

	return c;
}

uint fetch_raw_depth(int2 xy)
{
#if PS_TEX_IS_FB == 1
	float4 col = RtLoad(xy);
#else
	float4 col = Texture.Load(int3(xy, 0));
#endif
	return (uint)(col.r * exp2(32.0f));
}

float4 fetch_raw_color(int2 xy)
{
#if PS_TEX_IS_FB == 1
	return RtLoad(xy);
#else
	return Texture.Load(int3(xy, 0));
#endif
}

float4 fetch_c(int2 uv)
{
#if PS_TEX_IS_FB == 1
	return RtLoad(uv);
#else
	return Texture.Load(int3(uv, 0));
#endif
}

int2 clamp_wrap_uv_depth(int2 uv)
{
	int4 mask = asint(MinMax) << 4;
	if (PS_WMS == PS_WMT)
	{
		if (PS_WMS == 2)
		{
			uv = clamp(uv, mask.xy, mask.zw);
		}
		else if (PS_WMS == 3)
		{
			uv = (uv & mask.xy) | mask.zw;
		}
	}
	else
	{
		if (PS_WMS == 2)
		{
			uv.x = clamp(uv.x, mask.x, mask.z);
		}
		else if (PS_WMS == 3)
		{
			uv.x = (uv.x & mask.x) | mask.z;
		}
		if (PS_WMT == 2)
		{
			uv.y = clamp(uv.y, mask.y, mask.w);
		}
		else if (PS_WMT == 3)
		{
			uv.y = (uv.y & mask.y) | mask.w;
		}
	}
	return uv;
}

float4 sample_depth(float2 st, float2 pos)
{
	float2 uv_f = (float2)clamp_wrap_uv_depth(int2(st)) * (float2)ScaledScaleFactor;

#if PS_REGION_RECT == 1
	uv_f = clamp(uv_f + STRange.xy, STRange.xy, STRange.zw);
#endif

	int2 uv = (int2)uv_f;
	float4 t = (float4)(0.0f);

	if (PS_TALES_OF_ABYSS_HLE == 1)
	{
		uint depth = fetch_raw_depth(pos);

		t = Palette.Load(int3((depth >> 8u) & 0xFFu, 0, 0)) * 255.0f;
	}
	else if (PS_URBAN_CHAOS_HLE == 1)
	{

		uint depth = fetch_raw_depth(pos);

		t = Palette.Load(int3(depth & 0xFFu, 0, 0)) * 255.0f;

		float green = (float)((depth >> 8u) & 0xFFu) * 36.0f;
		green = min(green, 255.0f);
		t.g += green;
	}
	else if (PS_DEPTH_FMT == 1)
	{

		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = float4(uint4((d & 0xFFu), ((d >> 8) & 0xFFu), ((d >> 16) & 0xFFu), (d >> 24)));
	}
	else if (PS_DEPTH_FMT == 2)
	{

		uint d = uint(fetch_c(uv).r * exp2(32.0f));
		t = float4(uint4((d & 0x1Fu), ((d >> 5) & 0x1Fu), ((d >> 10) & 0x1Fu), (d >> 15) & 0x01u)) * float4(8.0f, 8.0f, 8.0f, 128.0f);
	}
	else if (PS_DEPTH_FMT == 3)
	{
		t = fetch_c(uv) * 255.0f;
	}

	if (PS_AEM_FMT == FMT_24)
	{
		t.a = ((PS_AEM == 0) || any(bool3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	else if (PS_AEM_FMT == FMT_16)
	{
		t.a = t.a >= 128.0f ? 255.0f * TA.y : ((PS_AEM == 0) || any(bool3(t.rgb))) ? 255.0f * TA.x : 0.0f;
	}
	else if (PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE)
	{
		t = trunc(sample_4p(uint4(t.aaaa))[0] * 255.0f + 0.05f);
	}

	return t;
}

float4 fetch_red(int2 xy)
{
	float4 rt;

	if ((PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2))
	{
		uint depth = (fetch_raw_depth(xy)) & 0xFFu;
		rt = (float4)(depth) / 255.0f;
	}
	else
	{
		rt = fetch_raw_color(xy);
	}

	return sample_p_norm(rt.r) * 255.0f;
}

float4 fetch_green(int2 xy)
{
	float4 rt;

	if ((PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2))
	{
		uint depth = (fetch_raw_depth(xy) >> 8u) & 0xFFu;
		rt = (float4)(depth) / 255.0f;
	}
	else
	{
		rt = fetch_raw_color(xy);
	}

	return sample_p_norm(rt.g) * 255.0f;
}

float4 fetch_blue(int2 xy)
{
	float4 rt;

	if ((PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2))
	{
		uint depth = (fetch_raw_depth(xy) >> 16u) & 0xFFu;
		rt = (float4)(depth) / 255.0f;
	}
	else
	{
		rt = fetch_raw_color(xy);
	}

	return sample_p_norm(rt.b) * 255.0f;
}

float4 fetch_alpha(int2 xy)
{
	float4 rt = fetch_raw_color(xy);
	return sample_p_norm(rt.a) * 255.0f;
}

float4 fetch_rgb(int2 xy)
{
	float4 rt = fetch_raw_color(xy);
	float4 c = float4(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0);
	return c * 255.0f;
}

float4 fetch_gXbY(int2 xy)
{
	if ((PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2))
	{
		uint depth = fetch_raw_depth(xy);
		uint bg = (depth >> (8u + uint(ChannelShuffle.w))) & 0xFFu;
		return (float4)(bg);
	}
	else
	{
		int4 rt = (int4)(fetch_raw_color(xy) * 255.0);
		int green = (rt.g >> ChannelShuffle.w) & ChannelShuffle.z;
		int blue = (rt.b << ChannelShuffle.y) & ChannelShuffle.x;
		return (float4)(green | blue);
	}
}

float4 sample_color(float2 st, float uv_w, int2 xy)
{
	#if PS_TCOFFSETHACK
	st += TC_OffsetHack.xy;
	#endif

	float4 t;
	float4x4 c;
	float2 dd;

	if (PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_REGION_RECT == 0 && PS_WMS < 2 && PS_WMT < 2)
	{
		c[0] = sample_c(st, uv_w, xy);
	}
	else
	{
		float4 uv;

		if(PS_LTF)
		{
			uv = st.xyxy + HalfTexel;
			dd = frac(uv.xy * WH.zw);

			if(PS_FST == 0)
			{
				dd = clamp(dd, (float2)0.0f, (float2)0.9999999f);
			}
		}
		else
		{
			uv = st.xyxy;
		}

		uv = clamp_wrap_uv(uv);

#if PS_PAL_FMT != 0
			c = sample_4p(sample_4_index(uv, uv_w, xy));
#else
			c = sample_4c(uv, uv_w, xy);
#endif
	}

	[unroll]
	for (uint i = 0; i < 4; i++)
	{
		if(PS_AEM_FMT == FMT_24)
		{
			c[i].a = !PS_AEM || any(c[i].rgb) ? TA.x : 0;
		}
		else if(PS_AEM_FMT == FMT_16)
		{
			c[i].a = c[i].a >= 0.5 ? TA.y : !PS_AEM || any(int3(c[i].rgb * 255.0f) & 0xF8) ? TA.x : 0;
		}
	}

	if(PS_LTF)
	{
		t = lerp(lerp(c[0], c[1], dd.x), lerp(c[2], c[3], dd.x), dd.y);
	}
	else
	{
		t = c[0];
	}

	if (PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_RTA_SRC_CORRECTION)
		t.a = t.a * (128.5f / 255.0f);
			
	return trunc(t * 255.0f + 0.05f);
}

float4 tfx(float4 T, float4 C)
{
	float4 C_out;
	float4 FxT = trunc((C * T) / 128.0f);

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

bool atst(float4 C)
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

float4 fog(float4 c, float f)
{
	if(PS_FOG)
	{
		c.rgb = trunc(lerp(FogColor, c.rgb, (f * 255.0f) / 256.0f));
	}

	return c;
}

float4 ps_color(PS_INPUT input)
{
#if PS_FST == 0
	float2 st = input.t.xy / input.t.w;
	float2 st_int = input.ti.zw / input.t.w;
#else
	float2 st = input.ti.xy;
	float2 st_int = input.ti.zw;
#endif

#if PS_CHANNEL_FETCH == 1
	float4 T = fetch_red(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 2
	float4 T = fetch_green(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 3
	float4 T = fetch_blue(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 4
	float4 T = fetch_alpha(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 5
	float4 T = fetch_rgb(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_CHANNEL_FETCH == 6
	float4 T = fetch_gXbY(int2(input.p.xy + ChannelShuffleOffset));
#elif PS_DEPTH_FMT > 0
	float4 T = sample_depth(st_int, input.p.xy);
#else
	float4 T = sample_color(st, input.t.w, int2(input.p.xy));
#endif

	if (PS_SHUFFLE && !PS_SHUFFLE_SAME && !PS_READ16_SRC && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE))
	{
		uint4 denorm_c_before = uint4(T);
		if (PS_PROCESS_BA & SHUFFLE_READ)
		{
			T.r = float((denorm_c_before.b << 3) & 0xF8u);
			T.g = float(((denorm_c_before.b >> 2) & 0x38u) | ((denorm_c_before.a << 6) & 0xC0u));
			T.b = float((denorm_c_before.a << 1) & 0xF8u);
			T.a = float(denorm_c_before.a & 0x80u);
		}
		else
		{
			T.r = float((denorm_c_before.r << 3) & 0xF8u);
			T.g = float(((denorm_c_before.r >> 2) & 0x38u) | ((denorm_c_before.g << 6) & 0xC0u));
			T.b = float((denorm_c_before.g << 1) & 0xF8u);
			T.a = float(denorm_c_before.g & 0x80u);
		}
		
		T.a = (T.a >= 127.5f ? TA.y : !PS_AEM || any(int3(T.rgb) & 0xF8) ? TA.x : 0) * 255.0f;
	}

	float4 C = tfx(T, input.c);

	C = fog(C, input.t.z);

	return C;
}

void ps_fbmask(inout float4 C, float2 pos_xy)
{
	if (PS_FBMASK)
	{
		float multi = PS_COLCLIP_HW ? 65535.0f : 255.0f;
		float4 RT = trunc(RtLoad(int2(pos_xy)) * multi + 0.1f);
		C = (float4)(((uint4)C & ~FbMask) | ((uint4)RT & FbMask));
	}
}

void ps_dither(inout float3 C, float As, float2 pos_xy)
{
	if (PS_DITHER > 0 && PS_DITHER < 3)
	{
		int2 fpos;

		if (PS_DITHER == 2)
			fpos = int2(pos_xy);
		else
			fpos = int2(pos_xy * RcpScaleFactor);

		float value = DitherMatrix[fpos.x & 3][fpos.y & 3];
		
		if (PS_DITHER_ADJUST)
		{
			float Alpha = PS_BLEND_C == 2 ? Af : As;
			value *= Alpha > 0.0f ? min(1.0f / Alpha, 1.0f) : 1.0f;
		}
		
		if (PS_ROUND_INV)
			C -= value;
		else
			C += value;
	}
}

void ps_color_clamp_wrap(inout float3 C)
{
	if (SW_BLEND || (PS_DITHER > 0 && PS_DITHER < 3) || PS_FBMASK)
	{
		if (PS_DST_FMT == FMT_16 && PS_BLEND_MIX == 0 && PS_ROUND_INV)
			C += 7.0f;

		if (PS_COLCLIP == 0 && PS_COLCLIP_HW == 0)
			C = clamp(C, (float3)0.0f, (float3)255.0f);

		if (PS_DST_FMT == FMT_16 && PS_DITHER != 3 && (PS_BLEND_MIX == 0 || PS_DITHER))
			C = (float3)((int3)C & (int3)0xF8);
		else if (PS_COLCLIP == 1 || PS_COLCLIP_HW == 1)
			C = (float3)((int3)C & (int3)0xFF);
	}
	else if (PS_DST_FMT == FMT_16 && PS_DITHER != 3 && PS_BLEND_MIX == 0 && PS_BLEND_HW == 0)
		C = (float3)((int3)C & (int3)0xF8);
}

void ps_blend(inout float4 Color, inout float4 As_rgba, float2 pos_xy)
{
	float As = As_rgba.a;

	if (SW_BLEND)
	{
		if (PS_PABE)
		{
			if (As < 1.0f)
			{
				As_rgba.rgb = (float3)0.0f;
				return;
			}

			As_rgba.rgb = (float3)1.0f;
		}

		float4 RT = SW_BLEND_NEEDS_RT ? RtLoad(int2(pos_xy)) : (float4)0.0f;

		if (PS_SHUFFLE && SW_BLEND_NEEDS_RT)
		{
			uint4 denorm_rt = uint4(RT);
			if (PS_PROCESS_BA & SHUFFLE_WRITE)
			{
				RT.r = float((denorm_rt.b << 3) & 0xF8u);
				RT.g = float(((denorm_rt.b >> 2) & 0x38u) | ((denorm_rt.a << 6) & 0xC0u));
				RT.b = float((denorm_rt.a << 1) & 0xF8u);
				RT.a = float(denorm_rt.a & 0x80u);
			}
			else
			{
				RT.r = float((denorm_rt.r << 3) & 0xF8u);
				RT.g = float(((denorm_rt.r >> 2) & 0x38u) | ((denorm_rt.g << 6) & 0xC0u));
				RT.b = float((denorm_rt.g << 1) & 0xF8u);
				RT.a = float(denorm_rt.g & 0x80u);
			}
		}
		
		float Ad = PS_RTA_CORRECTION ? trunc(RT.a * 128.0f + 0.1f) / 128.0f : trunc(RT.a * 255.0f + 0.1f) / 128.0f;
		float color_multi = PS_COLCLIP_HW ? 65535.0f : 255.0f;
		float3 Cd = trunc(RT.rgb * color_multi + 0.1f);
		float3 Cs = Color.rgb;

		float3 A = (PS_BLEND_A == 0) ? Cs : ((PS_BLEND_A == 1) ? Cd : (float3)0.0f);
		float3 B = (PS_BLEND_B == 0) ? Cs : ((PS_BLEND_B == 1) ? Cd : (float3)0.0f);
		float  C = (PS_BLEND_C == 0) ? As : ((PS_BLEND_C == 1) ? Ad : Af);
		float3 D = (PS_BLEND_D == 0) ? Cs : ((PS_BLEND_D == 1) ? Cd : (float3)0.0f);

		float C_clamped = C;
		if (PS_BLEND_MIX > 0 && PS_BLEND_HW != 1 && PS_BLEND_HW != 2)
			C_clamped = saturate(C_clamped);

		if (PS_BLEND_A == PS_BLEND_B)
			Color.rgb = D;
		else if (PS_BLEND_MIX == 2)
			Color.rgb = ((A - B) * C_clamped + D) + (124.0f / 256.0f);
		else if (PS_BLEND_MIX == 1)
			Color.rgb = ((A - B) * C_clamped + D) - (124.0f / 256.0f);
		else
			Color.rgb = trunc(((A - B) * C) + D);

		if (PS_BLEND_HW == 1)
		{
			As_rgba.rgb = (float3)C;
			float3 alpha_compensate = max((float3)1.0f, Color.rgb / (float3)255.0f);
			As_rgba.rgb -= alpha_compensate;
		}
		else if (PS_BLEND_HW == 2)
		{
			float division_alpha = 1.0f + C;
			Color.rgb /= (float3)division_alpha;
		}
		else if (PS_BLEND_HW == 3)
		{
			As_rgba.rgb = (float3)C_clamped;
			float3 overflow_check = (Color.rgb - (float3)255.0f) / 255.0f;
			float3 alpha_compensate = max((float3)0.0f, overflow_check);
			As_rgba.rgb -= alpha_compensate;
		}
	}
	else
	{
		float3 Alpha = PS_BLEND_C == 2 ? (float3)Af : (float3)As;

		if (PS_BLEND_HW == 1)
		{
			Color.rgb = (float3)255.0f;
		}
		else if (PS_BLEND_HW == 2)
		{
			Color.rgb = saturate(Alpha - (float3)1.0f) * (float3)255.0f;
		}
		else if (PS_BLEND_HW == 3 && PS_RTA_CORRECTION == 0)
		{
			float max_color = max(max(Color.r, Color.g), Color.b);
			float color_compensate = 255.0f / max(128.0f, max_color);
			Color.rgb *= (float3)color_compensate;
		}
		else if (PS_BLEND_HW == 4)
		{
			As_rgba.rgb = Alpha * (float3)(128.0f / 255.0f);
			Color.rgb = (float3)127.5f;
		}
		else if (PS_BLEND_HW == 5)
		{
			Alpha *= (float3)(128.0f / 255.0f);
			As_rgba.rgb = (Alpha - (float3)0.5f);
			Color.rgb = (Color.rgb * Alpha);
		}
		else if (PS_BLEND_HW == 6)
		{
			Alpha *= (float3)(128.0f / 255.0f);
			As_rgba.rgb = Alpha;
			Color.rgb *= (Alpha - (float3)0.5f);
		}
	}
}

#if PS_ROV_EARLYDEPTHSTENCIL
[earlydepthstencil]
#endif

#if PS_ROV_COLOR || PS_ROV_DEPTH
	#define DISCARD { rov_discard_color = true; rov_discard_depth = true; }
	#define DISCARD_COLOR rov_discard_color = true
	#define DISCARD_DEPTH rov_discard_depth = true
#else
	#define DISCARD discard
	#define DISCARD_COLOR o_col0 = RtLoad(input.p.xy)
	#define DISCARD_DEPTH input.p.z = DepthLoad(input.p.xy)
#endif

#if (PS_RETURN_COLOR || PS_RETURN_DEPTH)
PS_OUTPUT ps_main(PS_INPUT input)
#else
void ps_main(PS_INPUT input)
#endif
{
#if PS_ZFLOOR
	input.p.z = floor(input.p.z * exp2(32.0f)) * exp2(-32.0f);
#endif

#if PS_ROV_COLOR
	rov_rt_value = RtTextureRov[input.p.xy];
#endif

#if PS_ROV_DEPTH
	rov_depth_value = DepthTextureRov[input.p.xy];
#endif

#if PS_ROV_COLOR || PS_ROV_DEPTH
	bool rov_discard_color = false;
	bool rov_discard_depth = false;
#endif

#if PS_ZTST == ZTST_GEQUAL
	if (input.p.z < DepthLoad(input.p.xy))
		DISCARD;
#elif PS_ZTST == ZTST_GREATER
	if (input.p.z <= DepthLoad(input.p.xy))
		DISCARD;
#endif

	float4 C = ps_color(input);

#if PS_AA1
	#if PS_AA1 == PS_AA1_LINE
		float cov = clamp(LineCovScale * (1.0f - abs(input.inv_cov)), 0.0f, 1.0f);
	#else
		float cov = clamp(1.0f - abs(input.inv_cov), 0.0f, 1.0f);
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

	if (PS_SCANMSK & 2)
	{
		if ((int(input.p.y) & 1) == (PS_SCANMSK & 1))
			discard;
	}

	float4 alpha_blend = (float4)0.0f;
	if (SW_AD_TO_HW)
	{
		float4 RT = PS_RTA_CORRECTION ? trunc(RtLoad(input.p.xy) * 128.0f + 0.1f) : trunc(RtLoad(input.p.xy) * 255.0f + 0.1f);
		alpha_blend = (float4)(RT.a / 128.0f);
	}
	else
	{
		alpha_blend = (float4)(C.a / 128.0f);
	}

	if (PS_DST_FMT == FMT_16)
	{
		float A_one = 128.0f;
		C.a = PS_FBA ? A_one : step(A_one, C.a) * A_one;
	}
	else if ((PS_DST_FMT == FMT_32) && PS_FBA)
	{
		float A_one = 128.0f;
		if (C.a < A_one) C.a += A_one;
	}

#if PS_DATE >= 5

#if PS_WRITE_RG == 1
	float rt_a = RtLoad(input.p.xy).g;
#else
	float rt_a = RtLoad(input.p.xy).a;
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

if (bad)
	discard;
#endif

#if PS_DATE == 3
	int stencil_ceil = int(PrimMinTexture.Load(int3(input.p.xy, 0)));
	if (int(input.primid) > stencil_ceil)
		discard;
#endif

#if !PS_NO_COLOR
	#if PS_DATE == 1 || PS_DATE == 2
		float o_col0;
	#else
		float4 o_col0;
		#if !PS_NO_COLOR1
			float4 o_col1;
		#endif
	#endif
#endif

#if PS_DATE == 1
	o_col0 = (C.a > 127.5f) ? float(input.primid) : float(0x7FFFFFFF);

#elif PS_DATE == 2

	o_col0 = (C.a < 127.5f) ? float(input.primid) : float(0x7FFFFFFF);

#else

	ps_blend(C, alpha_blend, input.p.xy);

	if (PS_SHUFFLE)
	{
		if (!PS_SHUFFLE_SAME && !PS_READ16_SRC && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE))
		{
			uint4 denorm_c_after = uint4(C);
			if (PS_PROCESS_BA & SHUFFLE_READ)
			{
				C.b = float(((denorm_c_after.r >> 3) & 0x1Fu) | ((denorm_c_after.g << 2) & 0xE0u));
				C.a = float(((denorm_c_after.g >> 6) & 0x3u) | ((denorm_c_after.b >> 1) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			}
			else
			{
				C.r = float(((denorm_c_after.r >> 3) & 0x1Fu) | ((denorm_c_after.g << 2) & 0xE0u));
				C.g = float(((denorm_c_after.g >> 6) & 0x3u) | ((denorm_c_after.b >> 1) & 0x7Cu) | (denorm_c_after.a & 0x80u));
			}
		}


		if (PS_SHUFFLE_SAME)
		{
			uint4 denorm_c = uint4(C);
			
			if (PS_PROCESS_BA & SHUFFLE_READ)
				C = (float4)(float((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
			else
				C.ga = C.rg;
		}
		else if (PS_READ16_SRC)
		{
			uint4 denorm_c = uint4(C);
			uint2 denorm_TA = uint2(float2(TA.xy) * 255.0f + 0.5f);
			C.rb = (float2)float((denorm_c.r >> 3) | (((denorm_c.g >> 3) & 0x7u) << 5));
			C.ga = (float2)float((denorm_c.g >> 6) | ((denorm_c.b >> 3) << 2) | (denorm_TA.x & 0x80u));
		}
		else if (PS_SHUFFLE_ACROSS)
		{
			if (PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
			{
				C.br = C.rb;
				C.ag = C.ga;
			}
			else if(PS_PROCESS_BA & SHUFFLE_READ)
			{
				C.rb = C.bb;
				C.ga = C.aa;
			}
			else
			{
				C.rb = C.rr;
				C.ga = C.gg;
			}
		}
	}

	ps_dither(C.rgb, alpha_blend.a, input.p.xy);

	ps_color_clamp_wrap(C.rgb);

	ps_fbmask(C, input.p.xy);

#if (PS_AFAIL == AFAIL_RGB_ONLY_DSB) && !PS_NO_COLOR1
	alpha_blend.a = float(atst_pass);
#endif

#if !PS_NO_COLOR
	o_col0.a = PS_RTA_CORRECTION ? C.a / 128.0f : C.a / 255.0f;
	o_col0.rgb = PS_COLCLIP_HW ? float3(C.rgb / 65535.0f) : C.rgb / 255.0f;
#if !PS_NO_COLOR1
	o_col1 = alpha_blend;
#endif
#endif

#if PS_AFAIL == AFAIL_FB_ONLY
	if (!atst_pass)
		DISCARD_DEPTH;
#elif PS_AFAIL == AFAIL_ZB_ONLY
	if (!atst_pass)
		DISCARD_COLOR;
#elif PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
	if (!atst_pass)
	{
		o_col0.a = RtLoad(input.p.xy).a;
	#if PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
		DISCARD_DEPTH;
	#endif
	}
#endif

#endif

#if PS_ZCLAMP
	input.p.z = min(input.p.z, MaxDepthPS);
#endif

#if PS_AA1 == PS_AA1_TRIANGLE_SW_Z
	if (!bool(input.interior))
		DISCARD_DEPTH;
#endif

#if (PS_RETURN_COLOR || PS_RETURN_DEPTH)
	PS_OUTPUT output;
#endif

#if PS_RETURN_COLOR
	output.c0 = o_col0;
	#if !PS_NO_COLOR1
		output.c1 = o_col1;
	#endif
#elif PS_RETURN_COLOR_ROV
	o_col0 = (FbMask == 0xFFu) ? RtLoad(input.p.xy) : o_col0;
	if (!rov_discard_color)
		RtWrite(input.p.xy, o_col0);
#endif

#if PS_RETURN_DEPTH
	output.depth = input.p.z;
	#if SW_DEPTH && PS_NO_COLOR1 && PS_DEPTH_FEEDBACK_SUPPORT == 2
		output.depth_color = input.p.z;
	#endif
#elif PS_RETURN_DEPTH_ROV
	if (!rov_discard_depth)
		DepthWrite(input.p.xy, input.p.z);
#endif

#if (PS_RETURN_COLOR || PS_RETURN_DEPTH)
	return output;
#endif
}

#endif

#ifdef VERTEX_SHADER

#ifdef DX12
cbuffer cb0 : register(b0)
#else
cbuffer cb0
#endif
{
	float2 VertexScale;
	float2 VertexOffset;
	float2 TextureScale;
	float2 TextureOffset;
	float2 PointSize;
	uint MaxDepth;
	float LineAA1Width;
	float2 vr_stereo;
	uint vr_map_mode;
	uint vr_band_count;
	float4 vr_splits;
	float4 vr_band[4];
};

#ifdef DX12
cbuffer cb2 : register(b2)
#else
cbuffer cb2
#endif
{
	uint BaseVertex;
	uint BaseIndex;
	uint _cb2_pad0;
	uint _cb2_pad1;
};

VS_OUTPUT vs_main(VS_INPUT input)
{
	input.z = min(input.z, MaxDepth);

	VS_OUTPUT output;

	output.p = float4(input.p, input.z, 1.0f) - float4(0.05f, 0.05f, 0, 0);

	output.p.xy = output.p.xy * float2(VertexScale.x, -VertexScale.y) - float2(VertexOffset.x, -VertexOffset.y);
	output.p.z *= exp2(-32.0f);

	if(VS_TME)
	{
		float2 uv = input.uv - TextureOffset;
		float2 st = input.st - TextureOffset;

		output.ti.xy = uv * TextureScale;

		if (VS_FST)
		{
			output.ti.zw = uv;
		}
		else
		{
			output.ti.zw = st / TextureScale;
		}
		output.t.xy = st;
		output.t.w = input.q;
	}
	else
	{
		output.t.xy = 0;
		output.t.w = 1.0f;
		output.ti = 0;
	}

	output.c = input.c;
	output.t.z = input.f.r;

	output.inv_cov = 0.0f;
	output.interior = 0;

	return output;
}

#if VS_EXPAND != VS_EXPAND_NONE

struct VS_RAW_INPUT
{
	float2 ST;
	uint RGBA;
	float Q;
	uint XY;
	uint Z;
	uint UV;
	uint FOG;
};

StructuredBuffer<VS_RAW_INPUT> vertices : register(t0);
StructuredBuffer<uint> IndexBuffer : register(t5);

uint load_index(uint _i)
{
	uint i = _i + BaseIndex;
	uint shift = (i & 1u) << 4u;
	return (IndexBuffer.Load(i >> 1u) >> shift) & 0xFFFFu;
}

VS_INPUT load_vertex(uint index)
{
	VS_RAW_INPUT raw = vertices.Load(BaseVertex + index);

	VS_INPUT vert;
	vert.st = raw.ST;
	vert.c = uint4(raw.RGBA & 0xFFu, (raw.RGBA >> 8) & 0xFFu, (raw.RGBA >> 16) & 0xFFu, raw.RGBA >> 24);
	vert.q = raw.Q;
	vert.p = uint2(raw.XY & 0xFFFFu, raw.XY >> 16);
	vert.z = raw.Z;
	vert.uv = uint2(raw.UV & 0xFFFFu, raw.UV >> 16);
	vert.f = float4(float(raw.FOG & 0xFFu), float((raw.FOG >> 8) & 0xFFu), float((raw.FOG >> 16) & 0xFFu), float(raw.FOG >> 24)) / 255.0f;
	return vert;
}

float2 get_xy_unscaled(float2 xy)
{
	return round(xy / VertexScale) / 16.0f;
}

float2x2 get_xy_deltas_unscaled(VS_OUTPUT v0, VS_OUTPUT v1, VS_OUTPUT v2)
{
	float2 xy0 = get_xy_unscaled(v0.p.xy);
	float2 xy1 = get_xy_unscaled(v1.p.xy);
	float2 xy2 = get_xy_unscaled(v2.p.xy);
	return float2x2(xy1 - xy0, xy2 - xy0);
}

float2 get_aa1_triangle_expand_dir(VS_OUTPUT v0, VS_OUTPUT v1, VS_OUTPUT v2)
{
	float2x2 xy_deltas = get_xy_deltas_unscaled(v0, v1, v2);
	float2 line_delta = xy_deltas[0];
	float2 line_opposite = xy_deltas[1];

	float2 line_normal = float2(line_delta.y, -line_delta.x);
	float2 line_expand = abs(line_delta.x) >= abs(line_delta.y) ? float2(0.0f, 1.0f) : float2(1.0f, 0.0f);

	if ((dot(line_expand, line_normal) >= 0.0f) == (dot(line_opposite, line_normal) >= 0.0f))
	{
		line_expand = -line_expand;
	}

	return line_expand;
}

float2x2 get_inverse(float2x2 mat, float det)
{
	return float2x2(mat[1][1], -mat[0][1], -mat[1][0], mat[0][0]) * (1 / det);
}

void extrapolate_aa1_triangle_edge(inout VS_OUTPUT v0, VS_OUTPUT v1, VS_OUTPUT v2, float2x2 dp_mat, float2 dp)
{
	#if VS_TME
		#if VS_FST
			float2x2 dt = float2x2(v1.ti.zw - v0.ti.zw, v2.ti.zw - v0.ti.zw);
		#else
			float2x2 dt = float2x2(v1.t.xy - v0.t.xy, v2.t.xy - v0.t.xy);
		#endif
	#endif

	#if VS_IIP
		float2x4 dc = float2x4(v1.c - v0.c, v2.c - v0.c);
	#endif

	float2 dz = float2(v1.p.z - v0.p.z, v2.p.z - v0.p.z);

	float2 df = float2(v1.t.z - v0.t.z, v2.t.z - v0.t.z);

	float2 dq = float2(v1.t.w - v0.t.w, v2.t.w - v0.t.w);

	float dp_det = determinant(dp_mat);
	float len0 = length(dp_mat[0]);
	float len1 = length(dp_mat[1]);
	float len2 = length(dp_mat[1] - dp_mat[0]);
	float min_perp_length = abs(dp_det) / max(max(len0, len1), len2);

	float2x2 inv_dp_mat = get_inverse(dp_mat, dp_det);

	float2 weights = min_perp_length < 2 ? 0 : mul(dp, inv_dp_mat);

	v0.p.xy += dp * PointSize;

	#if VS_TME
		#if VS_FST
			v0.ti.zw += mul(weights, dt);
			v0.ti.xy = v0.ti.zw * TextureScale;
		#else
			v0.t.xy += mul(weights, dt);
			v0.ti.zw = v0.t.xy / TextureScale;
			v0.t.w += dot(weights, dq);
		#endif
	#endif

	#if VS_IIP
		v0.c += mul(weights, dc);
		v0.c = clamp(v0.c, 0, 255);
	#endif

	v0.p.z += dot(weights, dz);

	v0.t.z += dot(weights, df);
}

VS_OUTPUT vs_main_expand(uint vid : SV_VertexID)
{
#if VS_EXPAND == VS_EXPAND_POINT

	VS_OUTPUT vtx = vs_main(load_vertex(vid >> 2));

	vtx.p.x += ((vid & 1u) != 0u) ? PointSize.x : 0.0f;
	vtx.p.y += ((vid & 2u) != 0u) ? PointSize.y : 0.0f;

	return vtx;

#elif (VS_EXPAND == VS_EXPAND_LINE) || (VS_EXPAND == VS_EXPAND_LINE_AA1)

	uint vid_base = vid >> 2;
	bool is_bottom = vid & 2;
	bool is_right = vid & 1;
	uint vid_other = is_bottom ? vid_base - 1 : vid_base + 1;
	VS_OUTPUT vtx = vs_main(load_vertex(vid_base));
	VS_OUTPUT other = vs_main(load_vertex(vid_other));

	float2 line_delta = is_bottom ? (vtx.p.xy - other.p.xy) : (other.p.xy - vtx.p.xy);
	float2 line_vector = normalize(line_delta / VertexScale);
	float2 line_expand = float2(line_vector.y, -line_vector.x);
#if VS_EXPAND == VS_EXPAND_LINE_AA1
	line_expand *= 2.0f * LineAA1Width;
#endif
	float2 line_width = (line_expand * PointSize) / 2;
	float2 offset = is_right ? line_width : -line_width;
	vtx.p.xy += offset;

#if VS_EXPAND == VS_EXPAND_LINE_AA1
	vtx.inv_cov = is_right ? 1.0f : -1.0f;
#endif

	return vtx;

#elif VS_EXPAND == VS_EXPAND_SPRITE

	uint vid_base = vid >> 1;
	uint vid_lt = vid_base & ~1u;
	uint vid_rb = vid_base | 1u;

	VS_OUTPUT lt = vs_main(load_vertex(vid_lt));
	VS_OUTPUT rb = vs_main(load_vertex(vid_rb));
	VS_OUTPUT vtx = rb;

	bool is_right = ((vid & 1u) != 0u);
	vtx.p.x = is_right ? lt.p.x : vtx.p.x;
	vtx.t.x = is_right ? lt.t.x : vtx.t.x;
	vtx.ti.xz = is_right ? lt.ti.xz : vtx.ti.xz;

	bool is_bottom = ((vid & 2u) != 0u);
	vtx.p.y = is_bottom ? lt.p.y : vtx.p.y;
	vtx.t.y = is_bottom ? lt.t.y : vtx.t.y;
	vtx.ti.yw = is_bottom ? lt.ti.yw : vtx.ti.yw;

	return vtx;

#elif VS_EXPAND == VS_EXPAND_TRIANGLE_AA1

	uint prim_id = vid / 39;
	uint prim_offset = vid - 39 * prim_id;
	bool interior = prim_offset < 3;
	bool edge = 3 <= prim_offset && prim_offset < 21;

	VS_OUTPUT vtx;
	if (interior)
	{
		vtx = vs_main(load_vertex(load_index(3 * prim_id + prim_offset)));
		vtx.inv_cov = 0.0f;
		vtx.interior = 1;
	}
	else if (edge)
	{
		uint prim_offset_edges = prim_offset - 3;
		uint i0 = prim_offset_edges / 6;
		uint i1 = (i0 >= 2) ? i0 - 2 : i0 + 1;
		uint i2 = (i0 >= 1) ? i0 - 1 : i0 + 2;
		uint edge_offset = prim_offset_edges - 6 * i0;

		bool is_bottom = (2 <= edge_offset) && (edge_offset <= 4);
		bool is_outside = edge_offset & 1;

		vtx = vs_main(load_vertex(load_index(3 * prim_id + (is_bottom ? i1 : i0))));
		VS_OUTPUT other = vs_main(load_vertex(load_index(3 * prim_id + (is_bottom ? i0 : i1))));
		VS_OUTPUT opposite = vs_main(load_vertex(load_index(3 * prim_id + i2)));

		float2x2 pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		float2 expand_dir = is_outside ? get_aa1_triangle_expand_dir(vtx, other, opposite) : 0;

		extrapolate_aa1_triangle_edge(vtx, other, opposite, pos_deltas, expand_dir);

		vtx.inv_cov = is_outside ? 1.0f : 0.0f;

		vtx.interior = 0;
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

		vtx = vs_main(load_vertex(load_index(3 * prim_id + i0)));
		VS_OUTPUT other = vs_main(load_vertex(load_index(3 * prim_id + (is_first_tri ? i1 : i2))));
		VS_OUTPUT opposite = vs_main(load_vertex(load_index(3 * prim_id + (is_first_tri ? i2 : i1))));

		float2x2 pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		float2 edge_expand_dir_0 = get_aa1_triangle_expand_dir(vtx, other, opposite);
		float2 edge_expand_dir_1 = get_aa1_triangle_expand_dir(vtx, opposite, other);

		bool corner_filled = all(edge_expand_dir_0 == edge_expand_dir_1);

		float2 far_corner_dir = corner_filled ? 0 : -normalize((pos_deltas[0] + pos_deltas[1]) / 2);

		float2 expand_dir = is_near_corner ? 0 :
		                    is_far_corner ? far_corner_dir :
		                    edge_expand_dir_0;

		extrapolate_aa1_triangle_edge(vtx, other, opposite, pos_deltas, expand_dir);

		vtx.inv_cov = is_near_corner ? 0.0f : 1.0f;
	
		vtx.interior = 0;
	}

	return vtx;

#endif
}

#endif

#endif
