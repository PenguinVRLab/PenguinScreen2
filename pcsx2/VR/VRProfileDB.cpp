// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRProfileDB.h"
#include "VR/VRModFiles.h"
#include "VR/FovMath.h"
#include "VR/NearField.h"

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
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace VR::ProfileDB
{
	static bool parseProfile(const std::string_view serial, const ryml::NodeRef& node, Profile& out);
}

static constexpr char VRPROFILES_YAML_FILE_NAME[] = "vr-profiles.yaml";
static constexpr char VRPROFILES_DIR_NAME[] = "vr-profiles";
static constexpr char VRPROFILES_MODDB_FILE_NAME[] = "mod-db.yaml";
static constexpr char VRPROFILES_SEED_FILE_NAME[] = "SLUS-20851.yaml";
static constexpr char VRPROFILES_SEED_MARKER[] = "# seeded-from-shipped-sig: ";

static std::unordered_map<std::string, std::vector<VR::ProfileDB::Profile>> s_profiles;
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

static std::set<std::pair<std::string, u32>> s_bind_noticed;
static u32 s_bind_notices = 0;

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

static std::optional<float> parseWholeFloat(std::string_view text, const std::string_view key)
{
	const auto is_ws = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!text.empty() && is_ws(text.front()))
		text.remove_prefix(1);
	while (!text.empty() && is_ws(text.back()))
		text.remove_suffix(1);
	std::string_view rest;
	const std::optional<float> v = StringUtil::FromChars<float>(text, &rest);
	if (!v.has_value())
		return std::nullopt;
#ifndef VR_PROFILE_FLOAT_PLANTED_ACCEPT
	if (!rest.empty())
	{
		Console.WarningFmt("(VR) ProfileDB: refused {} = '{}' (not a number)", key, text);
		return std::nullopt;
	}
#else
	(void)key;
#endif
#ifndef VR_PROFILE_FLOAT_PLANTED_ACCEPT_NONFINITE
	if (!std::isfinite(v.value()))
	{
		Console.WarningFmt("(VR) ProfileDB: refused {} = '{}' (not finite)", key, text);
		return std::nullopt;
	}
#endif
	return v;
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

namespace
{
	void failGH(std::string& err, std::string why)
	{
		if (err.empty())
			err = std::move(why);
	}

	std::string firstUnknownKey(const ryml::ConstNodeRef& node, std::initializer_list<const char*> known)
	{
#ifdef VR_HANDCAP_PLANTED_PARSE_LENIENT
		(void)node;
		(void)known;
		return {};
#else
		for (const ryml::ConstNodeRef& c : node.children())
		{
			if (!c.has_key())
				continue;
			const std::string_view k(c.key().data(), c.key().size());
			bool found = false;
			for (const char* kn : known)
				found = found || (k == kn);
			if (!found)
				return std::string(k);
		}
		return {};
#endif
	}

	void needMap(std::string& err, const ryml::ConstNodeRef& node, const char* where, std::initializer_list<const char*> known)
	{
		if (!node.is_map())
		{
			failGH(err, fmt::format("{} is not a map", where));
			return;
		}
		const std::string k = firstUnknownKey(node, known);
		if (!k.empty())
			failGH(err, fmt::format("unknown key '{}' in {}", k, where));
	}

	ryml::ConstNodeRef needChild(std::string& err, const ryml::ConstNodeRef& node, const char* key, const char* where)
	{
		if (!node.is_map() || !node.has_child(ryml::to_csubstr(key)))
		{
			failGH(err, fmt::format("{} has no {}", where, key));
			return node;
		}
		return node[ryml::to_csubstr(key)];
	}

	double scalarDouble(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		const std::string text(nodeVal(node));
		char* end = nullptr;
		const double v = text.empty() ? 0.0 : std::strtod(text.c_str(), &end);
		if (text.empty() || !end || *end != '\0' || !std::isfinite(v))
		{
			failGH(err, fmt::format("{} = '{}' is not a finite number", what, text));
			return 0.0;
		}
		return v;
	}

	u32 scalarU32(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		const std::string text(nodeVal(node));
		char* end = nullptr;
		const unsigned long long v = text.empty() ? 0 : std::strtoull(text.c_str(), &end, 0);
		if (text.empty() || text[0] == '-' || !end || *end != '\0' || v > 0xFFFFFFFFull)
		{
			failGH(err, fmt::format("{} = '{}' is not a whole number", what, text));
			return 0;
		}
		return static_cast<u32>(v);
	}

	std::string scalarWord(std::string& err, const ryml::ConstNodeRef& node, const char* what, std::initializer_list<const char*> words)
	{
		const std::string text(nodeVal(node));
		for (const char* w : words)
			if (text == w)
				return text;
		failGH(err, fmt::format("{} = '{}' is not one this build reads", what, text));
		return {};
	}

	template <size_t N>
	std::array<double, N> seqDoubles(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		std::array<double, N> out{};
		if (!node.is_seq() || node.num_children() != N)
		{
			failGH(err, fmt::format("{} wants a list of {} numbers", what, N));
			return out;
		}
		size_t i = 0;
		for (const ryml::ConstNodeRef& c : node.children())
			out[i++] = scalarDouble(err, c, what);
		return out;
	}

	std::array<u32, 2> rangeU32(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		if (!node.is_seq() || node.num_children() != 2)
		{
			failGH(err, fmt::format("{} wants [lo, hi]", what));
			return {0, 0};
		}
		std::array<u32, 2> out{};
		size_t i = 0;
		for (const ryml::ConstNodeRef& c : node.children())
			out[i++] = scalarU32(err, c, what);
		return out;
	}

	VR::HandCapture::Box box6(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		const std::array<double, 6> b = seqDoubles<6>(err, node, what);
		VR::HandCapture::Box out;
		out.lo = {std::min(b[0], b[3]), std::min(b[1], b[4]), std::min(b[2], b[5])};
		out.hi = {std::max(b[0], b[3]), std::max(b[1], b[4]), std::max(b[2], b[5])};
		return out;
	}

	u32 pixelsToLog2(std::string& err, const ryml::ConstNodeRef& node, const char* what)
	{
		const u32 px = scalarU32(err, node, what);
#ifdef VR_HANDCAP_PLANTED_TW_AS_LOG2
		return px;
#else
		for (u32 l = 0; l <= 10; l++)
			if (px == (1u << l))
				return l;
		failGH(err, fmt::format("{} = {} is not a texture size in pixels (a power of two up to 1024)", what, px));
		return 0;
#endif
	}
}

static std::optional<VR::HandCapture::Recipe> parseGameHands(const std::string_view serial, const ryml::ConstNodeRef& node)
{
	using namespace VR::HandCapture;
	Recipe r;
	r.serial = StringUtil::toUpper(serial);
	std::string err;
	{
		needMap(err, node, "gameHands", {"version", "route", "capture", "selector", "gate", "stable_frames", "max_wait_s",
										"max_frame_step", "max_motion_px", "fingerprint", "projection", "side", "trim_box", "fingers", "rest_fix", "other_side",
										"hide", "texture", "notes"});
		const u32 version = scalarU32(err, needChild(err, node, "version", "gameHands"), "version");
		if (version != 2)
			failGH(err, fmt::format("version {} is not 2 (version 1 keyed on per-draw triangle counts)", version));
		scalarWord(err, needChild(err, node, "route", "gameHands"), "route", {"extract"});
		scalarWord(err, needChild(err, node, "capture", "gameHands"), "capture", {"gs_posed"});
		scalarWord(err, needChild(err, node, "fingers", "gameHands"), "fingers", {"rigid"});
		scalarWord(err, needChild(err, node, "texture", "gameHands"), "texture", {"sample"});

		const ryml::ConstNodeRef sel = needChild(err, node, "selector", "gameHands");
		needMap(err, sel, "selector", {"texture", "w_range", "object", "vu_program"});
		const ryml::ConstNodeRef tex = needChild(err, sel, "texture", "selector");
		needMap(err, tex, "selector.texture", {"tbp0", "psm", "tw", "th", "cbp", "cpsm"});
		r.tbp0 = scalarU32(err, needChild(err, tex, "tbp0", "selector.texture"), "tbp0");
		r.psm = scalarU32(err, needChild(err, tex, "psm", "selector.texture"), "psm");
		r.tw = pixelsToLog2(err, needChild(err, tex, "tw", "selector.texture"), "selector.texture.tw");
		r.th = pixelsToLog2(err, needChild(err, tex, "th", "selector.texture"), "selector.texture.th");
		if (tex.has_child("cbp") != tex.has_child("cpsm"))
			failGH(err, "selector.texture names cbp and cpsm together or neither");
		else if (tex.has_child("cbp"))
		{
			r.has_clut = true;
			r.cbp = scalarU32(err, tex["cbp"], "cbp");
			r.cpsm = scalarU32(err, tex["cpsm"], "cpsm");
		}
		const std::array<double, 2> wr = seqDoubles<2>(err, needChild(err, sel, "w_range", "selector"), "w_range");
		r.w_min = wr[0];
		r.w_max = wr[1];
		if (sel.has_child("object"))
		{
			const ryml::ConstNodeRef ob = sel["object"];
			needMap(err, ob, "selector.object", {"uv_box", "uv_min_coverage"});
			r.has_uv_box = true;
			r.uv_box = seqDoubles<4>(err, needChild(err, ob, "uv_box", "selector.object"), "selector.object.uv_box");
			if (ob.has_child("uv_min_coverage"))
				r.uv_min_coverage = scalarDouble(err, ob["uv_min_coverage"], "selector.object.uv_min_coverage");
		}
		if (sel.has_child("vu_program"))
			r.vu_program = scalarU32(err, sel["vu_program"], "vu_program");

		if (node.has_child("gate"))
			r.gate = scalarWord(err, node["gate"], "gate", {"none", "headlook_armed"}) == "none" ? Gate::None : Gate::HeadlookArmed;
		if (node.has_child("stable_frames"))
			r.stable_frames = scalarU32(err, node["stable_frames"], "stable_frames");
		if (node.has_child("max_wait_s"))
			r.max_wait_s = static_cast<float>(scalarDouble(err, node["max_wait_s"], "max_wait_s"));
		if (node.has_child("max_frame_step"))
		{
			r.max_frame_step = scalarU32(err, node["max_frame_step"], "max_frame_step");
			if (err.empty() && r.max_frame_step == 0)
				failGH(err, "max_frame_step must be at least 1 (a frame-number step; leave it out for no limit)");
		}
		if (node.has_child("max_motion_px"))
		{
			r.max_motion_px = scalarDouble(err, node["max_motion_px"], "max_motion_px");
			if (err.empty() && !(r.max_motion_px > 0.0 && std::isfinite(r.max_motion_px)))
				failGH(err, "max_motion_px must be a positive number of pixels (leave it out for no stillness test)");
		}

		const ryml::ConstNodeRef fp = needChild(err, node, "fingerprint", "gameHands");
		needMap(err, fp, "fingerprint", {"vertices", "triangles", "open_edges", "texture"});
		const std::array<u32, 2> fv = rangeU32(err, needChild(err, fp, "vertices", "fingerprint"), "fingerprint.vertices");
		const std::array<u32, 2> ft = rangeU32(err, needChild(err, fp, "triangles", "fingerprint"), "fingerprint.triangles");
		r.vertices_min = fv[0];
		r.vertices_max = fv[1];
		r.triangles_min = ft[0];
		r.triangles_max = ft[1];
		if (fp.has_child("open_edges"))
		{
			const std::array<u32, 2> fo = rangeU32(err, fp["open_edges"], "fingerprint.open_edges");
			r.open_edges_min = fo[0];
			r.open_edges_max = fo[1];
		}
		if (fp.has_child("texture"))
		{
			const ryml::ConstNodeRef ftex = fp["texture"];
			needMap(err, ftex, "fingerprint.texture", {"w", "h"});
			r.texture_w = scalarU32(err, needChild(err, ftex, "w", "fingerprint.texture"), "fingerprint.texture.w");
			r.texture_h = scalarU32(err, needChild(err, ftex, "h", "fingerprint.texture"), "fingerprint.texture.h");
		}

		const ryml::ConstNodeRef pr = needChild(err, node, "projection", "gameHands");
		needMap(err, pr, "projection", {"fx", "fy", "cx", "cy", "w_source", "k"});
		r.projection.fx = scalarDouble(err, needChild(err, pr, "fx", "projection"), "projection.fx");
		r.projection.fy = scalarDouble(err, needChild(err, pr, "fy", "projection"), "projection.fy");
		r.projection.cx = scalarDouble(err, needChild(err, pr, "cx", "projection"), "projection.cx");
		r.projection.cy = scalarDouble(err, needChild(err, pr, "cy", "projection"), "projection.cy");
		scalarWord(err, needChild(err, pr, "w_source", "projection"), "projection.w_source", {"q"});
		if (pr.has_child("k"))
			r.projection.k = scalarDouble(err, pr["k"], "projection.k");

		const ryml::ConstNodeRef side = needChild(err, node, "side", "gameHands");
		needMap(err, side, "side", {"rule", "left", "right"});
		const ryml::ConstNodeRef rf = needChild(err, node, "rest_fix", "gameHands");
		needMap(err, rf, "rest_fix", {"metres_per_unit", "wrist"});
		r.metres_per_unit = scalarDouble(err, needChild(err, rf, "metres_per_unit", "rest_fix"), "rest_fix.metres_per_unit");
		const ryml::ConstNodeRef wrist = needChild(err, rf, "wrist", "rest_fix");
		needMap(err, wrist, "rest_fix.wrist", {"left", "right"});
		static constexpr const char* kSide[2] = {"left", "right"};
		if (side.has_child("rule"))
		{
			scalarWord(err, side["rule"], "side.rule", {"centroid_x"});
			r.side_rule = SideRule::CentroidX;
			if (side.has_child("left") || side.has_child("right"))
				failGH(err, "side names a rule or boxes, not both");
		}
		for (int s = 0; s < 2; s++)
		{
			const bool listed = r.side_rule == SideRule::CentroidX ? wrist.has_child(ryml::to_csubstr(kSide[s])) :
																	 side.has_child(ryml::to_csubstr(kSide[s]));
			if (!listed)
				continue;
			r.has_side[s] = true;
			if (r.side_rule == SideRule::Boxes)
			{
				const ryml::ConstNodeRef sn = side[ryml::to_csubstr(kSide[s])];
				needMap(err, sn, "a side", {"box"});
				r.side_box[s] = box6(err, needChild(err, sn, "box", "a side"), "side box");
			}
			const ryml::ConstNodeRef wn = needChild(err, wrist, kSide[s], "rest_fix.wrist");
			needMap(err, wn, "a wrist frame", {"origin", "x", "y", "back"});
			r.wrist[s].origin = seqDoubles<3>(err, needChild(err, wn, "origin", "a wrist frame"), "wrist origin");
			r.wrist[s].x_axis = seqDoubles<3>(err, needChild(err, wn, "x", "a wrist frame"), "wrist x");
			r.wrist[s].y_axis = seqDoubles<3>(err, needChild(err, wn, "y", "a wrist frame"), "wrist y");
			if (wn.has_child("back"))
				r.back[s] = scalarWord(err, wn["back"], "wrist back", {"middle", "thin"}) == "middle" ? BackRank::Middle : BackRank::Thin;
		}

		const ryml::ConstNodeRef trim = needChild(err, node, "trim_box", "gameHands");
		if (trim.is_seq())
		{
			r.has_trim = true;
			r.trim_box = box6(err, trim, "trim_box");
		}
		else if (nodeVal(trim) != "none")
			failGH(err, "trim_box is neither [x0, y0, z0, x1, y1, z1] nor none");

		const std::string other = scalarWord(err, needChild(err, node, "other_side", "gameHands"), "other_side", {"capture", "mirror", "glove"});
		r.other_side = other == "capture" ? OtherSide::Capture : other == "mirror" ? OtherSide::Mirror : OtherSide::Glove;

		const ryml::ConstNodeRef hide = needChild(err, node, "hide", "gameHands");
		needMap(err, hide, "hide", {"mode"});
		r.hide_mode = scalarWord(err, needChild(err, hide, "mode", "hide"), "hide.mode", {"split", "skip_draw"}) == "skip_draw" ?
						  HideMode::SkipDraw :
						  HideMode::Split;

		std::string why;
		if (err.empty() && !ValidateRecipe(r, &why))
			failGH(err, why);
	}
	if (!err.empty())
	{
		advise(serial, "gameHands",
			fmt::format("(VR) ProfileDB: {} gameHands: {} — the whole block is DROPPED, so the game's own hands are never "
						"captured.",
				StringUtil::toUpper(serial), err));
		return std::nullopt;
	}
	return r;
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

	const float d = EvalBand(map, SelectBand(map, q), q);
	if (map.crossed_limit > 0.0f && d < 0.0f)
	{
#if defined(VR_CROSSED_PLANTED_SIGN)
		return map.crossed_limit * std::tanh(std::min(map.crossed_gain * (-d) / map.crossed_limit, 9.0f));
#elif defined(VR_CROSSED_PLANTED_NOLIMIT)
		return map.crossed_gain * d;
#else
		return -map.crossed_limit * std::tanh(std::min(map.crossed_gain * (-d) / map.crossed_limit, 9.0f));
#endif
	}
	return std::max(0.0f, d);
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
	return user_authored;
}

static std::optional<u32> parseHexU32(const std::string_view str)
{
	std::string_view hex = str;
	if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
		hex = hex.substr(2);
	std::string_view rest;
	const std::optional<u32> v = StringUtil::FromChars<u32>(hex, 16, &rest);
	if (!v.has_value() || !rest.empty())
		return std::nullopt;
	return v;
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

static bool parseGuardList(const std::string_view serial, const ryml::ConstNodeRef& seq,
	const char* what, std::vector<VR::ProfileDB::CameraGuard>& out)
{
	const size_t before = out.size();
	size_t entries = 0;
	for (const ryml::ConstNodeRef& g : seq.children())
	{
		entries++;
		if (!g.is_map())
		{
			Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a {} that is not a map; skipping it.", serial, what);
			continue;
		}
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
	return (out.size() - before) == entries;
}

static std::string readGateList(const std::string_view serial, const ryml::ConstNodeRef& node, const char* key,
	const char* what, std::vector<VR::ProfileDB::CameraGuard>& out)
{
	const ryml::csubstr k = ryml::to_csubstr(key);
	if (!node.has_child(k))
		return {};
	const ryml::ConstNodeRef list = node[k];
	if (!list.is_seq())
		return fmt::format("`{}` is not a list of guards", key);
#ifndef VR_PLANTED_GATE_SKIP
	const size_t before = out.size();
	if (!parseGuardList(serial, list, what, out))
		return fmt::format("{} of its {} `{}` guards could not be read (a gate missing a term would act in scenes "
						   "it is not for, menus included)",
			list.num_children() - (out.size() - before), list.num_children(), key);
#else
	parseGuardList(serial, list, what, out);
#endif
	return {};
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
			const std::optional<float> v = parseWholeFloat(raw, "splits");
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
				const std::optional<float> v = parseWholeFloat(nodeVal(b["conv"]), "bands[].conv");
				if (v.has_value())
					band.conv = v.value();
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' {} has an invalid bands[].conv; "
									   "leaving it 0.", serial, where);
			}
			if (b.has_child("sep"))
			{
				const std::optional<float> v = parseWholeFloat(nodeVal(b["sep"]), "bands[].sep");
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
			const std::optional<float> v = parseWholeFloat(nodeVal(ln[key]), key);
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

static constexpr std::pair<const char*, VR::ProfileDB::CameraClass> kCameraClassNames[] = {
	{"fullTracking", VR::ProfileDB::CameraClass::FullTracking},
	{"rotationOnly", VR::ProfileDB::CameraClass::RotationOnly},
	{"translationOnly", VR::ProfileDB::CameraClass::TranslationOnly},
	{"fixedProjection", VR::ProfileDB::CameraClass::FixedProjection},
	{"hybridEnvironment", VR::ProfileDB::CameraClass::HybridEnvironment},
};

const char* VR::ProfileDB::CameraClassName(const CameraClass c)
{
	for (const auto& [name, value] : kCameraClassNames)
	{
		if (value == c)
			return name;
	}
	return "?";
}

static std::optional<VR::ProfileDB::CameraClass> readCameraClass(const std::string_view serial, const ryml::ConstNodeRef& cnode)
{
	if (!cnode.has_child("class"))
		return std::nullopt;
	const ryml::ConstNodeRef v = cnode["class"];
	if (!v.has_val())
	{
		advise(serial, "camera.class", fmt::format("(VR) ProfileDB: Serial '{}' camera.class is not a single value; "
												   "ignoring it.", serial));
		return std::nullopt;
	}
	const std::string_view s = nodeVal(v);
#ifndef VR_PROFILEDB_PLANTED_CAMERA_CLASS_UNREAD
	for (const auto& [name, value] : kCameraClassNames)
	{
		if (s == name)
			return value;
	}
#endif
	for (const auto& [name, value] : kCameraClassNames)
	{
		if (StringUtil::compareNoCase(s, name))
		{
			advise(serial, "camera.class", fmt::format("(VR) ProfileDB: Serial '{}' camera.class '{}' is IGNORED: "
													   "values are case-sensitive — write '{}'.", serial, s, name));
			return std::nullopt;
		}
	}
	advise(serial, "camera.class", fmt::format("(VR) ProfileDB: Serial '{}' has an unknown camera.class '{}'; ignoring it "
											   "(one of fullTracking | rotationOnly | translationOnly | fixedProjection | "
											   "hybridEnvironment).", serial, s));
	return std::nullopt;
}

static void warnUnknownKeys(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* where, std::initializer_list<const char*> known, const char* advise_site = nullptr,
	bool advise_unknown = false);

static void readOptionalFloat(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* key, const char* what, float& dst)
{
	if (!node.has_child(key))
		return;
	const std::optional<float> v = parseWholeFloat(nodeVal(node[key]), key);
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
				const std::optional<float> v = parseWholeFloat(nodeVal(r[ck]), keys[k]);
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
		std::string why;
		const auto fail = [&](std::string w) {
			why = std::move(w);
			return std::optional<SplitParams::Probe>();
		};
		const auto parseProbe = [&](const ryml::ConstNodeRef& a) -> std::optional<SplitParams::Probe> {
			if (!a.is_map() || !a.has_child("address"))
				return fail("active needs a map with at least an address");
			SplitParams::Probe probe;
			const std::optional<u32> addr = parseAddress(nodeVal(a["address"]));
			if (!addr.has_value())
				return fail("active.address does not parse");
			probe.ee_address = addr.value();
			const bool has_eq = a.has_child("equals");
			const bool has_al = a.has_child("atLeast");
			if (has_eq == has_al)
				return fail("active needs exactly ONE of equals|atLeast");
			const std::optional<u32> cmp = parseHexU32(nodeVal(a[has_eq ? "equals" : "atLeast"]));
			if (!cmp.has_value())
				return fail("active comparand does not parse");
			probe.threshold = cmp.value();
			probe.at_least = has_al;
			if (a.has_child("width"))
			{
				const std::optional<u32> w = parseHexU32(nodeVal(a["width"]));
				if (!w.has_value() || (w.value() != 1 && w.value() != 2 && w.value() != 4))
					return fail("active.width must be 1, 2 or 4");
				probe.width = static_cast<u8>(w.value());
			}
			if (!inMainRam(probe.ee_address, probe.width))
				return fail(fmt::format("active.address {:#x} is outside EE main RAM", probe.ee_address));
			return probe;
		};
		const ryml::ConstNodeRef a = snode["active"];
		if (a.is_seq())
		{
			if (a.num_children() == 0 || a.num_children() > 4)
				return reject("active as a list needs 1 to 4 probes (all must hold)");
			for (const ryml::ConstNodeRef& e : a.children())
			{
				const std::optional<SplitParams::Probe> probe = parseProbe(e);
				if (!probe.has_value())
					return reject(why);
				if (!sp.active.has_value())
					sp.active = probe;
				else
					sp.also.push_back(probe.value());
			}
		}
		else
		{
			const std::optional<SplitParams::Probe> probe = parseProbe(a);
			if (!probe.has_value())
				return reject(why);
			sp.active = probe;
		}
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
		const std::optional<float> v = parseWholeFloat(nodeVal(snode["sideScale"]), "sideScale");
		if (!v.has_value() || !(v.value() > 0.05f) || !(v.value() <= 1.0f))
			return reject("sideScale must be in (0.05, 1.0]");
		sp.side_scale = v.value();
	}
	if (snode.has_child("sideAngleDeg"))
	{
		const std::optional<float> v = parseWholeFloat(nodeVal(snode["sideAngleDeg"]), "sideAngleDeg");
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

	if (cnode.num_children() == 1 && cnode.has_child("class"))
		return std::nullopt;

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
			if (w.has_child("whileProjected"))
			{
				if (!StringUtil::compareNoCase(nodeVal(w["whileProjected"]), "headOnly") || op.compose != CameraCompose::Delta)
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera write op with whileProjected \"{}\" (only headOnly, on compose: delta); skipping it.",
						serial, nodeVal(w["whileProjected"]));
					continue;
				}
				op.head_only_projected = true;
			}
			if (w.has_child("anchorHead"))
			{
				op.anchor_head = !StringUtil::compareNoCase(nodeVal(w["anchorHead"]), "false");
				if (op.compose != CameraCompose::Anchored)
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' sets anchorHead on a non-anchored camera write op; it only affects compose: anchored.", serial);
			}
			if (w.has_child("angles"))
			{
				const std::string_view a = nodeVal(w["angles"]);
				if (StringUtil::compareNoCase(a, "head"))
					op.fitted_angles = false;
				else if (!StringUtil::compareNoCase(a, "fitted"))
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera write op with angles \"{}\" (not head or fitted); skipping it.", serial, a);
					continue;
				}
				if (op.source != CameraSource::HeadYaw && op.source != CameraSource::HeadPitch)
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' sets angles on a camera write op whose source is not head_yaw or head_pitch; it has no effect there.", serial);
			}
			if (!op.relative && !inMainRam(op.ee_address))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera write address {:#x} is outside main RAM; skipping it.", serial, op.ee_address);
				continue;
			}
			if (const std::string why = readGateList(serial, w, "when", "camera write when-guard", op.when); !why.empty())
			{
				advise(serial, "camera.writes", fmt::format("(VR) ProfileDB: Serial '{}' camera write at {:#x} DROPPED: {}.",
													serial, op.ee_address, why));
				continue;
			}
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
			warnUnknownKeys(serial, m, "a camera.matrixWrites op",
				{"address", "relative", "order", "compose", "transposeAddress", "axisSignYaw", "axisSignPitch",
					"axisSignRoll", "when", "yawMin", "yawMax", "pitchMin", "pitchMax", "rollMin", "rollMax"},
				"camera.matrixWrites", true);
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
			for (auto [key, sign] : {std::pair<const char*, float*>{"axisSignYaw", &op.axis_sign_yaw},
					 {"axisSignPitch", &op.axis_sign_pitch}, {"axisSignRoll", &op.axis_sign_roll}})
			{
				if (!m.has_child(ryml::to_csubstr(key)))
					continue;
				const std::optional<float> v = parseWholeFloat(nodeVal(m[ryml::to_csubstr(key)]), key);
				if (v.has_value())
				{
					*sign = v.value();
					continue;
				}
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid camera matrix {}; keeping the default.", serial, key);
				advise(serial, "camera.matrixWrites",
					fmt::format("(VR) ProfileDB: Serial '{}' camera matrix op {} is not a finite number; keeping 1.", serial, key));
			}
			for (auto [key, sign] : {std::pair<const char*, float*>{"axisSignYaw", &op.axis_sign_yaw},
					 {"axisSignPitch", &op.axis_sign_pitch}, {"axisSignRoll", &op.axis_sign_roll}})
			{
				if (!std::isfinite(*sign))
				{
					advise(serial, "camera.matrixWrites",
						fmt::format("(VR) ProfileDB: Serial '{}' camera matrix op {} is not finite; keeping 1.", serial, key));
					*sign = 1.0f;
				}
			}
			struct ClampPair
			{
				const char* lo_key;
				const char* hi_key;
				float* lo;
				float* hi;
			};
			const ClampPair pairs[] = {
				{"yawMin", "yawMax", &op.yaw_min_deg, &op.yaw_max_deg},
				{"pitchMin", "pitchMax", &op.pitch_min_deg, &op.pitch_max_deg},
				{"rollMin", "rollMax", &op.roll_min_deg, &op.roll_max_deg},
			};
			for (const ClampPair& cp : pairs)
			{
				if (!m.has_child(cp.lo_key) && !m.has_child(cp.hi_key))
					continue;
				float lo = *cp.lo, hi = *cp.hi;
				readOptionalFloat(serial, m, cp.lo_key, "camera matrix clamp", lo);
				readOptionalFloat(serial, m, cp.hi_key, "camera matrix clamp", hi);
				const bool lo_given = m.has_child(cp.lo_key), hi_given = m.has_child(cp.hi_key);
				const bool wraps = (cp.lo == &op.yaw_min_deg) || (cp.lo == &op.roll_min_deg);
				std::string why;
				if (wraps && lo_given != hi_given)
					why = "is one-sided on a wrapped axis: yaw and roll need both limits";
				else if ((lo_given && !std::isfinite(lo)) || (hi_given && !std::isfinite(hi)))
					why = "is not finite";
				else if ((lo_given && std::fabs(lo) > 180.0f) || (hi_given && std::fabs(hi) > 180.0f))
					why = "is outside -180..180 degrees";
				else if (lo > hi)
					why = "has a minimum greater than its maximum";
				else if (lo > 0.0f || hi < 0.0f)
					why = "does not contain 0, the recentered pose";
				if (!why.empty())
				{
					advise(serial, "camera.matrixWrites",
						fmt::format("(VR) ProfileDB: Serial '{}' camera matrix clamp {}/{} ({:g}, {:g}) {}; that axis is left unclamped.",
							serial, cp.lo_key, cp.hi_key, lo, hi, why));
					continue;
				}
				*cp.lo = lo;
				*cp.hi = hi;
				if (!op.anchored)
					advise(serial, "camera.matrixWrites",
						fmt::format("(VR) ProfileDB: Serial '{}' camera matrix op is compose: delta with {}/{}: a delta op "
									"re-baselines on every gap and recenter, so the window bounds the head motion since "
									"the last re-baseline only, not the total turn. Use compose: anchored for a hard limit.",
							serial, cp.lo_key, cp.hi_key));
			}
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
			if (const std::string why = readGateList(serial, m, "when", "camera matrix when-guard", op.when); !why.empty())
			{
				advise(serial, "camera.matrixWrites",
					fmt::format("(VR) ProfileDB: Serial '{}' camera matrix write at {:#x} DROPPED: {}.", serial, op.ee_address, why));
				continue;
			}
			cam.matrix_writes.push_back(op);
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
				const std::optional<u32> cnt =
					bnode.has_child("count") ? StringUtil::FromChars<u32>(nodeVal(bnode["count"])) : std::nullopt;
				if (!cnt.has_value() || cnt.value() == 0 || cnt.value() > 65536)
				{
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base indexed mode needs `count:` (the number of "
									   "elements, decimal 1..65536) so a runtime index cannot run off the array; ignoring the base.",
						serial);
					base_ok = false;
				}
				else
				{
					base.count = cnt.value();
					const s64 last = static_cast<s64>(base.indexed_base) + base.array_offset +
									 static_cast<s64>(base.count - 1) * static_cast<s64>(base.stride);
					const s64 first = static_cast<s64>(base.indexed_base) + base.array_offset;
					if (first < 0 || last + 4 > static_cast<s64>(EE_MAIN_RAM_BYTES))
					{
						Console.WarningFmt("(VR) ProfileDB: Serial '{}' camera.base indexed array (count {}, stride {:#x}) runs "
										   "outside main RAM; ignoring the base.", serial, base.count, base.stride);
						base_ok = false;
					}
				}
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

	if (const std::string why = readGateList(serial, cnode, "guards", "camera guard", cam.guards); !why.empty())
	{
		advise(serial, "camera.guards", fmt::format("(VR) ProfileDB: Serial '{}' camera block DROPPED: {}.", serial, why));
		return std::nullopt;
	}

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

	if (cnode.has_child("directLook") && cnode["directLook"].is_map())
	{
		const ryml::ConstNodeRef dnode = cnode["directLook"];
		CameraDirectLook dl;
		std::string why;

		const auto parseRegister = [](const std::string_view text) -> std::optional<u8> {
			static const char* const names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
				"t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1",
				"gp", "sp", "fp", "ra"};
			for (u8 i = 0; i < 32; i++)
			{
				if (StringUtil::compareNoCase(text, names[i]))
					return i;
			}
			const std::optional<u32> n = StringUtil::FromChars<u32>(text);
			if (n.has_value() && n.value() <= 31)
				return static_cast<u8>(n.value());
			return std::nullopt;
		};
		const auto parseLever = [&](const char* axis) -> std::optional<CameraDirectLever> {
			if (!dnode.has_child(ryml::to_csubstr(axis)))
				return std::nullopt;
			const ryml::ConstNodeRef n = dnode[ryml::to_csubstr(axis)];
			if (!n.is_map())
			{
				why = fmt::format("{} is not a map", axis);
				return std::nullopt;
			}
			CameraDirectLever lv;
			const std::optional<u32> hook = n.has_child("hook") ? parseAddress(nodeVal(n["hook"])) : std::nullopt;
			const std::optional<u32> expect = n.has_child("expect") ? parseAddress(nodeVal(n["expect"])) : std::nullopt;
			if (!hook.has_value() || hook.value() == 0 || (hook.value() & 3) != 0)
			{
				why = fmt::format("{} needs `hook`, the address of an instruction", axis);
				return std::nullopt;
			}
			if (!expect.has_value() || expect.value() == 0)
			{
				why = fmt::format("{} needs `expect`, the instruction word the game holds at the hook (without it the "
								  "hook cannot tell this disc from another revision)",
					axis);
				return std::nullopt;
			}
			lv.hook_address = hook.value();
			lv.expect = expect.value();
			const bool is_aim = (std::string_view(axis) == "aim");
			const bool has_reg = n.has_child("register");
			const bool has_view = n.has_child("view");
			if (is_aim)
			{
				const bool has_dir = n.has_child("direction");
				const bool has_eye = n.has_child("eye"), has_target = n.has_child("target");
				if (has_reg || has_view || n.has_child("unitsPerTurn") || n.has_child("axisSign") || n.has_child("maxDeg"))
				{
					why = "aim takes `direction` or `eye` + `target`, `yawSign`, `pitchSign`, `maxYawDeg`, `maxPitchDeg` "
						  "(register, view, unitsPerTurn, axisSign and maxDeg belong to the yaw and pitch levers)";
					return std::nullopt;
				}
				if (has_dir == (has_eye || has_target) || has_eye != has_target)
				{
					why = "aim needs exactly one of `direction` (the register that holds a forward vector's address) and "
						  "`eye` + `target` (the registers that hold the two points' addresses)";
					return std::nullopt;
				}
				const std::optional<u8> r = has_dir ? parseRegister(nodeVal(n["direction"])) : parseRegister(nodeVal(n["target"]));
				const std::optional<u8> e = has_eye ? parseRegister(nodeVal(n["eye"])) : std::optional<u8>(static_cast<u8>(0));
				if (!r.has_value() || r.value() == 0 || !e.has_value() || (has_eye && e.value() == 0) || r.value() == e.value())
				{
					why = "aim's registers are not integer registers (fp, a1, ... or 1..31), or name the same one twice";
					return std::nullopt;
				}
				lv.kind = CameraDirectLever::Kind::Aim;
				lv.reg = r.value();
				lv.eye_reg = e.value();
			}
			else if (has_reg == has_view)
			{
				why = fmt::format("{} needs exactly one of `register` and `view`", axis);
				return std::nullopt;
			}
			else if (has_reg)
			{
				const std::optional<u8> r = parseRegister(nodeVal(n["register"]));
				if (!r.has_value() || r.value() == 0)
				{
					why = fmt::format("{} register is not an integer register (s2, a1, ... or 1..31)", axis);
					return std::nullopt;
				}
				lv.kind = CameraDirectLever::Kind::Register;
				lv.reg = r.value();
			}
			else
			{
				const std::optional<u32> v = parseAddress(nodeVal(n["view"]));
				if (!v.has_value() || v.value() == 0 || (v.value() & 3) != 0)
				{
					why = fmt::format("{} view is not the address of a matrix", axis);
					return std::nullopt;
				}
				lv.kind = CameraDirectLever::Kind::View;
				lv.view_address = v.value();
			}
			if (n.has_child("caller") || n.has_child("callerAt"))
			{
				const std::optional<u32> who = n.has_child("caller") ? parseAddress(nodeVal(n["caller"])) : std::nullopt;
				const auto parseOffset = [](const std::string_view t) -> std::optional<u32> {
					if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
						return StringUtil::FromChars<u32>(t.substr(2), 16);
					return StringUtil::FromChars<u32>(t);
				};
				const std::optional<u32> at = n.has_child("callerAt") ? parseOffset(nodeVal(n["callerAt"])) : std::nullopt;
				if (!who.has_value() || !at.has_value() || who.value() == 0 || at.value() == 0 || (at.value() & 3) != 0 ||
					at.value() > 0x4000 || (who.value() & 3) != 0)
				{
					why = fmt::format("{} needs `caller` (a return address) and `callerAt` (its stack offset) together", axis);
					return std::nullopt;
				}
				lv.caller = who.value();
				lv.caller_sp_offset = at.value();
			}
			if (is_aim)
			{
				if (n.has_child("axes"))
				{
					const std::string_view a = nodeVal(n["axes"]);
					if (StringUtil::compareNoCase(a, "yaw"))
						lv.carry_pitch = false;
					else if (StringUtil::compareNoCase(a, "pitch"))
						lv.carry_yaw = false;
					else if (!StringUtil::compareNoCase(a, "both"))
					{
						why = fmt::format("aim axes \"{}\" is not yaw, pitch or both", a);
						return std::nullopt;
					}
				}
				readOptionalFloat(serial, n, "yawSign", "camera.directLook yawSign", lv.axis_sign);
				readOptionalFloat(serial, n, "pitchSign", "camera.directLook pitchSign", lv.pitch_sign);
				readOptionalFloat(serial, n, "maxYawDeg", "camera.directLook maxYawDeg", lv.max_deg);
				readOptionalFloat(serial, n, "maxPitchDeg", "camera.directLook maxPitchDeg", lv.max_pitch_deg);
				if (!(lv.axis_sign == 1.0f || lv.axis_sign == -1.0f) || !(lv.pitch_sign == 1.0f || lv.pitch_sign == -1.0f))
				{
					why = fmt::format("aim yawSign {:g} / pitchSign {:g}: each must be 1 or -1", lv.axis_sign, lv.pitch_sign);
					return std::nullopt;
				}
				if (!std::isfinite(lv.max_deg) || lv.max_deg <= 0.0f || lv.max_deg > 180.0f || !std::isfinite(lv.max_pitch_deg) ||
					lv.max_pitch_deg <= 0.0f || lv.max_pitch_deg > 89.0f)
				{
					why = fmt::format("aim maxYawDeg {:g} / maxPitchDeg {:g} out of range (need 0 < maxYawDeg <= 180 and "
									  "0 < maxPitchDeg <= 89)",
						lv.max_deg, lv.max_pitch_deg);
					return std::nullopt;
				}
				return lv;
			}
			readOptionalFloat(serial, n, "unitsPerTurn", "camera.directLook unitsPerTurn", lv.units_per_turn);
			readOptionalFloat(serial, n, "axisSign", "camera.directLook axisSign", lv.axis_sign);
			readOptionalFloat(serial, n, "maxDeg", "camera.directLook maxDeg", lv.max_deg);
			if (!(lv.axis_sign == 1.0f || lv.axis_sign == -1.0f))
			{
				why = fmt::format("{} axisSign {:g} is not 1 or -1", axis, lv.axis_sign);
				return std::nullopt;
			}
			if (!std::isfinite(lv.max_deg) || lv.max_deg <= 0.0f || lv.max_deg > 180.0f)
			{
				why = fmt::format("{} maxDeg {:g} is out of range (need 0 < maxDeg <= 180)", axis, lv.max_deg);
				return std::nullopt;
			}
			if (has_reg && (!std::isfinite(lv.units_per_turn) || lv.units_per_turn <= 0.0f))
			{
				why = fmt::format("{} unitsPerTurn {:g} is not a positive number", axis, lv.units_per_turn);
				return std::nullopt;
			}
			return lv;
		};

		dl.yaw = parseLever("yaw");
		if (why.empty())
			dl.pitch = parseLever("pitch");
		if (why.empty())
			dl.aim = parseLever("aim");
		if (why.empty() && !dl.yaw.has_value() && !dl.pitch.has_value() && !dl.aim.has_value())
			why = "it has neither a `yaw`, a `pitch` nor an `aim` lever";
		if (why.empty())
		{
			const std::optional<CameraDirectLever>* const levers[3] = {&dl.yaw, &dl.pitch, &dl.aim};
			for (int i = 0; i < 3 && why.empty(); i++)
			{
				for (int j = i + 1; j < 3 && why.empty(); j++)
				{
					if (levers[i]->has_value() && levers[j]->has_value() && (*levers[i])->hook_address == (*levers[j])->hook_address)
						why = "two levers name the same hook address (one instruction carries one lever)";
				}
			}
		}
		if (why.empty() && dnode.has_child("when"))
		{
			const ryml::ConstNodeRef wnode = dnode["when"];
			if (!wnode.is_seq() || wnode.num_children() == 0)
				why = "`when` is not a list of guards";
			else
			{
				parseGuardList(serial, wnode, "camera.directLook when-guard", dl.when);
				if (dl.when.size() != wnode.num_children())
					why = fmt::format("{} of its {} `when` guards could not be read (a lever with a gate missing would act in "
									  "scenes it is not for)",
						wnode.num_children() - dl.when.size(), wnode.num_children());
			}
		}
		if (dnode.has_child("angles"))
		{
			const std::string_view a = nodeVal(dnode["angles"]);
			if (StringUtil::compareNoCase(a, "fitted"))
				dl.fitted_angles = true;
			else if (!StringUtil::compareNoCase(a, "head") && why.empty())
				why = fmt::format("angles \"{}\" is not head or fitted", a);
		}

		if (!why.empty())
		{
			advise(serial, "camera.directLook",
				fmt::format("(VR) ProfileDB: {} camera.directLook: {} — the whole directLook block is DROPPED; "
							"padLook, if the profile has one, carries the head-look.",
					StringUtil::toUpper(serial), why));
		}
		else
		{
			cam.direct_look = dl;
		}
	}

	if (cnode.has_child("requiresWorldProjection"))
	{
		const std::string_view v = nodeVal(cnode["requiresWorldProjection"]);
		if (StringUtil::compareNoCase(v, "true"))
			cam.requires_world_projection = true;
		else if (!StringUtil::compareNoCase(v, "false"))
		{
			advise(serial, "camera.requiresWorldProjection",
				fmt::format("(VR) ProfileDB: Serial '{}' camera block DROPPED: requiresWorldProjection must be true or "
							"false, not '{}'.", serial, v));
			return std::nullopt;
		}
	}

	if (cam.writes.empty() && cam.matrix_writes.empty() && cam.code_hooks.empty() && !cam.pad_look.has_value() &&
		!cam.direct_look.has_value())
	{
		Console.WarningFmt("(VR) ProfileDB: Serial '{}' has a camera block with no usable ops; ignoring it.", serial);
		return std::nullopt;
	}

	return cam;
}

static constexpr float kMaxCollimateDisparity = 0.01815f;
static constexpr float kSoftCollimateDisparity = 0.0135f;

static void warnUnknownKeys(const std::string_view serial, const ryml::ConstNodeRef& node,
	const char* where, std::initializer_list<const char*> known, const char* advise_site, bool advise_unknown)
{
	for (const ryml::ConstNodeRef& c : node.children())
	{
		if (!c.has_key())
			continue;
		const std::string_view k(c.key().data(), c.key().size());
		bool found = false;
		const char* case_only = nullptr;
		for (const char* kn : known)
		{
			if (k == kn)
			{
				found = true;
				break;
			}
			if (StringUtil::compareNoCase(k, kn))
				case_only = kn;
		}
		if (!found && case_only)
		{
			std::string msg = fmt::format("(VR) ProfileDB: {} has the key '{}' in {}, which is IGNORED: keys are "
										  "case-sensitive — write '{}'.",
				StringUtil::toUpper(serial), k, where, case_only);
			if (advise_site)
				advise(serial, advise_site, std::move(msg));
			else
				Console.Warning(msg);
		}
		else if (!found)
		{
			std::string msg = fmt::format("(VR) ProfileDB: Serial '{}' has an unknown key '{}' in {}; IGNORING it. "
										  "Check the spelling against bin/resources/vr-profiles/README.md — an ignored "
										  "constraint silently widens the rule.",
				serial, k, where);
			if (advise_site && advise_unknown)
				advise(serial, advise_site, std::move(msg));
			else
				Console.Warning(msg);
		}
	}
}

[[maybe_unused]] static constexpr float kMaxCrossedLimit = 0.01815f;
static constexpr float kSoftCrossedLimit = 0.01f;

static void applyCrossed(const std::string_view serial, const char* where, const ryml::ConstNodeRef& node,
	VR::ProfileDB::StereoResolvedMap& map)
{
	map.crossed_limit = 0.0f;
	map.crossed_gain = 0.0f;
	if (!node.has_child("crossed"))
		return;
	const ryml::ConstNodeRef c = node["crossed"];
	if (!c.is_map())
	{
		advise(serial, "stereo.crossed",
			fmt::format("(VR) ProfileDB: Serial '{}' {}.crossed is not a map ({{limit, gain}}); IGNORED, the "
						"stereo stays clamped at the zero plane.", serial, where));
		return;
	}
	warnUnknownKeys(serial, c, "stereo.crossed", {"limit", "gain"});
	float limit = 0.0f;
	float gain = 0.0f;
	readOptionalFloat(serial, c, "limit", "stereo.crossed.limit", limit);
	readOptionalFloat(serial, c, "gain", "stereo.crossed.gain", gain);
	if (map.map != VR::ProfileDB::StereoMap::Bands)
	{
		advise(serial, "stereo.crossed",
			fmt::format("(VR) ProfileDB: Serial '{}' {}.crossed needs a 'map: bands' map beside it, and this one is not "
						"banded (or was rejected); IGNORED.", serial, where));
		return;
	}
	if (!(limit > 0.0f) || !(gain > 0.0f) || gain > 1.0f)
	{
		advise(serial, "stereo.crossed",
			fmt::format("(VR) ProfileDB: Serial '{}' {}.crossed needs BOTH limit > 0 and gain in (0, 1] (got limit {}, "
						"gain {}); IGNORED, the stereo stays clamped at the zero plane.", serial, where, limit, gain));
		return;
	}
#ifndef VR_CROSSED_PLANTED_NOCAP
	if (limit > kMaxCrossedLimit)
	{
		advise(serial, "stereo.crossed",
			fmt::format("(VR) ProfileDB: Serial '{}' {}.crossed.limit {:.5f} is above the largest accepted value "
						"{:.5f}; IGNORED.", serial, where, limit, kMaxCrossedLimit));
		return;
	}
#endif
	if (limit > kSoftCrossedLimit)
		advise(serial, "stereo.crossed",
			fmt::format("(VR) ProfileDB: Serial '{}' {}.crossed.limit {:.5f} is above {:.3f}; keeping it, but expect "
						"eye strain on the nearest content.", serial, where, limit, kSoftCrossedLimit));
	map.crossed_limit = limit;
	map.crossed_gain = gain;
	Console.WriteLnFmt("(VR) ProfileDB: Serial '{}' {}: crossed near side ON (limit {:.5f}, gain {}): content "
					   "nearer than band 0's zero plane (w {}) renders IN FRONT of it.",
		serial, where, limit, gain, map.conv[0]);
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
				const std::optional<float> pv = parseWholeFloat(nodeVal(c), "regionPct");
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
				const std::optional<float> pv = parseWholeFloat(nodeVal(c), "uvRect");
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

	if (const std::string why = readGateList(serial, c, "when", "controls when-guard", spec.when); !why.empty())
		return reject(why);

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


static bool parseFovTarget(const std::string_view serial, const ryml::ConstNodeRef& node,
	const std::string& where, VR::ProfileDB::FovTarget& out)
{
	using namespace VR::ProfileDB;

	if (node.is_map())
	{
		if (node.has_child("degrees"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(node["degrees"]), "degrees");
			if (!v.has_value() || !std::isfinite(v.value()))
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.degrees is not a number.", serial, where);
				return false;
			}
			if (v.value() < VR::FovMath::kMinFovDeg || v.value() > VR::FovMath::kMaxFovDeg)
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.degrees {:.2f} is outside {:.0f}-{:.0f}.",
					serial, where, v.value(), VR::FovMath::kMinFovDeg, VR::FovMath::kMaxFovDeg);
				return false;
			}
			out.kind = FovTargetKind::Degrees;
			out.value = v.value();
			return true;
		}
		if (node.has_child("widen"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(node["widen"]), "widen");
			if (!v.has_value() || !std::isfinite(v.value()) || v.value() <= 0.0f)
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.widen must be a number > 0 "
								 "(it is a tan-space factor).", serial, where);
				return false;
			}
			out.kind = FovTargetKind::Widen;
			out.value = v.value();
			return true;
		}
		if (node.has_child("value"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(node["value"]), "value");
			if (!v.has_value() || !std::isfinite(v.value()))
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.value is not a number.", serial, where);
				return false;
			}
			out.kind = FovTargetKind::Value;
			out.value = v.value();
			return true;
		}

		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} is a map but names none of degrees/widen/value.", serial, where);
		return false;
	}

	const std::string_view sv = nodeVal(node);
	if (StringUtil::compareNoCase(sv, "screen"))
	{
		out.kind = FovTargetKind::Screen;
		return true;
	}
	if (StringUtil::compareNoCase(sv, "game"))
	{
		out.kind = FovTargetKind::Game;
		return true;
	}
	const std::optional<float> deg = parseWholeFloat(sv, "fov");
	if (!deg.has_value() || !std::isfinite(deg.value()))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} must be `screen`, `game`, a number of degrees, "
						 "or a map {{degrees|widen|value}} — it is '{}'.", serial, where, sv);
		return false;
	}
	if (deg.value() < VR::FovMath::kMinFovDeg || deg.value() > VR::FovMath::kMaxFovDeg)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} {:.2f}deg is outside {:.0f}-{:.0f}.",
			serial, where, deg.value(), VR::FovMath::kMinFovDeg, VR::FovMath::kMaxFovDeg);
		return false;
	}
	out.kind = FovTargetKind::Degrees;
	out.value = deg.value();
	return true;
}

static bool parseFovCalibration(const std::string_view serial, const ryml::ConstNodeRef& node,
	const std::string& where, VR::ProfileDB::FovCalibration& out)
{
	using namespace VR::ProfileDB;

	if (!node.is_map() || !node.has_child("model") || !node.has_child("pairs"))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} needs `model` and `pairs`.", serial, where);
		return false;
	}

	const std::string_view model = nodeVal(node["model"]);
	if (StringUtil::compareNoCase(model, "tan"))
		out.model = FovModel::Tan;
	else if (StringUtil::compareNoCase(model, "invtan"))
		out.model = FovModel::InvTan;
	else
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.model must be `tan` or `invtan` — it is '{}'.",
			serial, where, model);
		return false;
	}

	if (!node["pairs"].is_seq())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.pairs must be a list of [constant, degrees].", serial, where);
		return false;
	}
	for (const ryml::ConstNodeRef& pn : node["pairs"].children())
	{
		if (!pn.is_seq() || pn.num_children() != 2)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.pairs has an entry that is not [constant, degrees].",
				serial, where);
			return false;
		}
		const std::optional<float> c = parseWholeFloat(nodeVal(pn[0]), "pairs");
		const std::optional<float> d = parseWholeFloat(nodeVal(pn[1]), "pairs");
		if (!c.has_value() || !d.has_value())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.pairs has a non-numeric entry.", serial, where);
			return false;
		}
		out.pairs.emplace_back(c.value(), d.value());
	}

	std::string why;
	if (!VR::FovMath::Fit(out, &why))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}: {}.", serial, where, why);
		return false;
	}

	if (out.fit_error_deg > 1.0f)
	{
		advise(serial, "fov.calibration",
			fmt::format("(VR) ProfileDB: {} {}: the {} fit misses its own measurements by up to {:.2f}deg. "
						"Measure a third pair, or try the other model.",
				StringUtil::toUpper(serial), where,
				(out.model == FovModel::Tan) ? "tan" : "invtan", out.fit_error_deg));
	}
	return true;
}

static bool parseFovLeverBase(const std::string_view serial, const ryml::ConstNodeRef& node,
	const std::string& where, VR::ProfileDB::CameraBase& out)
{
	using namespace VR::ProfileDB;

	if (!node.is_map() || !node.has_child("pointer"))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} must be a map with `pointer` (a static slot holding a pointer).",
			serial, where);
		return false;
	}
	const std::optional<u32> ptr = parseAddress(nodeVal(node["pointer"]));
	if (!ptr.has_value() || !inMainRam(ptr.value(), 4))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.pointer is missing or outside main RAM.", serial, where);
		return false;
	}
	out.is_pointer = true;
	out.pointer_addr = ptr.value();

	if (node.has_child("derefOffsets"))
	{
		if (!node["derefOffsets"].is_seq())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.derefOffsets must be a list.", serial, where);
			return false;
		}
		for (const ryml::ConstNodeRef& hn : node["derefOffsets"].children())
		{
			const std::optional<s64> hop = parseSignedOffset(nodeVal(hn));
			if (!hop.has_value() || (hop.value() & 3) != 0 ||
				hop.value() < -static_cast<s64>(EE_MAIN_RAM_BYTES) ||
				hop.value() > static_cast<s64>(EE_MAIN_RAM_BYTES))
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.derefOffsets has an entry that is not a "
								 "word-aligned in-RAM offset.", serial, where);
				return false;
			}
			out.deref_offsets.push_back(static_cast<s32>(hop.value()));
		}
		if (out.deref_offsets.size() > 4)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.derefOffsets has more than 4 hops.", serial, where);
			return false;
		}
	}

	if (!node.has_child("validate") || !node["validate"].is_map())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} needs a `validate` block "
						 "({{offset, equals}}) — a chain with no guard word resolves onto anything.", serial, where);
		return false;
	}
	const ryml::ConstNodeRef vnode = node["validate"];
	const std::optional<s64> voff = vnode.has_child("offset") ? parseSignedOffset(nodeVal(vnode["offset"])) : std::nullopt;
	const std::string_view veq_raw = vnode.has_child("equals") ? nodeVal(vnode["equals"]) : std::string_view{};
	const std::optional<u32> veq = vnode.has_child("equals") ? parseHexU32(veq_raw) : std::nullopt;
	if (!voff.has_value() || !veq.has_value())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.validate needs both `offset` and `equals`.", serial, where);
		return false;
	}
	out.has_validate = true;
	out.validate_offset = voff.value();
	out.validate_equals = veq.value();
	warnHexTrap(serial, veq_raw, veq.value(), "fov lever base validate.equals");
	return true;
}

static bool projectionOpSign(const std::string_view serial, const std::optional<VR::ProfileDB::CameraProfile>& camera,
	const char* what, u32 op_index, bool sign_given, float& sign, VR::ProfileDB::CameraSource axis,
	VR::ProfileDB::CameraCompose need = VR::ProfileDB::CameraCompose::Anchored)
{
	using namespace VR::ProfileDB;
	if (!camera.has_value() || op_index >= camera->writes.size())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection {}: camera.writes[{}] does not exist; ignoring the "
						 "projection block.", serial, what, op_index);
		return false;
	}
	const CameraWriteOp& op = camera->writes[op_index];
	if (op.compose != need)
	{
		if (need == CameraCompose::Delta)
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection {}: camera.writes[{}] is not compose: delta, so the "
							 "driver keeps no running total for `minusOwn`; ignoring the projection block.", serial, what, op_index);
		else
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection {}: camera.writes[{}] is not anchored, so it has no rest "
							 "to measure from; ignoring the projection block.", serial, what, op_index);
		return false;
	}
	if (op.source != axis)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection {}: camera.writes[{}] does not take the head's {} (it is "
						 "another axis's op); ignoring the projection block.", serial, what, op_index,
			axis == CameraSource::HeadYaw ? "yaw" : "pitch");
		return false;
	}
	if (!sign_given)
		sign = op.axis_sign;
	if (sign != op.axis_sign)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection {}: sign {} is not camera.writes[{}]'s axisSign {} (the view "
						 "would turn the mirror way); ignoring the projection block.", serial, what, sign, op_index, op.axis_sign);
		return false;
	}
	return true;
}

static std::optional<VR::ProfileDB::ProjectionProfile> parseProjectionAxes(const std::string_view serial,
	const ryml::ConstNodeRef& node, const std::optional<VR::ProfileDB::CameraProfile>& camera)
{
	using namespace VR::ProfileDB;
	using PP = ProjectionProfile;
	const auto refuse = [&](const std::string& why) -> std::optional<PP> {
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: {}; ignoring the projection block.", serial, why);
		return std::nullopt;
	};
	const auto key_of = [](const ryml::ConstNodeRef& c) { return std::string_view(c.key().data(), c.key().size()); };
	const auto address = [&](const ryml::ConstNodeRef& n) -> std::optional<u32> {
		const std::optional<u32> a = parseAddress(nodeVal(n));
		if (!a.has_value() || !inMainRam(a.value(), 4) || (a.value() & 3) != 0)
			return std::nullopt;
		return a;
	};
	for (const char* flat : {"yaw", "pitch", "fovReadback", "yawOp", "pitchOp", "yawSign", "pitchSign", "frameAge"})
		if (node.has_child(ryml::to_csubstr(flat)))
			return refuse(fmt::format("`{}` is a flat key; the flat keys and `axes:` are two spellings, use one", flat));
	warnUnknownKeys(serial, node, "projection", {"axes", "order", "frustum", "timing", "when", "notes"});

	PP out;
	out.general = true;
	const ryml::ConstNodeRef axes = node["axes"];
	if (!axes.is_map() || axes.num_children() == 0)
		return refuse("`axes` must be a map of the axes the lever carries");
	bool have_yaw = false, have_pitch = false, have_rotation = false;
	for (const ryml::ConstNodeRef& ax : axes.children())
	{
		const std::string_view name = key_of(ax);
		if (!ax.is_map())
			return refuse(fmt::format("axes.{} must be a map (drawn, body, units, sign)", name));
		warnUnknownKeys(serial, ax, "projection axis", {"drawn", "body", "units", "sign", "roll"});
		const ryml::ConstNodeRef drawn = ax["drawn"];
		if (!ax.has_child("drawn") || !drawn.is_map() || drawn.num_children() != 1)
			return refuse(fmt::format("axes.{}.drawn must name one source (copy | matrix; written, hook not built)", name));
		const std::string_view kind = key_of(drawn.child(0));
		if (kind == "written")
			return refuse(fmt::format("axes.{}.drawn: `written` is not built yet (E2)", name));
		if (kind == "hook" && name != "rotation")
			return refuse(fmt::format("axes.{}.drawn: a scalar `hook` is not built yet (E7; only `rotation` takes a "
									  "hook, the camera's 3x3)", name));

		if (name == "rotation" && kind == "hook")
		{
			const ryml::ConstNodeRef h = drawn["hook"];
			if (!h.is_map())
				return refuse("axes.rotation.drawn.hook must be a map (at, expect, base, offset, basis, maxAge)");
			warnUnknownKeys(serial, h, "projection hook", {"at", "expect", "base", "offset", "basis", "maxAge"});
			const std::optional<u32> at = h.has_child("at") ? address(h["at"]) : std::nullopt;
			const std::optional<u32> expect = h.has_child("expect") ? parseHexU32(nodeVal(h["expect"])) : std::nullopt;
			if (!at.has_value())
				return refuse("axes.rotation.drawn.hook.at is not an aligned main-RAM instruction address");
			if (!expect.has_value() || expect.value() == 0)
				return refuse("axes.rotation.drawn.hook.expect must be the instruction word the game holds there (not 0)");
			static constexpr const char* kGprNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1",
				"t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1",
				"gp", "sp", "fp", "ra"};
			int reg = -1;
			if (h.has_child("base"))
			{
				const std::string_view b = nodeVal(h["base"]);
				for (int i = 1; i < 32 && reg < 0; i++)
					if (StringUtil::compareNoCase(b, kGprNames[i]))
						reg = i;
				if (reg < 0)
				{
					const std::optional<u32> n = StringUtil::FromChars<u32>(b);
					if (n.has_value() && n.value() >= 1 && n.value() <= 31)
						reg = static_cast<int>(n.value());
				}
			}
			if (reg < 0)
				return refuse("axes.rotation.drawn.hook.base must name a register 1..31 (a0, s1, ... or its number)");
			const std::string_view off_raw = h.has_child("offset") ? nodeVal(h["offset"]) : std::string_view("0x0");
			const bool off_hex = off_raw.size() > 2 && off_raw[0] == '0' && (off_raw[1] == 'x' || off_raw[1] == 'X');
			const std::optional<u32> offset = off_hex ? parseHexU32(off_raw) : std::nullopt;
			if (!offset.has_value() || (offset.value() & 3) != 0 || offset.value() > 0x10000)
				return refuse("axes.rotation.drawn.hook.offset must be an aligned hex offset up to 0x10000, written with "
							  "0x (camera+240 is 0xF0)");
			const ryml::ConstNodeRef bn = h["basis"];
			if (!h.has_child("basis") || !bn.is_seq() || bn.num_children() != 3)
				return refuse("axes.rotation.drawn.hook.basis must list three rows for OpenXR x, y, z: +-1..3 "
							  "(NASCAR: [1, 3, -2], right, up, back)");
			int bi = 0;
			int seen = 0, inversions = 0, bsign = 1, perm[3] = {0, 0, 0};
			for (const ryml::ConstNodeRef& e : bn.children())
			{
				const std::optional<s32> v = StringUtil::FromChars<s32>(nodeVal(e));
				const int a = v.has_value() ? (v.value() < 0 ? -v.value() : v.value()) : 0;
				if (!v.has_value() || a < 1 || a > 3 || (seen & (1 << a)))
					return refuse("axes.rotation.drawn.hook.basis must be a signed permutation of rows 1, 2, 3");
				seen |= 1 << a;
				perm[bi] = a;
				bsign *= v.value() < 0 ? -1 : 1;
				out.hook_basis[bi++] = static_cast<s8>(v.value());
			}
			for (int x = 0; x < 3; x++)
				for (int y = x + 1; y < 3; y++)
					inversions += perm[x] > perm[y] ? 1 : 0;
			if (bsign * ((inversions % 2) ? -1 : 1) != 1)
				return refuse("axes.rotation.drawn.hook.basis is a mirror (determinant -1): the view would turn inside out");
			if (h.has_child("maxAge"))
			{
				const std::optional<u32> m = StringUtil::FromChars<u32>(nodeVal(h["maxAge"]));
				if (!m.has_value() || m.value() < 1 || m.value() > 8)
					return refuse("axes.rotation.drawn.hook.maxAge must be 1..8 vsyncs");
				out.visit_max_age = m.value();
			}
			const ryml::ConstNodeRef b = ax["body"];
			if (!ax.has_child("body") || !b.is_map() || !b.has_child("hook"))
				return refuse("axes.rotation.body for a hook must be { hook: pre } or { hook: rest, lever: <op> }");
			warnUnknownKeys(serial, b, "projection hook body", {"hook", "lever"});
			const std::string_view bk = nodeVal(b["hook"]);
			if (StringUtil::compareNoCase(bk, "pre"))
				out.hook_body = PP::HookBody::Pre;
			else if (StringUtil::compareNoCase(bk, "rest"))
				out.hook_body = PP::HookBody::Rest;
			else
				return refuse("axes.rotation.body.hook must be `pre` or `rest`");
			bool op_rolls = false;
			if (b.has_child("lever"))
			{
				const std::optional<u32> op = StringUtil::FromChars<u32>(nodeVal(b["lever"]));
				if (!op.has_value() || !camera.has_value() || op.value() >= camera->matrix_writes.size())
					return refuse("axes.rotation.body.lever names no camera.matrixWrites op");
				const CameraMatrixOp& mop = camera->matrix_writes[op.value()];
				if (mop.relative || !mop.anchored)
					return refuse("axes.rotation.body.lever must be an anchored, absolute matrix op (its 4x4 is the head's)");
				out.lever_address = mop.ee_address;
				op_rolls = mop.axis_sign_roll != 0.0f;
			}
			else if (out.hook_body == PP::HookBody::Rest)
				return refuse("axes.rotation.body { hook: rest } needs `lever:` (the matrix op the hook's lever applies)");
			if (ax.has_child("roll"))
				return refuse("axes.rotation.roll: a hook declares what the game drew; the lever's roll is its op's "
							  "axisSignRoll");
			if (ax.has_child("sign") || ax.has_child("units"))
				return refuse("axes.rotation takes no sign or units: the basis carries them");
			out.drawn = PP::Drawn::Hook;
			out.hook_pc = at.value();
			out.hook_expect = expect.value();
			out.hook_reg = static_cast<u8>(reg);
			out.hook_offset = offset.value();
			out.roll_declared = op_rolls;
			out.write_to_read = 0;
			have_rotation = true;
			continue;
		}
		if (name == "rotation")
		{
			if (kind != "matrix")
				return refuse("axes.rotation.drawn must be a `matrix` or a `hook`");
			const ryml::ConstNodeRef m = drawn["matrix"];
			if (!m.is_map() || !m.has_child("address") || !m.has_child("op"))
				return refuse("axes.rotation.drawn.matrix needs `address` and `op` (the camera.matrixWrites index)");
			const std::optional<u32> a = address(m["address"]);
			const std::optional<u32> op = StringUtil::FromChars<u32>(nodeVal(m["op"]));
			if (!a.has_value())
				return refuse("axes.rotation.drawn.matrix.address is not an aligned main-RAM address");
			if (!op.has_value() || !camera.has_value() || op.value() >= camera->matrix_writes.size())
				return refuse("axes.rotation.drawn.matrix.op names no camera.matrixWrites op");
			const CameraMatrixOp& mop = camera->matrix_writes[op.value()];
			if (mop.relative || mop.ee_address != a.value())
				return refuse(fmt::format("axes.rotation.drawn.matrix.address {:#x} is not camera.matrixWrites[{}]'s own "
										  "absolute address", a.value(), op.value()));
			if (!mop.anchored)
				return refuse("axes.rotation: a delta matrix op composes onto the game's own rotation, so its matrix is "
							  "not the head's (only an anchored op's is)");
			if (!ax.has_child("body") || !StringUtil::compareNoCase(nodeVal(ax["body"]), "identity"))
				return refuse("axes.rotation.body must be `identity` (the op's anchored base, the identity the "
							  "profile installs)");
			if (ax.has_child("sign") || ax.has_child("units"))
				return refuse("axes.rotation takes no sign or units: the op's signs are inside the matrix");
			const bool op_rolls = mop.axis_sign_roll != 0.0f;
			if (ax.has_child("roll"))
			{
				bool roll = op_rolls;
				const std::string_view v = nodeVal(ax["roll"]);
				if (StringUtil::compareNoCase(v, "true") || v == "1")
					roll = true;
				else if (StringUtil::compareNoCase(v, "false") || v == "0")
					roll = false;
				else
					return refuse("axes.rotation.roll must be true or false");
				if (roll != op_rolls)
					return refuse(fmt::format("axes.rotation.roll is {} but camera.matrixWrites[{}].axisSignRoll is {} (a "
											  "roll written and not declared is a double roll)", roll, op.value(), mop.axis_sign_roll));
			}
			out.drawn = PP::Drawn::Matrix;
			out.matrix_address = a.value();
			out.matrix_op = op.value();
			out.roll_declared = op_rolls;
			out.write_to_read = 0;
			have_rotation = true;
			continue;
		}
		if (name == "roll")
			return refuse("axes.roll on a copy source is not built yet (E5); GT4's roll stays with the runtime");
		if (name != "yaw" && name != "pitch")
			return refuse(fmt::format("axes.{}: the axes are yaw, pitch, roll, or rotation (a matrix)", name));
		if (kind == "matrix")
			return refuse(fmt::format("axes.{}: a matrix carries every axis; name it `rotation`", name));
		if (kind != "copy" && kind != "lookAt")
			return refuse(fmt::format("axes.{}.drawn: unknown source `{}`", name, kind));
		std::optional<u32> a;
		u32 look_eye = 0, look_target = 0;
		if (kind == "lookAt")
		{
			const ryml::ConstNodeRef l = drawn["lookAt"];
			if (!l.is_map() || !l.has_child("eye") || !l.has_child("target"))
				return refuse(fmt::format("axes.{}.drawn.lookAt needs eye and target (hex offsets from camera.base)", name));
			warnUnknownKeys(serial, l, "projection lookAt", {"eye", "target"});
			const auto offset = [&](const ryml::ConstNodeRef& n) -> std::optional<u32> {
				const std::string_view raw = nodeVal(n);
				const bool hex = raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X');
				const std::optional<u32> v = hex ? parseHexU32(raw) : std::nullopt;
				if (!v.has_value() || (v.value() & 3) != 0 || v.value() > 0xFFF0)
					return std::nullopt;
				return v;
			};
			const std::optional<u32> e = offset(l["eye"]), t = offset(l["target"]);
			if (!e.has_value() || !t.has_value() || e.value() == t.value())
				return refuse(fmt::format("axes.{}.drawn.lookAt: eye and target must be two aligned hex offsets (0x required)", name));
			if (!camera.has_value() || !camera->base.has_value())
				return refuse(fmt::format("axes.{}.drawn.lookAt reads through camera.base; the camera block has none", name));
			if (out.drawn == PP::Drawn::LookAt && (out.look_eye_offset != e.value() || out.look_target_offset != t.value()))
				return refuse("axes.yaw and axes.pitch must read the same lookAt (one eye, one target)");
			if ((have_yaw || have_pitch) && out.drawn != PP::Drawn::LookAt)
				return refuse("a lookAt axis and a copy axis in one block: name one source for both");
			look_eye = e.value();
			look_target = t.value();
		}
		else
		{
			const ryml::ConstNodeRef c = drawn["copy"];
			if (c.is_map())
				return refuse(fmt::format("axes.{}.drawn.copy: a relative copy is not built yet (E3; a look-at is `lookAt`)", name));
			a = address(c);
			if (!a.has_value())
				return refuse(fmt::format("axes.{}.drawn.copy is not an aligned main-RAM address", name));
			if (out.drawn == PP::Drawn::LookAt)
				return refuse("a lookAt axis and a copy axis in one block: name one source for both");
		}
		if (ax.has_child("units") && !StringUtil::compareNoCase(nodeVal(ax["units"]), "degrees"))
		{
			if (StringUtil::compareNoCase(nodeVal(ax["units"]), "radians"))
				return refuse(fmt::format("axes.{}.units: radians are not built yet (E4)", name));
			return refuse(fmt::format("axes.{}.units must be degrees", name));
		}
		if (ax.has_child("roll"))
			return refuse(fmt::format("axes.{}.roll belongs to a matrix source", name));
		PP::Body body = PP::Body::Anchored;
		std::optional<u32> op;
		const ryml::ConstNodeRef b = ax["body"];
		if (!ax.has_child("body"))
			return refuse(fmt::format("axes.{} needs a body (anchored: <op> | identity)", name));
		if (StringUtil::compareNoCase(nodeVal(b), "identity"))
			body = PP::Body::Identity;
		else if (b.is_map() && b.num_children() == 1)
		{
			const std::string_view bk = key_of(b.child(0));
			if (bk == "bias" || bk == "value")
				return refuse(fmt::format("axes.{}.body: `{}` is not built yet (E1)", name, bk));
			if (bk == "hookPre")
				return refuse(fmt::format("axes.{}.body: `hookPre` is not built yet (E1, E7)", name));
			if (bk == "minusOwn")
			{
				body = PP::Body::MinusOwn;
				op = StringUtil::FromChars<u32>(nodeVal(b["minusOwn"]));
				if (!op.has_value() || op.value() > 63)
					return refuse(fmt::format("axes.{}.body.minusOwn is not a camera.writes index", name));
			}
			else if (bk == "identity")
				body = PP::Body::Identity;
			else if (bk == "anchored")
			{
				op = StringUtil::FromChars<u32>(nodeVal(b["anchored"]));
				if (!op.has_value() || op.value() > 63)
					return refuse(fmt::format("axes.{}.body.anchored is not a camera.writes index", name));
			}
			else
				return refuse(fmt::format("axes.{}.body: unknown kind `{}`", name, bk));
		}
		else
			return refuse(fmt::format("axes.{}.body must be `identity` or one kind (anchored: <op>)", name));
		float sign = 1.0f;
		const bool sign_given = ax.has_child("sign");
		if (sign_given)
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(ax["sign"]), "sign");
			if (!v.has_value() || (v.value() != 1.0f && v.value() != -1.0f))
				return refuse(fmt::format("axes.{}.sign must be 1 or -1", name));
			sign = v.value();
		}
		if (body == PP::Body::Anchored || body == PP::Body::MinusOwn)
		{
			if (!projectionOpSign(serial, camera, name == "yaw" ? "axes.yaw" : "axes.pitch", op.value(), sign_given, sign,
					name == "yaw" ? CameraSource::HeadYaw : CameraSource::HeadPitch,
					body == PP::Body::MinusOwn ? CameraCompose::Delta : CameraCompose::Anchored))
				return std::nullopt;
		}
		else if (!sign_given)
			return refuse(fmt::format("axes.{}: an identity body has no op to take the sign from; give `sign`", name));
		if (kind == "lookAt")
		{
			out.drawn = PP::Drawn::LookAt;
			out.look_eye_offset = look_eye;
			out.look_target_offset = look_target;
		}
		if (name == "yaw")
		{
			out.yaw_address = a.value_or(0);
			out.yaw_body = body;
			if (op.has_value())
				out.yaw_op = op;
			out.yaw_sign = sign;
			have_yaw = true;
		}
		else
		{
			out.pitch_address = a.value_or(0);
			out.pitch_body = body;
			if (op.has_value())
				out.pitch_op = op;
			out.pitch_sign = sign;
			have_pitch = true;
		}
	}
	if (have_rotation && (have_yaw || have_pitch))
		return refuse("axes.rotation (a matrix) carries every axis; it takes no yaw or pitch beside it");
	if (!have_rotation && !have_yaw)
		return refuse("axes needs `yaw` (or `rotation`)");

	if (node.has_child("order"))
	{
		if (have_rotation)
			return refuse("`order` belongs to copy axes; a matrix carries its own");
		if (!StringUtil::compareNoCase(nodeVal(node["order"]), "yaw-pitch"))
			return refuse(fmt::format("order `{}` is not built yet (E5); only yaw-pitch", nodeVal(node["order"])));
	}

	const bool has_frustum = node.has_child("frustum"), has_timing = node.has_child("timing");
	if (has_frustum != has_timing)
		return refuse("a view needs `frustum` and `timing` together");
	if (has_frustum)
	{
		const ryml::ConstNodeRef f = node["frustum"];
		if (!f.is_map())
			return refuse("frustum must be a map (model, readback, tanX, tanY)");
		warnUnknownKeys(serial, f, "projection frustum", {"model", "readback", "tanX", "tanY"});
		const std::string_view model = f.has_child("model") ? nodeVal(f["model"]) : std::string_view("degrees");
		if (StringUtil::compareNoCase(model, "invtan"))
			return refuse("frustum model `invtan` is not built yet (E6)");
		if (StringUtil::compareNoCase(model, "degrees"))
		{
			out.frustum_model = PP::FrustumModel::Degrees;
			const std::optional<u32> rb = f.has_child("readback") ? address(f["readback"]) : std::nullopt;
			if (!rb.has_value())
				return refuse("frustum model `degrees` needs `readback`, an aligned main-RAM f32 word");
			out.fov_readback = rb;
		}
		else if (StringUtil::compareNoCase(model, "constant"))
		{
			out.frustum_model = PP::FrustumModel::Constant;
			if (f.has_child("readback"))
				return refuse("frustum model `constant` reads no word; drop `readback` or use `degrees`");
		}
		else
			return refuse(fmt::format("frustum model `{}` is unknown (degrees | constant)", model));
		const std::optional<float> tx = f.has_child("tanX") ? parseWholeFloat(nodeVal(f["tanX"]), "tanX") : std::nullopt;
		const std::optional<float> ty = f.has_child("tanY") ? parseWholeFloat(nodeVal(f["tanY"]), "tanY") : std::nullopt;
		if (!tx.has_value() || !ty.has_value() || !(tx.value() > 0.0f) || !(ty.value() > 0.0f) || tx.value() > 100.0f ||
			ty.value() > 100.0f)
			return refuse("frustum needs positive tanX and tanY");
		out.tan_x = tx.value();
		out.tan_y = ty.value();

		const ryml::ConstNodeRef t = node["timing"];
		if (!t.is_map())
			return refuse("timing must be a map (close, frameAge)");
		warnUnknownKeys(serial, t, "projection timing",
			{"close", "frameAge", "frameAges", "delay", "writeToRead", "latch", "readToDraw"});
		const bool close_drawn = t.has_child("close") && StringUtil::compareNoCase(nodeVal(t["close"]), "drawn");
		const bool latch_closing = t.has_child("latch") && StringUtil::compareNoCase(nodeVal(t["latch"]), "closing");
		if (t.has_child("close") && !close_drawn && !StringUtil::compareNoCase(nodeVal(t["close"]), "shown"))
			return refuse("timing close must be `shown` or `drawn`");
		if (close_drawn != latch_closing)
			return refuse(close_drawn ? "timing close `drawn` is built only with `latch: closing` (E13)" :
										"timing latch `closing` needs `close: drawn` (E13: the composite is closed into "
										"the buffer it drew, which the registers do not name)");
		if (t.has_child("delay"))
			return refuse("timing delay belongs to a `written` source, not built yet (E2)");
		if (t.has_child("frameAge") == t.has_child("frameAges"))
			return refuse("timing needs `frameAge` or `frameAges` (one of them, measured)");
		if (t.has_child("frameAges"))
		{
			const ryml::ConstNodeRef as = t["frameAges"];
			if (!as.is_seq() || as.num_children() == 0 || as.num_children() > 4)
				return refuse("timing frameAges must list 1 to 4 ages");
			for (const ryml::ConstNodeRef& e : as.children())
			{
				const std::optional<u32> a = StringUtil::FromChars<u32>(nodeVal(e));
				if (!a.has_value() || a.value() < 1 || a.value() > 8)
					return refuse("timing frameAges: each age must be 1..8 vsyncs (measured)");
				for (u32 seen_age : out.frame_ages)
					if (seen_age == a.value())
						return refuse("timing frameAges lists an age twice");
				out.frame_ages.push_back(a.value());
			}
			out.frame_age = out.frame_ages.front();
		}
		else
		{
			const std::optional<u32> age = StringUtil::FromChars<u32>(nodeVal(t["frameAge"]));
			if (!age.has_value() || age.value() < 1 || age.value() > 8)
				return refuse("timing frameAge must be 1..8 vsyncs (measured)");
			out.frame_age = age.value();
		}
		if (t.has_child("latch"))
		{
			const std::string_view l = nodeVal(t["latch"]);
			if (StringUtil::compareNoCase(l, "firstDraw"))
				out.latch_first_draw = true;
			else if (latch_closing)
				out.latch_closing = true;
			else if (!StringUtil::compareNoCase(l, "mark"))
				return refuse("timing latch must be `mark` (GT4), `firstDraw` (E9) or `closing` (E13)");
		}
		if (t.has_child("readToDraw"))
		{
			const std::optional<u32> d = StringUtil::FromChars<u32>(nodeVal(t["readToDraw"]));
			if (!out.latch_first_draw)
				return refuse("timing readToDraw belongs to `latch: firstDraw`");
			if (!d.has_value() || d.value() > 8)
				return refuse("timing readToDraw must be 0..8 marks");
			out.read_to_draw = d.value();
		}
		if (t.has_child("writeToRead"))
		{
			const std::optional<u32> w = StringUtil::FromChars<u32>(nodeVal(t["writeToRead"]));
			if (!w.has_value() || w.value() > 8)
				return refuse("timing writeToRead must be 0..8 vsyncs");
			out.write_to_read = w.value();
		}
		out.view = true;
	}

	if (const std::string why = readGateList(serial, node, "when", "projection when-guard", out.when); !why.empty())
		return refuse(why);
	if (out.when.empty())
		return refuse("needs a non-empty `when` (the scene whose camera the source describes)");
	return out;
}

static std::optional<VR::ProfileDB::ProjectionProfile> parseProjection(const std::string_view serial,
	const ryml::ConstNodeRef& node, const std::optional<VR::ProfileDB::CameraProfile>& camera)
{
	using namespace VR::ProfileDB;
	if (!node.is_map())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: must be a map (yaw, pitch, fovReadback, when); "
						 "ignoring it.", serial);
		return std::nullopt;
	}
	if (node.has_child("axes"))
		return parseProjectionAxes(serial, node, camera);
	warnUnknownKeys(serial, node, "projection",
		{"yaw", "pitch", "fovReadback", "when", "yawOp", "pitchOp", "yawSign", "pitchSign", "frustum", "frameAge", "notes"});
	ProjectionProfile out;
	const auto word = [&](const char* key, u32& dst) {
		const std::optional<u32> a = parseAddress(nodeVal(node[key]));
		if (!a.has_value() || !inMainRam(a.value(), 4) || (a.value() & 3) != 0)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection.{} is not an aligned main-RAM address; ignoring "
							 "the projection block.", serial, key);
			return false;
		}
		dst = a.value();
		return true;
	};
	if (!node.has_child("yaw") || !word("yaw", out.yaw_address))
	{
		if (!node.has_child("yaw"))
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: needs `yaw`; ignoring it.", serial);
		return std::nullopt;
	}
	if (node.has_child("pitch"))
	{
		u32 a = 0;
		if (!word("pitch", a))
			return std::nullopt;
		out.pitch_address = a;
	}
	if (node.has_child("fovReadback"))
	{
		u32 a = 0;
		if (!word("fovReadback", a))
			return std::nullopt;
		out.fov_readback = a;
	}
	if (const std::string why = readGateList(serial, node, "when", "projection when-guard", out.when); !why.empty())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: {}; ignoring it.", serial, why);
		return std::nullopt;
	}
	if (out.when.empty())
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: needs a non-empty `when` (the scene whose camera "
						 "the copy describes); ignoring it.", serial);
		return std::nullopt;
	}
	const auto op_index = [&](const char* key, std::optional<u32>& dst) {
		const std::optional<u32> v = StringUtil::FromChars<u32>(nodeVal(node[key]));
		if (!v.has_value() || v.value() > 63)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection.{} is not a camera.writes index; ignoring the "
							 "projection block.", serial, key);
			return false;
		}
		dst = v.value();
		return true;
	};
	const auto sign = [&](const char* key, float& dst) {
		const std::optional<float> v = parseWholeFloat(nodeVal(node[key]), key);
		if (!v.has_value() || (v.value() != 1.0f && v.value() != -1.0f))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection.{} must be 1 or -1; ignoring the projection "
							 "block.", serial, key);
			return false;
		}
		dst = v.value();
		return true;
	};
	if (node.has_child("yawOp") && !op_index("yawOp", out.yaw_op))
		return std::nullopt;
	if (node.has_child("pitchOp") && !op_index("pitchOp", out.pitch_op))
		return std::nullopt;
	if (node.has_child("yawSign") && !sign("yawSign", out.yaw_sign))
		return std::nullopt;
	if (node.has_child("pitchSign") && !sign("pitchSign", out.pitch_sign))
		return std::nullopt;
	if (node.has_child("frustum"))
	{
		const ryml::ConstNodeRef f = node["frustum"];
		const std::optional<float> tx = (f.is_map() && f.has_child("tanX")) ? parseWholeFloat(nodeVal(f["tanX"]), "tanX") : std::nullopt;
		const std::optional<float> ty = (f.is_map() && f.has_child("tanY")) ? parseWholeFloat(nodeVal(f["tanY"]), "tanY") : std::nullopt;
		if (!tx.has_value() || !ty.has_value() || !(tx.value() > 0.0f) || !(ty.value() > 0.0f) || tx.value() > 100.0f ||
			ty.value() > 100.0f)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection.frustum needs positive tanX and tanY; ignoring the "
							 "projection block.", serial);
			return std::nullopt;
		}
		out.tan_x = tx.value();
		out.tan_y = ty.value();
	}
	if (node.has_child("frameAge"))
	{
		const std::optional<u32> v = StringUtil::FromChars<u32>(nodeVal(node["frameAge"]));
		if (!v.has_value() || v.value() < 1 || v.value() > 8)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection.frameAge must be 1..8 vsyncs (measured); ignoring "
							 "the projection block.", serial);
			return std::nullopt;
		}
		out.frame_age = v.value();
	}
	const bool view = out.yaw_op.has_value() || out.tan_x > 0.0f || out.frame_age != 0;
	if (view && (!out.yaw_op.has_value() || !(out.tan_x > 0.0f) || !out.fov_readback.has_value() || out.frame_age == 0))
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' projection: a view needs yawOp, frustum, fovReadback and frameAge "
						 "together; ignoring the projection block.", serial);
		return std::nullopt;
	}
	if (out.yaw_op.has_value() &&
		!projectionOpSign(serial, camera, "yawSign", out.yaw_op.value(), node.has_child("yawSign"), out.yaw_sign,
			CameraSource::HeadYaw))
		return std::nullopt;
	if (out.pitch_op.has_value() &&
		!projectionOpSign(serial, camera, "pitchSign", out.pitch_op.value(), node.has_child("pitchSign"), out.pitch_sign,
			CameraSource::HeadPitch))
		return std::nullopt;
	out.view = view;
	return out;
}

static bool parseFov(const std::string_view serial, const ryml::ConstNodeRef& fnode, VR::ProfileDB::FovProfile& out)
{
	using namespace VR::ProfileDB;

	if (!fnode.has_child("levers") || !fnode["levers"].is_seq() || fnode["levers"].num_children() == 0)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' fov: needs a non-empty `levers` list.", serial);
		return false;
	}
	for (const ryml::ConstNodeRef& ln : fnode["levers"].children())
	{
		FovLever lever;
		const std::string where = fmt::format("fov.levers[{}]", out.levers.size());
		if (!ln.is_map() || !ln.has_child("name") || !ln.has_child("address"))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} needs `name` and `address`.", serial, where);
			return false;
		}
		lever.name.assign(nodeVal(ln["name"]));
		if (lever.name.empty())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} has an empty name.", serial, where);
			return false;
		}
		for (const FovLever& seen : out.levers)
		{
			if (seen.name == lever.name)
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' has two fov levers named '{}'.", serial, lever.name);
				return false;
			}
		}

		const std::optional<u32> addr = parseAddress(nodeVal(ln["address"]));
		if (!addr.has_value())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.address is not an address.", serial, where);
			return false;
		}
		lever.address = addr.value();

		if (ln.has_child("encoding"))
		{
			const std::optional<CameraEncoding> enc = parseCameraEncoding(nodeVal(ln["encoding"]));
			if (!enc.has_value())
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.encoding is not a known encoding.", serial, where);
				return false;
			}
			lever.encoding = enc.value();
		}

		if (ln.has_child("base"))
		{
			CameraBase base;
			if (!parseFovLeverBase(serial, ln["base"], where + ".base", base))
				return false;
			lever.base = std::move(base);
		}
		else if (!inMainRam(lever.address, 4))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.address {:#x} is outside main RAM.",
				serial, where, lever.address);
			return false;
		}

		if (ln.has_child("calibration"))
		{
			FovCalibration cal;
			if (!parseFovCalibration(serial, ln["calibration"], where + ".calibration", cal))
				return false;
			lever.calibration = std::move(cal);
		}

		if (ln.has_child("readback"))
		{
			const std::optional<u32> rb = parseAddress(nodeVal(ln["readback"]));
			if (!rb.has_value() || !inMainRam(rb.value(), 4))
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.readback is not an in-RAM address.", serial, where);
				return false;
			}
			lever.has_readback = true;
			lever.readback_address = rb.value();
		}

		out.levers.push_back(std::move(lever));
	}

	if (fnode.has_child("default"))
	{
		const std::string_view dflt = nodeVal(fnode["default"]);
		if (!StringUtil::compareNoCase(dflt, "game"))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' fov.default can only be `game` — it is '{}'. "
							 "Every other target needs a scene's lever to apply to.", serial, dflt);
			return false;
		}
		out.default_target.kind = FovTargetKind::Game;
	}

	if (!fnode.has_child("scenes") || !fnode["scenes"].is_seq() || fnode["scenes"].num_children() == 0)
	{
		Console.ErrorFmt("(VR) ProfileDB: Serial '{}' fov: needs a non-empty `scenes` list.", serial);
		return false;
	}
	for (const ryml::ConstNodeRef& sn : fnode["scenes"].children())
	{
		FovScene scene;
		const std::string where = fmt::format("fov.scenes[{}]", out.scenes.size());
		if (!sn.is_map() || !sn.has_child("label") || !sn.has_child("lever") || !sn.has_child("target"))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} needs `label`, `lever` and `target`.", serial, where);
			return false;
		}
		scene.label.assign(nodeVal(sn["label"]));
		if (scene.label.empty())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} has an empty label.", serial, where);
			return false;
		}

		const std::string_view lever_name = nodeVal(sn["lever"]);
		bool found = false;
		for (size_t i = 0; i < out.levers.size(); i++)
		{
			if (out.levers[i].name == lever_name)
			{
				scene.lever = i;
				found = true;
				break;
			}
		}
		if (!found)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} names lever '{}', which no lever defines.",
				serial, where, lever_name);
			return false;
		}

		if (!parseFovTarget(serial, sn["target"], where + ".target", scene.target))
			return false;

		if (!sn.has_child("when") || !sn["when"].is_seq() || sn["when"].num_children() == 0)
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {} needs a non-empty `when` list "
							 "(use fov.default for the no-scene case).", serial, where);
			return false;
		}
		if (const std::string why = readGateList(serial, sn, "when", "fov scene when-guard", scene.when); !why.empty())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}: {}.", serial, where, why);
			return false;
		}
		if (scene.when.empty())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.when has no usable guard.", serial, where);
			return false;
		}

		if (sn.has_child("ramp"))
		{
			const std::optional<float> r = parseWholeFloat(nodeVal(sn["ramp"]), "ramp");
			if (!r.has_value() || !std::isfinite(r.value()) || r.value() < 0.0f || r.value() > 10.0f)
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' {}.ramp must be 0-10 seconds.", serial, where);
				return false;
			}
			scene.ramp_seconds = r.value();
		}

		out.scenes.push_back(std::move(scene));
	}

	const auto needs_calibration = [](const FovTarget& t) {
		return t.kind == FovTargetKind::Degrees || t.kind == FovTargetKind::Screen ||
			   t.kind == FovTargetKind::Widen;
	};
	for (const FovScene& scene : out.scenes)
	{
		if (needs_calibration(scene.target) && !out.levers[scene.lever].calibration.has_value())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' fov scene '{}' asks for a field of view in degrees, but "
							 "lever '{}' has no calibration. Measure two (constant, degrees) pairs, "
							 "or use a raw {{value: c}} target.",
				serial, scene.label, out.levers[scene.lever].name);
			return false;
		}
	}
	return true;
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
		const ryml::ConstNodeRef cnode = node["crcs"];
		if (!cnode.is_seq())
		{
			const std::string_view v = nodeVal(cnode);
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' crcs: is not a list — write crcs: [{}] with the brackets. "
							 "Refusing the entry: read as a list of nothing it would match every disc.",
				serial, v.empty() ? std::string_view("0x...") : v);
			return false;
		}
		for (const ryml::ConstNodeRef& c : cnode.children())
		{
			const std::string_view crc_str = nodeVal(c);
			const std::optional<u32> crc = parseCrc(crc_str);
			if (!crc.has_value())
			{
				Console.ErrorFmt("(VR) ProfileDB: Serial '{}' has an invalid CRC '{}' (want a hex CRC, e.g. 0x39B574F0); "
								 "refusing the entry.", serial, crc_str);
				return false;
			}
			out.crcs.push_back(crc.value());
		}
	}

	if (node.has_child("basedOn"))
		out.based_on.assign(nodeVal(node["basedOn"]));

	if (node.has_child("requires"))
	{
		const ryml::ConstNodeRef rnode = node["requires"];
		if (!rnode.is_map())
		{
			advise(serial, "requires",
				fmt::format("(VR) ProfileDB: {} requires: is not a map — IGNORED, so no setting this disc needs will be offered.",
					StringUtil::toUpper(serial)));
		}
		else
		{
			warnUnknownKeys(serial, rnode, "requires", {"extraMemory"}, "requires");
			if (rnode.has_child("extraMemory"))
			{
				const std::string_view v = nodeVal(rnode["extraMemory"]);
				if (StringUtil::compareNoCase(v, "true"))
					out.requirements.extra_memory = true;
				else if (StringUtil::compareNoCase(v, "false"))
					out.requirements.extra_memory = false;
				else
					advise(serial, "requires.extraMemory",
						fmt::format("(VR) ProfileDB: {} requires.extraMemory is '{}', not true or false — IGNORED, so "
									"Extended RAM will not be offered for this disc.",
							StringUtil::toUpper(serial), v));
			}
		}
	}
	if (node.has_child("recommends"))
	{
		const ryml::ConstNodeRef rnode = node["recommends"];
		if (!rnode.is_map())
		{
			advise(serial, "recommends",
				fmt::format("(VR) ProfileDB: {} recommends: is not a map — IGNORED.", StringUtil::toUpper(serial)));
		}
		else
		{
			warnUnknownKeys(serial, rnode, "recommends", {"textures"}, "recommends");
			if (rnode.has_child("textures"))
			{
				const std::string_view v = nodeVal(rnode["textures"]);
				if (!rnode["textures"].is_keyval() || v.empty())
					advise(serial, "recommends.textures",
						fmt::format("(VR) ProfileDB: {} recommends.textures is not a texture-pack id — IGNORED.",
							StringUtil::toUpper(serial)));
				else
					out.recommendations.textures.assign(v);
			}
		}
	}

	if (node.has_child("textures"))
	{
		const ryml::ConstNodeRef tnode = node["textures"];
		if (!tnode.is_map())
		{
			advise(serial, "textures", fmt::format("(VR) ProfileDB: {} textures: is not a map — IGNORED.", StringUtil::toUpper(serial)));
		}
		else
		{
			warnUnknownKeys(serial, tnode, "textures", {"pack", "classicNames"}, "textures");
			if (tnode.has_child("pack"))
			{
				static const std::regex pack_re("^[a-z0-9]+(?:-[a-z0-9]+)*--v[0-9][0-9a-z.\\-]*$");
				const std::string v(nodeVal(tnode["pack"]));
				if (std::regex_match(v, pack_re))
					out.textures.pack = v;
				else
					advise(serial, "textures.pack",
						fmt::format("(VR) ProfileDB: {} textures.pack '{}' is not a versioned pack folder name "
									"(<name>--v<version>) — IGNORED.", StringUtil::toUpper(serial), v));
			}
			if (tnode.has_child("classicNames"))
			{
				const std::string_view v = nodeVal(tnode["classicNames"]);
				if (StringUtil::compareNoCase(v, "true"))
					out.textures.classic_names = true;
				else if (StringUtil::compareNoCase(v, "false"))
					out.textures.classic_names = false;
				else
					advise(serial, "textures.classicNames",
						fmt::format("(VR) ProfileDB: {} textures.classicNames '{}' is not true or false — IGNORED.",
							StringUtil::toUpper(serial), v));
			}
		}
	}

	if (node.has_child("stereo") && node["stereo"].is_map())
	{
		const ryml::ConstNodeRef snode = node["stereo"];
		StereoParams sp;

		if (snode.has_child("separation"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(snode["separation"]), "separation");
			if (v.has_value())
				sp.separation = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo.separation; ignoring it.", serial);
		}
		if (snode.has_child("convergence"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(snode["convergence"]), "convergence");
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
		applyCrossed(serial, "stereo", snode, sp.resolved);

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
					const std::optional<float> v = parseWholeFloat(nodeVal(sc["separation"]), "separation");
					if (v.has_value())
						rule.separation = v.value();
					else
						Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid stereo scene separation; inheriting base.", serial);
				}
				if (sc.has_child("convergence"))
				{
					const std::optional<float> v = parseWholeFloat(nodeVal(sc["convergence"]), "convergence");
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
						{
							applyCrossed(serial, "stereo scene", sc, sresolved);
							rule.map_override = sresolved;
						}
						else if (sc.has_child("crossed"))
							advise(serial, "stereo.crossed",
								fmt::format("(VR) ProfileDB: Serial '{}' has a stereo scene whose map was rejected; its "
											"'crossed' block is IGNORED with it.", serial));
					}
					else if (sc.has_child("crossed"))
					{
						advise(serial, "stereo.crossed",
							fmt::format("(VR) ProfileDB: Serial '{}' has a stereo scene with 'crossed' but no 'map:' of its "
										"own; IGNORED. Repeat the band set in the scene ('map: bands', 'splits', "
										"'bands') and put 'crossed' beside it.", serial));
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
		if (scr.has_child("lagMs"))
		{
			const std::string_view v = nodeVal(scr["lagMs"]);
			const std::optional<float> ms = parseWholeFloat(v, "lagMs");
			if (StringUtil::compareNoCase(v, "frameAge"))
			{
				if (node.has_child("projection") && node["projection"].is_map())
					out.screen_lag_ms = 0.0f;
				else
					Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.lagMs: frameAge but no projection block measuring it; ignoring it.", serial);
			}
			else if (ms.has_value() && std::isfinite(ms.value()) && ms.value() > 0.0f)
				out.screen_lag_ms = std::clamp(ms.value(), 0.0f, 250.0f);
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.lagMs '{}' (not a positive number of ms or frameAge); ignoring it.", serial, v);
			if (out.screen_lag_ms.has_value() && !out.screen_follow_head.value_or(false))
			{
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.lagMs without screen.follow: head; it only applies to a follow-head screen, ignoring it.", serial);
				out.screen_lag_ms.reset();
			}
		}
		if (scr.has_child("distance"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(scr["distance"]), "distance");
			if (v.has_value())
				out.screen_distance = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.distance; ignoring it.", serial);
		}
		if (scr.has_child("height"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(scr["height"]), "height");
			if (v.has_value())
				out.screen_height = v.value();
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.height; ignoring it.", serial);
		}
		if (scr.has_child("arc"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(scr["arc"]), "arc");
			if (v.has_value())
				out.screen_arc_deg = std::clamp(v.value(), 0.0f, 270.0f);
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.arc; ignoring it.", serial);
		}
		if (scr.has_child("curveFallback"))
		{
			const std::string_view v = nodeVal(scr["curveFallback"]);
			const std::optional<ScreenCurve::Fallback> f = ScreenCurve::ParseFallback(v);
			if (f.has_value())
				out.screen_curve_fallback = f;
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.curveFallback '{}' (not flat|facets|projector); ignoring it.", serial, v);
		}
		if (scr.has_child("forceFallback"))
		{
			const std::string_view v = nodeVal(scr["forceFallback"]);
			const std::optional<ScreenCurve::Force> f = ScreenCurve::ParseForce(v);
			if (f.has_value())
				out.screen_force_fallback = f;
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.forceFallback '{}' (not none|facets|projector); ignoring it.", serial, v);
		}
		if (scr.has_child("projectorSupersample"))
		{
			const std::optional<float> v = parseWholeFloat(nodeVal(scr["projectorSupersample"]), "projectorSupersample");
			if (v.has_value() && std::isfinite(v.value()))
				out.screen_projector_supersample = std::clamp(v.value(), 0.5f, 2.0f);
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has an invalid screen.projectorSupersample; ignoring it.", serial);
		}
		if (scr.has_child("projectorDepth"))
		{
			const std::string_view v = nodeVal(scr["projectorDepth"]);
			if (StringUtil::compareNoCase(v, "on") || StringUtil::compareNoCase(v, "true"))
				out.screen_projector_depth = true;
			else if (StringUtil::compareNoCase(v, "off") || StringUtil::compareNoCase(v, "false"))
				out.screen_projector_depth = false;
			else
				Console.WarningFmt("(VR) ProfileDB: Serial '{}' has screen.projectorDepth '{}' (not on|off); ignoring it.", serial, v);
		}
	}

	if (node.has_child("camera") && node["camera"].is_map())
	{
		if (node["camera"].has_child("fov"))
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' still has `camera.fov`. The game's field of view is a "
							 "TOP-LEVEL `fov:` block now, applies without head-look, switches by scene and restores "
							 "the game's value. Move the setting there.", serial);
			return false;
		}
		out.camera_class = readCameraClass(serial, node["camera"]);
		out.camera = parseCamera(serial, node["camera"]);
	}

	if (node.has_child("fov"))
	{
		if (!node["fov"].is_map())
		{
			Console.ErrorFmt("(VR) ProfileDB: Serial '{}' fov: must be a map (levers + scenes).", serial);
			return false;
		}
		FovProfile fov;
		if (!parseFov(serial, node["fov"], fov))
			return false;
		out.fov = std::move(fov);
	}

	if (node.has_child("projection"))
		out.projection = parseProjection(serial, node["projection"], out.camera);
	if (out.camera.has_value() && out.camera->requires_world_projection && !out.projection.has_value())
		advise(serial, "camera.requiresWorldProjection",
			fmt::format("(VR) ProfileDB: Serial '{}' camera block requires world projection but the profile has no "
						"usable `projection:` block: it never arms.", serial));

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

	if (node.has_child("hands"))
	{
		const std::string_view v = nodeVal(node["hands"]);
		VR::NearField::Hands h = VR::NearField::Hands::Off;
		if (VR::NearField::ParseHands(v, &h))
			out.hands = h;
		else
			advise(serial, "hands",
				fmt::format("(VR) ProfileDB: {} hands: '{}' is not off | glove-v1.1 — IGNORED, so this game draws no hands.",
					StringUtil::toUpper(serial), v));
	}

	if (node.has_child("gameHands"))
		out.game_hands = parseGameHands(serial, node["gameHands"]);

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

static bool loadVariantList(const std::string& path, const std::string& serial, ryml::NodeRef seq,
	std::vector<VR::ProfileDB::Profile>& out)
{
	const std::string shown = StringUtil::toUpper(serial);
	bool ok = true;
	size_t index = 0;
	for (ryml::NodeRef vn : seq.children())
	{
		const size_t n = ++index;
		if (!vn.is_map())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: variant {} of '{}' is not a map.", path, n, shown);
			s_load_issues.push_back({path, fmt::format("variant {} of '{}' is not a map", n, shown)});
			ok = false;
			continue;
		}
		VR::ProfileDB::Profile profile;
		if (!VR::ProfileDB::parseProfile(serial, vn, profile))
		{
			s_load_issues.push_back({path, fmt::format("variant {} of '{}' failed validation — details in the emulog above this summary", n, shown)});
			ok = false;
			continue;
		}
		if (vn.has_child("crcs") && profile.crcs.empty())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: variant {} of '{}' has an empty crcs list.", path, n, shown);
			s_load_issues.push_back({path, fmt::format("variant {} of '{}' has an empty crcs list, so it would match every disc — "
													   "list its discs' CRCs, or drop crcs: for the one any-disc variant", n, shown)});
			ok = false;
			continue;
		}
		out.push_back(std::move(profile));
	}

	if (index == 0)
	{
		Console.WarningFmt("(VR) ProfileDB: {}: '{}' is an empty variant list.", path, shown);
		s_load_issues.push_back({path, fmt::format("'{}' is an empty variant list", shown)});
		return false;
	}

	if (ok)
	{
		const size_t any_crc = std::count_if(out.begin(), out.end(), [](const VR::ProfileDB::Profile& p) { return p.crcs.empty(); });
		if (any_crc > 1)
		{
			Console.ErrorFmt("(VR) ProfileDB: {}: '{}' has {} variants without crcs; at most one may match any disc.", path, shown, any_crc);
			s_load_issues.push_back({path, fmt::format("'{}' has {} variants without crcs — at most one variant may omit crcs; "
													   "give the others the CRCs of the discs they are for",
											   shown, any_crc)});
			ok = false;
		}
		for (size_t i = 0; i < out.size(); i++)
		{
			for (size_t j = i + 1; j < out.size(); j++)
			{
				for (const u32 crc : out[i].crcs)
				{
					if (std::find(out[j].crcs.begin(), out[j].crcs.end(), crc) == out[j].crcs.end())
						continue;
					Console.ErrorFmt("(VR) ProfileDB: {}: CRC {:08X} is listed by variants {} and {} of '{}'.", path, crc, i + 1, j + 1, shown);
					s_load_issues.push_back({path, fmt::format("CRC {:08X} is listed by variants {} and {} of '{}' — a CRC may select only one variant",
													   crc, i + 1, j + 1, shown)});
					ok = false;
				}
			}
		}
	}

	if (!ok)
		out.clear();
	return ok;
}

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

		if (!n.is_map() && !n.is_seq())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: serial '{}' is not a map; skipping.", path, serial);
			s_load_issues.push_back({path, "serial '" + serial + "' is not a map (one profile) or a list (variants)"});
			continue;
		}

		std::vector<VR::ProfileDB::Profile> variants;
		if (n.is_map())
		{
			VR::ProfileDB::Profile profile;
			if (!VR::ProfileDB::parseProfile(serial, n, profile))
			{
				s_load_issues.push_back({path, "profile '" + serial + "' failed validation — details in the emulog above this summary"});
				continue;
			}
			variants.push_back(std::move(profile));
		}
		else if (!loadVariantList(path, serial, n, variants))
		{
			continue;
		}

		if (railsApplyTo(tier == LoadTier::User))
		{
			for (const VR::ProfileDB::Profile& profile : variants)
				railProfile(serial, profile);
		}
		for (VR::ProfileDB::Profile& profile : variants)
		{
			profile.source_path = path;
			profile.from_user_folder = (tier == LoadTier::User);
		}
		if (tier == LoadTier::User)
			user_serials.insert(serial);
		if (variants.size() > 1)
			Console.WriteLnFmt("(VR) ProfileDB: {} carries {} variants, chosen by disc CRC.", StringUtil::toUpper(serial), variants.size());
		s_profiles.emplace(std::move(serial), std::move(variants));
		loaded++;
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
		std::vector<VR::ModFiles::File> mod_files;
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

	sr.mod_files = VR::ModFiles::ScanAll();
	for (const VR::ModFiles::File& f : sr.mod_files)
		foldFile(sr.stamp, f.path, static_cast<std::time_t>(f.mtime), f.size);

	return sr;
}

static u32 loadConversions(const std::vector<VR::ModFiles::File>& files)
{
	u32 added = 0;
	for (const VR::ModFiles::File& f : files)
	{
		std::string why;
		const std::optional<VR::ModFiles::Name> name = VR::ModFiles::ParseName(f.path, &why);
		if (!name.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: {}", f.path, why);
			s_load_issues.push_back({f.path, why});
			continue;
		}
		const std::optional<std::string> buffer = FileSystem::ReadFileToString(f.path.c_str());
		if (!buffer.has_value())
		{
			s_load_issues.push_back({f.path, "file could not be read"});
			continue;
		}
		Error error;
		std::optional<ryml::Tree> tree =
			ParseYAMLFromString(ryml::to_csubstr(*buffer), ryml::to_csubstr(f.path), &error, true);
		if (!tree.has_value())
		{
			s_load_issues.push_back({f.path, error.GetDescription()});
			continue;
		}
		const std::optional<VR::ModFiles::Entry> entry = VR::ModFiles::CheckEntry(tree->crootref(), name.value(), &why);
		if (!entry.has_value())
		{
			Console.WarningFmt("(VR) ProfileDB: {}: {}", f.path, why);
			s_load_issues.push_back({f.path, why});
			continue;
		}
		if (entry->kind != VR::ModFiles::Kind::Conversion)
			continue;

		ryml::NodeRef top = tree->rootref().first_child();
		ryml::NodeRef e = entry->list_form ? top.first_child() : top;
		const std::string serial = StringUtil::toLower(name->serial);
		VR::ProfileDB::Profile profile;
		if (!VR::ProfileDB::parseProfile(serial, e, profile))
		{
			s_load_issues.push_back({f.path, "the conversion's profile failed validation — details in the emulog above this summary"});
			continue;
		}
		if (profile.crcs.empty())
		{
			s_load_issues.push_back({f.path, "a conversion needs its discs' crcs: without them it cannot be told from its base game"});
			continue;
		}
		const ryml::ConstNodeRef mod = e["mod"];
		VR::ProfileDB::ModIdentity id;
		id.name.assign(nodeVal(mod["name"]));
		id.version.assign(nodeVal(mod["version"]));
		id.by.assign(nodeVal(mod["by"]));
		if (mod.has_child("url"))
			id.url.assign(nodeVal(mod["url"]));
		id.file = f.path;
		profile.mod = std::move(id);
		profile.source_path = f.path;
		profile.from_user_folder = f.user;

		std::vector<VR::ProfileDB::Profile>& variants = s_profiles[serial];
		std::optional<u32> clash;
		for (const VR::ProfileDB::Profile& v : variants)
		{
			for (const u32 crc : profile.crcs)
			{
				if (std::find(v.crcs.begin(), v.crcs.end(), crc) != v.crcs.end())
					clash = crc;
			}
		}
		if (clash.has_value())
		{
			Console.ErrorFmt("(VR) ProfileDB: {}: CRC {:08X} is already listed for {}.", f.path, clash.value(), StringUtil::toUpper(serial));
			s_load_issues.push_back({f.path, fmt::format("CRC {:08X} is already listed by another variant of {} — a CRC may select only one",
												 clash.value(), StringUtil::toUpper(serial))});
			if (variants.empty())
				s_profiles.erase(serial);
			continue;
		}
		if (railsApplyTo(f.user))
			railProfile(serial, profile);
		Console.WriteLnFmt("(VR) ProfileDB: {}: the conversion '{}' v{} (by {}) is a variant chosen by {} CRC(s).",
			StringUtil::toUpper(serial), profile.mod->name, profile.mod->version, profile.mod->by, profile.crcs.size());
		variants.push_back(std::move(profile));
		added++;
	}
	return added;
}

static void loadScanned(const ScanResult& sr)
{
	Common::Timer timer;
	s_profiles.clear();
	s_load_issues.clear();
	s_rail_findings.clear();
	s_advisories.clear();
	s_bind_noticed.clear();
	s_bind_notices = 0;

	if (sr.shipped == ShippedSource::Legacy)
	{
		Console.Error("(VR) ProfileDB: ==================================================================");
		Console.ErrorFmt("(VR) ProfileDB: LEGACY single-file catalog in use — resources/{} is missing.", VRPROFILES_DIR_NAME);
		Console.Error("(VR) ProfileDB: This binary expects the per-game profile folder; the install or");
		Console.Error("(VR) ProfileDB: deployment is SKEWED (binary and resources from different");
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
		if (StringUtil::compareNoCase(Path::GetFileName(e.path), VRPROFILES_MODDB_FILE_NAME))
		{
			Console.WriteLnFmt("(VR) ProfileDB: {} is reserved for a later mod database and is not read by this build.", e.path);
			continue;
		}
		loadProfileFile(e.path, e.tier, user_serials);
		if (e.tier == LoadTier::User)
			user_files++;
		else
			shipped_files++;
	}

	const u32 conversions = loadConversions(VR::ModFiles::Scan(nullptr));

	s_stamp = sr.stamp;
	s_loaded = true;

	Console.WriteLnFmt(Color_StrongGreen,
		"(VR) ProfileDB: {} profile(s) on record from {} shipped + {} user file(s), {} conversion(s) from vr-mods "
		"(user files override shipped per game; loaded in {:.2f}ms).",
		s_profiles.size(), shipped_files, user_files, conversions, timer.GetTimeMilliseconds());
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


u32 VR::ProfileDB::BindNoticesLogged()
{
	return s_bind_notices;
}

static std::string crcList(const std::vector<u32>& crcs)
{
	if (crcs.empty())
		return "any CRC";
	std::string out;
	for (const u32 c : crcs)
		out += fmt::format("{}{:08X}", out.empty() ? "" : ", ", c);
	return out;
}

static std::string sourceOf(const VR::ProfileDB::Profile& p)
{
	const char* folder = p.from_user_folder ? "user" : "shipped";
	if (p.source_path.empty())
		return fmt::format("{} profile (built in memory)", folder);
	return fmt::format("{} {} {}", folder, p.mod.has_value() ? "vr-mods file" : "profile", p.source_path);
}

std::string VR::ProfileDB::DescribeBinding(std::string_view serial, u32 crc, const std::vector<Profile>& variants, const BindDecision& d)
{
	const std::string shown = StringUtil::toUpper(serial);
	const std::string disc = crc != 0 ? fmt::format("{} (CRC {:08X})", shown, crc) : fmt::format("{} (no CRC)", shown);
	switch (d.outcome)
	{
		case BindOutcome::NoProfile:
			return fmt::format("(VR) ProfileDB: {} has no VR profile.", disc);
		case BindOutcome::CrcMatch:
		case BindOutcome::OnlyVariant:
			return fmt::format("(VR) ProfileDB: {} uses the {}{}.", disc, sourceOf(variants[d.variant]),
				variants.size() > 1 ? fmt::format(" (variant {} of {})", d.variant + 1, variants.size()) : std::string());
		case BindOutcome::AnyCrcBase:
		{
			size_t specific = 0;
			for (const Profile& v : variants)
				specific += v.crcs.empty() ? 0 : 1;
			return fmt::format("(VR) ProfileDB: {} uses the {}{}.", disc, sourceOf(variants[d.variant]),
				specific > 0 ? fmt::format(", the any-CRC base: none of the {} CRC-specific variant(s) lists this CRC", specific) :
							   std::string());
		}
		case BindOutcome::CrcMiss:
		case BindOutcome::NeedsCrc:
		{
			std::string listed;
			for (const Profile& v : variants)
				listed += fmt::format(" {} lists {}.", v.source_path.empty() ? std::string("(in memory)") : v.source_path, crcList(v.crcs));
			if (d.outcome == BindOutcome::NeedsCrc)
				return fmt::format("(VR) ProfileDB: {}: no CRC to choose between {} variants, so no VR profile applies.{}",
					disc, variants.size(), listed);
			return fmt::format("(VR) ProfileDB: {}: no VR profile applies, because no variant lists this disc's CRC. "
							   "This is probably another build of the game; its addresses may differ, so check them on "
							   "this build before adding the CRC.{}",
				disc, listed);
		}
	}
	return {};
}

const VR::ProfileDB::Profile* VR::ProfileDB::Lookup(const std::string_view serial, u32 crc)
{
	EnsureLoaded();

	std::string key = StringUtil::toLower(serial);
	const auto it = s_profiles.find(key);
	if (it == s_profiles.end() || it->second.empty())
		return nullptr;

	const std::vector<Profile>& variants = it->second;
	const BindDecision d = DecideBinding(variants.size(), crc, [&variants](size_t i) {
		return std::pair<const std::vector<u32>&, bool>(variants[i].crcs, variants[i].mod.has_value());
	});

	if (s_bind_noticed.emplace(std::move(key), crc).second)
	{
		s_bind_notices++;
		const std::string line = DescribeBinding(serial, crc, variants, d);
		if (d.Bound())
			Console.WriteLn(line);
		else
			Console.Warning(line);
	}
	return d.Bound() ? &variants[d.variant] : nullptr;
}

std::optional<size_t> VR::ProfileDB::BoundSummaryRow(const std::vector<Summary>& rows, std::string_view serial, u32 crc)
{
	if (serial.empty())
		return std::nullopt;
	std::vector<size_t> mine;
	for (size_t i = 0; i < rows.size(); i++)
	{
		if (StringUtil::compareNoCase(rows[i].serial, serial))
			mine.push_back(i);
	}
#ifdef VR_PROFILEDB_PLANTED_SERIAL_RANK
	int best_rank = 0;
	std::optional<size_t> best;
	for (const size_t i : mine)
	{
		const std::vector<u32>& c = rows[i].crcs;
		const int rank = (crc != 0 && std::find(c.begin(), c.end(), crc) != c.end()) ? 3 : c.empty() ? 2 : 1;
		if (rank > best_rank)
		{
			best_rank = rank;
			best = i;
		}
	}
	return best;
#else
	const BindDecision d = DecideBinding(mine.size(), crc, [&rows, &mine](size_t i) {
		return std::pair<const std::vector<u32>&, bool>(rows[mine[i]].crcs, rows[mine[i]].conversion);
	});
	if (!d.Bound())
		return std::nullopt;
	return mine[d.variant];
#endif
}

std::vector<VR::ProfileDB::RequirementGap> VR::ProfileDB::FindRequirementGaps(const ProfileRequirements& req, bool extra_memory_on)
{
	std::vector<RequirementGap> gaps;
	if (req.extra_memory.value_or(false) && !extra_memory_on)
	{
		RequirementGap g;
		g.kind = RequirementGap::Kind::ExtraMemory;
		g.section = "EmuCore/CPU";
		g.key = "ExtraMemory";
		g.value = true;
		g.needs_restart = true;
		gaps.push_back(g);
	}
	return gaps;
}

static std::mutex s_offer_mutex;
static VR::ProfileDB::RequirementOfferHandler s_offer_handler;
static std::unordered_set<std::string> s_offered;

void VR::ProfileDB::SetRequirementOfferHandler(RequirementOfferHandler handler)
{
	std::lock_guard lock(s_offer_mutex);
	s_offer_handler = std::move(handler);
}

static const char* requirementLabel(VR::ProfileDB::RequirementGap::Kind kind)
{
	switch (kind)
	{
		case VR::ProfileDB::RequirementGap::Kind::ExtraMemory:
			return "Extended RAM";
	}
	return "a setting";
}

VR::ProfileDB::OfferOutcome VR::ProfileDB::OfferRequirements(
	const Profile* bound, u32 crc, bool extra_memory_on, const std::string& game_settings_path)
{
	if (!bound || !bound->requirements.Any())
		return OfferOutcome::Nothing;

	RequirementOffer offer;
	offer.gaps = FindRequirementGaps(bound->requirements, extra_memory_on);
	if (offer.gaps.empty())
		return OfferOutcome::Nothing;

	offer.serial = StringUtil::toUpper(bound->serial);
	offer.crc = crc;
	offer.name = bound->name;
	offer.game_settings_path = game_settings_path;

	RequirementOfferHandler handler;
	{
		std::lock_guard lock(s_offer_mutex);
		if (!s_offered.insert(fmt::format("{}:{:08X}", offer.serial, crc)).second)
			return OfferOutcome::AlreadyOffered;
		handler = s_offer_handler;
	}

	const std::string& shown = offer.name.empty() ? offer.serial : offer.name;
	for (const RequirementGap& g : offer.gaps)
	{
		Console.WarningFmt("(VR) ProfileDB: this disc is {} ({}, CRC {:08X}), which needs {} ([{}] {} = {}). "
						   "It is off, and nothing was changed.{}",
			shown, offer.serial, crc, requirementLabel(g.kind), g.section, g.key, g.value ? "true" : "false",
			handler ? " Asking whether to turn it on for this game." :
					  " To run it, turn it on in this game's own settings.");
	}

	if (!handler)
		return OfferOutcome::Logged;
	handler(offer);
	return OfferOutcome::Offered;
}

void VR::ProfileDB::Reset()
{
	std::lock_guard lock(s_load_mutex);
	s_profiles.clear();
	s_load_issues.clear();
	s_rail_findings.clear();
	s_advisories.clear();
	s_bind_noticed.clear();
	s_bind_notices = 0;
	s_stamp = LoadStamp{};
	s_loaded = false;
	std::lock_guard offer_lock(s_offer_mutex);
	s_offered.clear();
}

std::optional<std::string> VR::ProfileDB::ConversionTitle(const std::string_view serial, u32 crc)
{
	if (crc == 0)
		return std::nullopt;
	EnsureLoaded();
	std::lock_guard lock(s_load_mutex);
	const auto it = s_profiles.find(StringUtil::toLower(serial));
	if (it == s_profiles.end())
		return std::nullopt;
	for (const Profile& v : it->second)
	{
		if (v.mod.has_value() && std::find(v.crcs.begin(), v.crcs.end(), crc) != v.crcs.end())
			return fmt::format("{} (v{})", v.mod->name, v.mod->version);
	}
	return std::nullopt;
}

std::vector<VR::ProfileDB::Summary> VR::ProfileDB::ListProfiles()
{
	EnsureLoaded();

	std::lock_guard lock(s_load_mutex);

	std::vector<Summary> out;
	out.reserve(s_profiles.size());
	for (const auto& [serial, variants] : s_profiles)
	{
		for (const Profile& profile : variants)
		{
			Summary s;
			s.serial = StringUtil::toUpper(serial);
			s.name = profile.mod.has_value() ? profile.mod->name : profile.name;
			s.conversion = profile.mod.has_value();
			if (profile.mod.has_value())
				s.mod_version = profile.mod->version;
			s.crcs = profile.crcs;
			s.source_path = profile.source_path;
			s.from_user_folder = profile.from_user_folder;
			s.has_stereo = profile.stereo.has_value();
			s.has_camera = profile.camera.has_value();
			if (profile.stereo.has_value())
			{
				s.separation = profile.stereo->separation;
				s.convergence = profile.stereo->convergence;
			}
			out.push_back(std::move(s));
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const Summary& a, const Summary& b) { return a.serial < b.serial; });
	return out;
}


std::optional<u32> VR::ProfileDB::ResolveIndexedBase(const CameraBase& spec, u32 index, IndexedRead32 read32)
{
#ifndef VR_FORKBUGS_PLANTED_INDEXED
	if (index >= spec.count)
		return std::nullopt;
#endif
	const s64 element = static_cast<s64>(spec.indexed_base) + spec.array_offset +
						static_cast<s64>(index) * static_cast<s64>(spec.stride);
	if (element < 0 || element >= static_cast<s64>(EE_MAIN_RAM_BYTES))
		return std::nullopt;
#ifndef VR_FORKBUGS_PLANTED_INDEXED
	if (spec.has_validate)
	{
		const s64 vaddr = element + spec.validate_offset;
		u32 v = 0;
		if (vaddr < 0 || vaddr + 4 > static_cast<s64>(EE_MAIN_RAM_BYTES) || !read32 ||
			!read32(static_cast<u32>(vaddr), &v) || v != spec.validate_equals)
			return std::nullopt;
	}
#endif
	return static_cast<u32>(element);
}
