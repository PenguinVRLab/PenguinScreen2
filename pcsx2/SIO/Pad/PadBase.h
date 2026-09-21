// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadTypes.h"

#include "StateWrapper.h"

#include <array>

class PadBase
{
public:
	size_t ejectTicks = 0;

protected:
	std::array<u8, 32> rawInputs = {};
	u8 unifiedSlot;
	bool isInConfig = false;
	Pad::Mode currentMode = Pad::Mode::NOT_SET;
	Pad::Command currentCommand = Pad::Command::NOT_SET;
	size_t commandBytesReceived = 0;
	bool analogLight = false;
	bool analogLocked = false;

public:
	PadBase(u8 unifiedSlot, size_t ejectTicks = 0);
	virtual ~PadBase();

	void SoftReset();
	void FullReset();

	virtual Pad::ControllerType GetType() const = 0;
	virtual const Pad::ControllerInfo& GetInfo() const = 0;

	virtual void Set(u32 index, float value) = 0;
	virtual void SetRawAnalogs(const std::tuple<u8, u8> left, const std::tuple<u8, u8> right) = 0;
	virtual void SetRawPressureButton(u32 index, const std::tuple<bool, u8> value) = 0;
	virtual void SetAxisScale(float deadzone, float scale) = 0;
	virtual float GetAxisScale() const { return 1.0f; }
	virtual float GetVibrationScale(u32 motor) const = 0;
	virtual void SetVibrationScale(u32 motor, float scale) = 0;
	virtual float GetPressureModifier() const = 0;
	virtual void SetPressureModifier(float mod) = 0;
	virtual void SetButtonDeadzone(float deadzone) = 0;
	virtual void SetAnalogInvertL(bool x, bool y) = 0;
	virtual void SetAnalogInvertR(bool x, bool y) = 0;
	virtual float GetEffectiveInput(u32 index) const = 0;
	virtual u8 GetRawInput(u32 index) const = 0;
	virtual std::tuple<u8, u8> GetRawLeftAnalog() const = 0;
	virtual std::tuple<u8, u8> GetRawRightAnalog() const = 0;
	virtual u32 GetButtons() const = 0;
	virtual u8 GetPressure(u32 index) const = 0;
	virtual bool IsAnalogLightEnabled() const = 0;
	virtual bool IsAnalogLocked() const = 0;

	virtual bool Freeze(StateWrapper& sw);

	virtual u8 SendCommandByte(u8 commandByte) = 0;
};
