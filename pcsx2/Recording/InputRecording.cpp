// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "MTGS.h"
#include "SaveState.h"

bool SaveStateBase::InputRecordingFreeze()
{
	if (!FreezeTag("InputRecording"))
		return false;

	Freeze(g_FrameCount);
	return IsOkay();
}

#include "InputRecording.h"

#include "InputRecordingControls.h"
#include "Utilities/InputRecordingLogger.h"

#include "common/FileSystem.h"
#include "common/StringUtil.h"
#include "Counters.h"
#include "SaveState.h"
#include "VMManager.h"
#include "Host.h"
#include "ImGui/ImGuiOverlays.h"
#include "DebugTools/Debug.h"
#include "GameDatabase.h"
#include "fmt/format.h"
#include "GS.h"
#include "Host.h"

InputRecording g_InputRecording;

bool InputRecording::create(const std::string& fileName, const bool fromSaveState, const std::string& authorName)
{
	if (!m_file.openNew(fileName, fromSaveState))
	{
		return false;
	}

	m_controls.setRecordMode();
	if (fromSaveState)
	{
		std::string savestatePath = fmt::format("{}_SaveState.p2s", fileName);
		if (FileSystem::FileExists(savestatePath.c_str()))
		{
			FileSystem::CopyFilePath(savestatePath.c_str(), fmt::format("{}.bak", savestatePath).c_str(), true);
		}
		m_type = Type::FROM_SAVESTATE;
		m_is_active = true;
		m_initial_load_complete = true;
		m_watching_for_rerecords = true;
		setStartingFrame(g_FrameCount);
		VMManager::SaveState(savestatePath.c_str(), true, false, [](const std::string& error) {
			SaveState_ReportSaveErrorOSD(error, std::nullopt);
		});
	}
	else
	{
		m_starting_frame = 0;
		m_type = Type::POWER_ON;
		m_initial_load_complete = false;
		m_is_active = true;
		VMManager::Reset();
	}

	m_file.setEmulatorVersion();
	m_file.setAuthor(authorName);
	m_file.setGameName(VMManager::GetTitle(false));
	m_file.writeHeader();
	initializeState();
	InputRec::log(TRANSLATE_STR("InputRecording", "Started new input recording"), Host::OSD_INFO_DURATION);
	InputRec::consoleLog(fmt::format("Filename {}", m_file.getFilename()));
	return true;
}

bool InputRecording::play(const std::string& filename)
{
	if (!m_file.openExisting(filename))
	{
		return false;
	}

	if (m_file.fromSaveState())
	{
		std::string savestatePath = fmt::format("{}_SaveState.p2s", m_file.getFilename());
		if (!FileSystem::FileExists(savestatePath.c_str()))
		{
			InputRec::consoleLog(fmt::format("Could not locate savestate file at location - {}", savestatePath));
			InputRec::log(TRANSLATE_STR("InputRecording", "Failed to load state for input recording"), Host::OSD_ERROR_DURATION);
			m_file.close();
			return false;
		}
		m_type = Type::FROM_SAVESTATE;
		m_initial_load_complete = false;
		m_is_active = true;
		const auto loaded = VMManager::LoadState(savestatePath.c_str());
		if (!loaded)
		{
			InputRec::log(TRANSLATE_STR("InputRecording", "Failed to load state for input recording, unsupported version?"), Host::OSD_ERROR_DURATION);
			m_file.close();
			m_is_active = false;
			return false;
		}
	}
	else
	{
		m_starting_frame = 0;
		m_type = Type::POWER_ON;
		m_initial_load_complete = false;
		m_is_active = true;
		VMManager::Reset();
	}
	m_controls.setReplayMode();
	initializeState();
	InputRec::log(TRANSLATE_STR("InputRecording", "Replaying input recording"), Host::OSD_INFO_DURATION);
	m_file.logRecordingMetadata();
	if (VMManager::GetTitle(false) != m_file.getGameName())
	{
		InputRec::consoleLog(fmt::format("Input recording was possibly constructed for a different game. Expected: {}, Actual: {}", m_file.getGameName(), VMManager::GetTitle(false)));
	}
	return true;
}

void InputRecording::closeActiveFile()
{
	if (!m_is_active)
	{
		return;
	}
	if (m_file.close())
	{
		m_is_active = false;
		InputRec::log(TRANSLATE_STR("InputRecording", "Input recording stopped"), Host::OSD_ERROR_DURATION);
		MTGS::PresentCurrentFrame();
	}
	else
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "Unable to stop input recording"), Host::OSD_ERROR_DURATION);
	}
}

void InputRecording::stop()
{
	if (VMManager::GetState() == VMState::Paused)
	{
		closeActiveFile();
	}
	else
	{
		m_recordingQueue.push([&]() {
			closeActiveFile();
		});
	}
}

void InputRecording::handleControllerDataUpdate()
{
	for (int i = 0; i < 2; i++)
	{
		PadData frameData(i, 0);
		if (m_is_active)
		{
			if (m_controls.isRecording())
			{
				saveControllerData(frameData, i, 0);
			}
			else if (m_controls.isReplaying())
			{
				const auto& modifiedFrameData = updateControllerData(i, 0);
				if (modifiedFrameData)
				{
					frameData = modifiedFrameData.value();
				}
			}
		}
		frameData.LogPadData();
	}
}

void InputRecording::saveControllerData(const PadData& data, const int port, const int slot)
{
	if (!m_file.writePadData(m_frame_counter, data))
	{
		InputRec::consoleLog(fmt::format("Failed to write input data at [{}:{}:{}]", m_frame_counter, port, slot));
	}
}
std::optional<PadData> InputRecording::updateControllerData(const int port, const int slot)
{
	const auto frameData = m_file.readPadData(m_frame_counter, port, slot);
	if (frameData)
	{
		frameData->OverrideActualController();
	}
	else
	{
		InputRec::consoleLog(fmt::format("Failed to read input data at [{}:{}:{}]", m_frame_counter, port, slot));
	}
	return frameData;
}

void InputRecording::processRecordQueue()
{
	while (!m_recordingQueue.empty())
	{
		m_recordingQueue.front()();
		m_recordingQueue.pop();
	}
}

void InputRecording::incFrameCounter()
{
	if (!m_is_active)
	{
		return;
	}

	if (m_frame_counter == std::numeric_limits<u32>::max())
	{
		InputRec::log(TRANSLATE_STR("InputRecording", "Congratulations, you've been playing for far too long and thus have reached the limit of input recording! Stopping recording now..."), Host::OSD_CRITICAL_ERROR_DURATION);
		stop();
		return;
	}
	m_frame_counter++;

	if (m_controls.isReplaying())
	{
		InformGSThread();
		if (m_frame_counter == m_file.getTotalFrames())
		{
			VMManager::SetPaused(true);
			m_watching_for_rerecords = false;
		}
	}
	if (m_controls.isRecording())
	{
		m_frame_counter_stateless++;
		m_file.setTotalFrames(m_frame_counter);
		if (m_watching_for_rerecords)
		{
			m_file.incrementUndoCount();
			m_watching_for_rerecords = false;
		}
		InformGSThread();
	}
}

u32 InputRecording::getFrameCounter() const
{
	return m_frame_counter;
}

u32 InputRecording::getFrameCounterStateless() const
{
	return m_frame_counter_stateless;
}

bool InputRecording::isActive() const
{
	return m_is_active;
}

void InputRecording::handleExceededFrameCounter()
{
	if (m_frame_counter >= m_file.getTotalFrames() && m_controls.isReplaying())
	{
		m_controls.setRecordMode(false);
	}
}

void InputRecording::handleReset()
{
	if (m_initial_load_complete)
	{
		adjustFrameCounterOnReRecord(0);
	}
	m_initial_load_complete = true;
}

void InputRecording::handleLoadingSavestate()
{
	if (isTypeSavestate() && !m_initial_load_complete)
	{
		setStartingFrame(g_FrameCount);
		m_initial_load_complete = true;
	}
	else
	{
		adjustFrameCounterOnReRecord(g_FrameCount);
		m_watching_for_rerecords = true;
	}
}

bool InputRecording::isTypeSavestate() const
{
	return m_type == Type::FROM_SAVESTATE;
}

void InputRecording::setStartingFrame(u32 startingFrame)
{
	if (m_type == Type::POWER_ON)
	{
		return;
	}
	InputRec::consoleLog(fmt::format("Internal Starting Frame: {}", startingFrame));
	m_starting_frame = startingFrame;
	InformGSThread();
}

u32 InputRecording::getStartingFrame()
{
	return m_starting_frame;
}

void InputRecording::adjustFrameCounterOnReRecord(u32 newFrameCounter)
{
	if (newFrameCounter > m_starting_frame + m_file.getTotalFrames())
	{
		InputRec::consoleLog("Warning, you've loaded PCSX2 emulation to a point after the end of the original recording. This should be avoided.");
		InputRec::consoleLog("Save state's framecount has been ignored, using the max length of the recording instead.");
		m_frame_counter = m_file.getTotalFrames();
		if (getControls().isReplaying())
		{
			getControls().setRecordMode();
		}
		return;
	}
	if (newFrameCounter < m_starting_frame)
	{
		InputRec::consoleLog("Warning, you've loaded PCSX2 emulation to a point before the start of the original recording. This should be avoided.");
		InputRec::consoleLog("Save state's framecount has been ignored, starting from the beginning in replay mode.");
		m_frame_counter = 0;
		if (getControls().isRecording())
		{
			getControls().setReplayMode();
		}
		return;
	}
	else if (newFrameCounter == 0 && getControls().isRecording())
	{
		getControls().setReplayMode();
	}
	m_frame_counter = newFrameCounter - m_starting_frame;
	m_frame_counter_stateless--;
	m_file.setTotalFrames(m_frame_counter);
	InformGSThread();
}

InputRecordingControls& InputRecording::getControls()
{
	return m_controls;
}

const InputRecordingFile& InputRecording::getData() const
{
	return m_file;
}

void InputRecording::initializeState()
{
	m_frame_counter = 0;
	m_watching_for_rerecords = false;
	InformGSThread();
}

void InputRecording::InformGSThread()
{
	TinyString recording_active_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Input Recording Active: {}"), g_InputRecording.getData().getFilename());
	TinyString frame_data_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Frame: {}/{} ({})"), g_InputRecording.getFrameCounter(), g_InputRecording.getData().getTotalFrames(), g_InputRecording.getFrameCounterStateless());
	TinyString undo_count_message = TinyString::from_format(TRANSLATE_FS("InputRecording", "Undo Count: {}"), g_InputRecording.getData().getUndoCount());

	MTGS::RunOnGSThread([recording_active_message, frame_data_message, undo_count_message](bool is_recording = g_InputRecording.getControls().isRecording()) {
		g_InputRecordingData.is_recording = is_recording;
		g_InputRecordingData.recording_active_message = recording_active_message;
		g_InputRecordingData.frame_data_message = frame_data_message;
		g_InputRecordingData.undo_count_message = undo_count_message;
	});
}
