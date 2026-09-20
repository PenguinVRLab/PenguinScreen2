// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include "glad/gl.h"

#include <memory>
#include <tuple>
#include <vector>

class GLStreamBuffer
{
public:
	virtual ~GLStreamBuffer();

	__fi GLuint GetGLBufferId() const { return m_buffer_id; }
	__fi GLenum GetGLTarget() const { return m_target; }
	__fi u32 GetSize() const { return m_size; }

	void Bind();
	void Unbind();

	struct MappingResult
	{
		void* pointer;
		u32 buffer_offset;
		u32 index_aligned;
		u32 space_aligned;
	};

	virtual MappingResult Map(u32 alignment, u32 min_size) = 0;
	virtual void Unmap(u32 used_size) = 0;

	virtual u32 GetChunkSize() const = 0;

	static std::unique_ptr<GLStreamBuffer> Create(GLenum target, u32 size);

protected:
	GLStreamBuffer(GLenum target, GLuint buffer_id, u32 size);

	GLenum m_target;
	GLuint m_buffer_id;
	u32 m_size;
};
