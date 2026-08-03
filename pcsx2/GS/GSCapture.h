// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <string>
#include <utility>
#include <vector>

#include "common/SmallString.h"
#include "GSVector.h"

namespace Threading
{
class ThreadHandle;
}

class GSTexture;
class GSDownloadTexture;

namespace GSCapture
{
	bool BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename);
	bool DeliverVideoFrame(GSTexture* stex);
	void DeliverAudioPacket(const float* frames);
	void EndCapture();

	bool IsCapturing();
	bool IsCapturingVideo();
	bool IsCapturingAudio();
	TinyString GetElapsedTime();
	const Threading::ThreadHandle& GetEncoderThreadHandle();
	GSVector2i GetSize();
	std::string GetNextCaptureFileName();
	void Flush();

	using CodecName = std::pair<std::string, std::string>;
	using CodecList = std::vector<CodecName>;
	CodecList GetVideoCodecList(const char* container);
	CodecList GetAudioCodecList(const char* container);

	using FormatName = std::pair<int, std::string>;
	using FormatList = std::vector<FormatName>;
	FormatList GetVideoFormatList(const char* codec);
};
