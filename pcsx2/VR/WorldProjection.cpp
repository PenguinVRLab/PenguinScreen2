// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0+

#include "VR/WorldProjection.h"

#include "VR/CameraDriver.h"
#include "VR/ExecHook.h"
#include "VR/GuestRam.h"
#include "VR/StereoState.h"
#include "VR/VRManager.h"
#include "VR/VRProfileDB.h"

#include "GS/Renderers/Common/GSRenderer.h"
#include "Config.h"
#include "Host.h"
#include "MTGS.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
	using namespace VR::WorldProjection;


	EeSide s_ee;
	const VR::ProfileDB::ProjectionProfile* s_ee_last_profile = nullptr;
	u64 s_ee_head_seq = 0;

	enum class HookState : int
	{
		Idle = 0,
		Queued = 1,
		Armed = 2,
		Refused = 3,
	};
	std::atomic<int> s_hook_state{static_cast<int>(HookState::Idle)};
	u32 s_hook_pc = 0;
	u64 s_hook_retry = 0;
	u64 s_hook_vsyncs = 0;

	GsSide s_gs;
	[[maybe_unused]] const bool s_gs_bound = [] {
		s_gs.display_fbp = []() { return g_gs_renderer ? g_gs_renderer->VRDisplayFbp() : Stamps::kNoBuffer; };
		s_gs.SetFarShift([]() { return VR::StereoState::Get().FarField(); });
		return true;
	}();

	std::mutex s_present_mutex;
	Presented s_last_present;

	void SetLastPresent(const Presented& p)
	{
		std::lock_guard<std::mutex> lock(s_present_mutex);
		s_last_present = p;
	}


	Source ToSource(const VR::ProfileDB::ProjectionProfile& p)
	{
		using PP = VR::ProfileDB::ProjectionProfile;
		Source s;
		s.drawn = (p.drawn == PP::Drawn::Matrix) ? Source::Drawn::Matrix :
		          (p.drawn == PP::Drawn::LookAt) ? Source::Drawn::LookAt : Source::Drawn::Copy;
		s.look_eye_offset = p.look_eye_offset;
		s.look_target_offset = p.look_target_offset;
		s.yaw_address = p.yaw_address;
		s.has_pitch = p.pitch_address.has_value();
		s.pitch_address = p.pitch_address.value_or(0);
		s.has_fov_readback = p.fov_readback.has_value();
		s.fov_readback = p.fov_readback.value_or(0);
		const auto body = [](PP::Body b) {
			return b == PP::Body::Identity ? Source::Body::Identity :
			       b == PP::Body::MinusOwn ? Source::Body::MinusOwn : Source::Body::Anchored;
		};
		s.yaw_body = body(p.yaw_body);
		s.yaw_op = p.yaw_op.value_or(0);
		if (!s.has_pitch || (!p.general && !p.pitch_op.has_value()))
			s.pitch_body = Source::Body::Latched;
		else
			s.pitch_body = body(p.pitch_body);
		s.pitch_op = p.pitch_op.value_or(0);
		s.matrix_address = p.matrix_address;
		s.roll_declared = p.roll_declared;
		s.has_view = p.view;
		s.model = ViewModel{p.yaw_sign, p.pitch_sign, p.tan_x, p.tan_y,
			(p.frustum_model == PP::FrustumModel::Constant) ? FrustumModel::Constant : FrustumModel::Degrees};
		s.frame_age = p.frame_age;
		s.write_to_read = p.write_to_read;
		for (size_t i = 0; i < p.frame_ages.size() && i < static_cast<size_t>(Timing::kMaxAges); i++)
			s.frame_ages[i] = p.frame_ages[i];
		s.latch_first_draw = p.latch_first_draw;
		s.read_to_draw = p.read_to_draw;
		s.latch_closing = p.latch_closing;
		if (p.drawn == PP::Drawn::Hook)
		{
			s.drawn = Source::Drawn::Hook;
			s.hook_offset = p.hook_offset;
			for (int i = 0; i < 3; i++)
				s.basis.axis[i] = p.hook_basis[i];
			s.hook_body = (p.hook_body == PP::HookBody::Rest) ? HookBody::Rest : HookBody::Pre;
			s.visit_max_age = p.visit_max_age;
		}
		return s;
	}

	class LiveSources final : public EeSources
	{
	public:
		explicit LiveSources(const std::vector<VR::ProfileDB::CameraGuard>& when)
			: m_when(when)
		{
		}
		std::optional<float> F32(u32 address) override
		{
			return VR::GuestRam::DecodeGuestValue(m_ee, address, VR::ProfileDB::CameraEncoding::F32);
		}
		bool GatesPass() override { return VR::GuestRam::GuardListPass(m_ee, m_when); }
		bool AnchoredBase(u32 op, float* base) override { return VR::CameraDriver::AnchoredBase(op, base); }
		bool Base(u32* base) override { return VR::CameraDriver::ResolvedBaseNow(base); }
		bool DeltaTotal(u32 op, float* total) override { return VR::CameraDriver::DeltaTotal(op, total); }
		bool Reference(Quat* q, float p[3]) override
		{
			float r[4];
			if (!VR::CameraDriver::Reference(r, p))
				return false;
			*q = Quat{r[0], r[1], r[2], r[3]};
			return true;
		}
		bool LastVisit(Visit* v) override
		{
			if (m_hook_pc == 0)
				return false;
			VR::ExecVisitRecord r;
			if (!VR::ExecHookLastVisit(m_hook_pc, &r))
				return false;
			v->seq = r.seq;
			v->base = r.base;
			for (int i = 0; i < 9; i++)
			{
				v->pre[i] = r.pre[i];
				v->lever[i] = r.lever[i];
			}
			v->has_lever = r.has_lever;
			return true;
		}
		void SetHookPc(u32 pc) { m_hook_pc = pc; }
		bool Head(Quat* q) override
		{
			float h[4];
			u64 seq = 0;
			if (!VR::CameraDriver::HeadRelative(h, &seq) || seq == s_ee_head_seq)
				return false;
			s_ee_head_seq = seq;
			*q = Quat{h[0], h[1], h[2], h[3]};
			return true;
		}

	private:
		EEMemoryInterface m_ee;
		const std::vector<VR::ProfileDB::CameraGuard>& m_when;
		u32 m_hook_pc = 0;
	};

	void ManageRecordHook(const VR::ProfileDB::ProjectionProfile* proj, bool on)
	{
		s_hook_vsyncs++;
		const bool want = on && proj && proj->drawn == VR::ProfileDB::ProjectionProfile::Drawn::Hook;
		const HookState st = static_cast<HookState>(s_hook_state.load());
		if (!want || (st != HookState::Idle && s_hook_pc != proj->hook_pc))
		{
			if (st != HookState::Idle)
			{
				s_hook_state.store(static_cast<int>(HookState::Idle));
				s_hook_pc = 0;
				Host::RunOnCPUThread([]() { VR::ExecHookClearAll(VR::ExecOwner::Projection); });
			}
			if (!want)
				return;
		}
		if (st == HookState::Refused && s_hook_vsyncs >= s_hook_retry)
			s_hook_state.store(static_cast<int>(HookState::Idle));
		if (static_cast<HookState>(s_hook_state.load()) != HookState::Idle)
			return;
		if (memRead32(proj->hook_pc) != proj->hook_expect)
			return;
		VR::ExecHookSpec spec;
		spec.pc = proj->hook_pc;
		spec.expect_op = proj->hook_expect;
		spec.action = VR::ExecAction::Record;
		spec.reg = proj->hook_reg;
		spec.record_offset = proj->hook_offset;
		spec.record_lever = proj->lever_address;
		spec.owner = VR::ExecOwner::Projection;
		int g = 0;
		for (const VR::ProfileDB::CameraGuard& w : proj->when)
		{
			if (w.is_chain || g >= VR::EXEC_GATES_MAX)
				continue;
			spec.gates[g].address = w.ee_address;
			spec.gates[g].equals = w.equals;
			spec.gates[g].width = w.width;
			spec.gates[g].not_equals = w.not_equals;
			g++;
		}
		s_hook_pc = proj->hook_pc;
		s_hook_state.store(static_cast<int>(HookState::Queued));
		Host::RunOnCPUThread([spec]() {
			const bool ok = VR::ExecHookArm(spec) >= 0;
			Console.WriteLn(ok ? fmt::format("(VR) world projection: visit record armed at {:#010x}.", spec.pc) :
			                     fmt::format("(VR) world projection: visit record at {:#010x} REFUSED; retrying once a second.", spec.pc));
			s_hook_state.store(static_cast<int>(ok ? HookState::Armed : HookState::Refused));
		});
		s_hook_retry = s_hook_vsyncs + 60;
	}

	GateTally s_tally;
	const VR::ProfileDB::ProjectionProfile* s_tally_proj = nullptr;
	std::vector<std::vector<VR::ProfileDB::CameraGuard>> s_tally_single;

	void TallyRead(const VR::ProfileDB::ProjectionProfile& proj, const Pose& pose)
	{
		if (s_tally_proj != &proj)
		{
			s_tally_proj = &proj;
			s_tally_single.clear();
			const int n = static_cast<int>(std::min<size_t>(proj.when.size(), GateTally::kMaxGates));
			std::uint32_t addr[GateTally::kMaxGates], eq[GateTally::kMaxGates];
			std::uint8_t width[GateTally::kMaxGates];
			bool ne[GateTally::kMaxGates], chain[GateTally::kMaxGates];
			for (int i = 0; i < n; i++)
			{
				const VR::ProfileDB::CameraGuard& g = proj.when[i];
				s_tally_single.push_back({g});
				addr[i] = g.is_chain ? g.pointer_addr : g.ee_address;
				eq[i] = g.equals;
				width[i] = static_cast<std::uint8_t>(g.width);
				ne[i] = g.not_equals;
				chain[i] = g.is_chain;
			}
			s_tally.Begin(n, addr, eq, width, ne, chain);
		}
		EEMemoryInterface ee;
		bool held[GateTally::kMaxGates];
		std::uint32_t value[GateTally::kMaxGates];
		for (int i = 0; i < s_tally.count; i++)
		{
			held[i] = VR::GuestRam::GuardListPass(ee, s_tally_single[i]);
			const VR::ProfileDB::CameraGuard& g = proj.when[i];
			value[i] = g.is_chain ? 0u : (g.width == 1) ? ee.Read8(g.ee_address) : (g.width == 2) ? ee.Read16(g.ee_address) : ee.Read32(g.ee_address);
		}
		WhyNot why = WhyNot::Declarable;
		if (!pose.declarable)
		{
			const bool hook = proj.drawn == VR::ProfileDB::ProjectionProfile::Drawn::Hook;
			if (hook && pose.visit_vsync == 0)
				why = WhyNot::NoVisit;
			else if (hook && (pose.ee_vsync < pose.visit_vsync || pose.ee_vsync - pose.visit_vsync > proj.visit_max_age))
				why = WhyNot::StaleVisit;
			else if (!pose.has_view)
				why = WhyNot::NoView;
			else
				why = WhyNot::Refused;
		}
		s_tally.Add(held, value, pose.declarable, why);
		if (s_tally.Full())
		{
			Console.WriteLn(s_tally.Line());
			s_tally.Restart();
		}
	}

	void PostToGs(EeSide::GsWork work)
	{
		MTGS::RunOnGSThread([work = std::move(work)]() {
			work(s_gs);
			SetLastPresent(s_gs.Last());
		});
	}
}

namespace VR::WorldProjection
{
	void OnEeVsync()
	{
		if (
			!EmuConfig.VR.WorldProjection && !s_ee.Enabled())
		{
			s_ee.Skip();
			return;
		}

		const ProfileDB::ProjectionProfile* proj = nullptr;
		const VMState vm = VMManager::GetState();
		if ((vm == VMState::Running || vm == VMState::Paused) && EffectiveVREnabled(EmuConfig.VR.Enable))
		{
			ProfileDB::EnsureLoaded();
			const ProfileDB::Profile* profile = ProfileDB::Lookup(VMManager::GetDiscSerial(), VMManager::GetDiscCRC());
			if (profile && profile->projection.has_value())
				proj = &profile->projection.value();
		}
		const bool wanted =
			EmuConfig.VR.WorldProjection;
		const bool was = s_ee.Enabled();

		static const std::vector<ProfileDB::CameraGuard> s_no_gates;
		LiveSources live(proj ? proj->when : s_no_gates);
		const Source src = proj ? ToSource(*proj) : Source{};
		ManageRecordHook(proj, wanted && proj != nullptr);
		if (proj && proj->drawn == ProfileDB::ProjectionProfile::Drawn::Hook &&
			static_cast<HookState>(s_hook_state.load()) == HookState::Armed)
			live.SetHookPc(proj->hook_pc);
		const std::optional<Pose> read = s_ee.Vsync(proj ? &src : nullptr, wanted, live, PostToGs);
		if (proj && read.has_value())
			TallyRead(*proj, read.value());
		else
			s_tally_proj = nullptr;

		if (s_ee.Enabled() != was)
			Console.WriteLn(s_ee.Enabled() ?
								"(VR) world projection: on; each present whose stamp is on time is shown as a projection layer." :
								"(VR) world projection: off; every stamp forgotten.");
		if (!s_ee.Enabled())
			s_ee_last_profile = nullptr;
		else if (proj != s_ee_last_profile)
		{
			s_ee_last_profile = proj;
			using PP = ProfileDB::ProjectionProfile;
			const bool matrix = proj->drawn == PP::Drawn::Matrix || proj->drawn == PP::Drawn::Hook;
			std::string ages = std::to_string(proj->frame_age);
			for (size_t i = 1; i < proj->frame_ages.size(); i++)
				ages += fmt::format(",{}", proj->frame_ages[i]);
			Console.WriteLn(fmt::format("(VR) world projection: source {} ({}{}), frustum {}, frame age {}{}.",
				proj->drawn == PP::Drawn::Hook ? (proj->hook_body == PP::HookBody::Rest ? "hook (rest)" : "hook (pre)") :
				proj->drawn == PP::Drawn::LookAt ? (proj->yaw_body == PP::Body::MinusOwn ? "lookAt (minusOwn)" : "lookAt") :
				                                   (matrix ? "matrix" : "copy"),
				proj->general ? "axes:" : "flat keys", matrix ? (proj->roll_declared ? ", roll declared" : ", roll 0") : "",
				proj->frustum_model == PP::FrustumModel::Constant ? "constant" : "degrees", ages,
				proj->latch_first_draw ? fmt::format(", first-draw latch {} marks back", proj->read_to_draw) :
				proj->latch_closing    ? std::string(", closing latch (the read at the mark that closes the frame's first interval, closed into the buffer drawn)") :
				                         std::string()));
		}
	}

	void OnStateLoaded()
	{
		VR::ExecHookForgetVisits(VR::ExecOwner::Projection);
		s_ee.StateLoaded(PostToGs);
	}

	void OnReset()
	{
		OnStateLoaded();
	}

	void OnRendererSwitch()
	{
		if (!s_gs.Active())
			return;
		s_gs.RendererSwitch();
		SetLastPresent(s_gs.Last());
	}

	bool EeEnabled()
	{
		return s_ee.Enabled();
	}

	bool GsActive()
	{
		return s_gs.Active();
	}

	void OnGsDraw(u32 frame_fbp, u32 display_fbp)
	{
		s_gs.Draw(frame_fbp, display_fbp);
	}

	void OnGsPresent(u32 display_fbp)
	{
		const Presented p = s_gs.Present(display_fbp);
		SetLastPresent(p);
	}

	Presented LastPresent()
	{
		std::lock_guard<std::mutex> lock(s_present_mutex);
		return s_last_present;
	}
}
