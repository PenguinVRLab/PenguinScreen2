// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadBase.h"

#include <array>

static inline bool IsButtonBitSet(u32 value, size_t bit)
{
	return !(value & (1 << bit));
}

class PadDualshock2 final : public PadBase
{
public:
	enum Inputs
	{
		PAD_UP,
		PAD_RIGHT,
		PAD_DOWN,
		PAD_LEFT,
		PAD_TRIANGLE,
		PAD_CIRCLE,
		PAD_CROSS,
		PAD_SQUARE,
		PAD_SELECT,
		PAD_START,
		PAD_L1,
		PAD_L2,
		PAD_R1,
		PAD_R2,
		PAD_L3,
		PAD_R3,
		PAD_ANALOG,
		PAD_PRESSURE,
		PAD_L_UP,
		PAD_L_RIGHT,
		PAD_L_DOWN,
		PAD_L_LEFT,
		PAD_R_UP,
		PAD_R_RIGHT,
		PAD_R_DOWN,
		PAD_R_LEFT,
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
	std::array<u8, VIBRATION_MOTORS> vibrationMotors = {};
	float axisScale = 1.0f;
	float axisDeadzone = 0.0f;
	std::array<float, 2> vibrationScale = {1.0f, 1.0f};
	float pressureModifier = 0.5f;
	float buttonDeadzone = 0.0f;
	u8 smallMotorLastConfig = 0xff;
	u8 largeMotorLastConfig = 0xff;

	static constexpr std::array<u8, Inputs::LENGTH> bitmaskMapping = {{
		12,
		13,
		14,
		15,
		4,
		5,
		6,
		7,
		8,
		11,
		2,
		0,
		3,
		1,
		9,
		10,
		16,
		17,
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
	PadDualshock2(u8 unifiedSlot, size_t ejectTicks);
	~PadDualshock2() override;

	static inline bool IsAnalogKey(int index)
	{
		return ((index >= Inputs::PAD_L_UP) && (index <= Inputs::PAD_R_LEFT));
	}

	static inline bool IsTriggerKey(int index)
	{
		return (index == Inputs::PAD_L2 || index == Inputs::PAD_R2);
	}

	Pad::ControllerType GetType() const override;
	const Pad::ControllerInfo& GetInfo() const override;
	void Set(u32 index, float value) override;
	void SetRawAnalogs(const std::tuple<u8, u8> left, const std::tuple<u8, u8> right) override;
	void SetRawPressureButton(u32 index, const std::tuple<bool, u8> value) override;
	void SetAxisScale(float deadzone, float scale) override;
	float GetAxisScale() const override { return this->axisScale; }
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
