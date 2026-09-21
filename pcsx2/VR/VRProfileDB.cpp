// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
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
#include <cmath>
#include <cstdlib>
#include <limits>
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
static constexpr char VRPROFILES_SEED_FILE_NAME[] = "SLUS-20851.yaml";
static constexpr char VRPROFILES_SEED_MARKER[] = "# seeded-from-shipped-sig: ";

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

static std::optional<VR::ProfileDB::StereoMap> parseStereoMap(const std::string_view s)
{
	using VR::ProfileDB::StereoMap;
	if (StringUtil::compareNoCase(s, "linear"))
		return StereoMap::Linear;
	if (StringUtil::compareNoCase(s, "bands"))
		return StereoMap::Bands;
	if (StringUtil::compareNoCase(s, "log"))
		return StereoMap::Log;
	return std::nullopt;
}

static constexpr float kStereoBudgetNdc = 0.02f;

static constexpr float kProfilePi = 3.14159265358979323846f;
static constexpr float kRadToArcmin = 60.0f * 180.0f / kProfilePi;

static constexpr float kRefIpdMetres [[maybe_unused]] = 0.06336f;
static constexpr float kNarrowIpdMetres = 0.053f;
static constexpr float kShipScreenDistanceM = 2.0f;
static constexpr float kShipScreenArcDeg = 100.0f;

static constexpr float kDivergenceToleranceArcmin = 51.0f;

static constexpr float kFixationGapArcmin = 20.0f;

static float arcminPerNdc(float screen_arc_deg)
{
	return screen_arc_deg * 60.0f;
}

static float divergenceArcmin(float ipd_m, float screen_distance_m)
{
	if (!(screen_distance_m > 0.0f))
		screen_distance_m = kShipScreenDistanceM;
	return (ipd_m / screen_distance_m) * kRadToArcmin;
}

static std::vector<VR::ProfileDB::StereoRailFinding> s_rail_findings;

static std::vector<VR::ProfileDB::ProfileAdvisory> s_advisories;

static void advise(const std::string_view serial, const char* site, std::string msg)
{
	Console.Warning(msg);
	s_advisories.push_back({StringUtil::toUpper(serial), site, std::move(msg)});
}

static bool resolveDepthMap(const std::string_view serial, const char* where,
	VR::ProfileDB::StereoMap map, const std::vector<float>& splits_w,
	const std::vector<VR::ProfileDB::StereoBand>& bands,
	const std::optional<VR::ProfileDB::StereoLogParams>& log_params,
	VR::ProfileDB::StereoResolvedMap& out)
{
	using VR::ProfileDB::StereoMap;
	using VR::ProfileDB::StereoResolvedMap;

	out = StereoResolvedMap{};

	if (map == StereoMap::Linear)
		return true;

	const auto reject = [&](const std::string_view why) {
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' {}: {}; falling back to 'map: linear' "
						   "(the separation/convergence pair still applies).", serial, where, why);
		out = StereoResolvedMap{};
		return false;
	};

	constexpr float kSplitPad = -std::numeric_limits<float>::max();

	if (map == StereoMap::Log)
	{
		if (!log_params.has_value())
			return reject("'map: log' without a 'log:' block");
		const VR::ProfileDB::StereoLogParams& lp = log_params.value();
		if (!(lp.w0 > 0.0f))
			return reject(fmt::format("log.w0 {} must be > 0", lp.w0));
		if (!(lp.w1 > lp.w0))
			return reject(fmt::format("log.w1 {} must be > log.w0 {}", lp.w1, lp.w0));
		if (!(lp.dfar >= 0.0f))
			return reject(fmt::format("log.dfar {} must be >= 0", lp.dfar));

		out.map = StereoMap::Log;
		out.band_count = 1;
		out.log_w0 = lp.w0;
		out.log_w1 = lp.w1;
		out.log_dfar = lp.dfar;
		for (int i = 0; i < 3; i++)
			out.split_q[i] = kSplitPad;
		if (lp.dfar > kStereoBudgetNdc)
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {}: log.dfar {} exceeds the {} NDC comfort "
							   "budget; rendering as authored, but expect fusion strain at the far end.",
				serial, where, lp.dfar, kStereoBudgetNdc);
		return true;
	}

	if (splits_w.size() > 3)
		return reject(fmt::format("{} splits, at most 3 are supported", splits_w.size()));
	if (bands.size() > 4)
		return reject(fmt::format("{} bands, at most 4 are supported", bands.size()));
	if (bands.empty())
		return reject("'map: bands' without a 'bands:' list");
	if (bands.size() != splits_w.size() + 1)
		return reject(fmt::format("{} bands needs exactly {} splits, found {}",
			bands.size(), bands.size() - 1, splits_w.size()));

	for (size_t i = 0; i < splits_w.size(); i++)
	{
		if (!(splits_w[i] > 0.0f))
			return reject(fmt::format("splits[{}] = {} must be > 0 (w-units)", i, splits_w[i]));
		if (i > 0 && !(splits_w[i] > splits_w[i - 1]))
			return reject(fmt::format("splits must be strictly ascending in w (near to far); "
									  "splits[{}] = {} is not greater than splits[{}] = {}",
				i, splits_w[i], i - 1, splits_w[i - 1]));
	}

	for (size_t i = 0; i < bands.size(); i++)
	{
		if (!(bands[i].sep > 0.0f))
			return reject(fmt::format("bands[{}].sep = {} must be > 0", i, bands[i].sep));
		if (!(bands[i].conv >= 0.0f))
			return reject(fmt::format("bands[{}].conv = {} must be >= 0 (a negative convergence "
									  "inverts depth)", i, bands[i].conv));
	}

	const u32 n = static_cast<u32>(bands.size());
	out.map = StereoMap::Bands;
	out.band_count = n;
	for (u32 i = 0; i + 1 < n; i++)
		out.split_q[i] = 1.0f / splits_w[i];
	for (u32 i = (n > 0) ? (n - 1) : 0; i < 3; i++)
		out.split_q[i] = kSplitPad;
	for (u32 i = 0; i < n; i++)
	{
		out.conv[i] = bands[i].conv;
		out.sep[i] = bands[i].sep;
	}

	out.bias[0] = 0.0f;
	for (u32 i = 0; i + 1 < n; i++)
	{
		const float s = out.split_q[i];
		out.bias[i + 1] = out.bias[i] + out.sep[i] * (1.0f - out.conv[i] * s) -
						  out.sep[i + 1] * (1.0f - out.conv[i + 1] * s);
	}

	for (u32 i = n; i < 4; i++)
	{
		out.conv[i] = out.conv[n - 1];
		out.sep[i] = out.sep[n - 1];
		out.bias[i] = out.bias[n - 1];
	}

	for (u32 i = 0; i + 1 < n; i++)
	{
		const float d = out.bias[i] + out.sep[i] * (1.0f - out.conv[i] * out.split_q[i]);
		if (std::abs(d) > kStereoBudgetNdc)
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {}: displacement {} at split {} (w = {}) "
							   "exceeds the {} NDC comfort budget; rendering as authored.",
				serial, where, d, i, splits_w[i], kStereoBudgetNdc);
	}

	{
		const float d_inf = VR::ProfileDB::EvalDisparity(out, 0.0f, 0.0f, 0.0f);
		if (d_inf > kStereoBudgetNdc)
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {}: displacement {} AT INFINITY (q = 0, the "
							   "map's maximum) exceeds the {} NDC comfort budget; rendering as authored.",
				serial, where, d_inf, kStereoBudgetNdc);
	}
	return true;
}

u32 VR::ProfileDB::SelectBand(const StereoResolvedMap& map, float q)
{
	if (q >= map.split_q[0])
		return 0;
	if (q >= map.split_q[1])
		return 1;
	if (q >= map.split_q[2])
		return 2;
	return 3;
}

float VR::ProfileDB::EvalBand(const StereoResolvedMap& map, u32 band, float q)
{
	return map.bias[band] + map.sep[band] * (1.0f - map.conv[band] * q);
}

float VR::ProfileDB::EvalDisparity(const StereoResolvedMap& map, float separation, float convergence, float q)
{
	if (map.map == StereoMap::Linear)
	{
		return separation * std::max(0.0f, 1.0f - convergence * q);
	}

	if (map.map == StereoMap::Log)
	{
		const float w = 1.0f / std::max(q, 1e-8f);
		const float t = std::clamp(std::log(w / map.log_w0) / std::log(map.log_w1 / map.log_w0), 0.0f, 1.0f);
		return map.log_dfar * t;
	}

	return std::max(0.0f, EvalBand(map, SelectBand(map, q), q));
}

static float maxDisplacementNdc(const VR::ProfileDB::StereoResolvedMap& map, float separation, float convergence)
{
	float d = VR::ProfileDB::EvalDisparity(map, separation, convergence, 0.0f);
	for (u32 i = 0; i + 1 < map.band_count && i < 3; i++)
		d = std::max(d, VR::ProfileDB::EvalDisparity(map, separation, convergence, map.split_q[i]));
	return d;
}

static void railAtInfinity(const std::string_view serial, const std::string& site,
	const VR::ProfileDB::StereoResolvedMap& map, float separation, float convergence,
	bool from_map, float arcmin_per_ndc, float screen_distance_m)
{
	using VR::ProfileDB::StereoRail;

	const float d_ndc = maxDisplacementNdc(map, separation, convergence);
	const float arcmin = d_ndc * arcmin_per_ndc;
	const std::string display = StringUtil::toUpper(serial);
	const float parallel_arcmin = divergenceArcmin(kNarrowIpdMetres, screen_distance_m);
	const float hard_arcmin = parallel_arcmin + kDivergenceToleranceArcmin;

	if (arcmin > hard_arcmin)
	{
		std::string msg = fmt::format(
			"(VR) ProfileDB: {} {}: far-field separation {:.4f} NDC is past this profile's safe range "
			"({:.4f} NDC). Distant content cannot be fused and will appear doubled — this one is not "
			"content-dependent; fix the profile. Rendering as authored.",
			display, site, d_ndc, hard_arcmin / arcmin_per_ndc);
		Console.Warning(msg);
		s_rail_findings.push_back({StereoRail::Divergence, display, site, arcmin, from_map, std::move(msg)});
	}
	else if (arcmin > parallel_arcmin)
	{
		std::string msg = fmt::format(
			"(VR) ProfileDB: {} {}: far-field separation {:.4f} NDC is past the comfortable range "
			"({:.4f} NDC) but still within tolerance. Whether it reads well depends on the content: "
			"small, sharp, distant objects are the ones that double, hazy ones are not. ADVISORY — "
			"verify it in a headset. Rendering as authored.",
			display, site, d_ndc, parallel_arcmin / arcmin_per_ndc);
		Console.Warning(msg);
		s_rail_findings.push_back({StereoRail::Divergence, display, site, arcmin, from_map, std::move(msg)});
	}
	else if (arcmin > kFixationGapArcmin)
	{
		std::string msg = fmt::format(
			"(VR) ProfileDB: {} {}: far-field separation {:.4f} NDC is {:.1f}x this profile's "
			"comfortable range. Flat overlay draws render at zero separation, so this "
			"is also the worst-case step between a HUD/reticle/target bracket and the world drawn behind "
			"it. ADVISORY: it only bites if this game draws symbology over distant content.",
			display, site, d_ndc, arcmin / kFixationGapArcmin);
		Console.Warning(msg);
		s_rail_findings.push_back({StereoRail::FixationGap, display, site, arcmin, from_map, std::move(msg)});
	}
}

static void railProfile(const std::string_view serial, const VR::ProfileDB::Profile& p)
{
	if (!p.stereo.has_value())
		return;
	const VR::ProfileDB::StereoParams& st = p.stereo.value();

	const float arc_deg = p.screen_arc_deg.value_or(kShipScreenArcDeg);
	const float dist_m = p.screen_distance.value_or(kShipScreenDistanceM);
	const float arcmin_per_ndc = arcminPerNdc(arc_deg);

	railAtInfinity(serial, "stereo", st.resolved, st.separation, st.convergence,
		st.resolved.map != VR::ProfileDB::StereoMap::Linear, arcmin_per_ndc, dist_m);

	for (size_t i = 0; i < st.scenes.size(); i++)
	{
		const VR::ProfileDB::StereoSceneRule& r = st.scenes[i];
		const float sep = r.separation.value_or(st.separation);
		const float conv = r.convergence.value_or(st.convergence);
		const bool has_override = r.map_override.has_value();
		const VR::ProfileDB::StereoResolvedMap& m = has_override ? r.map_override.value() : st.resolved;

		if (!has_override && !r.separation.has_value() && !r.convergence.has_value())
			continue;

		railAtInfinity(serial,
			fmt::format("stereo scene[{}]{}", i, r.label.empty() ? "" : fmt::format(" ({})", r.label)),
			m, sep, conv, true, arcmin_per_ndc, dist_m);
	}
}

static bool railsApplyTo(bool user_authored)
{
	static const bool s_all_tiers = (std::getenv("PCSX2_VR_PROFILE_RAILS") != nullptr);
	return user_authored || s_all_tiers;
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

static std::optional<s64> parseSignedOffset(std::string_view sv);

static void parseGuardList(const std::string_view serial, const ryml::ConstNodeRef& seq,
	const char* what, std::vector<VR::ProfileDB::CameraGuard>& out)
{
	for (const ryml::ConstNodeRef& g : seq.children())
	{
		if (!g.is_map())
			continue;
		const bool chain_form = g.has_child("pointer");
		if (chain_form && g.has_child("address"))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} with BOTH address and pointer; skipping it.", serial, what);
			continue;
		}
		const std::optional<u32> addr = g.has_child("address") ? parseAddress(nodeVal(g["address"])) : std::nullopt;
		const std::optional<u32> root = chain_form ? parseAddress(nodeVal(g["pointer"])) : std::nullopt;
		const bool has_eq = g.has_child("equals");
		const bool has_ne = g.has_child("notEquals");
		if (has_eq && has_ne)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} with BOTH equals and notEquals; skipping it.", serial, what);
			continue;
		}
		const std::string_view eq_raw = has_eq ? nodeVal(g["equals"]) : (has_ne ? nodeVal(g["notEquals"]) : std::string_view{});
		const std::optional<u32> eq = (has_eq || has_ne) ? parseHexU32(eq_raw) : std::nullopt;
		if ((!addr.has_value() && !root.has_value()) || !eq.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} with a missing/invalid address/pointer or equals/notEquals; skipping it.", serial, what);
			continue;
		}
		VR::ProfileDB::CameraGuard guard;
		guard.not_equals = has_ne;
		if (g.has_child("width"))
		{
			const std::optional<u32> wdt = StringUtil::FromChars<u32>(nodeVal(g["width"]));
			if (wdt.has_value() && (wdt.value() == 1 || wdt.value() == 2 || wdt.value() == 4))
				guard.width = static_cast<u8>(wdt.value());
		}
		if (chain_form)
		{
			if (!inMainRam(root.value(), 4))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} pointer {:#x} outside main RAM; skipping it.", serial, what, root.value());
				continue;
			}
			constexpr size_t kMaxGuardHops = 4;
			bool ok = true;
			if (g.has_child("derefOffsets"))
			{
				if (!g["derefOffsets"].is_seq())
					ok = false;
				else
				{
					for (const ryml::ConstNodeRef& hn : g["derefOffsets"].children())
					{
						const std::optional<s64> hop = parseSignedOffset(nodeVal(hn));
						if (guard.deref_offsets.size() >= kMaxGuardHops || !hop.has_value() ||
							hop.value() < static_cast<s64>(std::numeric_limits<s32>::min()) ||
							hop.value() > static_cast<s64>(std::numeric_limits<s32>::max()) || (hop.value() & 3) != 0)
						{
							ok = false;
							break;
						}
						guard.deref_offsets.push_back(static_cast<s32>(hop.value()));
					}
				}
			}
			if (ok && g.has_child("offset"))
			{
				const std::optional<s64> off = parseSignedOffset(nodeVal(g["offset"]));
				if (!off.has_value() || off.value() < static_cast<s64>(std::numeric_limits<s32>::min()) ||
					off.value() > static_cast<s64>(std::numeric_limits<s32>::max()))
					ok = false;
				else
					guard.offset = static_cast<s32>(off.value());
			}
			if (!ok)
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} with an invalid derefOffsets/offset (max 4 word-aligned hops); skipping it.", serial, what);
				continue;
			}
			guard.is_chain = true;
			guard.pointer_addr = root.value();
		}
		else
		{
			if (!inMainRam(addr.value(), guard.width))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} address {:#x}+{} outside/crossing main RAM; skipping it.", serial, what, addr.value(), guard.width);
				continue;
			}
			guard.ee_address = addr.value();
		}
		guard.equals = eq.value();
		warnHexTrap(serial, eq_raw, eq.value(), what);
		warnWidthFit(serial, eq.value(), guard.width, what);
		out.push_back(guard);
	}
}

static bool parseAuthoredDepthMap(const std::string_view serial, const char* where,
	const ryml::ConstNodeRef& node, VR::ProfileDB::StereoMap& map, std::vector<float>& splits,
	std::vector<VR::ProfileDB::StereoBand>& bands,
	std::optional<VR::ProfileDB::StereoLogParams>& log_params)
{
	bool declared = false;

	if (node.has_child("map"))
	{
		declared = true;
		const std::optional<VR::ProfileDB::StereoMap> m = parseStereoMap(nodeVal(node["map"]));
		if (m.has_value())
			map = m.value();
		else
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid map '{}' "
							   "(expected linear|bands|log); using 'linear'.",
				serial, where, nodeVal(node["map"]));
	}

	if (node.has_child("splits") && node["splits"].is_seq())
	{
		for (const ryml::ConstNodeRef& s : node["splits"].children())
		{
			const std::string_view raw = nodeVal(s);
			const std::optional<float> v = StringUtil::FromChars<float>(raw);
			if (v.has_value())
				splits.push_back(v.value());
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid splits entry '{}'; "
								   "ignoring it (the band count check will then reject the map).",
					serial, where, raw);
		}
	}

	if (node.has_child("bands") && node["bands"].is_seq())
	{
		for (const ryml::ConstNodeRef& b : node["bands"].children())
		{
			if (!b.is_map())
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has a bands entry that is not a "
								   "map; ignoring it.", serial, where);
				continue;
			}
			VR::ProfileDB::StereoBand band;
			if (b.has_child("conv"))
			{
				const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(b["conv"]));
				if (v.has_value())
					band.conv = v.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid bands[].conv; "
									   "leaving it 0.", serial, where);
			}
			if (b.has_child("sep"))
			{
				const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(b["sep"]));
				if (v.has_value())
					band.sep = v.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid bands[].sep; "
									   "leaving it 0 (the map will be rejected).", serial, where);
			}
			bands.push_back(band);
		}
	}

	if (node.has_child("log") && node["log"].is_map())
	{
		const ryml::ConstNodeRef ln = node["log"];
		VR::ProfileDB::StereoLogParams lp;
		const auto readOne = [&](const char* key, float& dst) {
			if (!ln.has_child(key))
				return;
			const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(ln[key]));
			if (v.has_value())
				dst = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid log.{}; "
								   "leaving it at its default.", serial, where, key);
		};
		readOne("w0", lp.w0);
		readOne("w1", lp.w1);
		readOne("dfar", lp.dfar);
		if (ln.has_child("dnear"))
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} sets log.dnear, which does not exist: "
							   "Tier-2 stereo is depth-into-screen only and the near end is always 0. "
							   "Ignoring it.", serial, where);
		log_params = lp;
	}

	return declared;
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


static std::optional<VR::ProfileDB::SplitParams> parseSplit(
	const std::string_view serial, const ryml::ConstNodeRef& snode)
{
	using SplitParams = VR::ProfileDB::SplitParams;
	SplitParams sp;
	const auto reject = [&](std::string msg) -> std::optional<SplitParams> {
		advise(serial, "split", std::move(msg) + " — the whole split block is DROPPED; "
			"this game will never split-present until the profile is fixed.");
		return std::nullopt;
	};

	if (snode.has_child("layout"))
	{
		const std::string_view v = nodeVal(snode["layout"]);
		if (StringUtil::compareNoCase(v, "horizontal"))
			sp.layout = SplitParams::Layout::Horizontal;
		else if (StringUtil::compareNoCase(v, "vertical"))
			sp.layout = SplitParams::Layout::Vertical;
		else
			return reject(fmt::format("layout '{}' is not horizontal|vertical", v));
	}

	if (snode.has_child("views"))
	{
		const std::optional<u32> v = parseHexU32(nodeVal(snode["views"]));
		if (!v.has_value() || v.value() != 2)
			return reject("views must be 2 (the only supported count; the key exists so 4-way is additive later)");
		sp.views = 2;
	}

	if (snode.has_child("localView"))
	{
		const std::optional<u32> v = parseHexU32(nodeVal(snode["localView"]));
		if (!v.has_value() || v.value() > 1)
			return reject("localView must be 0 or 1 (reading order)");
		sp.local_view = static_cast<u8>(v.value());
	}

	if (snode.has_child("localPadPort"))
	{
		const std::optional<u32> v = parseHexU32(nodeVal(snode["localPadPort"]));
		if (!v.has_value() || v.value() > 1)
			return reject("localPadPort must be 0 or 1");
		sp.local_pad_port = static_cast<u8>(v.value());
	}

	if (sp.layout == SplitParams::Layout::Horizontal)
	{
		sp.rects[0] = {0.0f, 0.0f, 1.0f, 0.5f};
		sp.rects[1] = {0.0f, 0.5f, 1.0f, 0.5f};
	}
	else
	{
		sp.rects[0] = {0.0f, 0.0f, 0.5f, 1.0f};
		sp.rects[1] = {0.5f, 0.0f, 0.5f, 1.0f};
	}
	if (snode.has_child("rects"))
	{
		if (!snode["rects"].is_seq() || snode["rects"].num_children() != 2)
			return reject("rects must be a sequence of exactly 2 entries");
		size_t i = 0;
		for (const ryml::ConstNodeRef& r : snode["rects"].children())
		{
			SplitParams::Rect rect;
			const char* keys[4] = {"x", "y", "w", "h"};
			float* dsts[4] = {&rect.x, &rect.y, &rect.w, &rect.h};
			for (int k = 0; k < 4; k++)
			{
				const ryml::csubstr ck = ryml::to_csubstr(keys[k]);
				if (!r.has_child(ck))
					return reject(fmt::format("rects[{}] is missing '{}'", i, keys[k]));
				const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(r[ck]));
				if (!v.has_value() || !std::isfinite(v.value()))
					return reject(fmt::format("rects[{}].{} is not a finite number", i, keys[k]));
				*dsts[k] = v.value();
			}
			if (rect.x < 0.0f || rect.y < 0.0f || rect.w <= 0.0f || rect.h <= 0.0f ||
				rect.x + rect.w > 1.0f + 1e-4f || rect.y + rect.h > 1.0f + 1e-4f)
				return reject(fmt::format("rects[{}] must lie inside the unit square with positive size", i));
			sp.rects[i++] = rect;
		}
		{
			const auto& a = sp.rects[0];
			const auto& b = sp.rects[1];
			const float ox = std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
			const float oy = std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
			if (ox * oy > 0.001f)
				return reject(fmt::format("rects overlap by {:.1f}% of the frame — split viewports must be disjoint",
					ox * oy * 100.0f));
		}
		sp.rects_explicit = true;
	}

	if (snode.has_child("active"))
	{
		const ryml::ConstNodeRef a = snode["active"];
		if (!a.is_map() || !a.has_child("address"))
			return reject("active needs a map with at least an address");
		SplitParams::Probe probe;
		const std::optional<u32> addr = parseAddress(nodeVal(a["address"]));
		if (!addr.has_value())
			return reject("active.address does not parse");
		probe.ee_address = addr.value();
		const bool has_eq = a.has_child("equals");
		const bool has_al = a.has_child("atLeast");
		if (has_eq == has_al)
			return reject("active needs exactly ONE of equals|atLeast");
		const std::optional<u32> cmp = parseHexU32(nodeVal(a[has_eq ? "equals" : "atLeast"]));
		if (!cmp.has_value())
			return reject("active comparand does not parse");
		probe.threshold = cmp.value();
		probe.at_least = has_al;
		if (a.has_child("width"))
		{
			const std::optional<u32> w = parseHexU32(nodeVal(a["width"]));
			if (!w.has_value() || (w.value() != 1 && w.value() != 2 && w.value() != 4))
				return reject("active.width must be 1, 2 or 4");
			probe.width = static_cast<u8>(w.value());
		}
		if (!inMainRam(probe.ee_address, probe.width))
			return reject(fmt::format("active.address {:#x} is outside EE main RAM", probe.ee_address));
		sp.active = probe;
	}

	if (snode.has_child("mode"))
	{
		const std::string_view v = nodeVal(snode["mode"]);
		if (StringUtil::compareNoCase(v, "focus"))
			sp.mode = SplitParams::Mode::Focus;
		else if (StringUtil::compareNoCase(v, "duo"))
			sp.mode = SplitParams::Mode::Duo;
		else if (StringUtil::compareNoCase(v, "mirror"))
			sp.mode = SplitParams::Mode::Mirror;
		else if (StringUtil::compareNoCase(v, "local"))
			sp.mode = SplitParams::Mode::Local;
		else
			return reject(fmt::format("mode '{}' is not focus|duo|mirror|local", v));
	}

	if (snode.has_child("sideScale"))
	{
		const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(snode["sideScale"]));
		if (!v.has_value() || !(v.value() > 0.05f) || !(v.value() <= 1.0f))
			return reject("sideScale must be in (0.05, 1.0]");
		sp.side_scale = v.value();
	}
	if (snode.has_child("sideAngleDeg"))
	{
		const std::optional<float> v = StringUtil::FromChars<float>(nodeVal(snode["sideAngleDeg"]));
		if (!v.has_value() || !(std::fabs(v.value()) <= 90.0f))
			return reject("sideAngleDeg must be within +/-90");
		sp.side_angle_deg = v.value();
	}
	if (snode.has_child("stereo"))
	{
		const std::string_view v = nodeVal(snode["stereo"]);
		if (StringUtil::compareNoCase(v, "on") || StringUtil::compareNoCase(v, "true"))
			sp.stereo_on = true;
		else if (StringUtil::compareNoCase(v, "off") || StringUtil::compareNoCase(v, "false"))
			sp.stereo_on = false;
		else
			return reject(fmt::format("stereo '{}' is not on|off", v));
	}

	if (sp.layout == SplitParams::Layout::Vertical)
		advise(serial, "split",
			fmt::format("(VR) ProfileDB: {} split: vertical layout parsed, but the "
						"separation rescale a vertical split needs is not wired up yet — "
						"stereo will read about twice as deep until it is. "
						"Horizontal splits are unaffected.", serial));

	return sp;
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
			if (w.has_child("anchorHead"))
			{
				op.anchor_head = !StringUtil::compareNoCase(nodeVal(w["anchorHead"]), "false");
				if (op.compose != CameraCompose::Anchored)
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' sets anchorHead on a non-anchored camera write op; it only affects compose: anchored.", serial);
			}
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
			if (ptr.has_value() && !inMainRam(ptr.value(), 4))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base pointer {:#x} is outside main RAM; ignoring the base.",
					serial, ptr.value());
			}
			else if (ptr.has_value())
			{
				base.is_pointer = true;
				base.pointer_addr = ptr.value();
				base_ok = true;
				if (bnode.has_child("derefOffsets"))
				{
					constexpr size_t kMaxDerefHops = 4;
					if (!bnode["derefOffsets"].is_seq())
					{
						Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base derefOffsets must be a sequence "
										   "(e.g. [0x188]); ignoring the whole base.", serial);
						base_ok = false;
					}
					else
					{
						for (const ryml::ConstNodeRef& hn : bnode["derefOffsets"].children())
						{
							if (base.deref_offsets.size() >= kMaxDerefHops)
							{
								Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base derefOffsets has more than {} hops; "
												   "ignoring the whole base.", serial, kMaxDerefHops);
								base_ok = false;
								break;
							}
							const std::optional<s64> hop = parseSignedOffset(nodeVal(hn));
							if (!hop.has_value() ||
								hop.value() < static_cast<s64>(std::numeric_limits<s32>::min()) ||
								hop.value() > static_cast<s64>(std::numeric_limits<s32>::max()))
							{
								Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base has an invalid derefOffsets "
												   "entry; ignoring the whole base (relative ops will not fire).", serial);
								base_ok = false;
								break;
							}
							if ((hop.value() & 3) != 0)
							{
								Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base derefOffsets entry {:#x} is not "
												   "word-aligned; ignoring the whole base.", serial, hop.value());
								base_ok = false;
								break;
							}
							base.deref_offsets.push_back(static_cast<s32>(hop.value()));
						}
						if (base_ok && base.deref_offsets.empty())
						{
							Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base derefOffsets is empty; that is a "
											   "plain one-hop pointer base — drop the key.", serial);
						}
					}
				}
				if (bnode.has_child("pattern"))
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base has both pointer and pattern; using the pointer.", serial);
			}
		}
		else
		{
			base_ok = bnode.has_child("pattern") &&
			          parseAobPattern(nodeVal(bnode["pattern"]), base.pattern, base.mask);
		}
		if (!base.is_pointer && bnode.has_child("derefOffsets"))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base has derefOffsets but is not a usable pointer-mode "
							   "base (needs a valid `pointer:`); ignoring the whole base.", serial);
			base_ok = false;
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
			const std::string_view veq_raw = vnode.has_child("equals") ? nodeVal(vnode["equals"]) : std::string_view{};
			const std::optional<u32> veq = vnode.has_child("equals") ? parseHexU32(veq_raw) : std::nullopt;
			if (voff.has_value() && veq.has_value())
			{
				base.has_validate = true;
				base.validate_offset = voff.value();
				base.validate_equals = veq.value();
				warnHexTrap(serial, veq_raw, veq.value(), "camera.base validate.equals");
			}
			else
				base_ok = false;
		}
		if (base_ok && base.is_pointer && !base.deref_offsets.empty() && !base.has_validate)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base uses derefOffsets without a `validate` block; "
							   "a pointer chain needs a guard word on the final object. Ignoring the whole base.", serial);
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
		readOptionalFloat(serial, pnode, "stickFloor", "camera.padLook stickFloor", pl.stick_floor);

		const bool ramp_ok = (pl.max_look_deg > 0.0f) && (pl.engage_deg >= 0.0f) &&
							 (pl.engage_deg < pl.max_look_deg);
		const bool gate_ok = (pl.release_deg >= 0.0f) && (pl.release_deg < pl.engage_deg);
		const bool floor_ok = std::isfinite(pl.stick_floor) && (pl.stick_floor >= 0.0f) &&
							  (pl.stick_floor < 1.0f);
		if (!ramp_ok)
		{
			advise(serial, "camera.padLook",
				fmt::format("(VR) ProfileDB: {} camera.padLook: engageDeg {:g} / maxLookDeg {:g} are out of "
							"order (need 0 <= engageDeg < maxLookDeg) — the whole padLook block is DROPPED, "
							"so this game gets no head-look at all.",
					StringUtil::toUpper(serial), pl.engage_deg, pl.max_look_deg));
		}
		else if (!gate_ok)
		{
			advise(serial, "camera.padLook",
				fmt::format("(VR) ProfileDB: {} camera.padLook: releaseDeg {:g} is not below engageDeg {:g} "
							"(need 0 <= releaseDeg < engageDeg) — a gate that releases at or above the angle "
							"it engages at is not hysteresis. The whole padLook block is DROPPED, so this "
							"game gets no head-look at all. releaseDeg DEFAULTS to 4 deg: an engageDeg at or "
							"under 4 must set it explicitly.",
					StringUtil::toUpper(serial), pl.release_deg, pl.engage_deg));
		}
		else if (!floor_ok)
		{
			advise(serial, "camera.padLook",
				fmt::format("(VR) ProfileDB: {} camera.padLook: stickFloor {:g} is out of range (need "
							"0 <= stickFloor < 1; it is a FRACTION of full stick deflection, not degrees "
							"and not a percent) — the whole padLook block is DROPPED, so this game gets no "
							"head-look at all.",
					StringUtil::toUpper(serial), pl.stick_floor));
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

static constexpr float kMaxCollimateDisparity = 0.01815f;
static constexpr float kSoftCollimateDisparity = 0.0135f;

static void warnUnknownKeys(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* where, std::initializer_list<const char*> known)
{
	for (const ryml::ConstNodeRef& c : node.children())
	{
		if (!c.has_key())
			continue;
		const std::string_view k(c.key().data(), c.key().size());
		bool found = false;
		for (const char* kn : known)
		{
			if (StringUtil::compareNoCase(k, kn))
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an unknown key '{}' in {}; IGNORING it. "
							   "Check the spelling against bin/resources/vr-profiles/README.md — an ignored "
							   "constraint silently widens the rule.",
				serial, k, where);
		}
	}
}

static void readOptionalS32(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* key, const char* what, s32& dst)
{
	if (!node.has_child(key))
		return;
	const std::optional<s32> v = StringUtil::FromChars<s32>(nodeVal(node[key]));
	if (v.has_value())
		dst = v.value();
	else
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid {}; keeping the default.", serial, what);
}

static void readTriState(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* key, const char* what, s8& dst)
{
	if (!node.has_child(key))
		return;
	const std::string_view v = nodeVal(node[key]);
	if (StringUtil::compareNoCase(v, "true"))
		dst = 1;
	else if (StringUtil::compareNoCase(v, "false"))
		dst = 0;
	else if (StringUtil::compareNoCase(v, "any"))
		dst = -1;
	else
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid {} (true|false|any); keeping the default.", serial, what);
}

static std::optional<VR::ProfileDB::HudCollimate> parseHudCollimate(
	const std::string_view serial, const ryml::ConstNodeRef& node, u32 max_rules)
{
	using namespace VR::ProfileDB;

	warnUnknownKeys(serial, node, "stereo.hudCollimate", {"disparity", "rules"});

	HudCollimate hc;
	readOptionalFloat(serial, node, "disparity", "stereo.hudCollimate.disparity", hc.disparity);
	if (!std::isfinite(hc.disparity) || hc.disparity == 0.0f)
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has stereo.hudCollimate with a missing/zero/non-finite "
						   "disparity; ignoring the block (nothing would be displaced).",
			serial);
		return std::nullopt;
	}
	if (std::fabs(hc.disparity) > kMaxCollimateDisparity)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate.disparity {:.4f} exceeds the hard "
						 "divergence limit {:.4f} NDC at the shipped screen geometry; IGNORING the block. "
						 "Author it to the disparity of the depth the symbology must agree with.",
			serial, hc.disparity, kMaxCollimateDisparity);
		return std::nullopt;
	}
	if (std::fabs(hc.disparity) > kSoftCollimateDisparity)
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate.disparity {:.4f} is above the "
						   "safe range for narrow eye spacing ({:.4f} NDC); keeping it, but expect "
						   "strain reports.",
			serial, hc.disparity, kSoftCollimateDisparity);
	}
	if (hc.disparity < 0.0f)
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate.disparity is NEGATIVE, which places "
						   "the symbology IN FRONT of the screen (crossed disparity). Intended only for "
						   "deliberately near overlays — aim symbology wants a positive value.",
			serial);
	}

	if (!node.has_child("rules") || !node["rules"].is_seq())
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has stereo.hudCollimate with no `rules:` list; ignoring "
						   "the block. There is deliberately no 'match every UV draw' shorthand — that would "
						   "move the HUD text and menus too.",
			serial);
		return std::nullopt;
	}

	for (const ryml::ConstNodeRef& rn : node["rules"].children())
	{
		if (!rn.is_map())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo.hudCollimate rule that is not a map; skipping it.", serial);
			continue;
		}
		if (hc.rules.size() >= max_rules)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has more than {} stereo.hudCollimate rules; ignoring the rest.", serial, max_rules);
			break;
		}
		warnUnknownKeys(serial, rn, "a stereo.hudCollimate rule",
			{"label", "prim", "tme", "abe", "minWidth", "maxWidth", "minHeight", "maxHeight", "regionPct", "uvRect"});

		CollimateRule r;
		if (rn.has_child("label"))
		{
			const std::string_view lv = nodeVal(rn["label"]);
			r.label.assign(lv.data(), lv.size());
		}
		if (rn.has_child("prim"))
		{
			const std::string_view p = nodeVal(rn["prim"]);
			if (StringUtil::compareNoCase(p, "point"))
				r.prim = 0;
			else if (StringUtil::compareNoCase(p, "line"))
				r.prim = 1;
			else if (StringUtil::compareNoCase(p, "triangle"))
				r.prim = 2;
			else if (StringUtil::compareNoCase(p, "sprite"))
				r.prim = 3;
			else if (StringUtil::compareNoCase(p, "any"))
				r.prim = -1;
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.hudCollimate rule prim "
								   "(point|line|triangle|sprite|any); keeping 'any'.", serial);
		}
		readTriState(serial, rn, "tme", "stereo.hudCollimate rule tme", r.tme);
		readTriState(serial, rn, "abe", "stereo.hudCollimate rule abe", r.abe);
		readOptionalS32(serial, rn, "minWidth", "stereo.hudCollimate rule minWidth", r.min_w);
		readOptionalS32(serial, rn, "maxWidth", "stereo.hudCollimate rule maxWidth", r.max_w);
		readOptionalS32(serial, rn, "minHeight", "stereo.hudCollimate rule minHeight", r.min_h);
		readOptionalS32(serial, rn, "maxHeight", "stereo.hudCollimate rule maxHeight", r.max_h);
		if (rn.has_child("regionPct") && rn["regionPct"].is_seq() && rn["regionPct"].num_children() == 4)
		{
			float v[4] = {};
			u32 i = 0;
			bool ok = true;
			for (const ryml::ConstNodeRef& c : rn["regionPct"].children())
			{
				const std::optional<float> pv = StringUtil::FromChars<float>(nodeVal(c));
				if (!pv.has_value() || !std::isfinite(pv.value()))
				{
					ok = false;
					break;
				}
				v[i++] = pv.value();
			}
			if (ok && v[2] > v[0] && v[3] > v[1] && v[0] >= 0.0f && v[1] >= 0.0f && v[2] <= 1.0f && v[3] <= 1.0f)
			{
				r.rx0 = v[0];
				r.ry0 = v[1];
				r.rx1 = v[2];
				r.ry1 = v[3];
			}
			else
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.hudCollimate rule regionPct "
								   "(want [x0, y0, x1, y1] fractions in 0..1 with x1 > x0 and y1 > y0); ignoring "
								   "the region — the rule is now WIDER than authored.", serial);
			}
		}
		else if (rn.has_child("regionPct"))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo.hudCollimate rule regionPct that is not a "
							   "4-element sequence; ignoring the region — the rule is now WIDER than authored.", serial);
		}
		if (rn.has_child("uvRect") && rn["uvRect"].is_seq() && rn["uvRect"].num_children() == 4)
		{
			float v[4] = {};
			u32 i = 0;
			bool ok = true;
			for (const ryml::ConstNodeRef& c : rn["uvRect"].children())
			{
				const std::optional<float> pv = StringUtil::FromChars<float>(nodeVal(c));
				if (!pv.has_value() || !std::isfinite(pv.value()))
				{
					ok = false;
					break;
				}
				v[i++] = pv.value();
			}
			if (ok && v[2] > v[0] && v[3] > v[1] && v[0] >= 0.0f && v[1] >= 0.0f)
			{
				r.tu0 = v[0];
				r.tv0 = v[1];
				r.tu1 = v[2];
				r.tv1 = v[3];
			}
			else
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.hudCollimate rule uvRect "
								   "(want [u0, v0, u1, v1] texels >= 0 with u1 > u0 and v1 > v0); ignoring the "
								   "UV constraint — the rule is now WIDER than authored.", serial);
			}
		}
		else if (rn.has_child("uvRect"))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo.hudCollimate rule uvRect that is not a "
							   "4-element sequence; ignoring the UV constraint — the rule is now WIDER than authored.", serial);
		}

		const bool has_extent = (r.min_w > 0 || r.max_w > 0 || r.min_h > 0 || r.max_h > 0);
		const bool has_region = (r.rx1 > r.rx0 && r.ry1 > r.ry0);
		const bool has_uv = (r.tu1 > r.tu0 && r.tv1 > r.tv0);
		if (r.prim < 0 && r.abe < 0 && !has_extent && !has_region && !has_uv)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo.hudCollimate rule '{}' with no "
							   "discriminating constraint (prim/abe/extent/regionPct/uvRect all unset); skipping "
							   "it. An unconstrained rule would collimate HUD text and menus too.",
				serial, r.label.empty() ? std::string_view("<unlabelled>") : std::string_view(r.label));
			continue;
		}
		if (!has_extent && !has_region && !has_uv)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate rule '{}' has NO geometric "
							   "constraint (extent, regionPct and uvRect are all unset), so it matches every "
							   "draw carrying that render state — on a 2D HUD that is most of the HUD, text "
							   "and menus included. If a key was misspelled, the unknown-key warning above "
							   "names it.",
				serial, r.label.empty() ? std::string_view("<unlabelled>") : std::string_view(r.label));
		}
		if (r.max_w > 0 && r.min_w > r.max_w)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate rule '{}' has minWidth > maxWidth; "
							   "skipping it (it can never match).", serial, r.label);
			continue;
		}
		if (r.max_h > 0 && r.min_h > r.max_h)
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' stereo.hudCollimate rule '{}' has minHeight > maxHeight; "
							   "skipping it (it can never match).", serial, r.label);
			continue;
		}
		hc.rules.push_back(std::move(r));
	}

	if (hc.rules.empty())
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has stereo.hudCollimate with no usable rule; ignoring the block.", serial);
		return std::nullopt;
	}
	return hc;
}

static bool validControlInstanceId(const std::string_view id)
{
	if (id.empty() || id.size() > 32)
		return false;
	for (const char ch : id)
	{
		const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
		if (!ok)
			return false;
	}
	return true;
}

static void readOptionalBool(const std::string_view serial, const ryml::ConstNodeRef& node, const char* key,
	const char* what, bool& dst)
{
	if (!node.has_child(key))
		return;
	const std::string_view v = nodeVal(node[key]);
	if (StringUtil::compareNoCase(v, "true") || StringUtil::compareNoCase(v, "yes") || StringUtil::compareNoCase(v, "on"))
		dst = true;
	else if (StringUtil::compareNoCase(v, "false") || StringUtil::compareNoCase(v, "no") || StringUtil::compareNoCase(v, "off"))
		dst = false;
	else
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} '{}' is not a boolean; keeping the default.", serial, what, v);
}

static std::optional<VR::ProfileDB::SpatialControlSpec> parseControlEntry(const std::string_view serial,
	const ryml::ConstNodeRef& c, size_t index, const std::unordered_set<std::string>& seen_ids)
{
	using namespace VR::SpatialControls;
	using VR::ProfileDB::SpatialControlSpec;

	const std::string site = fmt::format("controls[{}]", index);
	const std::string display = StringUtil::toUpper(serial);
	const auto reject = [&](const std::string& why) -> std::optional<SpatialControlSpec> {
		advise(serial, "controls",
			fmt::format("(VR) ProfileDB: {} {}: {} — the whole control is DROPPED, so this game gets no such device.",
				display, site, why));
		return std::nullopt;
	};

	if (!c.is_map())
		return reject("the entry is not a map");
	warnUnknownKeys(serial, c, site.c_str(), {"device", "id", "placement", "params", "when", "bind", "pad", "usbPort", "notes"});

	SpatialControlSpec spec;
	if (!c.has_child("device"))
		return reject("has no `device`");
	spec.device.assign(nodeVal(c["device"]));
	const DeviceDef* def = FindDevice(spec.device);
	if (!def)
	{
		return reject(fmt::format("device '{}' is not in the catalogue (Throttle, TwinThrottles, Wheel, Shifter, Stick, "
								  "LightGun — exact, case-sensitive)",
			spec.device));
	}
	spec.kind = def->kind;

	if (!c.has_child("id"))
		return reject("has no `id`");
	spec.id.assign(nodeVal(c["id"]));
	if (!validControlInstanceId(spec.id))
		return reject(fmt::format("id '{}' must be 1..32 characters of [A-Za-z0-9_-] (it becomes the device name VR-<id>)", spec.id));
	if (seen_ids.count(spec.id) != 0)
		return reject(fmt::format("id '{}' is already used by an earlier control", spec.id));

	if (def->kind == DeviceKind::Wheel || def->kind == DeviceKind::Stick)
		spec.release = ReleaseMode::Spring;
	if (def->kind == DeviceKind::Shifter)
		spec.grab_mode = GrabMode::Hold;

	if (c.has_child("placement"))
	{
		if (!c["placement"].is_map())
			return reject("placement is not a map");
		const ryml::ConstNodeRef p = c["placement"];
		const std::string where = site + ".placement";
		warnUnknownKeys(serial, p, where.c_str(), {"side", "height", "forward", "yawDeg"});
		readOptionalFloat(serial, p, "side", "controls placement side", spec.placement.side);
		readOptionalFloat(serial, p, "height", "controls placement height", spec.placement.height);
		readOptionalFloat(serial, p, "forward", "controls placement forward", spec.placement.forward);
		readOptionalFloat(serial, p, "yawDeg", "controls placement yawDeg", spec.placement.yaw_deg);
	}

	if (c.has_child("params"))
	{
		if (!c["params"].is_map())
			return reject("params is not a map");
		const ryml::ConstNodeRef p = c["params"];
		const std::string where = site + ".params";
		warnUnknownKeys(serial, p, where.c_str(),
			{"travel", "grabRadius", "release", "springRate", "oneWay", "detented", "power", "engage", "steerFullLock",
				"steerDeadband", "steerCurve", "steerSign", "outputFloor", "lockToLockDeg", "rimRadius", "gears", "reverse",
				"neutral", "travelX", "travelY", "twist", "twistRangeDeg", "aim", "aimHand", "preset", "mode", "leverSign",
				"grabMode", "breakAway", "grabLength", "sweep", "armLength", "upright"});
		readOptionalFloat(serial, p, "travel", "controls travel", spec.travel);
		readOptionalFloat(serial, p, "sweep", "controls sweep", spec.sweep_deg);
		readOptionalFloat(serial, p, "armLength", "controls armLength", spec.arm_length);
		readOptionalBool(serial, p, "upright", "controls upright", spec.upright);
		if (p.has_child("travel") && (def->kind == DeviceKind::Throttle || def->kind == DeviceKind::TwinThrottles))
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' {}: `travel` is the slide / Shifter key; a {} is a lever arm — use "
							   "`sweep` (degrees) and `armLength` (metres). Ignored.",
				serial, site, spec.device);
		}
		readOptionalFloat(serial, p, "grabRadius", "controls grabRadius", spec.grab_radius);
		readOptionalFloat(serial, p, "grabLength", "controls grabLength", spec.grab_length);
		readOptionalFloat(serial, p, "breakAway", "controls breakAway", spec.break_away);
		if (p.has_child("grabMode"))
		{
			const std::string_view v = nodeVal(p["grabMode"]);
			if (StringUtil::compareNoCase(v, "toggle"))
				spec.grab_mode = GrabMode::Toggle;
			else if (StringUtil::compareNoCase(v, "hold"))
				spec.grab_mode = GrabMode::Hold;
			else
				return reject(fmt::format("grabMode '{}' is not one of toggle, hold", v));
		}
		readOptionalFloat(serial, p, "springRate", "controls springRate", spec.spring_rate);
		readOptionalBool(serial, p, "oneWay", "controls oneWay", spec.one_way);
		readOptionalS32(serial, p, "detented", "controls detented", spec.detented);
		readOptionalFloat(serial, p, "engage", "controls engage", spec.engage);
		readOptionalFloat(serial, p, "steerFullLock", "controls steerFullLock", spec.steer_full_lock);
		readOptionalFloat(serial, p, "steerDeadband", "controls steerDeadband", spec.steer_deadband);
		readOptionalFloat(serial, p, "steerCurve", "controls steerCurve", spec.steer_curve);
		readOptionalFloat(serial, p, "steerSign", "controls steerSign", spec.steer_sign);
		readOptionalFloat(serial, p, "outputFloor", "controls outputFloor", spec.output_floor);
		readOptionalFloat(serial, p, "leverSign", "controls leverSign", spec.lever_sign);
		readOptionalFloat(serial, p, "lockToLockDeg", "controls lockToLockDeg", spec.lock_to_lock_deg);
		readOptionalFloat(serial, p, "rimRadius", "controls rimRadius", spec.rim_radius);
		readOptionalS32(serial, p, "gears", "controls gears", spec.gears);
		readOptionalBool(serial, p, "reverse", "controls reverse", spec.has_reverse);
		readOptionalBool(serial, p, "neutral", "controls neutral", spec.has_neutral);
		readOptionalFloat(serial, p, "travelX", "controls travelX", spec.travel_x);
		readOptionalFloat(serial, p, "travelY", "controls travelY", spec.travel_y);
		readOptionalBool(serial, p, "twist", "controls twist", spec.twist);
		readOptionalFloat(serial, p, "twistRangeDeg", "controls twistRangeDeg", spec.twist_range_deg);
		if (p.has_child("release"))
		{
			const std::string_view v = nodeVal(p["release"]);
			if (StringUtil::compareNoCase(v, "latch"))
				spec.release = ReleaseMode::Latch;
			else if (StringUtil::compareNoCase(v, "spring"))
				spec.release = ReleaseMode::Spring;
			else if (StringUtil::compareNoCase(v, "detent"))
				spec.release = ReleaseMode::Detent;
			else
				return reject(fmt::format("release '{}' is not one of latch, spring, detent", v));
		}
		if (p.has_child("power"))
		{
			const std::string_view v = nodeVal(p["power"]);
			if (StringUtil::compareNoCase(v, "sum"))
				spec.power = PowerMode::Sum;
			else if (StringUtil::compareNoCase(v, "max"))
				spec.power = PowerMode::Max;
			else
				return reject(fmt::format("power '{}' is not one of sum, max", v));
		}
		if (p.has_child("mode"))
		{
			const std::string_view v = nodeVal(p["mode"]);
			if (StringUtil::compareNoCase(v, "composed"))
				spec.mode = TwinMode::Composed;
			else if (StringUtil::compareNoCase(v, "direct"))
				spec.mode = TwinMode::Direct;
			else
				return reject(fmt::format("mode '{}' is not one of composed, direct", v));
		}
		if (p.has_child("aim"))
		{
			const std::string_view v = nodeVal(p["aim"]);
			if (StringUtil::compareNoCase(v, "pistol"))
				spec.rifle = false;
			else if (StringUtil::compareNoCase(v, "rifle"))
				spec.rifle = true;
			else
				return reject(fmt::format("aim '{}' is not one of pistol, rifle", v));
		}
		if (p.has_child("aimHand"))
		{
			const std::string_view v = nodeVal(p["aimHand"]);
			if (StringUtil::compareNoCase(v, "left"))
				spec.aim_left = true;
			else if (StringUtil::compareNoCase(v, "right"))
				spec.aim_left = false;
			else
				return reject(fmt::format("aimHand '{}' is not one of left, right", v));
		}
		if (p.has_child("preset"))
		{
			const std::string_view v = nodeVal(p["preset"]);
			if (!StringUtil::compareNoCase(v, "pod") && !StringUtil::compareNoCase(v, "tank"))
				return reject(fmt::format("preset '{}' is not one of pod, tank", v));
			spec.preset.assign(v);
		}
	}

	const auto finite_pos = [](float v) { return std::isfinite(v) && v > 0.0f; };
	const auto unit_range = [](float v) { return std::isfinite(v) && v >= 0.0f && v < 1.0f; };
	if (!finite_pos(spec.travel))
		return reject(fmt::format("travel {:g} must be > 0 (metres from rest to full)", spec.travel));
	if (!finite_pos(spec.grab_radius))
		return reject(fmt::format("grabRadius {:g} must be > 0 (metres: the grab capsule's radius about the handle)", spec.grab_radius));
	if (!(std::isfinite(spec.grab_length) && spec.grab_length >= 0.0f))
		return reject(fmt::format("grabLength {:g} must be >= 0 (metres: the grab capsule's length along the handle)", spec.grab_length));
	if (!(std::isfinite(spec.sweep_deg) && spec.sweep_deg > 0.0f && spec.sweep_deg <= 180.0f))
		return reject(fmt::format("sweep {:g} must be in (0, 180] degrees (the lever arm's full arc, rear stop to front stop)", spec.sweep_deg));
	if (!finite_pos(spec.arm_length))
		return reject(fmt::format("armLength {:g} must be > 0 (metres from the arm's pivot to the grip)", spec.arm_length));
	if (!(std::isfinite(spec.break_away) && spec.break_away >= 0.0f))
		return reject(fmt::format("breakAway {:g} must be >= 0 (metres from the handle; 0 disables)", spec.break_away));
	if (!finite_pos(spec.spring_rate))
		return reject(fmt::format("springRate {:g} must be > 0 (full-travel units per second)", spec.spring_rate));
	if (spec.detented != 0 && (spec.detented < 2 || spec.detented > kMaxNotches))
		return reject(fmt::format("detented {} must be 0 (continuous) or 2..{}", spec.detented, kMaxNotches));
	if (!unit_range(spec.engage))
		return reject(fmt::format("engage {:g} is out of range (need 0 <= engage < 1; 0 = continuous Power)", spec.engage));
	if (!finite_pos(spec.steer_full_lock))
		return reject(fmt::format("steerFullLock {:g} must be > 0 (the lever difference that means full lock)", spec.steer_full_lock));
	if (!unit_range(spec.steer_deadband))
		return reject(fmt::format("steerDeadband {:g} is out of range (need 0 <= steerDeadband < 1)", spec.steer_deadband));
	if (!(std::isfinite(spec.steer_curve) && spec.steer_curve >= 0.2f && spec.steer_curve <= 5.0f))
		return reject(fmt::format("steerCurve {:g} is out of range (need 0.2 <= steerCurve <= 5)", spec.steer_curve));
	if (!(spec.steer_sign == 1.0f || spec.steer_sign == -1.0f))
		return reject(fmt::format("steerSign {:g} must be 1 or -1 — determine it by observing the game, do not guess", spec.steer_sign));
	if (!(spec.lever_sign == 1.0f || spec.lever_sign == -1.0f))
		return reject(fmt::format("leverSign {:g} must be 1 or -1 — determine it by observing the game, do not guess", spec.lever_sign));
	if (!unit_range(spec.output_floor))
	{
		return reject(fmt::format("outputFloor {:g} is out of range (need 0 <= outputFloor < 1; it is a FRACTION of full "
								  "deflection — the game's own stick deadzone, measured, not a percent)",
			spec.output_floor));
	}
	if (!finite_pos(spec.lock_to_lock_deg))
		return reject(fmt::format("lockToLockDeg {:g} must be > 0", spec.lock_to_lock_deg));
	if (!finite_pos(spec.rim_radius))
		return reject(fmt::format("rimRadius {:g} must be > 0 (metres)", spec.rim_radius));
	if (spec.gears < 1 || spec.gears > 6)
		return reject(fmt::format("gears {} must be 1..6", spec.gears));
	if (!finite_pos(spec.travel_x) || !finite_pos(spec.travel_y))
		return reject(fmt::format("travelX {:g} / travelY {:g} must be > 0 (metres to full deflection)", spec.travel_x, spec.travel_y));
	if (!finite_pos(spec.twist_range_deg))
		return reject(fmt::format("twistRangeDeg {:g} must be > 0", spec.twist_range_deg));

	if (c.has_child("when"))
	{
		if (!c["when"].is_seq())
			return reject("when is not a list");
		parseGuardList(serial, c["when"], "controls when-guard", spec.when);
	}

	if (c.has_child("pad"))
	{
		const std::optional<u32> v = StringUtil::FromChars<u32>(nodeVal(c["pad"]));
		if (!v.has_value() || v.value() < 1 || v.value() > 8)
			return reject("pad must be 1..8 (the DualShock port, 1-based)");
		spec.pad_port = v.value() - 1;
	}
	if (c.has_child("usbPort"))
	{
		const std::optional<u32> v = StringUtil::FromChars<u32>(nodeVal(c["usbPort"]));
		if (!v.has_value() || v.value() < 1 || v.value() > 2)
			return reject("usbPort must be 1 or 2 (1-based)");
		spec.usb_port = static_cast<int>(v.value()) - 1;
	}

	if (c.has_child("bind"))
	{
		if (!c["bind"].is_map())
			return reject("bind is not a map");
		for (const ryml::ConstNodeRef& kv : c["bind"].children())
		{
			if (!kv.has_key())
				continue;
			const std::string_view control(kv.key().data(), kv.key().size());
			const ControlDef* cd = FindControl(*def, control);
			if (!cd)
			{
				std::string have;
				for (const ControlDef& d : def->controls)
				{
					if (!have.empty())
						have += ", ";
					have += d.name;
				}
				return reject(fmt::format("bind names '{}', which {} does not have (its controls: {})", control, spec.device, have));
			}
			const std::string_view target = nodeVal(kv);
			if (target.empty())
				return reject(fmt::format("bind '{}' has an empty target", control));
			SpatialControlSpec::Bind b;
			b.control.assign(control);
			b.target.assign(target);
			spec.bind.push_back(std::move(b));
		}
	}

	return spec;
}

static std::vector<VR::ProfileDB::SpatialControlSpec> parseControls(const std::string_view serial, const ryml::ConstNodeRef& seq)
{
	std::vector<VR::ProfileDB::SpatialControlSpec> out;
	std::unordered_set<std::string> seen;
	size_t index = 0;
	for (const ryml::ConstNodeRef& c : seq.children())
	{
		std::optional<VR::ProfileDB::SpatialControlSpec> spec = parseControlEntry(serial, c, index++, seen);
		if (!spec.has_value())
			continue;
		seen.insert(spec->id);
		out.push_back(std::move(spec.value()));
	}
	return out;
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

			static bool s_uv_draws_notice_logged = false;
			if (!s_uv_draws_notice_logged)
			{
				s_uv_draws_notice_logged = true;
				Console.WarningFmt("(VR) ProfileDB: stereo.uvDraws (first seen on '{}') is INFORMATIONAL ONLY in "
								   "this build: the UV/FST displacement exclusion is unconditional, so neither "
								   "'screen' nor 'world' changes any rendering. Keep it authored — do not tune "
								   "with it, and do not explain a stereo result by it.",
					serial);
			}
		}
		if (snode.has_child("pinUniformQ"))
			sp.pin_uniform_q = StringUtil::compareNoCase(nodeVal(snode["pinUniformQ"]), "true");

		if (snode.has_child("zDrivenDepth"))
			sp.z_driven_depth = StringUtil::compareNoCase(nodeVal(snode["zDrivenDepth"]), "true");

		if (snode.has_child("hudCollimate") && snode["hudCollimate"].is_map())
			sp.hud_collimate = parseHudCollimate(serial, snode["hudCollimate"], kMaxCollimateRules);
		else if (snode.has_child("hudCollimate"))
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a stereo.hudCollimate that is not a map; ignoring it.", serial);

		parseAuthoredDepthMap(serial, "stereo", snode, sp.map, sp.splits, sp.bands, sp.log_params);
		if (!resolveDepthMap(serial, "stereo", sp.map, sp.splits, sp.bands, sp.log_params, sp.resolved))
			sp.map = StereoMap::Linear;

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
				{
					StereoMap smap = StereoMap::Linear;
					std::vector<float> ssplits;
					std::vector<StereoBand> sbands;
					std::optional<StereoLogParams> slog;
					if (parseAuthoredDepthMap(serial, "stereo scene", sc, smap, ssplits, sbands, slog))
					{
						StereoResolvedMap sresolved;
						if (resolveDepthMap(serial, "stereo scene", smap, ssplits, sbands, slog, sresolved))
							rule.map_override = sresolved;
					}
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
		if (scr.has_child("follow"))
		{
			const std::string_view v = nodeVal(scr["follow"]);
			if (StringUtil::compareNoCase(v, "head"))
				out.screen_follow_head = true;
			else if (StringUtil::compareNoCase(v, "world"))
				out.screen_follow_head = false;
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.follow '{}' (not head|world); ignoring it.", serial, v);
		}
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

	if (node.has_child("split") && node["split"].is_map())
		out.split = parseSplit(serial, node["split"]);

	if (node.has_child("controls"))
	{
		if (node["controls"].is_seq())
			out.controls = parseControls(serial, node["controls"]);
		else
			advise(serial, "controls",
				fmt::format("(VR) ProfileDB: {} controls: is not a list — IGNORED, so this game gets no virtual control.",
					StringUtil::toUpper(serial)));
	}

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
			if (railsApplyTo(tier == LoadTier::User))
				railProfile(serial, profile);
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

static u64 fnv1aString(std::string_view s)
{
	constexpr u64 kPrime = 1099511628211ull;
	u64 h = 1469598103934665603ull;
	for (const unsigned char c : s)
		h = (h ^ c) * kPrime;
	return h;
}

static std::optional<std::string> findShippedProfileFile(const char* file_name)
{
	const std::string dir(Path::Combine(EmuFolders::Resources, VRPROFILES_DIR_NAME));
	if (!FileSystem::DirectoryExists(dir.c_str()))
		return std::nullopt;

	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(dir.c_str(), file_name,
			FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files) ||
		files.empty())
	{
		return std::nullopt;
	}
	return files.front().FileName;
}

static void ensureUserFolderSeed(const std::string& dir)
{
	const std::optional<std::string> src_path = findShippedProfileFile(VRPROFILES_SEED_FILE_NAME);
	if (!src_path.has_value())
		return;

	const std::optional<std::string> src_text = FileSystem::ReadFileToString(src_path->c_str());
	if (!src_text.has_value())
		return;

	const u64 src_sig = fnv1aString(src_text.value());
	const std::string dest(Path::Combine(dir, VRPROFILES_SEED_FILE_NAME));

	if (FileSystem::FileExists(dest.c_str()))
	{
		const std::optional<std::string> have = FileSystem::ReadFileToString(dest.c_str());
		if (!have.has_value())
			return;
		const size_t at = have->find(VRPROFILES_SEED_MARKER);
		if (at == std::string::npos)
			return;
		const u64 was = StringUtil::FromChars<u64>(
			std::string_view(*have).substr(at + (sizeof(VRPROFILES_SEED_MARKER) - 1), 16), 16)
							.value_or(src_sig);
		if (was != src_sig)
		{
			Console.WarningFmt(
				"(VR) ProfileDB: '{}' in your profiles folder was seeded from an OLDER shipped "
				"profile and still overrides it. Delete it to take the updated one, or re-apply "
				"your edits on top of a fresh copy.",
				VRPROFILES_SEED_FILE_NAME);
		}
		return;
	}

	const std::string header = fmt::format(
		"# ─────────────────────────────────────────────────────────────────────\n"
		"# A WORKING EXAMPLE, and it is LIVE — this file overrides the shipped\n"
		"# profile for this game right now. It was copied here unchanged on first\n"
		"# use so you have something real to edit.\n"
		"#\n"
		"# TRY IT: change `separation` below, save, boot the game. The emulog says\n"
		"#   \"user profile overrides shipped '{}'\"\n"
		"# on every launch while this file exists — that line is how you confirm\n"
		"# your copy, not ours, is in effect.\n"
		"#\n"
		"# TO GO BACK: delete this file. Nothing else to undo.\n"
		"#\n"
		"# Because it overrides, it also FREEZES this game at the values below: a\n"
		"# later update to the shipped profile will not reach you until you delete\n"
		"# this file. You will be warned in the log if that happens.\n"
		"{}{:016x}\n"
		"# ─────────────────────────────────────────────────────────────────────\n",
		StringUtil::toLower(std::string(Path::StripExtension(VRPROFILES_SEED_FILE_NAME))),
		VRPROFILES_SEED_MARKER, src_sig);

	if (FileSystem::WriteStringToFile(dest.c_str(), header + src_text.value()))
	{
		Console.WriteLnFmt("(VR) ProfileDB: seeded the user profiles folder with a live copy of '{}' "
						   "(edit it to customise; delete it to revert).",
			VRPROFILES_SEED_FILE_NAME);
	}
}

static void ensureUserFolderReadme(const std::string& dir)
{
	const std::string readme(Path::Combine(dir, "README.txt"));
	if (FileSystem::FileExists(readme.c_str()))
	{
		const std::optional<std::string> have = FileSystem::ReadFileToString(readme.c_str());
		if (!have.has_value() || have->rfind("VR profiles", 0) != 0 ||
			have->find(VRPROFILES_SEED_FILE_NAME) != std::string::npos)
		{
			return;
		}
	}
	const std::string text = fmt::format(
		"VR profiles — user folder\n"
		"=========================\n"
		"One game per file (SLUS-12345.yaml). Files here OVERRIDE the shipped\n"
		"profile for the same serial, per file. To customize a shipped game,\n"
		"copy its file from the install's resources/vr-profiles/ folder here\n"
		"and edit the copy. Every file is checked at launch; invalid files\n"
		"are reported in a dialog naming the file and the reason.\n"
		"\n"
		"{} was placed here for you on first use: a live, unmodified copy\n"
		"of the shipped profile, so you have a real working example to edit.\n"
		"It is ACTIVE - it overrides the shipped one for that game. Edit a\n"
		"value, boot the game, and the emulog line\n"
		"  \"user profile overrides shipped '{}'\"\n"
		"confirms your copy is the one in effect. Delete the file to revert.\n",
		VRPROFILES_SEED_FILE_NAME,
		StringUtil::toLower(std::string(Path::StripExtension(VRPROFILES_SEED_FILE_NAME))));
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
		ensureUserFolderSeed(EmuFolders::VRProfiles);
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
	s_rail_findings.clear();
	s_advisories.clear();

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

const std::vector<VR::ProfileDB::StereoRailFinding>& VR::ProfileDB::StereoRailFindings()
{
	EnsureLoaded();
	return s_rail_findings;
}

const std::vector<VR::ProfileDB::ProfileAdvisory>& VR::ProfileDB::ProfileAdvisories()
{
	EnsureLoaded();
	return s_advisories;
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
	s_rail_findings.clear();
	s_advisories.clear();
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

static int CountMultibandResolveMismatches(bool log)
{
	using VR::ProfileDB::StereoBand;
	using VR::ProfileDB::StereoLogParams;
	using VR::ProfileDB::StereoMap;
	using VR::ProfileDB::StereoResolvedMap;

	int fail = 0;
	const auto check = [&](bool ok, const char* name) {
		if (!ok)
		{
			++fail;
			if (log)
				Console.WriteLn("(VR) multiband resolve self-test FAIL: %s", name);
		}
	};
	const auto approxEq = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
	constexpr float kPad = -std::numeric_limits<float>::max();

	const auto evalBand = [](const StereoResolvedMap& m, u32 i, float q) {
		return VR::ProfileDB::EvalBand(m, i, q);
	};
	const auto selectBand = [](const StereoResolvedMap& m, float q) -> u32 {
		return VR::ProfileDB::SelectBand(m, q);
	};
	const auto resolve = [](StereoMap map, const std::vector<float>& splits,
							 const std::vector<StereoBand>& bands,
							 const std::optional<StereoLogParams>& lp, StereoResolvedMap& out) {
		return resolveDepthMap("SELFTEST", "self-test", map, splits, bands, lp, out);
	};
	const std::optional<StereoLogParams> kNoLog;

	StereoResolvedMap m;

	check(resolve(StereoMap::Linear, {}, {}, kNoLog, m), "linear resolves");
	check(m.map == StereoMap::Linear && m.band_count == 1, "linear -> Linear, 1 band");

	const std::vector<StereoBand> two = {{6.0f, 0.008f}, {40.0f, 0.012f}};
	check(resolve(StereoMap::Bands, {8.0f}, two, kNoLog, m), "2-band resolves");
	check(m.map == StereoMap::Bands && m.band_count == 2, "2-band -> Bands, 2 bands");
	check(approxEq(m.split_q[0], 0.125f), "2-band split_q = 1/w");
	check(approxEq(m.bias[0], 0.0f), "bias[0] == 0");
	check(approxEq(m.bias[1], 0.050f), "2-band bias[1] == 0.050 (hand-computed)");
	check(approxEq(evalBand(m, 0, 0.125f), 0.002f), "2-band d at split == 0.002");
	check(approxEq(evalBand(m, 0, 0.125f), evalBand(m, 1, 0.125f)), "2-band continuous at split");

	check(m.split_q[1] == kPad && m.split_q[2] == kPad, "2-band pads split_q with -FLT_MAX");
	check(approxEq(m.conv[2], m.conv[1]) && approxEq(m.conv[3], m.conv[1]), "2-band pads conv from last band");
	check(approxEq(m.sep[2], m.sep[1]) && approxEq(m.sep[3], m.sep[1]), "2-band pads sep from last band");
	check(approxEq(m.bias[2], m.bias[1]) && approxEq(m.bias[3], m.bias[1]), "2-band pads bias from last band");
	check(selectBand(m, 1.0f) == 0, "select: q above split -> band 0");
	check(selectBand(m, 0.01f) == 1, "select: q below split -> band 1 (pad falls through)");

	const std::vector<StereoBand> three = {{1.0f, 0.010f}, {10.0f, 0.012f}, {100.0f, 0.014f}};
	check(resolve(StereoMap::Bands, {2.0f, 8.0f}, three, kNoLog, m), "3-band resolves");
	check(m.band_count == 3, "3-band count");
	check(approxEq(m.split_q[0], 0.5f) && approxEq(m.split_q[1], 0.125f), "3-band w->q values");
	check(m.split_q[0] > m.split_q[1], "3-band split_q DESCENDING (w ascending inverts)");
	check(m.split_q[2] == kPad, "3-band pads the unused split only");
	check(approxEq(evalBand(m, 0, m.split_q[0]), evalBand(m, 1, m.split_q[0])), "3-band continuous at split 0");
	check(approxEq(evalBand(m, 1, m.split_q[1]), evalBand(m, 2, m.split_q[1])), "3-band continuous at split 1");
	check(selectBand(m, 0.01f) == 2, "3-band select: far q -> band 2");

	check(approxEq(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 0.01f),
			  std::max(0.0f, evalBand(m, selectBand(m, 0.01f), 0.01f))),
		"EvalDisparity == max(0, EvalBand(SelectBand(q), q))");
	check(approxEq(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 0.0f), m.bias[2] + m.sep[2]),
		"EvalDisparity at q=0 == bias[last] + sep[last] (the at-infinity rail value)");
	check(evalBand(m, 0, 2.0f) < 0.0f, "raw band 0 is negative at q=2");
	check(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 2.0f) == 0.0f, "bands deep-window clamp pins near content to 0");
	check(approxEq(VR::ProfileDB::EvalDisparity(m, 999.0f, 999.0f, 0.0f),
			  VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 0.0f)),
		"bands mode ignores the scalar separation/convergence pair");
	{
		StereoResolvedMap lin;
		check(lin.map == StereoMap::Linear, "default StereoResolvedMap is Linear");
		check(approxEq(VR::ProfileDB::EvalDisparity(lin, 0.02f, 6.0f, 0.0f), 0.02f),
			"linear d(inf) == separation");
		check(approxEq(VR::ProfileDB::EvalDisparity(lin, 0.02f, 6.0f, 1.0f / 12.0f), 0.01f),
			"linear d at q = 1/(2*conv) == sep/2");
		check(VR::ProfileDB::EvalDisparity(lin, 0.02f, 6.0f, 1.0f) == 0.0f,
			"linear deep-window clamp pins near content to 0");
	}

	const auto rejects = [&](const char* name, StereoMap map, const std::vector<float>& sp,
							  const std::vector<StereoBand>& bd, const std::optional<StereoLogParams>& lp) {
		StereoResolvedMap r;
		const bool ok = resolve(map, sp, bd, lp, r);
		check(!ok && r.map == StereoMap::Linear && r.band_count == 1, name);
	};
	rejects("reject: splits not strictly ascending", StereoMap::Bands, {8.0f, 2.0f}, three, kNoLog);
	rejects("reject: duplicate split", StereoMap::Bands, {8.0f, 8.0f}, three, kNoLog);
	rejects("reject: split <= 0", StereoMap::Bands, {0.0f}, two, kNoLog);
	rejects("reject: negative split", StereoMap::Bands, {-1.0f}, two, kNoLog);
	rejects("reject: band/split count mismatch", StereoMap::Bands, {2.0f, 8.0f}, two, kNoLog);
	rejects("reject: too many splits", StereoMap::Bands, {1.0f, 2.0f, 3.0f, 4.0f},
		{{1.0f, 0.01f}, {2.0f, 0.01f}, {3.0f, 0.01f}, {4.0f, 0.01f}, {5.0f, 0.01f}}, kNoLog);
	rejects("reject: empty bands", StereoMap::Bands, {}, {}, kNoLog);
	rejects("reject: sep <= 0 (zero)", StereoMap::Bands, {8.0f}, {{6.0f, 0.0f}, {40.0f, 0.012f}}, kNoLog);
	rejects("reject: sep < 0", StereoMap::Bands, {8.0f}, {{6.0f, -0.008f}, {40.0f, 0.012f}}, kNoLog);
	rejects("reject: conv < 0 (inverts depth)", StereoMap::Bands, {8.0f}, {{-1.0f, 0.008f}, {40.0f, 0.012f}}, kNoLog);
	rejects("reject: log without a log: block", StereoMap::Log, {}, {}, kNoLog);
	rejects("reject: log w0 <= 0", StereoMap::Log, {}, {}, StereoLogParams{0.0f, 100.0f, 0.02f});
	rejects("reject: log w1 <= w0", StereoMap::Log, {}, {}, StereoLogParams{100.0f, 100.0f, 0.02f});
	rejects("reject: log dfar < 0", StereoMap::Log, {}, {}, StereoLogParams{100.0f, 200.0f, -0.01f});

	check(resolve(StereoMap::Bands, {8.0f}, {{0.0f, 0.008f}, {40.0f, 0.012f}}, kNoLog, m),
		"conv == 0 is accepted");

	check(resolve(StereoMap::Log, {}, {}, StereoLogParams{2000.0f, 22000.0f, 0.02f}, m), "log resolves");
	check(m.map == StereoMap::Log, "log -> Log");
	check(approxEq(m.log_w0, 2000.0f) && approxEq(m.log_w1, 22000.0f) && approxEq(m.log_dfar, 0.02f),
		"log anchors carried through");
	check(m.split_q[0] == kPad && m.split_q[1] == kPad && m.split_q[2] == kPad, "log pads every split");
	check(approxEq(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 1.0f / 2000.0f), 0.0f), "log d(w0) == 0");
	check(approxEq(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 1.0f / 22000.0f), 0.02f), "log d(w1) == dfar");
	check(approxEq(VR::ProfileDB::EvalDisparity(m, 0.0f, 0.0f, 0.0f), 0.02f), "log d(inf) == dfar");

	check(resolve(StereoMap::Bands, {2.0f}, {{0.0f, 0.05f}, {0.0f, 0.05f}}, kNoLog, m),
		"over-budget map still resolves");
	check(m.map == StereoMap::Bands, "over-budget map stays Bands (warn, never clamp)");
	check(resolve(StereoMap::Log, {}, {}, StereoLogParams{100.0f, 200.0f, 0.5f}, m),
		"over-budget log still resolves");
	check(m.map == StereoMap::Log, "over-budget log stays Log");

	return fail;
}

bool VR::ProfileDB::SelfTestMultibandResolve()
{
	return CountMultibandResolveMismatches(false) == 0;
}
