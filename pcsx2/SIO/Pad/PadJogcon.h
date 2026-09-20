// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadBase.h"

class PadJogcon final : public PadBase
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

		PADDING1, PADDING2, PADDING3, PADDING4,

		PAD_DIAL_LEFT,
		PAD_DIAL_RIGHT,
		LENGTH,
	};

	static constexpr u8 VIBRATION_MOTORS = 2;

private:
	u32 buttons = 0xffffffffu;
	s16 dial = 0x0000;
	s16 lastdial = 0x0000;

	bool commandStage = false;
	std::array<u8, VIBRATION_MOTORS> vibrationMotors = {};
	std::array<float, 2> vibrationScale = {1.0f, 1.0f};
	float dialDeadzone = 0.0f;
	float dialScale = 1.0f;
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

public:
	PadJogcon(u8 unifiedSlot, size_t ejectTicks);
	~PadJogcon() override;

	static inline bool IsAnalogKey(int index)
	{
		return index == Inputs::PAD_DIAL_LEFT || index == Inputs::PAD_DIAL_RIGHT;
	}

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
