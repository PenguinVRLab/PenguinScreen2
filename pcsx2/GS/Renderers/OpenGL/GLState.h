// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"

#include "glad/gl.h"

class GSTextureOGL;

namespace GLState
{
	extern GLuint vao;
	extern GLuint fbo;
	extern GSVector2i viewport;
	extern GSVector4i scissor;

	extern bool point_size;
	extern float line_width;

	extern bool blend;
	extern u16 eq_RGB;
	extern u16 f_sRGB;
	extern u16 f_dRGB;
	extern u16 f_sA;
	extern u16 f_dA;
	extern u8 bf;
	extern u8 wrgba;

	extern bool depth;
	extern GLenum depth_func;
	extern bool depth_mask;

	extern bool stencil;
	extern GLenum stencil_func;
	extern GLenum stencil_pass;

	extern GLuint ps_ss;

	extern GSTextureOGL* rt;
	extern GSTextureOGL* ds_as_rt;
	extern GSTextureOGL* ds;

	extern u32 draw_buffers;

	extern bool rt_written;
	extern bool ds_as_rt_written;
	extern bool ds_written;

	extern GLuint tex_unit[8];

	extern u32 UpdateDrawBuffers();
	extern void Clear();
}
