// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadBase.h"

#include <array>

class PadPopn final : public PadBase
{
public:
	enum Inputs
	{
		PAD_YELLOW_LEFT,
		PAD_YELLOW_RIGHT,
		PAD_BLUE_LEFT,
		PAD_BLUE_RIGHT,
		PAD_WHITE_LEFT,
		PAD_WHITE_RIGHT,
		PAD_GREEN_LEFT,
		PAD_GREEN_RIGHT,
		PAD_RED,
		PAD_START,
		PAD_SELECT,
		LENGTH,
	};

	static constexpr u8 VIBRATION_MOTORS = 2;

private:
	struct Analogs
	{
		u8 lx = Pad::ANALOG_NEUTRAL_POSITION;
		u8 ly = Pad::ANALOG_NEUTRAL_POSITION;
		u8 rx = Pad::ANALOG_NEUTRAL_POSITION;
		u8 ry = Pad::ANALOG_NEUTRAL_POSITION;
		bool lxInvert = false;
		bool lyInvert = false;
		bool rxInvert = false;
		bool ryInvert = false;
	};

	u32 buttons = 0xffffffffu;
	Analogs analogs;
	bool analogPressed = false;
	bool commandStage = false;
	u32 responseBytes = 0;

	static constexpr std::array<u8, Inputs::LENGTH> bitmaskMapping = {{
		5,
		12,
		6,
		7,
		4,
		0,
		3,
		1,
		2,
		11,
		8,
	}};

	void ConfigLog();

	u8 Mystery(u8 commandByte);
	u8 ButtonQuery(u8 commandByte);
	u8 Poll(u8 commandByte);
	u8 Config(u8 commandByte);
	u8 ModeSwitch(u8 commandByte);
	u8 StatusInfo(u8 commandByte);
	u8 Constant1(u8 commandByte);
	u8 Constant2(u8 commandByte);
	u8 Constant3(u8 commandByte);
	u8 VibrationMap(u8 commandByte);
	u8 ResponseBytes(u8 commandByte);

public:
	PadPopn(u8 unifiedSlot, size_t ejectTicks);
	~PadPopn() override;

	Pad::ControllerType GetType() const override;
	const Pad::ControllerInfo& GetInfo() const override;
	void Set(u32 index, float value) override;
	void SetRawAnalogs(const std::tuple<u8, u8> left, const std::tuple<u8, u8> right) override;
	void SetRawPressureButton(u32 index, const std::tuple<bool, u8> value) override;
	void SetAxisScale(float deadzone, float scale) override;
	float GetVibrationScale(u32 motor) const override;
	void SetVibrationScale(u32 motor, float scale) override;
	float GetPressureModifier() const override;
	void SetPressureModifier(float mod) override;
	void SetButtonDeadzone(float deadzone) override;
	void SetAnalogInvertL(bool x, bool y) override;
	void SetAnalogInvertR(bool x, bool y) override;
	float GetEffectiveInput(u32 index) const override;
	u8 GetRawInput(u32 index) const override;
	std::tuple<u8, u8> GetRawLeftAnalog() const override;
	std::tuple<u8, u8> GetRawRightAnalog() const override;
	u32 GetButtons() const override;
	u8 GetPressure(u32 index) const override;
	bool IsAnalogLightEnabled() const override;
	bool IsAnalogLocked() const override;
	
	bool Freeze(StateWrapper& sw) override;

	u8 SendCommandByte(u8 commandByte) override;

	static const Pad::ControllerInfo ControllerInfo;
};
