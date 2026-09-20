// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SaveState.h"
#include "IopCounters.h"

#include <memory>

struct Pcsx2Config;

class AudioStream;

namespace SPU2
{
static constexpr u32 SAMPLE_RATE = 48000;

static constexpr u32 PSX_SAMPLE_RATE = 44100;

bool Open();
void Close();

void Reset(bool psxmode);

void CheckForConfigChanges(const Pcsx2Config& old_config);

u32 GetOutputVolume();

void SetOutputVolume(u32 volume);

bool SetOutputMuted(const bool muted);

bool IsOutputMuted();

void UpdateOutputVolume();

void SaveOutputVolume();

void SetOutputPaused(bool paused);

void OnTargetSpeedChanged();

bool IsRunningPSXMode();

u32 GetConsoleSampleRate();

void SetAudioCaptureActive(bool active);
bool IsAudioCaptureActive();
}

void SPU2write(u32 mem, u16 value);
u16 SPU2read(u32 mem);

void SPU2async();
s32 SPU2freeze(FreezeAction mode, freezeData* data);

void SPU2readDMA4Mem(u16* pMem, u32 size);
void SPU2writeDMA4Mem(u16* pMem, u32 size);
void SPU2interruptDMA4();
void SPU2interruptDMA7();
void SPU2readDMA7Mem(u16* pMem, u32 size);
void SPU2writeDMA7Mem(u16* pMem, u32 size);

extern u64 lClocks;

extern void CounterUpdate(u32 DMAICounter);
extern void TimeUpdate(u32 cClocks);
extern void SPU2_FastWrite(u32 rmem, u16 value);

