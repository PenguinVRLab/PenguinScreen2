// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/EnumOps.h"
#include "GS/GSVector.h"

#include <string>
#include <string_view>

enum class TextureUsage : u8
{
	Texture      = 0,
	DepthStencil = 1,
	RenderTarget = 2,
	Feedback     = 4,
	ShaderWrite  = 8,
};

MARK_ENUM_AS_FLAGS(TextureUsage);

class GSTexture
{
public:
	struct GSMap
	{
		u8* bits;
		int pitch;
	};

	using Usage = TextureUsage;

	static constexpr Usage ShaderWrite = Usage::ShaderWrite;
	static constexpr Usage Feedback = Usage::Feedback;
	static constexpr Usage FeedbackOrShaderWrite = Usage::Feedback | Usage::ShaderWrite;

	static constexpr Usage Texture = Usage::Texture;
	static constexpr Usage RenderTarget = Usage::RenderTarget;
	static constexpr Usage DepthStencil = Usage::DepthStencil;
	static constexpr Usage FeedbackTarget = Usage::RenderTarget | Usage::Feedback;
	static constexpr Usage FeedbackDepth = Usage::DepthStencil | Usage::Feedback;
	static constexpr Usage ShaderWriteTarget = Usage::RenderTarget | Usage::Feedback | Usage::ShaderWrite;
	static constexpr Usage ShaderWriteTexture = Usage::Texture | Usage::ShaderWrite;

	enum class Format : u8
	{
		Invalid = 0,
		Color,
		ColorHQ,
		ColorHDR,
		ColorClip,
		DepthStencil,
		DepthColor,
		UNorm8,
		UInt16,
		UInt32,
		PrimID,
		BC1,
		BC2,
		BC3,
		BC7,
		Last = BC7,
	};

	static bool ValidateUsageAndFormat(Usage usage, Format format);

	enum class State : u8
	{
		Dirty,
		Cleared,
		Invalidated
	};

	union ClearValue
	{
		u32 color;
		float depth;
	};

protected:
	GSVector2i m_size{};
	int m_mipmap_levels = 0;
	u32 m_array_layers = 1;
	Usage m_usage = Usage::Texture;
	Format m_format = Format::Invalid;
	State m_state = State::Dirty;

	u32 m_last_frame_used = 0;

	bool m_needs_mipmaps_generated = true;
	ClearValue m_clear_value = {};

#ifdef PCSX2_DEVBUILD
	std::string m_debug_name;
#endif
public:
	GSTexture();
	virtual ~GSTexture();

	virtual void* GetNativeHandle() const = 0;

	virtual bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) = 0;
	virtual bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) = 0;
	virtual void Unmap() = 0;
	virtual void GenerateMipmap() = 0;

#ifdef PCSX2_DEVBUILD
	virtual void SetDebugName(std::string_view name) = 0;
	const std::string& GetDebugName() { return m_debug_name; }
#endif

	bool Save(const std::string& fn);

	__fi int GetWidth() const { return m_size.x; }
	__fi int GetHeight() const { return m_size.y; }
	__fi const GSVector2i& GetSize() const { return m_size; }
	__fi GSVector4i GetRect() const { return GSVector4i::loadh(m_size); }

	__fi int GetMipmapLevels() const { return m_mipmap_levels; }
	__fi bool IsMipmap() const { return m_mipmap_levels > 1; }
	__fi u32 GetArrayLayers() const { return m_array_layers; }

	virtual GSTexture* GetLayerProxyTexture(u32 layer) { return this; }

	__fi Usage GetUsage() const { return m_usage; }
	__fi Format GetFormat() const { return m_format; }
	__fi bool IsCompressedFormat() const { return IsCompressedFormat(m_format); }

	static const char* GetFormatName(Format format);
	static bool IsBlockCompressedFormat(Format format);
	static u32 GetCompressedBytesPerBlock(Format format);
	static u32 GetCompressedBlockSize(Format format);
	static u32 CalcUploadPitch(Format format, u32 width);
	static u32 CalcUploadRowLengthFromPitch(Format format, u32 pitch);
	static u32 CalcUploadSize(Format format, u32 height, u32 pitch);
	static bool IsFeedbackFormat(Format format);
	static bool IsShaderWriteFormat(Format format);

	u32 GetCompressedBytesPerBlock() const;
	u32 GetCompressedBlockSize() const;
	u32 CalcUploadPitch(u32 width) const;
	u32 CalcUploadRowLengthFromPitch(u32 pitch) const;
	u32 CalcUploadSize(u32 height, u32 pitch) const;

	static __fi bool IsRenderTarget(Usage usage)
	{
		return (usage & RenderTarget);
	}
	static __fi bool IsDepthStencil(Usage usage)
	{
		return (usage & DepthStencil);
	}
	static __fi bool IsRenderTargetOrDepthStencil(Usage usage)
	{
		return IsRenderTarget(usage) || IsDepthStencil(usage);
	}
	static __fi bool IsDepthColor(Usage usage, Format format)
	{
		return IsRenderTarget(usage) && (format == Format::DepthColor);
	}
	static __fi bool IsTexture(Usage usage)
	{
		return usage == Texture;
	}
	static __fi bool IsDepthLike(Usage usage, Format format)
	{
		return IsDepthStencil(usage) || IsDepthColor(usage, format);
	}
	static __fi bool IsFeedback(Usage usage)
	{
		return (usage & Feedback);
	}
	static __fi bool IsShaderWrite(Usage usage)
	{
		return (usage & ShaderWrite);
	}
	static __fi bool IsFeedbackOrShaderWrite(Usage usage)
	{
		return IsFeedback(usage) || IsShaderWrite(usage);
	}


	__fi bool IsRenderTargetOrDepthStencil() const
	{
		return IsRenderTargetOrDepthStencil(m_usage);
	}
	__fi bool IsRenderTarget() const
	{
		return IsRenderTarget(m_usage);
	}
	__fi bool IsDepthStencil() const
	{
		return IsDepthStencil(m_usage);
	}
	__fi bool IsDepthColor() const
	{
		return IsDepthColor(m_usage, m_format);
	}
	__fi bool IsTexture() const
	{
		return IsTexture(m_usage);
	}
	__fi bool IsDepthLike() const
	{
		return IsDepthLike(m_usage, m_format);
	}
	__fi bool IsFeedback() const
	{
		return IsFeedback(m_usage);
	}
	__fi bool IsShaderWrite() const
	{
		return IsShaderWrite(m_usage);
	}
	__fi bool IsFeedbackOrShaderWrite() const
	{
		return IsFeedbackOrShaderWrite(m_usage);
	}

	virtual bool IsShaderWriteMode() const
	{
		return IsShaderWrite();
	}

	__fi State GetState() const { return m_state; }
	__fi void SetState(State state) { m_state = state; }

	__fi u32 GetLastFrameUsed() const { return m_last_frame_used; }
	void SetLastFrameUsed(u32 frame) { m_last_frame_used = frame; }

	__fi u32 GetClearColor() const { return m_clear_value.color; }
	__fi float GetClearDepth() const { return m_clear_value.depth; }
	__fi GSVector4 GetUNormClearColor() const { return GSVector4::unorm8(m_clear_value.color); }
	__fi GSVector4 GetClearForFormat() const
	{
		return IsDepthLike() ? GSVector4(m_clear_value.depth, 0.0f, 0.0f, 0.0f) : GetUNormClearColor();
	}

	__fi void SetClearColor(u32 color)
	{
		m_state = State::Cleared;
		m_clear_value.color = color;
	}
	__fi void SetClearDepth(float depth)
	{
		m_state = State::Cleared;
		m_clear_value.depth = depth;
	}

	void GenerateMipmapsIfNeeded();
	void ClearMipmapGenerationFlag() { m_needs_mipmaps_generated = false; }

	u32 GetMemUsage() const { return m_size.x * m_size.y * (m_format == Format::UNorm8 ? 1 : 4); }

	static bool IsCompressedFormat(Format format) { return (format >= Format::BC1 && format <= Format::BC7); }
};

class GSDownloadTexture
{
public:
	GSDownloadTexture(u32 width, u32 height, GSTexture::Format format);
	virtual ~GSDownloadTexture();

	__fi u32 GetWidth() const { return m_width; }
	__fi u32 GetHeight() const { return m_height; }
	__fi GSTexture::Format GetFormat() const { return m_format; }
	__fi bool NeedsFlush() const { return m_needs_flush; }
	__fi bool IsMapped() const { return (m_map_pointer != nullptr); }
	__fi const u8* GetMapPointer() const { return m_map_pointer; }
	__fi u32 GetMapPitch() const { return m_current_pitch; }

	u32 GetTransferPitch(u32 width, u32 pitch_align) const;

	void GetTransferSize(const GSVector4i& rc, u32* copy_offset, u32* copy_size, u32* copy_rows) const;

	virtual void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch = true) = 0;

	virtual bool Map(const GSVector4i& read_rc) = 0;

	virtual void Unmap() = 0;

	virtual void Flush() = 0;

#ifdef PCSX2_DEVBUILD
	virtual void SetDebugName(std::string_view name) = 0;
#endif

	bool ReadTexels(const GSVector4i& rc, void* out_ptr, u32 out_stride);

	static u32 GetBufferSize(u32 width, u32 height, GSTexture::Format format, u32 pitch_align = 1);

protected:
	u32 m_width;
	u32 m_height;
	GSTexture::Format m_format;

	const u8* m_map_pointer = nullptr;
	u32 m_current_pitch = 0;

	bool m_needs_flush = false;
};
