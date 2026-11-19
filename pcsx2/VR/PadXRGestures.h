// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VR::PadXRGestures
{
	using SpatialControls::Quat;
	using SpatialControls::Vec3;

	namespace Btn
	{
		static constexpr int None = -1;
		static constexpr int L2 = 0, R2 = 1, L1 = 2, R1 = 3, Triangle = 4, Circle = 5, Cross = 6, Square = 7;
		static constexpr int Select = 8, L3 = 9, R3 = 10, Start = 11, Up = 12, Right = 13, Down = 14, Left = 15;
	}

	enum class Gesture : uint8_t
	{
		Reach,
		Contact,
		Swing,
		Shake,
		Carry,
		Throw,
		MeshHandoff,
	};

	struct TitlePress
	{
		int bit = Btn::None;
		uint32_t transactions = 0;
		int hold_bit = Btn::None;
		uint32_t hold_lead = 0;
		uint8_t pressure = 0xFF;
	};

	uint32_t TitleCount(const TitlePress& tp);

	struct PressStep
	{
		int bit = Btn::None;
		uint32_t start_tx = 0;
		float start_ms = 0.0f;
		uint32_t min_tx = 0;
		float hold_ms = 0.0f;
		int through_step = -1;
		uint8_t pressure = 0xFF;
	};

	static constexpr size_t kMaxSteps = 4;

	enum class Refusal : uint8_t
	{
		None,
		NoTitleCount,
		NoButton,
		GateUnparsed,
		AssistOnline,
		NoClassThreshold,
	};

	struct PressPlan
	{
		Gesture gesture = Gesture::Reach;
		uint32_t seq = 0;
		double decided_ms = 0.0;
		double dated_ms = 0.0;
		std::array<PressStep, kMaxSteps> steps{};
		uint8_t count = 0;
		uint16_t withhold_mask = 0;
		bool neutral_left_stick = false;
		int hand = -1;
		int target = -1;
		uint8_t throw_class = 0;
	};

	struct Cue
	{
		uint8_t hand = 0;
		uint8_t locus = 0;
		float amplitude = 0.0f;
		uint16_t ms = 0;
	};

	struct Result
	{
		bool fired = false;
		PressPlan plan;
		bool cancel = false;
		Refusal refusal = Refusal::None;
		std::array<Cue, 2> cues{};
		uint8_t cue_count = 0;
	};

	struct Gate
	{
		bool parsed = true;
		bool open = true;
	};

	struct TxStep
	{
		int bit = Btn::None;
		uint32_t start = 0;
		uint32_t count = 0;
		uint8_t pressure = 0xFF;
	};

	size_t ToTransactions(const PressPlan& plan, float tx_per_second, std::array<TxStep, kMaxSteps>* out,
		bool* incomplete);

	bool AddPress(PressPlan* plan, const TitlePress& tp, float start_ms, float hold_ms, Refusal* why);

	struct BodyFrame
	{
		Vec3 origin;
		float yaw = 0.0f;
		bool valid = false;
	};
	inline constexpr float kYawHoldHorizontal = 0.17f;
	BodyFrame UpdateBodyFrame(const BodyFrame& last, const VRPose& head);

	struct Zone
	{
		float side = 0.0f;
		float height = 0.0f;
		float forward = 0.0f;
		float radius = 0.20f;
		TitlePress press;
	};
	Vec3 ZoneCentre(const BodyFrame& body, const Zone& z);

	struct Motion
	{
		static constexpr float kGap = -1.0f;
		static constexpr float kRepeat = -2.0f;
		bool have = false;
		Vec3 last;
		double last_ms = 0.0;
		float Update(const Vec3& pos, double now_ms, float max_gap_ms);
		void Reset() { have = false; }
	};

	struct GripEdge
	{
		bool closed = false;
		bool Step(float grip, float on, float off);
	};

	struct ReachParams
	{
		float exitMarginM = 0.03f;
		float gripOn = 0.6f;
		float gripOff = 0.4f;
		float pressDelayMs = 250.0f;
		bool releaseMoveStick = true;
	};

	class ReachDetector
	{
	public:
		static constexpr size_t kMaxZones = 8;

		ReachParams params;
		std::array<Zone, kMaxZones> zones{};
		size_t zone_count = 0;

		Result Step(const VRInputSnapshot& s, double now_ms, Gate when = {});
		void Reset();
		int ZoneOf(int hand) const { return m_hand[hand].zone; }
		uint32_t Entries() const { return m_entries; }

	private:
		struct HandState
		{
			int zone = -1;
			bool open_seen = false;
			GripEdge grip;
		};
		std::array<HandState, 2> m_hand{};
		BodyFrame m_body{};
		uint32_t m_seq = 0;
		uint32_t m_entries = 0;
	};

	struct GameCamera
	{
		std::array<float, 12> view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
		std::array<float, 9> axes{1, 0, 0, 0, 1, 0, 0, 0, 1};
		float metres_per_unit = 1.0f;
	};

	bool HandToGameWorld(const Vec3& hand, const VRPose& head, const GameCamera& cam, Vec3* world);

	float GameDistanceM(const Vec3& a, const Vec3& b, const GameCamera& cam);

	struct ContactObject
	{
		Vec3 position;
		bool valid = true;
		TitlePress press;
	};

	struct ContactParams
	{
		float radiusM = 0.15f;
		uint32_t heldSamples = 2;
		float gripOn = 0.6f;
		float gripOff = 0.4f;
	};

	class ContactDetector
	{
	public:
		static constexpr size_t kMaxObjects = 16;

		ContactParams params;
		GameCamera camera;
		std::array<ContactObject, kMaxObjects> objects{};
		size_t object_count = 0;

		Result Step(const VRInputSnapshot& s, double now_ms, Gate gameplay = {});
		void Reset();

	private:
		struct HandState
		{
			int object = -1;
			uint32_t held = 0;
			bool open_seen = false;
			GripEdge grip;
		};
		std::array<HandState, 2> m_hand{};
		uint32_t m_seq = 0;
	};

	struct SwingParams
	{
		float tipM = 0.40f;
		float speedOn = 2.2f;
		float speedOff = 0.8f;
		float travelM = 0.20f;
		float straightness = 0.6f;
		float pressMs = 100.0f;
		float cooldownMs = 250.0f;
		int hand = VRInputSnapshot::RIGHT;
		float maxGapMs = 100.0f;
	};

	class SwingDetector
	{
	public:
		SwingParams params;
		TitlePress attack;

		Result Step(const VRInputSnapshot& s, double now_ms, bool hand_free = true, Gate when = {});
		void Reset();

	private:
		Motion m_motion;
		bool m_in_stroke = false;
		bool m_stroke_fired = false;
		Vec3 m_start;
		float m_path = 0.0f;
		double m_last_fire_ms = -1e18;
		uint32_t m_seq = 0;
	};

	enum class ShakeMode : uint8_t
	{
		Embodied,
		Assist,
	};

	struct ShakeParams
	{
		float speed = 1.0f;
		uint32_t reversals = 3;
		float reversalWindowMs = 300.0f;
		float reversalMinSpeed = 0.25f;
		float holdMs = 250.0f;
		ShakeMode mode = ShakeMode::Embodied;
		float assistPressHz = 8.0f;
		float assistPressMs = 60.0f;
		float assistStickHz = 6.0f;
		float assistJitter = 0.2f;
		bool assistStick = false;
		float maxGapMs = 100.0f;
	};

	struct AssistSchedule
	{
		bool on = false;
		float press_hz = 0.0f;
		float press_ms = 0.0f;
		float stick_hz = 0.0f;
		float jitter = 0.0f;
		std::array<int, 2> bits{Btn::None, Btn::None};
		uint32_t transactions = 0;
	};

	float AssistPressStartMs(const AssistSchedule& a, uint32_t k, uint32_t seed);

	class ShakeDetector
	{
	public:
		ShakeParams params;
		std::array<TitlePress, 2> buttons{};

		Result Step(const VRInputSnapshot& s, double now_ms, Gate grabbed, uint16_t player_down = 0,
			bool online = false);
		void Reset();
		const AssistSchedule& Assist() const { return m_assist; }
		bool Active() const { return m_active; }

	private:
		struct HandState
		{
			Motion motion;
			bool have_dir = false;
			Vec3 dir;
		};
		std::array<HandState, 2> m_hand{};
		std::array<double, 16> m_reversal_ms{};
		size_t m_reversal_count = 0;
		size_t m_reversal_head = 0;
		double m_active_until = -1e18;
		bool m_active = false;
		bool m_running = false;
		uint32_t m_next_button = 0;
		AssistSchedule m_assist{};
		uint32_t m_seq = 0;
	};

	struct CarryParams
	{
		float withinMs = 1500.0f;
		float trackingGraceMs = 200.0f;
		float toRadiusM = 0.15f;
		float pressMs = 600.0f;
		float gripOn = 0.6f;
		float gripOff = 0.4f;
	};

	class CarryDetector
	{
	public:
		CarryParams params;
		std::array<Zone, 2> source{};
		TitlePress reload;
		int fire_bit = Btn::None;

		Result Step(const VRInputSnapshot& s, double now_ms, Gate ready = {});
		void Reset();
		bool Carrying(int hand) const { return m_hand[hand].carrying; }

	private:
		struct HandState
		{
			bool in_source = false;
			bool open_seen = false;
			bool carrying = false;
			double grabbed_ms = 0.0;
			double lost_since = -1.0;
			GripEdge grip;
		};
		std::array<HandState, 2> m_hand{};
		BodyFrame m_body{};
		uint32_t m_seq = 0;
	};

	enum class PoseSource : uint8_t
	{
		Controllers,
		Hands,
	};

	struct ThrowParams
	{
		float gripOn = 0.8f;
		float gripOff = 0.6f;
		float peakWindowMs = 50.0f;
		float backDateControllersMs = 11.0f;
		float backDateHandsMs = 80.0f;
		PoseSource source = PoseSource::Controllers;
		float lobTapMs = 60.0f;
		float bulletHoldMs = 250.0f;
		float rethrowBlockMs = 300.0f;
		float bulletSpeedMps = 0.0f;
		float minThrowSpeedMps = 0.0f;
	};

	class ThrowDetector
	{
	public:
		ThrowParams params;

		Result Step(const VRInputSnapshot& s, double now_ms, const TitlePress& receiver, Gate possession = {});
		void Reset();

	private:
		struct Sample
		{
			double ms = 0.0;
			float speed = 0.0f;
		};
		struct HandState
		{
			Motion motion;
			bool holding = false;
			bool lost = false;
			GripEdge grip;
			std::array<Sample, 32> hist{};
			size_t hist_count = 0;
			size_t hist_head = 0;
		};
		std::array<HandState, 2> m_hand{};
		double m_last_throw_ms = -1e18;
		uint32_t m_seq = 0;
	};

	struct MeshParams
	{
		float targetRadiusM = 0.08f;
		float handSpeedGate = 1.5f;
		Cue entryCue{0, 0, 0.4f, 40};
		Cue completeCue{0, 0, 0.8f, 80};
		float completeWindowMs = 150.0f;
		float commitOn = 0.6f;
		float commitOff = 0.4f;
		float maxGapMs = 100.0f;
	};

	class MeshHandoffDetector
	{
	public:
		MeshParams params;
		TitlePress landing;

		Result Step(const VRInputSnapshot& s, double now_ms, const Vec3& target, bool target_valid,
			Gate handoff_play = {});
		void Reset();

	private:
		struct HandState
		{
			Motion motion;
			float speed = -1.0f;
			bool in_target = false;
			double entered_ms = 0.0;
			bool done = false;
			GripEdge commit;
		};
		std::array<HandState, 2> m_hand{};
		uint32_t m_seq = 0;
	};
}

#if __has_include("VR/PadXRCore.h")
#include "VR/PadXRCore.h"
#define VR_PADXR_GESTURES_HAVE_CORE 1

namespace VR::PadXRGestures
{
	PadXR::MemberDesc Describe(Gesture g);

	class PlanRunner
	{
	public:
		void Load(const PressPlan& plan, float tx_per_second);
		void Cancel();
		bool Running() const { return m_running; }
		void Step(PadXR::EdgeScheduler* sched, PadXR::WordCell* word, PadXR::StickCell* lx, PadXR::StickCell* ly);

	private:
		std::array<TxStep, kMaxSteps> m_steps{};
		size_t m_count = 0;
		uint32_t m_tx = 0;
		uint32_t m_end = 0;
		uint16_t m_withhold = 0;
		bool m_neutral_stick = false;
		bool m_running = false;
	};
}
#endif
