// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef VERTEX_SHADER

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_tex;

layout(location = 0) out vec2 v_tex;

void main()
{
	gl_Position = vec4(a_pos.x, -a_pos.y, a_pos.z, a_pos.w);
	v_tex = a_tex;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec2 v_tex;
layout(location = 0) out vec4 o_col0;

layout(push_constant) uniform cb0
{
	vec4 ZrH;
};

layout(set = 0, binding = 0) uniform sampler2D samp0;


#ifdef ps_main0
void ps_main0()
{
	const int idx   = int(ZrH.x);
	const int field = idx & 1;
	const int vpos  = int(gl_FragCoord.y);

	if ((vpos & 1) == field)
		o_col0 = textureLod(samp0, v_tex, 0);
	else
		discard;
}
#endif


#ifdef ps_main1
void ps_main1()
{
	o_col0 = textureLod(samp0, v_tex, 0);
}
#endif


#ifdef ps_main2
void ps_main2()
{
	vec2 vstep = vec2(0.0f, ZrH.y);
	vec4 c0 = textureLod(samp0, v_tex - vstep, 0);
	vec4 c1 = textureLod(samp0, v_tex, 0);
	vec4 c2 = textureLod(samp0, v_tex + vstep, 0);

	o_col0 = (c0 + c1 * 2.0f + c2) / 4.0f;
}
#endif


#ifdef ps_main3
void ps_main3()
{

	const int  idx    = int(ZrH.x);
	const int  bank   = idx >> 1;
	const int  field  = idx & 1;
	const int  vres   = int(ZrH.z) >> 1;
	const int  lofs   = ((((vres + 1) >> 1) << 1) - vres) & bank;
	const int  vpos   = int(gl_FragCoord.y) + lofs;

	if ((vpos & 1) == field)
		o_col0 = textureLod(samp0, v_tex, 0);
	else
		discard;
}
#endif


#ifdef ps_main4
void ps_main4()
{

	const int   idx          = int(ZrH.x);
	const int   bank         = idx >> 1;
	const int   field        = idx & 1;
	const int   vpos         = int(gl_FragCoord.y);
	const float sensitivity  = ZrH.w;
	const vec3  motion_thr   = vec3(1.0, 1.0, 1.0) * sensitivity;
	const vec2  bofs         = vec2(0.0f, 0.5f);
	const vec2  vscale       = vec2(1.0f, 0.5f);
	const vec2  lofs         = vec2(0.0f, ZrH.y) * vscale;
	const vec2  iptr         = v_tex * vscale;

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

	vec4 hn = textureLod(samp0, p_t0 - lofs, 0);
	vec4 cn = textureLod(samp0, p_t1, 0);
	vec4 ln = textureLod(samp0, p_t0 + lofs, 0);

	vec4 ho = textureLod(samp0, p_t2 - lofs, 0);
	vec4 co = textureLod(samp0, p_t3, 0);
	vec4 lo = textureLod(samp0, p_t2 + lofs, 0);

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
		o_col0 = textureLod(samp0, p_t0, 0);
	}
	else if ((iptr.y > 0.5f - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		o_col0 = cn;
	}
	else
	{
		if(((mh_max > 0.0f) || (ml_max > 0.0f)) || (mc_max > 0.0f))
			o_col0 = (hn + ln) / 2.0f;
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
					o_col0 = (hn + ln) / 2.0f;
				else
					o_col0 = cn;
			}
			else
				o_col0 = cn;
		}
	}
}
#endif

#endif
