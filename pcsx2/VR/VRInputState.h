// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <array>
#include <cstdint>

namespace VR
{
struct VRPose
{
  std::array<float, 4> orientation_xyzw{0.f, 0.f, 0.f, 1.f};
  std::array<float, 3> position_xyz{0.f, 0.f, 0.f};
  bool valid = false;
};

struct VRHandState
{
  bool a = false;
  bool b = false;
  bool x = false;
  bool y = false;
  bool menu = false;
  bool thumbstick_click = false;
  float trigger = 0.f;
  float grip = 0.f;
  float thumbstick_x = 0.f;
  float thumbstick_y = 0.f;
  VRPose aim_pose;
  VRPose grip_pose;
  std::array<float, 3> grip_linear_velocity{0.f, 0.f, 0.f};
};

struct VRScreenHit
{
  float u = 0.5f;
  float v = 0.5f;
  bool valid = false;
};

struct VRInputSnapshot
{
  static constexpr int LEFT = 0;
  static constexpr int RIGHT = 1;
  std::array<VRHandState, 2> hands;
  VRScreenHit screen_hit;
  VRPose head_pose;
  bool actions_active = false;
  std::uint64_t generation = 0;
};

VRInputSnapshot GetInputSnapshot();

struct VRScreenTransform
{
  VRPose center;
  float half_width = 0.f;
  float half_height = 0.f;
};

VRScreenTransform GetScreenTransform();
}
