// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VrUiPanel.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <Sha256.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string_view>

namespace VR::VrUiPanel
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		using Value = rapidjson::Value;

		bool Refuse(Refusal& why, const char* code, std::string where, std::string detail)
		{
			why.code = code;
			why.where = std::move(where);
			why.detail = std::move(detail);
			return false;
		}

		const Value* Member(const Value& obj, const char* key)
		{
			if (!obj.IsObject())
				return nullptr;
			const Value::ConstMemberIterator it = obj.FindMember(key);
			return it == obj.MemberEnd() ? nullptr : &it->value;
		}

		bool Str(const Value& obj, const char* key, std::string& out)
		{
			const Value* v = Member(obj, key);
			if (!v || !v->IsString())
				return false;
			out.assign(v->GetString(), v->GetStringLength());
			return true;
		}

		bool IntVal(const Value& v, std::int64_t lo, std::int64_t hi, std::int64_t& out)
		{
			if (v.IsInt64())
			{
				const std::int64_t x = v.GetInt64();
				if (x < lo || x > hi)
					return false;
				out = x;
				return true;
			}
			if (v.IsDouble())
			{
				const double d = v.GetDouble();
				if (!std::isfinite(d) || d != std::floor(d) || d < static_cast<double>(lo) || d > static_cast<double>(hi))
					return false;
				out = static_cast<std::int64_t>(d);
				return true;
			}
			return false;
		}

		bool Int(const Value& obj, const char* key, std::int64_t lo, std::int64_t hi, std::int64_t& out)
		{
			const Value* v = Member(obj, key);
			return v && IntVal(*v, lo, hi, out);
		}

		bool Ints(const Value& obj, const char* key, std::int64_t lo, std::int64_t hi, std::int64_t* out, std::size_t count)
		{
			const Value* v = Member(obj, key);
			if (!v || !v->IsArray() || v->Size() != count)
				return false;
			for (rapidjson::SizeType i = 0; i < count; i++)
			{
				if (!IntVal((*v)[i], lo, hi, out[i]))
					return false;
			}
			return true;
		}

		bool NumVal(const Value& v, double lo, double hi, float& out)
		{
			if (!v.IsNumber())
				return false;
			const double d = v.GetDouble();
			if (!std::isfinite(d) || d < lo || d > hi)
				return false;
			out = static_cast<float>(d);
			return true;
		}

		bool Num(const Value& obj, const char* key, double lo, double hi, float& out)
		{
			const Value* v = Member(obj, key);
			return v && NumVal(*v, lo, hi, out);
		}

		bool Nums(const Value& obj, const char* key, double lo, double hi, float* out, std::size_t count)
		{
			const Value* v = Member(obj, key);
			if (!v || !v->IsArray() || v->Size() != count)
				return false;
			for (rapidjson::SizeType i = 0; i < count; i++)
			{
				if (!NumVal((*v)[i], lo, hi, out[i]))
					return false;
			}
			return true;
		}

		bool FindDuplicateKey(const Value& root, std::string& key)
		{
			std::vector<const Value*> stack{&root};
			std::vector<std::string_view> names;
			while (!stack.empty())
			{
				const Value* v = stack.back();
				stack.pop_back();
				if (v->IsObject())
				{
					names.clear();
					for (Value::ConstMemberIterator m = v->MemberBegin(); m != v->MemberEnd(); ++m)
					{
						names.emplace_back(m->name.GetString(), m->name.GetStringLength());
						stack.push_back(&m->value);
					}
					std::sort(names.begin(), names.end());
					for (std::size_t i = 1; i < names.size(); i++)
					{
						if (names[i] == names[i - 1])
						{
							key.assign(names[i]);
							return true;
						}
					}
				}
				else if (v->IsArray())
				{
					for (Value::ConstValueIterator e = v->Begin(); e != v->End(); ++e)
						stack.push_back(&*e);
				}
			}
			return false;
		}

		int VersionShape(const std::string& v)
		{
			std::size_t parts = 0, digits = 0;
			for (const char ch : v)
			{
				if (ch == '.')
				{
					if (digits == 0)
						return 0;
					parts++;
					digits = 0;
				}
				else if (ch >= '0' && ch <= '9')
				{
					digits++;
				}
				else
				{
					return 0;
				}
			}
			if (parts != 2 || digits == 0)
				return 0;
			const std::string_view major = std::string_view(v).substr(0, v.find('.'));
			if (major == "1")
				return 1;
			return major[0] == '0' ? 0 : 2;
		}

		bool IsCellId(const std::string& s)
		{
			if (s.empty() || s.size() > 64)
				return false;
			for (std::size_t i = 0; i < s.size(); i++)
			{
				const char ch = s[i];
				const bool alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
				if (!alpha && !(i > 0 && ch >= '0' && ch <= '9'))
					return false;
			}
			return true;
		}

		bool IsHex64(const std::string& s)
		{
			if (s.size() != 64)
				return false;
			for (const char ch : s)
			{
				if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
					return false;
			}
			return true;
		}

		bool IsPlainPngName(const std::string& s)
		{
			if (s.empty() || s == "." || s == ".." || s.find_first_of("/\\:") != std::string::npos)
				return false;
			return s.size() > 4 && StringUtil::compareNoCase(std::string_view(s).substr(s.size() - 4), ".png");
		}

		bool ParsePng(const Value& root, const char* key, std::string& file, std::uint32_t& w, std::uint32_t& h,
			std::string* sha, std::string& pixels_sha, Refusal& why)
		{
			const Value* e = Member(root, key);
			if (!e || !e->IsObject())
				return Refuse(why, "SCHEMA", fmt::format("manifest.{}", key), "missing or not an object");
			std::string format;
			std::string file_sha;
			if (!Str(*e, "file", file) || !Str(*e, "format", format) || !Member(*e, "size_px") ||
				!Str(*e, "sha256", file_sha) || !Str(*e, "pixels_sha256", pixels_sha))
				return Refuse(why, "SCHEMA", fmt::format("manifest.{}", key), "needs file, format, size_px, sha256, pixels_sha256");
			std::int64_t size[2] = {};
			if (!Ints(*e, "size_px", 1, kMaxPx, size, 2))
				return Refuse(why, "SCHEMA", fmt::format("manifest.{}.size_px", key), fmt::format("two integers in 1..{}", kMaxPx));
			if (!IsPlainPngName(file))
				return Refuse(why, "ATLAS", fmt::format("manifest.{}.file", key), fmt::format("'{}' is not a .png beside the manifest", file));
			if (format != "rgba8-straight")
				return Refuse(why, "ATLAS", fmt::format("manifest.{}.format", key), fmt::format("'{}' (this core reads rgba8-straight)", format));
			if (!IsHex64(file_sha) || !IsHex64(pixels_sha))
				return Refuse(why, "SCHEMA", fmt::format("manifest.{}", key), "sha256 fields are 64 lower-case hex digits");
			w = static_cast<std::uint32_t>(size[0]);
			h = static_cast<std::uint32_t>(size[1]);
			if (sha)
				*sha = file_sha;
			return true;
		}

		bool SpriteRef(const Value& sprites, const char* role, std::size_t count, std::int32_t& out)
		{
			std::int64_t v = -1;
			if (!Int(sprites, role, 0, INT64_MAX, v) || static_cast<std::uint64_t>(v) >= count)
				return false;
			out = static_cast<std::int32_t>(v);
			return true;
		}

		struct PngHeader
		{
			std::uint32_t width = 0, height = 0;
			std::uint8_t bit_depth = 0, colour_type = 0, interlace = 0;
		};
		bool ReadPngHeader(const std::uint8_t* d, std::size_t n, PngHeader& h)
		{
			static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
			if (n < 33 || std::memcmp(d, kSignature, 8) != 0)
				return false;
			const auto be32 = [d](std::size_t at) {
				return (static_cast<std::uint32_t>(d[at]) << 24) | (static_cast<std::uint32_t>(d[at + 1]) << 16) |
					   (static_cast<std::uint32_t>(d[at + 2]) << 8) | static_cast<std::uint32_t>(d[at + 3]);
			};
			if (be32(8) != 13 || std::memcmp(d + 12, "IHDR", 4) != 0)
				return false;
			h.width = be32(16);
			h.height = be32(20);
			h.bit_depth = d[24];
			h.colour_type = d[25];
			h.interlace = d[28];
			return true;
		}

		enum class ReadResult
		{
			Ok,
			Unreadable,
			TooLarge,
		};
		ReadResult ReadBounded(const std::string& path, std::uint64_t limit, std::vector<std::uint8_t>& out, s64& size)
		{
			FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
			size = fp ? FileSystem::FSize64(fp.get()) : -1;
			if (size < 0)
				return ReadResult::Unreadable;
			if (static_cast<std::uint64_t>(size) > limit)
				return ReadResult::TooLarge;
			out.resize(static_cast<std::size_t>(size));
			if (std::fseek(fp.get(), 0, SEEK_SET) != 0 ||
				(size > 0 && std::fread(out.data(), 1, out.size(), fp.get()) != out.size()))
				return ReadResult::Unreadable;
			return ReadResult::Ok;
		}

		void Over(std::uint8_t* dst, std::uint32_t dw, std::int32_t x0, std::int32_t y0, const std::uint8_t* src,
			std::uint32_t sw, std::int32_t sx, std::int32_t sy, std::int32_t w, std::int32_t h)
		{
			for (std::int32_t row = 0; row < h; row++)
			{
				std::uint8_t* d = dst + (static_cast<std::size_t>(y0 + row) * dw + static_cast<std::size_t>(x0)) * 4;
				const std::uint8_t* s = src + (static_cast<std::size_t>(sy + row) * sw + static_cast<std::size_t>(sx)) * 4;
				for (std::int32_t col = 0; col < w; col++, d += 4, s += 4)
				{
					const std::uint32_t sa = s[3];
					if (sa == 255)
					{
						std::memcpy(d, s, 4);
					}
					else if (sa != 0)
					{
						const std::uint32_t da = d[3];
						const std::uint32_t inv = (255u - sa) * da;
						const std::uint32_t oa255 = sa * 255u + inv;
						for (int k = 0; k < 3; k++)
							d[k] = static_cast<std::uint8_t>((s[k] * sa * 255u + d[k] * inv + oa255 / 2u) / oa255);
						d[3] = static_cast<std::uint8_t>((oa255 + 127u) / 255u);
					}
				}
			}
		}
	}

	std::string Sha256Hex(const void* data, std::size_t size)
	{
		CSha256 ctx;
		Sha256_Init(&ctx);
		Sha256_Update(&ctx, static_cast<const Byte*>(data), size);
		Byte digest[SHA256_DIGEST_SIZE];
		Sha256_Final(&ctx, digest);
		static constexpr char kHex[] = "0123456789abcdef";
		std::string out(SHA256_DIGEST_SIZE * 2, '0');
		for (std::size_t i = 0; i < SHA256_DIGEST_SIZE; i++)
		{
			out[i * 2] = kHex[digest[i] >> 4];
			out[i * 2 + 1] = kHex[digest[i] & 15];
		}
		return out;
	}

	bool ParseManifest(std::string_view json, Artifact& out, Refusal& why)
	{
		out = Artifact{};
		why = Refusal{};
		if (json.size() > kMaxManifestBytes)
			return Refuse(why, "SIZE", "manifest", fmt::format("{} bytes (limit {})", json.size(), kMaxManifestBytes));
		if (json.find('\0') != std::string_view::npos)
			return Refuse(why, "SCHEMA", "manifest", fmt::format("not JSON: a NUL byte at offset {}", json.find('\0')));
		rapidjson::Document doc;
		doc.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
		if (doc.HasParseError())
			return Refuse(why, "SCHEMA", "manifest",
				fmt::format("not JSON: {} (offset {})", rapidjson::GetParseError_En(doc.GetParseError()), doc.GetErrorOffset()));
		const Value& root = doc;
		if (!root.IsObject())
			return Refuse(why, "SCHEMA", "manifest", "the top level is not an object");
		if (std::string dup; FindDuplicateKey(root, dup))
			return Refuse(why, "SCHEMA", "manifest", fmt::format("the key '{}' appears twice in one object", dup));

		std::string format, version, kind;
		if (!Str(root, "format", format) || format != "vrui")
			return Refuse(why, "SCHEMA", "manifest.format", "not a vrui artifact");
		if (!Str(root, "version", version))
			return Refuse(why, "SCHEMA", "manifest.version", "missing or not a string");
		switch (VersionShape(version))
		{
			case 1:
				break;
			case 2:
				return Refuse(why, "VERSION", "manifest.version", fmt::format("'{}': this core reads vrui/1 only", version));
			default:
				return Refuse(why, "SCHEMA", "manifest.version", fmt::format("'{}' is not 1.MINOR.PATCH", version));
		}
		if (!Str(root, "kind", kind) || (kind != "panel" && kind != "card"))
			return Refuse(why, "SCHEMA", "manifest.kind", "panel or card");

		std::int64_t raster[2] = {};
		if (!Ints(root, "raster_px", 1, kMaxPx, raster, 2))
			return Refuse(why, "SCHEMA", "manifest.raster_px", fmt::format("two integers in 1..{}", kMaxPx));
		out.raster_w = static_cast<std::uint32_t>(raster[0]);
		out.raster_h = static_cast<std::uint32_t>(raster[1]);

		if (!ParsePng(root, "atlas", out.atlas_file, out.atlas_w, out.atlas_h, &out.atlas_sha256, out.atlas_pixels_sha256, why))
			return false;
		std::string preview_sha;
		if (!ParsePng(root, "preview", out.preview_file, out.preview_w, out.preview_h, &preview_sha,
				out.preview_pixels_sha256, why))
			return false;
		if (out.preview_w != out.raster_w || out.preview_h != out.raster_h)
			return Refuse(why, "ATLAS", "manifest.preview.size_px", "the preview is not the raster's size");

		const Value* sprites = Member(root, "sprites");
		if (!sprites || !sprites->IsArray() || sprites->Empty())
			return Refuse(why, "SCHEMA", "manifest.sprites", "a non-empty array");
		for (rapidjson::SizeType i = 0; i < sprites->Size(); i++)
		{
			const Value& s = (*sprites)[i];
			const std::string where = fmt::format("manifest.sprites[{}]", i);
			std::int64_t id = -1, r[4] = {};
			if (!s.IsObject() || !Int(s, "id", 0, INT64_MAX, id) || !Member(s, "rect"))
				return Refuse(why, "SCHEMA", where, "needs id and rect");
			if (!Ints(s, "rect", 0, kMaxPx, r, 4))
				return Refuse(why, "SCHEMA", where + ".rect", fmt::format("four integers in 0..{}", kMaxPx));
			if (id != static_cast<std::int64_t>(i))
				return Refuse(why, "ATLAS", where + ".id", "sprite ids are their index");
			if (r[2] < 1 || r[3] < 1 || r[0] + r[2] > out.atlas_w || r[1] + r[3] > out.atlas_h)
				return Refuse(why, "ATLAS", where + ".rect",
					fmt::format("[{}, {}, {}, {}] is outside the {}x{} atlas", r[0], r[1], r[2], r[3], out.atlas_w, out.atlas_h));
			out.sprites.push_back({static_cast<std::int32_t>(r[0]), static_cast<std::int32_t>(r[1]),
				static_cast<std::int32_t>(r[2]), static_cast<std::int32_t>(r[3])});
			if (i == 0)
			{
				std::string role;
				if (!Str(s, "role", role) || role != "background" || r[2] != raster[0] || r[3] != raster[1])
					return Refuse(why, "ATLAS", where, fmt::format("sprite 0 must be the {}x{} background", raster[0], raster[1]));
			}
		}

		const Value* cells = Member(root, "cells");
		if (!cells || !cells->IsArray())
			return Refuse(why, "SCHEMA", "manifest.cells", "an array");
		std::set<std::string> ids;
		for (rapidjson::SizeType i = 0; i < cells->Size(); i++)
		{
			const Value& c = (*cells)[i];
			const std::string where = fmt::format("manifest.cells[{}]", i);
			Cell cell;
			std::string cls;
			std::int64_t r[4] = {};
			const Value* csprites = Member(c, "sprites");
			if (!c.IsObject() || !Str(c, "id", cell.id) || !Str(c, "class", cls) || !Member(c, "rect_px") || !csprites ||
				!csprites->IsObject())
				return Refuse(why, "SCHEMA", where, "needs id, class, rect_px and sprites");
			if (!IsCellId(cell.id))
				return Refuse(why, "SCHEMA", where + ".id", fmt::format("'{}' is not an id ([A-Za-z_][A-Za-z0-9_]{{0,63}})", cell.id));
			if (!Ints(c, "rect_px", 0, kMaxPx, r, 4))
				return Refuse(why, "SCHEMA", where + ".rect_px", fmt::format("four integers in 0..{}", kMaxPx));
			if (!ids.insert(cell.id).second)
				return Refuse(why, "NAME", where + ".id", fmt::format("cell id '{}' is used twice", cell.id));
			if (r[2] < 1 || r[3] < 1 || r[0] + r[2] > raster[0] || r[1] + r[3] > raster[1])
				return Refuse(why, "ATLAS", where + ".rect_px",
					fmt::format("[{}, {}, {}, {}] is outside the {}x{} raster", r[0], r[1], r[2], r[3], raster[0], raster[1]));
			cell.x = static_cast<std::int32_t>(r[0]);
			cell.y = static_cast<std::int32_t>(r[1]);
			cell.w = static_cast<std::int32_t>(r[2]);
			cell.h = static_cast<std::int32_t>(r[3]);
			const std::size_t n = out.sprites.size();
			std::vector<std::int32_t> refs;
			if (cls == "lamp")
			{
				cell.cls = CellClass::Lamp;
				if (!SpriteRef(*csprites, "on", n, cell.sprite_on) || !SpriteRef(*csprites, "off", n, cell.sprite_off))
					return Refuse(why, "ATLAS", where + ".sprites", "a lamp needs on and off sprites that exist");
				refs = {cell.sprite_on, cell.sprite_off};
				const Value* d = Member(c, "default");
				if (!d || !d->IsBool())
					return Refuse(why, "SCHEMA", where + ".default", "a lamp's default is true or false");
				cell.default_on = d->GetBool();
			}
			else if (cls == "button")
			{
				cell.cls = CellClass::Button;
				if (!SpriteRef(*csprites, "idle", n, cell.sprite_idle) || !SpriteRef(*csprites, "hover", n, cell.sprite_hover) ||
					!SpriteRef(*csprites, "pressed", n, cell.sprite_pressed))
					return Refuse(why, "ATLAS", where + ".sprites", "a button needs idle, hover and pressed sprites that exist");
				refs = {cell.sprite_idle, cell.sprite_hover, cell.sprite_pressed};
			}
			else if (cls == "meter" || cls == "text")
			{
				return Refuse(why, "CLASS", where + ".class", fmt::format("'{}' cells are not drawn by this core yet (lamp, button)", cls));
			}
			else
			{
				return Refuse(why, "CLASS", where + ".class", fmt::format("unknown cell class '{}'", cls));
			}
			for (const std::int32_t ref : refs)
			{
				const Sprite& sp = out.sprites[static_cast<std::size_t>(ref)];
				if (sp.w < cell.w || sp.h < cell.h)
					return Refuse(why, "ATLAS", where + ".sprites", fmt::format("sprite {} is smaller than its cell", ref));
			}
			const Value* bind = Member(c, "bind");
			if (!bind || !(bind->IsNull() || (bind->IsString() && bind->GetStringLength() <= 128)))
				return Refuse(why, "SCHEMA", where + ".bind", "a string of at most 128 characters, or null");
			if (bind->IsString())
				cell.bind.assign(bind->GetString(), bind->GetStringLength());
			out.cells.push_back(std::move(cell));
		}

		const Value* pl = Member(root, "placement");
		if (!pl || !pl->IsObject())
			return Refuse(why, "SCHEMA", "manifest.placement", "missing or not an object");
		std::string anchor, show_when, layer;
		Placement& p = out.placement;
		if (!Str(*pl, "anchor", anchor) || !Member(*pl, "offset_m") || !Member(*pl, "yaw_deg") || !Member(*pl, "width_m") ||
			!Member(*pl, "height_m") || !Str(*pl, "show_when", show_when) || !Str(*pl, "layer", layer))
			return Refuse(why, "SCHEMA", "manifest.placement", "needs anchor, offset_m, yaw_deg, width_m, height_m, layer, show_when");
		if (!Nums(*pl, "offset_m", -5.0, 5.0, p.offset_m, 3))
			return Refuse(why, "SCHEMA", "manifest.placement.offset_m", "three numbers, each in [-5, 5]");
		if (!Num(*pl, "yaw_deg", -180.0, 180.0, p.yaw_deg))
			return Refuse(why, "SCHEMA", "manifest.placement.yaw_deg", "a number in [-180, 180]");
		if (!Num(*pl, "width_m", 0.0, 4.0, p.width_m) || !Num(*pl, "height_m", 0.0, 4.0, p.height_m) || p.width_m <= 0.0f ||
			p.height_m <= 0.0f)
			return Refuse(why, "SCHEMA", "manifest.placement", "width_m and height_m are in (0, 4]");
		if (anchor == "screen")
			p.anchor = Anchor::Screen;
		else if (anchor == "world")
			p.anchor = Anchor::World;
		else
			return Refuse(why, "ANCHOR", "manifest.placement.anchor", fmt::format("'{}' is not placed by this core yet (screen, world)", anchor));
		if (show_when != "always")
			return Refuse(why, "SHOW", "manifest.placement.show_when", fmt::format("'{}': this core shows 'always' panels only", show_when));
		if (layer != "panel" && layer != "card")
			return Refuse(why, "SCHEMA", "manifest.placement.layer", "panel or card");

		const Value* res = Member(root, "resources");
		std::int64_t layers = 0;
		if (!res || !Int(*res, "layers", 1, 2, layers))
			return Refuse(why, "SCHEMA", "manifest.resources.layers", "an integer in 1..2");
		if (layers > static_cast<std::int64_t>(kMaxLayers))
			return Refuse(why, "LAYERS", "manifest.resources.layers",
				fmt::format("{} layers; this core reserves {} for a vrui panel", layers, kMaxLayers));
		out.layers = static_cast<std::uint32_t>(layers);
		return true;
	}

	bool LoadFromBytes(std::string_view manifest_json, const std::vector<std::uint8_t>& atlas_png, Artifact& out, Refusal& why)
	{
		if (!ParseManifest(manifest_json, out, why))
			return false;
		if (atlas_png.size() > kMaxAtlasFileBytes)
			return Refuse(why, "ATLAS", "manifest.atlas.file",
				fmt::format("{} is {} bytes (a vrui/1 atlas is at most {})", out.atlas_file, atlas_png.size(), kMaxAtlasFileBytes));
		PngHeader hd;
		if (!ReadPngHeader(atlas_png.data(), atlas_png.size(), hd))
			return Refuse(why, "ATLAS", "manifest.atlas", fmt::format("{} is not a PNG (no signature and IHDR)", out.atlas_file));
		if (hd.width != out.atlas_w || hd.height != out.atlas_h)
			return Refuse(why, "ATLAS", "manifest.atlas.size_px",
				fmt::format("{}'s PNG header is {}x{}; size_px is {}x{}", out.atlas_file, hd.width, hd.height, out.atlas_w, out.atlas_h));
		if (hd.bit_depth != 8 || hd.colour_type != 6 || hd.interlace != 0)
			return Refuse(why, "ATLAS", "manifest.atlas.format",
				fmt::format("{} is not 8-bit RGBA, non-interlaced (bit depth {}, colour type {}, interlace {})", out.atlas_file,
					hd.bit_depth, hd.colour_type, hd.interlace));
		if (Sha256Hex(atlas_png.data(), atlas_png.size()) != out.atlas_sha256)
			return Refuse(why, "ATLAS", "manifest.atlas.sha256", fmt::format("{} does not match its sha256", out.atlas_file));
		RGBA8Image img;
		if (!img.LoadFromBuffer(out.atlas_file.c_str(), atlas_png.data(), atlas_png.size()))
			return Refuse(why, "ATLAS", "manifest.atlas", fmt::format("{} is not a readable PNG", out.atlas_file));
		if (img.GetWidth() != out.atlas_w || img.GetHeight() != out.atlas_h)
			return Refuse(why, "ATLAS", "manifest.atlas.size_px",
				fmt::format("{} decoded as {}x{}, the manifest says {}x{}", out.atlas_file, img.GetWidth(), img.GetHeight(), out.atlas_w, out.atlas_h));
		const std::size_t bytes = static_cast<std::size_t>(out.atlas_w) * out.atlas_h * 4;
		out.atlas.resize(bytes);
		std::memcpy(out.atlas.data(), img.GetPixels(), bytes);
		if (Sha256Hex(out.atlas.data(), out.atlas.size()) != out.atlas_pixels_sha256)
			return Refuse(why, "ATLAS", "manifest.atlas.pixels_sha256", fmt::format("{} pixels do not match", out.atlas_file));
		return true;
	}

	bool LoadDir(const std::string& dir, Artifact& out, Refusal& why)
	{
		out = Artifact{};
		const std::string man_path = Path::Combine(dir, "manifest.json");
		std::vector<std::uint8_t> man_bytes;
		s64 man_size = -1;
		switch (ReadBounded(man_path, kMaxManifestBytes, man_bytes, man_size))
		{
			case ReadResult::Ok:
				break;
			case ReadResult::TooLarge:
				return Refuse(why, "SIZE", "manifest", fmt::format("{} bytes (limit {})", man_size, kMaxManifestBytes));
			default:
				return Refuse(why, "ATLAS", "manifest", fmt::format("cannot read {}", man_path));
		}
		const std::string_view man(reinterpret_cast<const char*>(man_bytes.data()), man_bytes.size());
		Artifact probe;
		if (!ParseManifest(man, probe, why))
			return false;
		const std::string atlas_path = Path::Combine(dir, probe.atlas_file);
		std::vector<std::uint8_t> atlas;
		s64 atlas_size = -1;
		switch (ReadBounded(atlas_path, kMaxAtlasFileBytes, atlas, atlas_size))
		{
			case ReadResult::Ok:
				break;
			case ReadResult::TooLarge:
				return Refuse(why, "ATLAS", "manifest.atlas.file",
					fmt::format("{} is {} bytes (a vrui/1 atlas is at most {})", probe.atlas_file, atlas_size, kMaxAtlasFileBytes));
			default:
				return Refuse(why, "ATLAS", "manifest.atlas.file", fmt::format("cannot read {}", atlas_path));
		}
		return LoadFromBytes(man, atlas, out, why);
	}

	void Compose(const Artifact& a, const State& state, std::vector<std::uint8_t>& out)
	{
		const std::uint32_t W = a.raster_w, H = a.raster_h;
		out.assign(static_cast<std::size_t>(W) * H * 4, 0);
		if (a.atlas.empty() || a.sprites.empty())
			return;
		const Sprite& bg = a.sprites[0];
		for (std::uint32_t row = 0; row < H; row++)
		{
			std::memcpy(out.data() + static_cast<std::size_t>(row) * W * 4,
				a.atlas.data() + (static_cast<std::size_t>(bg.y + static_cast<std::int32_t>(row)) * a.atlas_w + static_cast<std::size_t>(bg.x)) * 4,
				static_cast<std::size_t>(W) * 4);
		}
		for (const Cell& c : a.cells)
		{
			const std::string* v = nullptr;
			for (const auto& kv : state)
			{
				if (kv.first == c.id)
					v = &kv.second;
			}
			std::int32_t sprite = -1;
			if (c.cls == CellClass::Lamp)
			{
				const bool on = v ? (*v == "true") : c.default_on;
				sprite = on ? c.sprite_on : c.sprite_off;
			}
			else
			{
				sprite = (v && *v == "hover") ? c.sprite_hover : (v && *v == "pressed") ? c.sprite_pressed : c.sprite_idle;
			}
			const Sprite& s = a.sprites[static_cast<std::size_t>(sprite)];
			Over(out.data(), W, c.x, c.y, a.atlas.data(), a.atlas_w, s.x, s.y, c.w, c.h);
		}
	}

	Diff MaxDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
	{
		Diff d;
		if (a.size() != b.size())
		{
			d.max_channel = 256;
			d.pixels = std::max(a.size(), b.size()) / 4;
			return d;
		}
		for (std::size_t i = 0; i + 3 < a.size(); i += 4)
		{
			std::uint32_t worst = 0;
			for (int k = 0; k < 4; k++)
				worst = std::max<std::uint32_t>(worst, static_cast<std::uint32_t>(std::abs(int(a[i + k]) - int(b[i + k]))));
			if (worst)
			{
				d.pixels++;
				d.max_channel = std::max(d.max_channel, worst);
			}
		}
		return d;
	}

	QuadPose Place(const Placement& p, const ScreenAnchor& anchor)
	{
		QuadPose q;
		q.width_m = p.width_m;
		q.height_m = p.height_m;
		const float yaw_offset = p.yaw_deg * (kPi / 180.0f);
		float yaw = yaw_offset;
		const float ox = p.offset_m[0], oy = p.offset_m[1], oz = p.offset_m[2];
		if (p.anchor == Anchor::Screen)
		{
			const float cy = std::cos(anchor.yaw), sy = std::sin(anchor.yaw);
			q.x = anchor.x + ox * cy + oz * sy;
			q.y = anchor.y + oy;
			q.z = anchor.z - ox * sy + oz * cy;
			yaw += anchor.yaw;
		}
		else
		{
			q.x = ox;
			q.y = oy;
			q.z = oz;
		}
		q.qx = 0.0f;
		q.qy = std::sin(yaw * 0.5f);
		q.qz = 0.0f;
		q.qw = std::cos(yaw * 0.5f);
		return q;
	}

	std::uint32_t LayersFor(const Artifact* loaded)
	{
#ifdef VRUI_PLANTED_NOLAYER
		(void)loaded;
		return 0;
#else
		return loaded ? loaded->layers : 0;
#endif
	}
}

#ifdef ENABLE_VULKAN

#include "VR/VRInternal.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

namespace VR::VrUiPanel::Runtime
{
	namespace
	{
		constexpr std::uint64_t kOneSecondNs = 1000000000ull;

		struct RuntimeState
		{
			bool selected = false;
			std::unique_ptr<Artifact> art;
			std::vector<std::uint8_t> pixels;
			std::string dump_dir;

			bool failed = false;
			XrSwapchain swapchain = XR_NULL_HANDLE;
			std::vector<XrSwapchainImageVulkan2KHR> images;
			bool ever_released = false;
			bool wait_pending = false;
			SwapchainWait::FailureTracker wait_failures;
			std::uint32_t pending_index = 0;
			std::vector<std::uint8_t> readback;
			bool readback_valid = false;
			int layers_logged = -1;

			std::uint32_t dump_count = 0;
			std::chrono::steady_clock::time_point dump_last{};
		};
		RuntimeState g;

		bool Fail(const char* what, int code)
		{
			if (!g.failed)
				Console.Error("(VR) VrUi: %s failed (%d); no vrui panel this session.", what, code);
			g.failed = true;
			return false;
		}

		void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
			VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
		{
			VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.srcAccessMask = src_access;
			b.dstAccessMask = dst_access;
			b.oldLayout = from;
			b.newLayout = to;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = image;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
		}

		bool Upload(const Internal::VulkanHandles& h, VkImage dst, bool swap_red_blue)
		{
			const std::uint32_t W = g.art->raster_w, H = g.art->raster_h;
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(W) * H * 4u;
			const bool readback = !g.dump_dir.empty();
			std::vector<std::uint8_t> px = g.pixels;
			if (swap_red_blue)
			{
				for (std::size_t i = 0; i + 3 < px.size(); i += 4)
					std::swap(px[i], px[i + 2]);
			}
			VkBuffer buf = VK_NULL_HANDLE;
			VkDeviceMemory mem = VK_NULL_HANDLE;
			void* map = nullptr;
			VkCommandPool pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			bool submitted = false, done = false;
			do
			{
				VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
				bci.size = readback ? bytes * 2u : bytes;
				bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | (readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0u);
				bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				if (vkCreateBuffer(h.device, &bci, nullptr, &buf) != VK_SUCCESS)
				{
					buf = VK_NULL_HANDLE;
					break;
				}
				VkMemoryRequirements mr = {};
				vkGetBufferMemoryRequirements(h.device, buf, &mr);
				VkPhysicalDeviceMemoryProperties mp = {};
				vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
				constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				std::uint32_t type = UINT32_MAX;
				for (std::uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
				{
					if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
						type = i;
				}
				if (type == UINT32_MAX)
					break;
				VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				mai.allocationSize = mr.size;
				mai.memoryTypeIndex = type;
				if (vkAllocateMemory(h.device, &mai, nullptr, &mem) != VK_SUCCESS)
				{
					mem = VK_NULL_HANDLE;
					break;
				}
				if (vkBindBufferMemory(h.device, buf, mem, 0) != VK_SUCCESS ||
					vkMapMemory(h.device, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
				{
					map = nullptr;
					break;
				}
				std::memcpy(map, px.data(), static_cast<std::size_t>(bytes));

				VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
				pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
				pci.queueFamilyIndex = h.queue_family;
				if (vkCreateCommandPool(h.device, &pci, nullptr, &pool) != VK_SUCCESS)
				{
					pool = VK_NULL_HANDLE;
					break;
				}
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				if (vkAllocateCommandBuffers(h.device, &cai, &cmd) != VK_SUCCESS)
				{
					cmd = VK_NULL_HANDLE;
					break;
				}
				VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
				bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
					break;
				Barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				VkBufferImageCopy region = {};
				region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.imageExtent = {W, H, 1};
				vkCmdCopyBufferToImage(cmd, buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				if (readback)
				{
					Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
						VK_PIPELINE_STAGE_TRANSFER_BIT);
					VkBufferImageCopy back = region;
					back.bufferOffset = bytes;
					vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &back);
					VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
					hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
					hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
					hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					hb.buffer = buf;
					hb.offset = bytes;
					hb.size = bytes;
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0,
						nullptr);
					Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				}
				else
				{
					Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				}
				if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
					break;
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				if (vkCreateFence(h.device, &fci, nullptr, &fence) != VK_SUCCESS)
				{
					fence = VK_NULL_HANDLE;
					break;
				}
				VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
				si.commandBufferCount = 1;
				si.pCommandBuffers = &cmd;
				submitted = (vkQueueSubmit(h.queue, 1, &si, fence) == VK_SUCCESS);
				if (submitted)
					done = (vkWaitForFences(h.device, 1, &fence, VK_TRUE, kOneSecondNs) == VK_SUCCESS);
			} while (false);
			if (done && readback && map)
			{
				const std::uint8_t* back = static_cast<const std::uint8_t*>(map) + bytes;
				g.readback.assign(back, back + bytes);
				if (swap_red_blue)
				{
					for (std::size_t i = 0; i + 3 < g.readback.size(); i += 4)
						std::swap(g.readback[i], g.readback[i + 2]);
				}
				g.readback_valid = true;
			}
			if (submitted && !done)
				vkQueueWaitIdle(h.queue);
			if (fence != VK_NULL_HANDLE)
				vkDestroyFence(h.device, fence, nullptr);
			if (pool != VK_NULL_HANDLE)
				vkDestroyCommandPool(h.device, pool, nullptr);
			if (map)
				vkUnmapMemory(h.device, mem);
			if (buf != VK_NULL_HANDLE)
				vkDestroyBuffer(h.device, buf, nullptr);
			if (mem != VK_NULL_HANDLE)
				vkFreeMemory(h.device, mem, nullptr);
			return done;
		}

		bool EnsureUploaded(const Frame& f, SwapchainWait::Budget& budget)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || f.session == XR_NULL_HANDLE)
				return false;
			const std::uint32_t W = g.art->raster_w, H = g.art->raster_h;
			if (g.swapchain == XR_NULL_HANDLE)
			{
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
								(g.dump_dir.empty() ? 0 : XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT);
				ci.format = f.swapchain_format;
				ci.sampleCount = 1;
				ci.width = W;
				ci.height = H;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				const XrResult res = xrCreateSwapchain(f.session, &ci, &g.swapchain);
				if (XR_FAILED(res))
				{
					g.swapchain = XR_NULL_HANDLE;
					return Fail("xrCreateSwapchain", static_cast<int>(res));
				}
				std::uint32_t count = 0;
				if (XR_FAILED(xrEnumerateSwapchainImages(g.swapchain, 0, &count, nullptr)) || count == 0)
					return Fail("xrEnumerateSwapchainImages", 0);
				g.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				if (XR_FAILED(xrEnumerateSwapchainImages(
						g.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(g.images.data()))))
					return Fail("xrEnumerateSwapchainImages", 1);
				Console.WriteLn("(VR) VrUi: swapchain created (%ux%u, %u images%s).", W, H, count,
					g.dump_dir.empty() ? "" : ", readable for the dump");
			}
			std::uint32_t index = 0;
			if (g.wait_pending)
			{
				index = g.pending_index;
			}
			else
			{
				XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain, &ai, &index)))
					return false;
				g.pending_index = index;
				g.wait_pending = true;
			}
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = budget.Next();
			const auto t0 = std::chrono::steady_clock::now();
			const XrResult wres = xrWaitSwapchainImage(g.swapchain, &wi);
			budget.Spend(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			const SwapchainWait::Action act = SwapchainWait::OnWait(g.wait_failures, wres);
			if (act == SwapchainWait::Action::Reacquire)
				g.wait_pending = false;
			if (act != SwapchainWait::Action::Use)
				return false;
			g.wait_pending = false;
			const bool written = Upload(h, g.images[index].image, f.swap_red_blue);
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const XrResult rres = xrReleaseSwapchainImage(g.swapchain, &ri);
			if (XR_FAILED(rres))
				return Fail("xrReleaseSwapchainImage", static_cast<int>(rres));
			if (!written)
				return Fail("the panel upload", 0);
			g.ever_released = true;
			Console.WriteLn("(VR) VrUi: panel uploaded (%ux%u RGBA8, %s).", W, H,
				f.swap_red_blue ? "red and blue swapped for a B8G8R8A8 swapchain" : "straight copy");
			return true;
		}


		void LogLayers(std::uint32_t appended, std::uint32_t layer_count, std::uint32_t capacity)
		{
			if (static_cast<int>(appended) == g.layers_logged)
				return;
			g.layers_logged = static_cast<int>(appended);
			Console.WriteLn("(VR) VrUi: %u panel layer(s); layers=%u after it, capacity %u.", appended, layer_count, capacity);
		}
	}

	std::uint32_t Wanted()
	{
		if (!g.selected)
		{
			g.selected = true;
			const char* dir = nullptr;
			if (dir && *dir)
			{
				auto a = std::make_unique<Artifact>();
				Refusal why;
				if (LoadDir(dir, *a, why))
				{
					Compose(*a, {}, g.pixels);
					const Placement& p = a->placement;
					Console.WriteLn("(VR) VrUi: %s loaded: %ux%u panel, %zu cell(s), atlas %s (%ux%u, decoded by libpng), "
									"%u layer(s), anchor %s at (%.2f, %.2f, %.2f) yaw %.1f deg, %.2f x %.2f m.",
						dir, a->raster_w, a->raster_h, a->cells.size(), a->atlas_file.c_str(), a->atlas_w, a->atlas_h, a->layers,
						p.anchor == Anchor::Screen ? "screen" : "world", p.offset_m[0], p.offset_m[1], p.offset_m[2], p.yaw_deg,
						p.width_m, p.height_m);
					g.art = std::move(a);
				}
				else
				{
					Console.Error("(VR) VrUi: %s refused (%s at %s: %s); no vrui panel.", dir, why.code.c_str(), why.where.c_str(),
						why.detail.c_str());
				}
			}
		}
		return LayersFor(g.art.get());
	}

	std::uint32_t Build(const Frame& f, SwapchainWait::Budget& budget, XrCompositionLayerQuad* quads,
		const XrCompositionLayerBaseHeader** layers, std::uint32_t& layer_count, std::uint32_t capacity)
	{
		const std::uint32_t want = LayersFor(g.art.get());
		if (want == 0 || g.failed || (!g.ever_released && !EnsureUploaded(f, budget)))
		{
			LogLayers(0, layer_count, capacity);
			return 0;
		}
		std::uint32_t appended = 0;
		if (layer_count < capacity)
		{
			const Artifact& a = *g.art;
			const QuadPose p = Place(a.placement, f.anchor);
			XrCompositionLayerQuad& q = quads[0];
			q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
			q.next = nullptr;
			q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
			q.space = f.space;
			q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			q.subImage = {g.swapchain, {{0, 0}, {static_cast<std::int32_t>(a.raster_w), static_cast<std::int32_t>(a.raster_h)}}, 0};
			q.pose.orientation = {p.qx, p.qy, p.qz, p.qw};
			q.pose.position = {p.x, p.y, p.z};
			q.size = {p.width_m, p.height_m};
			layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
			appended = 1;
		}
		LogLayers(appended, layer_count, capacity);
		return appended;
	}

	void Shutdown(bool xr_calls)
	{
		if (g.swapchain != XR_NULL_HANDLE && xr_calls)
			xrDestroySwapchain(g.swapchain);
		g.swapchain = XR_NULL_HANDLE;
		g.images.clear();
		g.failed = false;
		g.ever_released = false;
		g.wait_pending = false;
		g.wait_failures = {};
		g.pending_index = 0;
		g.readback.clear();
		g.readback_valid = false;
		g.layers_logged = -1;
	}
}

#endif
