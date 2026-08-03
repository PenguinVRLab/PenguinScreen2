// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

Texture2D Texture;
SamplerState Sampler;

cbuffer cb0
{
	float4 ZrH;
};

struct PS_INPUT
{
	float4 p : SV_Position;
	float2 t : TEXCOORD0;
};


float4 ps_main0(PS_INPUT input) : SV_Target0
{
	const int idx   = int(ZrH.x);
	const int field = idx & 1;
	const int vpos  = int(input.p.y);

	if ((vpos & 1) == field)
		return Texture.SampleLevel(Sampler, input.t, 0);
	else
		discard;

	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}


float4 ps_main1(PS_INPUT input) : SV_Target0
{
	return Texture.SampleLevel(Sampler, input.t, 0);
}


float4 ps_main2(PS_INPUT input) : SV_Target0
{
	float2 vstep = float2(0.0f, ZrH.y);
	float4 c0 = Texture.SampleLevel(Sampler, input.t - vstep, 0);
	float4 c1 = Texture.SampleLevel(Sampler, input.t, 0);
	float4 c2 = Texture.SampleLevel(Sampler, input.t + vstep, 0);

	return (c0 + c1 * 2 + c2) / 4;
}


float4 ps_main3(PS_INPUT input) : SV_Target0
{

	const int    idx    = int(ZrH.x);
	const int    bank   = idx >> 1;
	const int    field  = idx & 1;
	const int    vres   = int(ZrH.z) >> 1;
	const int    lofs   = ((((vres + 1) >> 1) << 1) - vres) & bank;
	const int    vpos   = int(input.p.y) + lofs;

	if ((vpos & 1) == field)
		return Texture.SampleLevel(Sampler, input.t, 0);
	else
		discard;

	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}


float4 ps_main4(PS_INPUT input) : SV_Target0
{

	const int    idx         = int(ZrH.x);
	const int    field       = idx & 1;
	const int    vpos        = int(input.p.y);
	const float  sensitivity = ZrH.w;
	const float3 motion_thr  = float3(1.0, 1.0, 1.0) * sensitivity;
	const float2 bofs        = float2(0.0f, 0.5f);
	const float2 vscale      = float2(1.0f, 0.5f);
	const float2 lofs        = float2(0.0f, ZrH.y) * vscale;
	const float2 iptr        = input.t * vscale;

	float2 p_t0;
	float2 p_t1;
	float2 p_t2;
	float2 p_t3;

	switch (idx)
	{
		case 1:
			p_t0 = iptr;
			p_t1 = iptr;
			p_t2 = iptr + bofs;
			p_t3 = iptr + bofs;
			break;
		case 2:
			p_t0 = iptr + bofs;
			p_t1 = iptr;
			p_t2 = iptr;
			p_t3 = iptr + bofs;
			break;
		case 3:
			p_t0 = iptr + bofs;
			p_t1 = iptr + bofs;
			p_t2 = iptr;
			p_t3 = iptr;
			break;
		default:
			p_t0 = iptr;
			p_t1 = iptr + bofs;
			p_t2 = iptr + bofs;
			p_t3 = iptr;
			break;
	}

	float4 hn = Texture.SampleLevel(Sampler, p_t0 - lofs, 0);
	float4 cn = Texture.SampleLevel(Sampler, p_t1, 0);
	float4 ln = Texture.SampleLevel(Sampler, p_t0 + lofs, 0);

	float4 ho = Texture.SampleLevel(Sampler, p_t2 - lofs, 0);
	float4 co = Texture.SampleLevel(Sampler, p_t3, 0);
	float4 lo = Texture.SampleLevel(Sampler, p_t2 + lofs, 0);

	float3 mh = hn.rgb - ho.rgb;
	float3 mc = cn.rgb - co.rgb;
	float3 ml = ln.rgb - lo.rgb;

	mh = max(mh, -mh) - motion_thr;
	mc = max(mc, -mc) - motion_thr;
	ml = max(ml, -ml) - motion_thr;

	#if 1
		float mh_max = max(max(mh.x, mh.y), mh.z);
		float mc_max = max(max(mc.x, mc.y), mc.z);
		float ml_max = max(max(ml.x, ml.y), ml.z);
	#else
		float mh_max = mh.x + mh.y + mh.z;
		float mc_max = mc.x + mc.y + mc.z;
		float ml_max = ml.x + ml.y + ml.z;
	#endif

	if ((vpos & 1) == field)
	{
		return Texture.SampleLevel(Sampler, p_t0, 0);
	}
	else if ((iptr.y > 0.5f - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		return cn;
	}
	else
	{
		if (((mh_max > 0.0f) || (ml_max > 0.0f)) || (mc_max > 0.0f))
			return (hn + ln) / 2.0f;
		else
		{
			if((mh_max != -motion_thr.x) || (ml_max != -motion_thr.x) || (mc_max != -motion_thr.x))
			{
				float3 mhln = hn.rgb - ln.rgb;
				float3 mchn = hn.rgb - cn.rgb;
				
				mhln = max(mhln, -mhln) - motion_thr;
				mchn = max(mchn, -mchn) - motion_thr;
				
				float mhln_max = max(max(mhln.x, mhln.y), mhln.z);
				float mchn_max = max(max(mchn.x, mchn.y), mchn.z);

				if (mhln_max < 0.0f && mchn_max >= (mhln_max * 0.90f))
					return (hn + ln) / 2.0f;
				else
					return cn;
			}
			else
				return cn;
		}
	}

	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
