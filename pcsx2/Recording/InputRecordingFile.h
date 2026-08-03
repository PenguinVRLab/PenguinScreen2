// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "PadData.h"

#include "common/Pcsx2Defs.h"

#include <optional>
#include <string>
#include <vector>

class InputRecordingFile
{
	struct InputRecordingFileHeader
	{
		u8 m_fileVersion = 1;
		char m_emulatorVersion[50]{};
		char m_author[255]{};
		char m_gameName[255]{};

	public:
		void init() noexcept;
	} m_header = {};


public:
	void setEmulatorVersion();
	void setAuthor(const std::string& author);
	void setGameName(const std::string& cdrom);
	const char* getEmulatorVersion() const noexcept;
	const char* getAuthor() const noexcept;
	const char* getGameName() const noexcept;

	~InputRecordingFile() { close(); }

	bool close() noexcept;
	
	
	bool fromSaveState() const noexcept;
	void incrementUndoCount();
	bool openExisting(const std::string& path);
	bool openNew(const std::string& path, bool fromSaveState);
	std::optional<PadData> readPadData(const uint frame, const uint port, const uint slot);
	void setTotalFrames(u32 frames);
	bool writeHeader() const;
	bool writePadData(const uint frame, const PadData data) const;


	const std::string& getFilename() const noexcept;
	u32 getTotalFrames() const noexcept;
	u32 getUndoCount() const noexcept;

	void logRecordingMetadata();
	std::vector<PadData> bulkReadPadData(u32 frameStart, u32 frameEnd, const uint port);

private:
	static constexpr size_t s_controllerPortsSupported = 2;
	static constexpr size_t s_controllerInputBytes = 18;
	static constexpr size_t s_inputBytesPerFrame = s_controllerInputBytes * s_controllerPortsSupported;
	static constexpr size_t s_headerSize = sizeof(InputRecordingFileHeader) + 4 + 4;
	static constexpr size_t s_recordingSavestateHeaderSize = sizeof(bool);
	static constexpr size_t s_seekpointTotalFrames = sizeof(InputRecordingFileHeader);
	static constexpr size_t s_seekpointUndoCount = sizeof(InputRecordingFileHeader) + 4;
	static constexpr size_t s_seekpointSaveStateHeader = s_seekpointUndoCount + 4;

	std::string m_filename = "";
	FILE* m_recordingFile = nullptr;
	bool m_savestate = false;

	u32 m_totalFrames = 0;
	u32 m_undoCount = 0;

	size_t getRecordingBlockSeekPoint(const u32 frame) const noexcept;
	bool verifyRecordingFileHeader();
};
