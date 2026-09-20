// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

//#version 420 // Keep it for editor detection

#ifdef FRAGMENT_SHADER

in vec4 PSin_p;
in vec2 PSin_t;
in vec4 PSin_c;

uniform vec4 ZrH;

layout(binding = 0) uniform sampler2D TextureSampler;

layout(location = 0) out vec4 SV_Target0;


void ps_main0()
{
	int idx   = int(ZrH.x);
	int field = idx & 1;
	int vpos  = int(gl_FragCoord.y);

	if ((vpos & 1) == field)
		SV_Target0 = textureLod(TextureSampler, PSin_t, 0);
	else
		discard;
}


void ps_main1()
{
	SV_Target0 = textureLod(TextureSampler, PSin_t, 0);
}


void ps_main2()
{
	vec2 vstep = vec2(0.0f, ZrH.y);
	vec4 c0 = textureLod(TextureSampler, PSin_t - vstep, 0);
	vec4 c1 = textureLod(TextureSampler, PSin_t, 0);
	vec4 c2 = textureLod(TextureSampler, PSin_t + vstep, 0);

	SV_Target0 = (c0 + c1 * 2.0f + c2) / 4.0f;
}


void ps_main3()
{

	int  idx    = int(ZrH.x);
	int  bank   = idx >> 1;
	int  field  = idx & 1;
	int  vres   = int(ZrH.z) >> 1;
	int  lofs   = ((((vres + 1) >> 1) << 1) - vres) & bank;
	int  vpos   = int(gl_FragCoord.y) + lofs;

	if ((vpos & 1) == field)
		SV_Target0 = textureLod(TextureSampler, PSin_t, 0);
	else
		discard;
}


void ps_main4()
{

	int   idx          = int(ZrH.x);
	int   field        = idx & 1;
	int   vpos         = int(gl_FragCoord.y);
	float sensitivity  = ZrH.w;
	vec3  motion_thr   = vec3(1.0, 1.0, 1.0) * sensitivity;
	vec2  bofs         = vec2(0.0f, 0.5f);
	vec2  vscale       = vec2(1.0f, 0.5f);
	vec2  lofs         = vec2(0.0f, ZrH.y) * vscale;
	vec2  iptr         = PSin_t * vscale;

	vec2 p_t0;
	vec2 p_t1;
	vec2 p_t2;
	vec2 p_t3;

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


	vec4 hn = textureLod(TextureSampler, p_t0 - lofs, 0);
	vec4 cn = textureLod(TextureSampler, p_t1, 0);
	vec4 ln = textureLod(TextureSampler, p_t0 + lofs, 0);

	vec4 ho = textureLod(TextureSampler, p_t2 - lofs, 0);
	vec4 co = textureLod(TextureSampler, p_t3, 0);
	vec4 lo = textureLod(TextureSampler, p_t2 + lofs, 0);

	vec3 mh = hn.rgb - ho.rgb;
	vec3 mc = cn.rgb - co.rgb;
	vec3 ml = ln.rgb - lo.rgb;

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
		SV_Target0 = textureLod(TextureSampler, p_t0, 0);
	}
	else if ((iptr.y > 0.5f - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		SV_Target0 = cn;
	}
	else
	{
		if(((mh_max > 0.0f) || (ml_max > 0.0f)) || (mc_max > 0.0f))
			SV_Target0 = (hn + ln) / 2.0f;
		else
		{
			if((mh_max != -motion_thr.x) || (ml_max != -motion_thr.x) || (mc_max != -motion_thr.x))
			{
				vec3 mhln = hn.rgb - ln.rgb;
				vec3 mchn = hn.rgb - cn.rgb;

				mhln = max(mhln, -mhln) - motion_thr;
				mchn = max(mchn, -mchn) - motion_thr;

				float mhln_max = max(max(mhln.x, mhln.y), mhln.z);
				float mchn_max = max(max(mchn.x, mchn.y), mchn.z);

				if (mhln_max < 0.0f && mchn_max >= (mhln_max * 0.90f))
					SV_Target0 = (hn + ln) / 2.0f;
				else
					SV_Target0 = cn;
			}
			else
				SV_Target0 = cn;
		}
	}
}

#endif
