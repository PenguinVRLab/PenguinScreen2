// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRProfileDB.h"

#include "Config.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Timer.h"
#include "common/YAML.h"

#include "fmt/format.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace VR::ProfileDB
{
	static bool parseProfile(const std::string_view serial, const ryml::NodeRef& node, Profile& out);
}

static constexpr char VRPROFILES_YAML_FILE_NAME[] = "vr-profiles.yaml";
static constexpr char VRPROFILES_DIR_NAME[] = "vr-profiles";

static std::unordered_map<std::string, VR::ProfileDB::Profile> s_profiles;
static std::vector<VR::ProfileDB::LoadIssue> s_load_issues;
static bool s_loaded = false;

static std::mutex s_load_mutex;

struct LoadStamp
{
	u64 sig = 1469598103934665603ull;
	size_t count = 0;
	bool operator==(const LoadStamp& o) const { return sig == o.sig && count == o.count; }
	bool operator!=(const LoadStamp& o) const { return !(*this == o); }
};
static LoadStamp s_stamp;

static void foldFile(LoadStamp& st, const std::string& path, std::time_t mtime, s64 size)
{
	constexpr u64 kPrime = 1099511628211ull;
	for (const unsigned char c : path)
		st.sig = (st.sig ^ c) * kPrime;
	const auto mix = [&st](u64 v) {
		for (int i = 0; i < 8; i++)
		{
			st.sig = (st.sig ^ (v & 0xFFu)) * kPrime;
			v >>= 8;
		}
	};
	mix(static_cast<u64>(mtime));
	mix(static_cast<u64>(size));
	st.count++;
}

static std::string_view nodeVal(const ryml::ConstNodeRef& node)
{
	return node.has_val() ? std::string_view(node.val().data(), node.val().size()) : std::string_view();
}

static std::optional<VR::ProfileDB::Tier> parseTier(const std::string_view s)
{
	using VR::ProfileDB::Tier;
	if (StringUtil::compareNoCase(s, "screen"))
		return Tier::Screen;
	if (StringUtil::compareNoCase(s, "stereo"))
		return Tier::Stereo;
	if (StringUtil::compareNoCase(s, "immersive"))
		return Tier::Immersive;
	return std::nullopt;
}

static std::optional<VR::ProfileDB::UvDrawPolicy> parseUvDrawPolicy(const std::string_view s)
{
	using VR::ProfileDB::UvDrawPolicy;
	if (StringUtil::compareNoCase(s, "screen"))
		return UvDrawPolicy::Screen;
	if (StringUtil::compareNoCase(s, "world"))
		return UvDrawPolicy::World;
	return std::nullopt;
}

static std::optional<u32> parseHexU32(const std::string_view str)
{
	std::string_view hex = str;
	if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
		hex = hex.substr(2);
	return StringUtil::FromChars<u32>(hex, 16);
}

static std::optional<u32> parseCrc(const std::string_view str)
{
	return parseHexU32(str);
}

static std::optional<u32> parseAddress(const std::string_view str)
{
	return parseHexU32(str);
}

static constexpr u32 EE_MAIN_RAM_BYTES = 0x02000000u;
static bool inMainRam(u32 addr, u32 width = 1)
{
	return static_cast<u64>(addr) + width <= EE_MAIN_RAM_BYTES;
}

static void warnHexTrap(const std::string_view serial, const std::string_view raw, u32 parsed, const char* field)
{
	const bool has_prefix = (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X'));
	if (has_prefix)
		return;
	const std::optional<u32> dec = StringUtil::FromChars<u32>(raw, 10);
	if (dec.has_value() && dec.value() != parsed)
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} '{}' is parsed as HEX {:#x}; if you meant "
						   "decimal {}, write it 0x-prefixed.", serial, field, raw, parsed, dec.value());
}

static void warnWidthFit(const std::string_view serial, u32 value, u32 width, const char* field)
{
	const u64 wmax = (width >= 4) ? 0xFFFFFFFFull : ((1ull << (width * 8)) - 1);
	if (value > wmax)
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} {:#x} does not fit the declared width {} "
						   "(max {:#x}); the probe can never match.", serial, field, value, width, wmax);
}

static void parseGuardList(const std::string_view serial, const ryml::ConstNodeRef& seq,
	const char* what, std::vector<VR::ProfileDB::CameraGuard>& out)
{
	for (const ryml::ConstNodeRef& g : seq.children())
	{
		if (!g.is_map())
			continue;
		const std::optional<u32> addr = g.has_child("address") ? parseAddress(nodeVal(g["address"])) : std::nullopt;
		const std::string_view eq_raw = g.has_child("equals") ? nodeVal(g["equals"]) : std::string_view{};
		const std::optional<u32> eq = g.has_child("equals") ? parseHexU32(eq_raw) : std::nullopt;
		if (!addr.has_value() || !eq.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} with a missing/invalid address or equals; skipping it.", serial, what);
			continue;
		}
		VR::ProfileDB::CameraGuard guard;
		if (g.has_child("width"))
		{
			const std::optional<u32> wdt = StringUtil::FromChars<u32>(nodeVal(g["width"]));
			if (wdt.has_value() && (wdt.value() == 1 || wdt.value() == 2 || wdt.value() == 4))
				guard.width = static_cast<u8>(wdt.value());
		}
		if (!inMainRam(addr.value(), guard.width))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} address {:#x}+{} outside/crossing main RAM; skipping it.", serial, what, addr.value(), guard.width);
			continue;
		}
		guard.ee_address = addr.value();
		guard.equals = eq.value();
		warnHexTrap(serial, eq_raw, eq.value(), what);
		warnWidthFit(serial, eq.value(), guard.width, what);
		out.push_back(guard);
	}
}

static std::optional<VR::ProfileDB::CameraEncoding> parseCameraEncoding(const std::string_view s)
{
	using VR::ProfileDB::CameraEncoding;
	if (StringUtil::compareNoCase(s, "f32"))
		return CameraEncoding::F32;
	if (StringUtil::compareNoCase(s, "s16.12"))
		return CameraEncoding::S16_12;
	if (StringUtil::compareNoCase(s, "s32angle"))
		return CameraEncoding::S32Angle;
	return std::nullopt;
}

static std::optional<VR::ProfileDB::CameraSource> parseCameraSource(const std::string_view s)
{
	using VR::ProfileDB::CameraSource;
	if (StringUtil::compareNoCase(s, "constant"))
		return CameraSource::Constant;
	if (StringUtil::compareNoCase(s, "head_yaw"))
		return CameraSource::HeadYaw;
	if (StringUtil::compareNoCase(s, "head_pitch"))
		return CameraSource::HeadPitch;
	if (StringUtil::compareNoCase(s, "head_roll"))
		return CameraSource::HeadRoll;
	if (StringUtil::compareNoCase(s, "head_x"))
		return CameraSource::HeadX;
	if (StringUtil::compareNoCase(s, "head_y"))
		return CameraSource::HeadY;
	if (StringUtil::compareNoCase(s, "head_z"))
		return CameraSource::HeadZ;
	return std::nullopt;
}

static std::optional<VR::ProfileDB::CameraCompose> parseCameraCompose(const std::string_view s)
{
	using VR::ProfileDB::CameraCompose;
	if (StringUtil::compareNoCase(s, "absolute"))
		return CameraCompose::Absolute;
	if (StringUtil::compareNoCase(s, "delta") || StringUtil::compareNoCase(s, "deltaRotation"))
		return CameraCompose::Delta;
	if (StringUtil::compareNoCase(s, "anchored"))
		return CameraCompose::Anchored;
	return std::nullopt;
}

static std::optional<VR::ProfileDB::CameraWrap> parseCameraWrap(const std::string_view s)
{
	using VR::ProfileDB::CameraWrap;
	if (StringUtil::compareNoCase(s, "none"))
		return CameraWrap::None;
	if (StringUtil::compareNoCase(s, "deg360"))
		return CameraWrap::Deg360;
	return std::nullopt;
}

static void readOptionalFloat(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* key, const char* what, float& dst)
{
	if (!node.has_child(key))
		return;
	const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(node[key]));
	if (v.has_value())
		dst = v.value();
	else
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid {}; keeping the default.", serial, what);
}

static std::optional<s64> parseSignedOffset(std::string_view str)
{
	bool neg = false;
	if (!str.empty() && (str[0] == '-' || str[0] == '+'))
	{
		neg = (str[0] == '-');
		str.remove_prefix(1);
	}
	const std::optional<u32> mag = parseHexU32(str);
	if (!mag.has_value())
		return std::nullopt;
	const s64 v = static_cast<s64>(mag.value());
	return neg ? -v : v;
}

static bool parseAobPattern(std::string_view str, std::vector<u8>& pattern, std::vector<u8>& mask)
{
	pattern.clear();
	mask.clear();
	size_t i = 0;
	while (i < str.size())
	{
		while (i < str.size() && (str[i] == ' ' || str[i] == '\t'))
			i++;
		if (i >= str.size())
			break;
		size_t j = i;
		while (j < str.size() && str[j] != ' ' && str[j] != '\t')
			j++;
		const std::string_view tok = str.substr(i, j - i);
		i = j;
		if (tok == "??")
		{
			pattern.push_back(0);
			mask.push_back(0);
			continue;
		}
		if (tok.size() != 2)
			return false;
		const std::optional<u32> b = StringUtil::FromChars<u32>(tok, 16);
		if (!b.has_value() || b.value() > 0xFF)
			return false;
		pattern.push_back(static_cast<u8>(b.value()));
		mask.push_back(1);
	}
	return !pattern.empty();
}

static std::optional<VR::ProfileDB::CameraProfile> parseCamera(const std::string_view serial, const ryml::ConstNodeRef& cnode)
{
	using namespace VR::ProfileDB;
	CameraProfile cam;

	if (cnode.has_child("writes") && cnode["writes"].is_seq())
	{
		for (const ryml::ConstNodeRef& w : cnode["writes"].children())
		{
			if (!w.is_map())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.writes has a non-map op; skipping it.", serial);
				continue;
			}

			const std::optional<u32> addr = w.has_child("address") ? parseAddress(nodeVal(w["address"])) : std::nullopt;
			const std::optional<CameraEncoding> enc = w.has_child("encoding") ? parseCameraEncoding(nodeVal(w["encoding"])) : std::nullopt;
			const std::optional<CameraSource> src = w.has_child("source") ? parseCameraSource(nodeVal(w["source"])) : std::nullopt;
			if (!addr.has_value() || !enc.has_value() || !src.has_value())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera write op with a missing/invalid address, encoding, or source; skipping it.", serial);
				continue;
			}

			CameraWriteOp op;
			op.ee_address = addr.value();
			op.encoding = enc.value();
			op.source = src.value();
			if (w.has_child("relative"))
				op.relative = StringUtil::compareNoCase(nodeVal(w["relative"]), "true");
			if (w.has_child("compose"))
			{
				const std::optional<CameraCompose> comp = parseCameraCompose(nodeVal(w["compose"]));
				if (comp.has_value())
					op.compose = comp.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera write op with an invalid compose; keeping absolute.", serial);
			}
			if (w.has_child("wrap"))
			{
				const std::optional<CameraWrap> wr = parseCameraWrap(nodeVal(w["wrap"]));
				if (wr.has_value())
					op.wrap = wr.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera write op with an invalid wrap; keeping none.", serial);
			}
			if ((op.compose == CameraCompose::Delta || op.compose == CameraCompose::Anchored) &&
				op.source == CameraSource::Constant)
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a delta/anchored camera write op with a constant source (the head term is always zero); skipping it.", serial);
				continue;
			}
			readOptionalFloat(serial, w, "scale", "camera op scale", op.scale);
			readOptionalFloat(serial, w, "bias", "camera op bias", op.bias);
			readOptionalFloat(serial, w, "clampMin", "camera op clampMin", op.clamp_min);
			readOptionalFloat(serial, w, "clampMax", "camera op clampMax", op.clamp_max);
			readOptionalFloat(serial, w, "axisSign", "camera op axisSign", op.axis_sign);

			if (!op.relative && !inMainRam(op.ee_address))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera write address {:#x} is outside main RAM; skipping it.", serial, op.ee_address);
				continue;
			}
			if (w.has_child("when") && w["when"].is_seq())
				parseGuardList(serial, w["when"], "camera write when-guard", op.when);
			cam.writes.push_back(op);
		}
	}

	if (cnode.has_child("matrixWrites") && cnode["matrixWrites"].is_seq())
	{
		for (const ryml::ConstNodeRef& m : cnode["matrixWrites"].children())
		{
			if (!m.is_map())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.matrixWrites has a non-map op; skipping it.", serial);
				continue;
			}
			const std::optional<u32> addr = m.has_child("address") ? parseAddress(nodeVal(m["address"])) : std::nullopt;
			if (!addr.has_value())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera matrix op with a missing/invalid address; skipping it.", serial);
				continue;
			}
			CameraMatrixOp op;
			op.ee_address = addr.value();
			if (m.has_child("relative"))
				op.relative = StringUtil::compareNoCase(nodeVal(m["relative"]), "true");
			if (m.has_child("order"))
			{
				const std::string_view o = nodeVal(m["order"]);
				if (StringUtil::compareNoCase(o, "pre"))
					op.order = MatrixComposeOrder::Pre;
				else if (StringUtil::compareNoCase(o, "post"))
					op.order = MatrixComposeOrder::Post;
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera matrix op has an invalid order; keeping pre.", serial);
			}
			if (m.has_child("compose"))
			{
				const std::string_view c = nodeVal(m["compose"]);
				if (StringUtil::compareNoCase(c, "anchored"))
					op.anchored = true;
				else if (StringUtil::compareNoCase(c, "delta"))
					op.anchored = false;
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera matrix op has an invalid compose (delta|anchored); keeping delta.", serial);
			}
			if (m.has_child("transposeAddress"))
			{
				const std::optional<u32> taddr = parseAddress(nodeVal(m["transposeAddress"]));
				if (taddr.has_value())
				{
					op.transpose_address = taddr.value();
					op.also_transpose = true;
				}
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera matrix op has an invalid transposeAddress; ignoring it.", serial);
			}
			readOptionalFloat(serial, m, "axisSignYaw", "camera matrix axisSignYaw", op.axis_sign_yaw);
			readOptionalFloat(serial, m, "axisSignPitch", "camera matrix axisSignPitch", op.axis_sign_pitch);
			readOptionalFloat(serial, m, "axisSignRoll", "camera matrix axisSignRoll", op.axis_sign_roll);

			if (!op.relative && !inMainRam(op.ee_address))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera matrix address {:#x} is outside main RAM; skipping it.", serial, op.ee_address);
				continue;
			}
			if (!op.relative && op.also_transpose && !inMainRam(op.transpose_address))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera matrix transpose address {:#x} is outside main RAM; skipping it.", serial, op.transpose_address);
				continue;
			}
			if (m.has_child("when") && m["when"].is_seq())
				parseGuardList(serial, m["when"], "camera matrix when-guard", op.when);
			cam.matrix_writes.push_back(op);
		}
	}

	if (cnode.has_child("fov") && cnode["fov"].is_map())
	{
		const ryml::ConstNodeRef fnode = cnode["fov"];
		const std::optional<u32> addr = fnode.has_child("address") ? parseAddress(nodeVal(fnode["address"])) : std::nullopt;
		const std::optional<CameraEncoding> enc = fnode.has_child("encoding") ? parseCameraEncoding(nodeVal(fnode["encoding"])) : std::nullopt;
		if (!addr.has_value() || !enc.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.fov is missing/invalid address or encoding; ignoring the fov block.", serial);
		}
		else if (!inMainRam(addr.value()))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.fov address {:#x} is outside main RAM; ignoring the fov block.", serial, addr.value());
		}
		else
		{
			CameraFov fov;
			fov.ee_address = addr.value();
			fov.encoding = enc.value();
			readOptionalFloat(serial, fnode, "scale", "camera.fov scale", fov.scale);
			cam.fov = fov;
		}
	}

	if (cnode.has_child("base") && cnode["base"].is_map())
	{
		const ryml::ConstNodeRef bnode = cnode["base"];
		CameraBase base;
		bool base_ok = false;
		if (bnode.has_child("indexed"))
		{
			const std::optional<u32> ib = parseAddress(nodeVal(bnode["indexed"]));
			const std::optional<u32> ia = bnode.has_child("indexAddr") ? parseAddress(nodeVal(bnode["indexAddr"])) : std::nullopt;
			const std::optional<s64> ao = bnode.has_child("arrayOffset") ? parseSignedOffset(nodeVal(bnode["arrayOffset"])) : std::nullopt;
			const std::optional<u32> st = bnode.has_child("stride") ? parseHexU32(nodeVal(bnode["stride"])) : std::nullopt;
			if (ib.has_value() && ia.has_value() && st.has_value() && st.value() != 0)
			{
				base.is_indexed = true;
				base.indexed_base = ib.value();
				base.index_addr = ia.value();
				base.array_offset = ao.value_or(0);
				base.stride = st.value();
				if (bnode.has_child("indexWidth"))
				{
					const std::optional<u32> iw = StringUtil::FromChars<u32>(nodeVal(bnode["indexWidth"]));
					if (iw.has_value() && (iw.value() == 1 || iw.value() == 2 || iw.value() == 4))
						base.index_width = static_cast<u8>(iw.value());
				}
				base_ok = true;
			}
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base indexed mode needs indexed + indexAddr + nonzero stride; ignoring the base.", serial);
		}
		else if (bnode.has_child("pointer"))
		{
			const std::optional<u32> ptr = parseAddress(nodeVal(bnode["pointer"]));
			if (ptr.has_value())
			{
				base.is_pointer = true;
				base.pointer_addr = ptr.value();
				base_ok = true;
				if (bnode.has_child("pattern"))
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base has both pointer and pattern; using the pointer.", serial);
			}
		}
		else
		{
			base_ok = bnode.has_child("pattern") &&
			          parseAobPattern(nodeVal(bnode["pattern"]), base.pattern, base.mask);
		}
		if (base_ok && !base.is_pointer && bnode.has_child("range") && bnode["range"].is_seq() && bnode["range"].num_children() == 2)
		{
			const std::optional<u32> lo = parseHexU32(nodeVal(bnode["range"][0]));
			const std::optional<u32> hi = parseHexU32(nodeVal(bnode["range"][1]));
			if (lo.has_value() && hi.has_value() && lo.value() < hi.value())
			{
				base.scan_start = lo.value();
				base.scan_end = hi.value();
			}
			else
				base_ok = false;
		}
		if (base_ok && bnode.has_child("offset"))
		{
			const std::optional<s64> off = parseSignedOffset(nodeVal(bnode["offset"]));
			if (off.has_value())
				base.base_offset = off.value();
			else
				base_ok = false;
		}
		if (base_ok && bnode.has_child("validate") && bnode["validate"].is_map())
		{
			const ryml::ConstNodeRef vnode = bnode["validate"];
			const std::optional<s64> voff =
				vnode.has_child("offset") ? parseSignedOffset(nodeVal(vnode["offset"])) : std::nullopt;
			const std::optional<u32> veq =
				vnode.has_child("equals") ? parseHexU32(nodeVal(vnode["equals"])) : std::nullopt;
			if (voff.has_value() && veq.has_value())
			{
				base.has_validate = true;
				base.validate_offset = voff.value();
				base.validate_equals = veq.value();
			}
			else
				base_ok = false;
		}
		if (base_ok)
			cam.base = std::move(base);
		else
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base is malformed; ignoring it (relative ops will not fire).", serial);
	}

	if (cnode.has_child("guards") && cnode["guards"].is_seq())
		parseGuardList(serial, cnode["guards"], "camera guard", cam.guards);

	if (cnode.has_child("silence") && cnode["silence"].is_seq())
	{
		for (const ryml::ConstNodeRef& sn : cnode["silence"].children())
		{
			if (!sn.is_map())
				continue;
			const std::optional<u32> addr = sn.has_child("address") ? parseAddress(nodeVal(sn["address"])) : std::nullopt;
			const std::optional<u32> von = sn.has_child("on") ? parseHexU32(nodeVal(sn["on"])) : std::nullopt;
			const std::optional<u32> voff = sn.has_child("off") ? parseHexU32(nodeVal(sn["off"])) : std::nullopt;
			if (!addr.has_value() || !von.has_value() || !voff.has_value())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera silence op with a missing/invalid address/on/off; skipping it.", serial);
				continue;
			}

			if (!inMainRam(addr.value(), 4))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera silence address {:#x} outside main RAM; skipping it.", serial, addr.value());
				continue;
			}
			cam.silence.push_back(CameraSilence{addr.value(), von.value(), voff.value()});
		}
	}

	if (cnode.has_child("codeHooks") && cnode["codeHooks"].is_seq())
	{
		const auto readReg = [&](const ryml::ConstNodeRef& n, const char* key, u8& dst) {
			if (!n.has_child(key))
				return;
			const std::optional<u32> r = StringUtil::FromChars<u32>(nodeVal(n[key]));
			if (r.has_value() && r.value() <= 31)
				dst = static_cast<u8>(r.value());
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' codeHook has an out-of-range {} (need 0..31); keeping the default.", serial, key);
		};
		for (const ryml::ConstNodeRef& h : cnode["codeHooks"].children())
		{
			if (!h.is_map())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.codeHooks has a non-map op; skipping it.", serial);
				continue;
			}
			const std::optional<u32> hook = h.has_child("hookAddress") ? parseAddress(nodeVal(h["hookAddress"])) : std::nullopt;
			const std::optional<u32> cave = h.has_child("caveAddress") ? parseAddress(nodeVal(h["caveAddress"])) : std::nullopt;
			const std::optional<u32> scr = h.has_child("scratchAddress") ? parseAddress(nodeVal(h["scratchAddress"])) : std::nullopt;
			const std::optional<u32> tail = h.has_child("tailJump") ? parseAddress(nodeVal(h["tailJump"])) : std::nullopt;
			if (!hook.has_value() || !cave.has_value() || !scr.has_value() || !tail.has_value())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' codeHook needs hookAddress/caveAddress/scratchAddress/tailJump; skipping it.", serial);
				continue;
			}
			CameraCodeHook ch;
			ch.hook_address = hook.value();
			ch.cave_address = cave.value();
			ch.scratch_address = scr.value();
			ch.tail_jump_address = tail.value();
			if (h.has_child("enabled"))
				ch.enabled = StringUtil::compareNoCase(nodeVal(h["enabled"]), "true");
			if (h.has_child("source"))
			{
				const std::optional<CameraSource> s = parseCameraSource(nodeVal(h["source"]));
				if (s.has_value())
					ch.source = s.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' codeHook has an invalid source; keeping head_yaw.", serial);
			}
			readOptionalFloat(serial, h, "scale", "codeHook scale", ch.scale);
			readOptionalFloat(serial, h, "axisSign", "codeHook axisSign", ch.axis_sign);
			readReg(h, "targetFpr", ch.target_fpr);
			readReg(h, "scratchFpr", ch.scratch_fpr);
			readReg(h, "addrGpr", ch.addr_gpr);
			cam.code_hooks.push_back(ch);
		}
	}

	if (cnode.has_child("padLook") && cnode["padLook"].is_map())
	{
		const ryml::ConstNodeRef pnode = cnode["padLook"];
		CameraPadLook pl;
		readOptionalFloat(serial, pnode, "maxLookDeg", "camera.padLook maxLookDeg", pl.max_look_deg);
		readOptionalFloat(serial, pnode, "engageDeg", "camera.padLook engageDeg", pl.engage_deg);
		readOptionalFloat(serial, pnode, "curve", "camera.padLook curve", pl.curve);
		pl.curve = std::clamp(pl.curve, 0.2f, 5.0f);
		if (pnode.has_child("latch"))
			pl.latch = StringUtil::compareNoCase(nodeVal(pnode["latch"]), "true");
		readOptionalFloat(serial, pnode, "releaseDeg", "camera.padLook releaseDeg", pl.release_deg);
		pl.release_deg = std::clamp(pl.release_deg, 0.5f, 45.0f);
		if (pl.max_look_deg <= 0.0f || pl.engage_deg < 0.0f || pl.engage_deg >= pl.max_look_deg)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.padLook has out-of-order engageDeg/maxLookDeg; ignoring the padLook block.", serial);
		}
		else
		{
			cam.pad_look = pl;
		}
	}

	if (cam.writes.empty() && cam.matrix_writes.empty() && cam.code_hooks.empty() && !cam.fov.has_value() && !cam.pad_look.has_value())
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera block with no usable ops; ignoring it.", serial);
		return std::nullopt;
	}

	return cam;
}

bool VR::ProfileDB::parseProfile(const std::string_view serial, const ryml::NodeRef& node, Profile& out)
{
	out.serial.assign(serial.data(), serial.size());

	if (node.has_child("tier"))
	{
		const std::optional<Tier> tier = parseTier(nodeVal(node["tier"]));
		if (!tier.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid tier; skipping entry.", serial);
			return false;
		}
		out.tier = tier.value();
	}

	if (node.has_child("crcs"))
	{
		for (const ryml::ConstNodeRef& c : node["crcs"].children())
		{
			const std::string_view crc_str = nodeVal(c);
			const std::optional<u32> crc = parseCrc(crc_str);
			if (!crc.has_value())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid CRC '{}'; skipping that CRC.", serial, crc_str);
				continue;
			}
			out.crcs.push_back(crc.value());
		}
	}

	if (node.has_child("stereo") && node["stereo"].is_map())
	{
		const ryml::ConstNodeRef snode = node["stereo"];
		StereoParams sp;

		if (snode.has_child("separation"))
		{
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(snode["separation"]));
			if (v.has_value())
				sp.separation = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.separation; ignoring it.", serial);
		}
		if (snode.has_child("convergence"))
		{
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(snode["convergence"]));
			if (v.has_value())
				sp.convergence = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.convergence; ignoring it.", serial);
		}
		if (snode.has_child("uvDraws"))
		{
			const std::optional<UvDrawPolicy> pol = parseUvDrawPolicy(nodeVal(snode["uvDraws"]));
			if (pol.has_value())
				sp.uv_draws = pol.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.uvDraws; using 'screen'.", serial);
		}
		if (snode.has_child("pinUniformQ"))
			sp.pin_uniform_q = StringUtil::compareNoCase(nodeVal(snode["pinUniformQ"]), "true");

		if (snode.has_child("scenes") && snode["scenes"].is_seq())
		{
			for (const ryml::ConstNodeRef& sc : snode["scenes"].children())
			{
				if (!sc.is_map() || !sc.has_child("when") || !sc["when"].is_map())
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo scene without a 'when' probe; skipping it.", serial);
					continue;
				}
				const ryml::ConstNodeRef w = sc["when"];
				const std::optional<u32> addr = w.has_child("address") ? parseAddress(nodeVal(w["address"])) : std::nullopt;
				const std::string_view eq_raw = w.has_child("equals") ? nodeVal(w["equals"]) : std::string_view{};
				const std::optional<u32> eq = w.has_child("equals") ? parseHexU32(eq_raw) : std::nullopt;
				if (!addr.has_value() || !eq.has_value())
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo scene with a missing/invalid when.address or when.equals; skipping it.", serial);
					continue;
				}
				StereoSceneRule rule;

				if (w.has_child("width"))
				{
					const std::optional<u32> wdt = StringUtil::FromChars<u32>(nodeVal(w["width"]));
					if (wdt.has_value() && (wdt.value() == 1 || wdt.value() == 2 || wdt.value() == 4))
						rule.width = static_cast<u8>(wdt.value());
				}

				if (!inMainRam(addr.value(), rule.width))
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo scene probe {:#x}+{} outside/crossing main RAM; skipping it.", serial, addr.value(), rule.width);
					continue;
				}
				rule.ee_address = addr.value();
				rule.equals = eq.value();
				warnHexTrap(serial, eq_raw, eq.value(), "stereo scene when.equals");
				warnWidthFit(serial, eq.value(), rule.width, "stereo scene when.equals");
				if (sc.has_child("separation"))
				{
					const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(sc["separation"]));
					if (v.has_value())
						rule.separation = v.value();
					else
						Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo scene separation; inheriting base.", serial);
				}
				if (sc.has_child("convergence"))
				{
					const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(sc["convergence"]));
					if (v.has_value())
						rule.convergence = v.value();
					else
						Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo scene convergence; inheriting base.", serial);
				}
				if (sc.has_child("label"))
				{
					const std::string_view lv = nodeVal(sc["label"]);
					rule.label.assign(lv.data(), lv.size());
				}
				sp.scenes.push_back(std::move(rule));
			}
		}

		out.stereo = sp;
	}

	if (node.has_child("screen") && node["screen"].is_map())
	{
		const ryml::ConstNodeRef scr = node["screen"];
		if (scr.has_child("distance"))
		{
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(scr["distance"]));
			if (v.has_value())
				out.screen_distance = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.distance; ignoring it.", serial);
		}
		if (scr.has_child("height"))
		{
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(scr["height"]));
			if (v.has_value())
				out.screen_height = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.height; ignoring it.", serial);
		}
		if (scr.has_child("arc"))
		{
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(scr["arc"]));
			if (v.has_value())
				out.screen_arc_deg = std::clamp(v.value(), 0.0f, 270.0f);
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.arc; ignoring it.", serial);
		}
	}

	if (node.has_child("camera") && node["camera"].is_map())
		out.camera = parseCamera(serial, node["camera"]);

	if (node.has_child("name"))
		out.name.assign(nodeVal(node["name"]));

	if (node.has_child("notes"))
		out.notes.assign(nodeVal(node["notes"]));

	return true;
}

namespace
{
	enum class LoadTier
	{
		User,
		Shipped,
	};
}

static const std::regex s_serial_stem_re("^[A-Za-z]{4}-\\d{5}$");

static u32 loadProfileFile(const std::string& path, LoadTier tier,
	std::unordered_set<std::string>& user_serials)
{
	const std::optional<std::string> buffer = FileSystem::ReadFileToString(path.c_str());
	if (!buffer.has_value())
	{
		Console.ErrorFmt("(VR) ProfileDB: unreadable profile file {}", path);
		s_load_issues.push_back({path, "file could not be read"});
		return 0;
	}

	Error error;
	const std::string name(Path::GetFileName(path));

	std::optional<ryml::Tree> tree =
		ParseYAMLFromString(ryml::to_csubstr(*buffer), ryml::to_csubstr(name), &error, true);
	if (!tree.has_value())
	{
		Console.ErrorFmt("(VR) ProfileDB: failed to parse {}:", path);
		Console.Error(error.GetDescription());
		s_load_issues.push_back({path, error.GetDescription()});
		return 0;
	}

	ryml::NodeRef root = tree->rootref();
	if (!root.is_map())
	{
		Console.ErrorFmt("(VR) ProfileDB: {}: top level is not a serial-keyed map.", path);
		s_load_issues.push_back({path, "top level is not a serial-keyed map (expected 'SLUS-12345:' at column 0)"});
		return 0;
	}

	u32 loaded = 0;
	std::vector<std::string> file_serials;
	for (const ryml::NodeRef& n : root.children())
	{

		if (!n.has_key())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: ignoring a top-level entry with no serial key.", path);
			s_load_issues.push_back({path, "a top-level entry has no serial key"});
			continue;
		}

		auto serial = StringUtil::toLower(std::string(n.key().str, n.key().len));
		file_serials.push_back(serial);

		if (s_profiles.count(serial) == 1)
		{
			if (tier == LoadTier::Shipped && user_serials.count(serial) == 1)
			{

				Console.WriteLnFmt("(VR) ProfileDB: user profile overrides shipped '{}' ({} skipped).",
					serial, name);
			}
			else
			{
				Console.WarningFmt("(VR) ProfileDB: {}: duplicate serial '{}' (already loaded from this tier); skipping. Serials are case-insensitive!",
					path, serial);
				s_load_issues.push_back({path, "duplicate serial '" + serial + "' — another file in the same folder already defines it"});
			}
			continue;
		}

		if (!n.is_map())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: serial '{}' is not a map; skipping.", path, serial);
			s_load_issues.push_back({path, "serial '" + serial + "' is not a map"});
			continue;
		}

		VR::ProfileDB::Profile profile;
		if (VR::ProfileDB::parseProfile(serial, n, profile))
		{
			if (tier == LoadTier::User)
				user_serials.insert(serial);
			s_profiles.emplace(std::move(serial), std::move(profile));
			loaded++;
		}
		else
		{
			s_load_issues.push_back({path, "profile '" + serial + "' failed validation — details in the emulog above this summary"});
		}
	}

	if (file_serials.empty())
	{
		Console.ErrorFmt("(VR) ProfileDB: {}: no profile entries.", path);
		s_load_issues.push_back({path, "no profile entries in file"});
	}

	// serial (copying SLUS-20851.yaml to SLES-12345.yaml and forgetting to

	std::string stem(Path::GetFileTitle(path));
	if (std::regex_match(stem, s_serial_stem_re))
	{
		const std::string want = StringUtil::toLower(stem);
		if (std::find(file_serials.begin(), file_serials.end(), want) == file_serials.end())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: file is named '{}' but does not define that serial.", path, stem);
			s_load_issues.push_back({path, "file is named '" + stem + "' but does not define that serial (rename the file or fix the key inside)"});
		}
	}

	return loaded;
}

static void ensureUserFolderReadme(const std::string& dir)
{
	const std::string readme(Path::Combine(dir, "README.txt"));
	if (FileSystem::FileExists(readme.c_str()))
		return;
	static constexpr char text[] =
		"VR profiles — user folder\n"
		"=========================\n"
		"One game per file (SLUS-12345.yaml). Files here OVERRIDE the shipped\n"
		"profile for the same serial, per file. To customize a shipped game,\n"
		"copy its file from the install's resources/vr-profiles/ folder here\n"
		"and edit the copy. Every file is checked at launch; invalid files\n"
		"are reported in a dialog naming the file and the reason.\n";
	FileSystem::WriteStringToFile(readme.c_str(), text);
}

namespace
{

	enum class ShippedSource
	{
		Folder,
		Legacy,
		None,
	};

	struct ScanEntry
	{
		std::string path;
		LoadTier tier;
	};

	struct ScanResult
	{
		std::vector<ScanEntry> entries;
		LoadStamp stamp;
		ShippedSource shipped = ShippedSource::None;
	};
}

static ScanResult scanProfiles()
{
	ScanResult sr;
	FileSystem::FindResultsArray files;

	if (!EmuFolders::VRProfiles.empty() && FileSystem::DirectoryExists(EmuFolders::VRProfiles.c_str()))
	{
		ensureUserFolderReadme(EmuFolders::VRProfiles);
		if (FileSystem::FindFiles(EmuFolders::VRProfiles.c_str(), "*.yaml",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				sr.entries.push_back({fd.FileName, LoadTier::User});
				foldFile(sr.stamp, fd.FileName, fd.ModificationTime, fd.Size);
			}
		}
	}

	const std::string shipped_dir(Path::Combine(EmuFolders::Resources, VRPROFILES_DIR_NAME));
	if (FileSystem::DirectoryExists(shipped_dir.c_str()))
	{
		sr.shipped = ShippedSource::Folder;
		if (FileSystem::FindFiles(shipped_dir.c_str(), "*.yaml",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files))
		{
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				sr.entries.push_back({fd.FileName, LoadTier::Shipped});
				foldFile(sr.stamp, fd.FileName, fd.ModificationTime, fd.Size);
			}
		}
	}
	else
	{

		const std::string legacy(Path::Combine(EmuFolders::Resources, VRPROFILES_YAML_FILE_NAME));
		FILESYSTEM_STAT_DATA st;
		if (FileSystem::StatFile(legacy.c_str(), &st))
		{
			sr.shipped = ShippedSource::Legacy;
			sr.entries.push_back({legacy, LoadTier::Shipped});
			foldFile(sr.stamp, legacy, st.ModificationTime, st.Size);
		}
	}

	return sr;
}

static void loadScanned(const ScanResult& sr)
{
	Common::Timer timer;
	s_profiles.clear();
	s_load_issues.clear();

	if (sr.shipped == ShippedSource::Legacy)
	{

		Console.Error("(VR) ProfileDB: ==================================================================");
		Console.ErrorFmt("(VR) ProfileDB: LEGACY single-file catalog in use — resources/{} is missing.", VRPROFILES_DIR_NAME);
		Console.Error("(VR) ProfileDB: This binary expects the per-game profile folder; the install or");
		Console.Error("(VR) ProfileDB: rig deployment is SKEWED (binary and resources from different");
		Console.Error("(VR) ProfileDB: revisions). Loading the legacy catalog so this session works.");
		Console.Error("(VR) ProfileDB: ==================================================================");
	}
	else if (sr.shipped == ShippedSource::None)
	{
		Console.Error("(VR) ProfileDB: no profile folder and no legacy catalog — no VR profiles loaded.");
	}

	u32 user_files = 0, shipped_files = 0;
	std::unordered_set<std::string> user_serials;

	for (const ScanEntry& e : sr.entries)
	{
		loadProfileFile(e.path, e.tier, user_serials);
		if (e.tier == LoadTier::User)
			user_files++;
		else
			shipped_files++;
	}

	s_stamp = sr.stamp;
	s_loaded = true;

	Console.WriteLnFmt(Color_StrongGreen,
		"(VR) ProfileDB: {} profile(s) on record from {} shipped + {} user file(s) "
		"(user files override shipped per game; loaded in {:.2f}ms).",
		s_profiles.size(), shipped_files, user_files, timer.GetTimeMilliseconds());
}

void VR::ProfileDB::EnsureLoaded()
{
	std::lock_guard lock(s_load_mutex);
	if (s_loaded)
		return;

	loadScanned(scanProfiles());
}

void VR::ProfileDB::ReloadIfChanged()
{
	std::lock_guard lock(s_load_mutex);
	if (!s_loaded)
	{
		loadScanned(scanProfiles());
		return;
	}

	ScanResult sr = scanProfiles();
	if (sr.stamp == s_stamp)
		return;

	Console.WriteLnFmt(Color_StrongGreen, "(VR) ProfileDB: profile folder change detected — reloading.");
	loadScanned(sr);
}

const std::vector<VR::ProfileDB::LoadIssue>& VR::ProfileDB::ValidateAtLaunch()
{

	EnsureLoaded();
	return s_load_issues;
}

const VR::ProfileDB::Profile* VR::ProfileDB::Lookup(const std::string_view serial, u32 crc)
{
	EnsureLoaded();

	const auto it = s_profiles.find(StringUtil::toLower(serial));
	if (it == s_profiles.end())
		return nullptr;

	const Profile& profile = it->second;

	if (profile.crcs.empty() || crc == 0)
		return &profile;

	if (std::find(profile.crcs.begin(), profile.crcs.end(), crc) != profile.crcs.end())
		return &profile;

	return nullptr;
}

void VR::ProfileDB::Reset()
{
	std::lock_guard lock(s_load_mutex);
	s_profiles.clear();
	s_load_issues.clear();
	s_stamp = LoadStamp{};
	s_loaded = false;
}

std::vector<VR::ProfileDB::Summary> VR::ProfileDB::ListProfiles()
{
	EnsureLoaded();

	std::lock_guard lock(s_load_mutex);

	std::vector<Summary> out;
	out.reserve(s_profiles.size());
	for (const auto& [serial, profile] : s_profiles)
	{
		Summary s;
		s.serial = StringUtil::toUpper(serial);
		s.name = profile.name;
		s.has_stereo = profile.stereo.has_value();
		s.has_camera = profile.camera.has_value();
		if (profile.stereo.has_value())
		{
			s.separation = profile.stereo->separation;
			s.convergence = profile.stereo->convergence;
		}
		out.push_back(std::move(s));
	}
	std::sort(out.begin(), out.end(), [](const Summary& a, const Summary& b) { return a.serial < b.serial; });
	return out;
}
