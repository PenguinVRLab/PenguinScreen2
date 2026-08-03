// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/Pad/PadBase.h"

class PadGuitar final : public PadBase
{
public:
	enum Inputs
	{
		STRUM_UP,
		STRUM_DOWN,
		SELECT,
		START,
		GREEN,
		RED,
		YELLOW,
		BLUE,
		ORANGE,
		WHAMMY,
		TILT,
		LENGTH,
	};

private:
	u32 buttons = 0xffffffffu;
	u8 whammy = Pad::ANALOG_NEUTRAL_POSITION;
	bool commandStage = false;
	float whammyAxisScale = 1.0f;
	float whammyDeadzone = 0.0f;
	float buttonDeadzone = 0.0f;

	static constexpr std::array<u8, Inputs::LENGTH> bitmaskMapping = {{
		12,
		14,
		8,
		11,
		1,
		5,
		4,
		6,
		7,
		0
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
	PadGuitar(u8 unifiedSlot, size_t ejectTicks);
	~PadGuitar() override;

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
