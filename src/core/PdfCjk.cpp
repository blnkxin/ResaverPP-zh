// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "PdfCjk.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

namespace ess::pdf
{
	namespace
	{
		std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]); }
		std::int16_t  S16(const std::uint8_t* p) { return static_cast<std::int16_t>(U16(p)); }
		std::uint32_t U32(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3]; }

		void Put16(std::vector<std::uint8_t>& d, std::uint16_t v)
		{
			d.push_back(static_cast<std::uint8_t>(v >> 8));
			d.push_back(static_cast<std::uint8_t>(v));
		}
		void Put32(std::vector<std::uint8_t>& d, std::uint32_t v)
		{
			d.push_back(static_cast<std::uint8_t>(v >> 24));
			d.push_back(static_cast<std::uint8_t>(v >> 16));
			d.push_back(static_cast<std::uint8_t>(v >> 8));
			d.push_back(static_cast<std::uint8_t>(v));
		}

		// ---- a parsed TrueType font ------------------------------------------------------------
		struct Face
		{
			std::vector<std::uint8_t>                                          data;
			std::map<std::string, std::pair<std::uint32_t, std::uint32_t>>     tables;  // tag -> (offset, length)
			std::uint16_t                                                      upem = 0, numGlyphs = 0, numHMetrics = 0;
			std::int16_t                                                       ascent = 0, descent = 0;
			std::int16_t                                                       bbox[4] = { 0, 0, 0, 0 };
			bool                                                               longLoca = false;
			std::vector<std::uint32_t>                                         loca;
			std::map<std::uint32_t, std::uint16_t>                             cmap;  // code point -> glyph id

			const std::uint8_t* table(const char* a_tag, std::uint32_t* a_len = nullptr) const
			{
				auto it = tables.find(a_tag);
				if (it == tables.end() || it->second.first + it->second.second > data.size()) {
					if (a_len) {
						*a_len = 0;
					}
					return nullptr;
				}
				if (a_len) {
					*a_len = it->second.second;
				}
				return data.data() + it->second.first;
			}

			std::uint32_t glyphOffset(std::uint16_t a_gid) const
			{
				return a_gid + 1 < loca.size() ? loca[a_gid] : 0;
			}
			std::uint32_t glyphLength(std::uint16_t a_gid) const
			{
				if (a_gid + 1 >= loca.size() || loca[a_gid + 1] < loca[a_gid]) {
					return 0;
				}
				return loca[a_gid + 1] - loca[a_gid];
			}
			// a pointer into the glyf table (nullptr for an empty glyph)
			const std::uint8_t* glyph(std::uint16_t a_gid, std::uint32_t* a_len = nullptr) const
			{
				std::uint32_t glyfLen = 0;
				const auto*   glyf = table("glyf", &glyfLen);
				if (!glyf) {
					if (a_len) {
						*a_len = 0;
					}
					return nullptr;
				}
				const auto off = glyphOffset(a_gid), len = glyphLength(a_gid);
				if (off + len > glyfLen || len < 10) {
					if (a_len) {
						*a_len = 0;
					}
					return nullptr;
				}
				if (a_len) {
					*a_len = len;
				}
				return glyf + off;
			}
			int advance(std::uint16_t a_gid) const
			{
				std::uint32_t len = 0;
				const auto*   hmtx = table("hmtx", &len);
				if (!hmtx || numHMetrics == 0) {
					return upem ? upem : 1000;
				}
				const std::uint32_t idx = a_gid < numHMetrics ? a_gid : static_cast<std::uint32_t>(numHMetrics - 1);
				if (4 * idx + 2 > len) {
					return upem ? upem : 1000;
				}
				return U16(hmtx + 4 * idx);
			}
		};

		bool ParseCmap(Face& f)
		{
			std::uint32_t len = 0;
			const auto*   cmap = f.table("cmap", &len);
			if (!cmap || len < 4) {
				return false;
			}
			const auto subtables = U16(cmap + 2);
			const std::uint8_t* best12 = nullptr;
			const std::uint8_t* best4 = nullptr;
			for (std::uint32_t i = 0; i < subtables; ++i) {
				if (4 + 8 * i + 8 > len) {
					break;
				}
				const auto* rec = cmap + 4 + 8 * i;
				const auto  platform = U16(rec), encoding = U16(rec + 2);
				const auto  off = U32(rec + 4);
				if (off + 2 > len) {
					continue;
				}
				const auto* sub = cmap + off;
				const auto  format = U16(sub);
				if (format == 12 && ((platform == 3 && encoding == 10) || (platform == 0 && encoding >= 4))) {
					best12 = sub;
				} else if (format == 4 && ((platform == 3 && encoding == 1) || platform == 0)) {
					best4 = sub;
				}
			}
			if (best12) {
				const auto groups = U32(best12 + 12);
				for (std::uint32_t g = 0; g < groups && 16 + 12 * g + 12 <= len; ++g) {
					const auto* grp = best12 + 16 + 12 * g;
					const auto  start = U32(grp), end = U32(grp + 4), startGid = U32(grp + 8);
					for (std::uint32_t cp = start; cp <= end && cp <= 0xFFFF; ++cp) {
						f.cmap[cp] = static_cast<std::uint16_t>(startGid + (cp - start));
					}
				}
				return !f.cmap.empty();
			}
			if (!best4) {
				return false;
			}
			const auto segX2 = U16(best4 + 6);
			if (segX2 == 0) {
				return false;
			}
			const auto segs = segX2 / 2;
			const auto* ends = best4 + 14;
			const auto* starts = ends + segX2 + 2;
			const auto* deltas = starts + segX2;
			const auto* rangeOffsets = deltas + segX2;
			if (reinterpret_cast<const std::uint8_t*>(rangeOffsets + segX2) > cmap + len) {
				return false;
			}
			for (std::uint32_t s = 0; s < segs; ++s) {
				const std::uint16_t start = U16(starts + 2 * s);
				const std::uint16_t end = U16(ends + 2 * s);
				const std::int16_t  delta = S16(deltas + 2 * s);
				const std::uint16_t ro = U16(rangeOffsets + 2 * s);
				if (start == 0xFFFF) {
					continue;
				}
				for (std::uint32_t cp = start; cp <= end; ++cp) {
					std::uint16_t gid = 0;
					if (ro == 0) {
						gid = static_cast<std::uint16_t>((cp + delta) & 0xFFFF);
					} else {
						const auto* p = rangeOffsets + 2 * s + ro + 2 * (cp - start);
						if (p + 2 > cmap + len) {
							continue;
						}
						gid = U16(p);
						if (gid != 0) {
							gid = static_cast<std::uint16_t>((gid + delta) & 0xFFFF);
						}
					}
					if (gid != 0) {
						f.cmap[cp] = gid;
					}
				}
			}
			return !f.cmap.empty();
		}

		bool Parse(Face& f, std::vector<std::uint8_t> a_data)
		{
			f.data = std::move(a_data);
			if (f.data.size() < 12) {
				return false;
			}
			const auto magic = U32(f.data.data());
			if (magic != 0x00010000 && magic != 0x74727565 /* 'true' */) {
				return false;  // collections (ttcf) and CFF fonts are not supported
			}
			const auto numTables = U16(f.data.data() + 4);
			for (std::uint32_t i = 0; i < numTables; ++i) {
				const auto off = 12 + 16 * i;
				if (off + 16 > f.data.size()) {
					break;
				}
				std::string tag(reinterpret_cast<const char*>(f.data.data() + off), 4);
				f.tables[tag] = { U32(f.data.data() + off + 8), U32(f.data.data() + off + 12) };
			}
			std::uint32_t headLen = 0, maxpLen = 0, hheaLen = 0, locaLen = 0;
			const auto*   head = f.table("head", &headLen);
			const auto*   maxp = f.table("maxp", &maxpLen);
			const auto*   hhea = f.table("hhea", &hheaLen);
			const auto*   loca = f.table("loca", &locaLen);
			if (!head || headLen < 54 || !maxp || maxpLen < 6 || !hhea || hheaLen < 36 || !loca || !f.table("glyf") || !f.table("hmtx")) {
				return false;
			}
			f.upem = U16(head + 18);
			f.longLoca = S16(head + 50) == 1;
			f.bbox[0] = S16(head + 36);
			f.bbox[1] = S16(head + 38);
			f.bbox[2] = S16(head + 40);
			f.bbox[3] = S16(head + 42);
			f.ascent = S16(hhea + 4);
			f.descent = S16(hhea + 6);
			f.numHMetrics = U16(hhea + 34);
			f.numGlyphs = U16(maxp + 4);
			const auto entries = static_cast<std::uint32_t>(f.numGlyphs) + 1;
			f.loca.reserve(entries);
			for (std::uint32_t i = 0; i < entries; ++i) {
				if (f.longLoca) {
					if (4 * i + 4 > locaLen) {
						return false;
					}
					f.loca.push_back(U32(loca + 4 * i));
				} else {
					if (2 * i + 2 > locaLen) {
						return false;
					}
					f.loca.push_back(2u * U16(loca + 2 * i));
				}
			}
			return f.upem != 0 && ParseCmap(f);
		}

		// component glyphs of a composite glyph, with the offsets of their glyph-index fields
		std::vector<std::pair<std::uint16_t, std::uint32_t>> Components(const Face& f, std::uint16_t a_gid)
		{
			std::vector<std::pair<std::uint16_t, std::uint32_t>> out;
			std::uint32_t                                        len = 0;
			const auto*                                          g = f.glyph(a_gid, &len);
			if (!g || S16(g) >= 0) {
				return out;
			}
			std::uint32_t o = 10;
			while (o + 4 <= len) {
				const auto flags = U16(g + o), child = U16(g + o + 2);
				out.emplace_back(child, o + 2);
				o += 4;
				o += (flags & 0x0001) ? 4 : 2;
				if (flags & 0x0008) {
					o += 2;
				} else if (flags & 0x0040) {
					o += 4;
				} else if (flags & 0x0080) {
					o += 8;
				}
				if (!(flags & 0x0020)) {
					break;
				}
			}
			return out;
		}
	}

	std::filesystem::path SystemFont()
	{
		// TrueType (not .ttc collections): SimHei is the smallest usable Chinese face on Windows
		static const char* kCandidates[] = { "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\Deng.ttf", "C:\\Windows\\Fonts\\simkai.ttf",
			"C:\\Windows\\Fonts\\simfang.ttf", "C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\msyh.ttc" };
		for (const char* c : kCandidates) {
			std::error_code ec;
			if (std::filesystem::exists(c, ec)) {
				return c;
			}
		}
		return {};
	}

	bool BuildSubset(const std::filesystem::path& a_font, const std::set<std::uint32_t>& a_codePoints, Subset& a_out)
	{
		a_out = Subset{};
		std::ifstream in(a_font, std::ios::binary);
		if (!in) {
			return false;
		}
		std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		Face                      f;
		if (!Parse(f, std::move(raw))) {
			return false;
		}

		// the glyphs to keep: the ones the text needs, plus the components of composite glyphs
		std::set<std::uint16_t> used;
		for (auto cp : a_codePoints) {
			auto it = f.cmap.find(cp);
			if (it != f.cmap.end() && it->second < f.numGlyphs) {
				used.insert(it->second);
			}
		}
		for (bool grew = true; grew;) {
			grew = false;
			for (auto gid : std::vector<std::uint16_t>(used.begin(), used.end())) {
				for (const auto& [child, offset] : Components(f, gid)) {
					if (child < f.numGlyphs && used.insert(child).second) {
						grew = true;
					}
				}
			}
		}
		if (used.empty()) {
			return false;
		}

		std::vector<std::uint16_t>          order;  // new glyph id -> old glyph id
		std::map<std::uint16_t, std::uint16_t> newGid;  // old glyph id -> new glyph id
		order.push_back(0);
		newGid[0] = 0;
		for (auto gid : used) {
			if (gid == 0) {
				continue;
			}
			newGid[gid] = static_cast<std::uint16_t>(order.size());
			order.push_back(gid);
		}
		const auto count = static_cast<std::uint16_t>(order.size());

		// glyf + loca + hmtx, with composite component indices renumbered
		std::vector<std::uint8_t> glyf;
		std::vector<std::uint32_t> loca;
		std::vector<std::uint8_t> hmtx;
		loca.push_back(0);
		for (auto old : order) {
			std::uint32_t len = 0;
			const auto*   g = f.glyph(old, &len);
			if (g && len > 0) {
				const auto start = glyf.size();
				glyf.insert(glyf.end(), g, g + len);
				for (const auto& [child, fieldOffset] : Components(f, old)) {
					auto it = newGid.find(child);
					if (it != newGid.end() && fieldOffset + 2 <= len) {
						glyf[start + fieldOffset] = static_cast<std::uint8_t>(it->second >> 8);
						glyf[start + fieldOffset + 1] = static_cast<std::uint8_t>(it->second & 0xFF);
					}
				}
			}
			while (glyf.size() % 4 != 0) {
				glyf.push_back(0);
			}
			loca.push_back(static_cast<std::uint32_t>(glyf.size()));
			Put16(hmtx, static_cast<std::uint16_t>(f.advance(old)));
			Put16(hmtx, 0);
		}

		// tables of the subset
		std::map<std::string, std::vector<std::uint8_t>> tables;
		{
			std::uint32_t     len = 0;
			const auto*       head = f.table("head", &len);
			std::vector<std::uint8_t> h(head, head + std::min<std::uint32_t>(len, 54));
			h.resize(54, 0);
			h[8] = h[9] = h[10] = h[11] = 0;                // checkSumAdjustment, filled in below
			h[50] = 0;
			h[51] = 1;                                      // indexToLocFormat = long
			tables["head"] = std::move(h);
		}
		{
			std::uint32_t             len = 0;
			const auto*               hhea = f.table("hhea", &len);
			std::vector<std::uint8_t> h(hhea, hhea + std::min<std::uint32_t>(len, 36));
			h.resize(36, 0);
			h[34] = static_cast<std::uint8_t>(count >> 8);
			h[35] = static_cast<std::uint8_t>(count & 0xFF);  // numberOfHMetrics
			tables["hhea"] = std::move(h);
		}
		{
			std::vector<std::uint8_t> maxp;
			Put32(maxp, 0x00010000);
			Put16(maxp, count);
			maxp.resize(32, 0);  // the other maxima are only hints for hinting tools
			tables["maxp"] = std::move(maxp);
		}
		tables["hmtx"] = std::move(hmtx);
		{
			std::vector<std::uint8_t> l;
			for (auto v : loca) {
				Put32(l, v);
			}
			tables["loca"] = std::move(l);
		}
		tables["glyf"] = std::move(glyf);
		{
			std::vector<std::uint8_t> post;
			Put32(post, 0x00030000);
			post.resize(32, 0);  // version 3: no glyph names
			tables["post"] = std::move(post);
		}
		if (std::uint32_t os2Len = 0; f.table("OS/2", &os2Len) != nullptr && os2Len >= 68) {
			const auto* os2 = f.table("OS/2");
			tables["OS/2"] = std::vector<std::uint8_t>(os2, os2 + os2Len);
		}

		// assemble the sfnt
		const auto n = static_cast<std::uint16_t>(tables.size());
		std::uint16_t searchRange = 1, entrySelector = 0;
		while (static_cast<std::uint32_t>(searchRange) * 2 <= n) {
			searchRange = static_cast<std::uint16_t>(searchRange * 2);
			++entrySelector;
		}
		searchRange = static_cast<std::uint16_t>(searchRange * 16);

		std::vector<std::uint8_t> out;
		Put32(out, 0x00010000);
		Put16(out, n);
		Put16(out, searchRange);
		Put16(out, entrySelector);
		Put16(out, static_cast<std::uint16_t>(n * 16 - searchRange));

		std::vector<std::uint8_t> body;
		std::map<std::string, std::uint32_t> offsets;
		for (const auto& [tag, data] : tables) {
			std::vector<std::uint8_t> padded = data;
			while (padded.size() % 4 != 0) {
				padded.push_back(0);
			}
			std::uint32_t sum = 0;
			for (std::size_t i = 0; i + 4 <= padded.size(); i += 4) {
				sum += U32(padded.data() + i);
			}
			offsets[tag] = static_cast<std::uint32_t>(12 + 16 * n + body.size());
			body.insert(body.end(), padded.begin(), padded.end());
			out.insert(out.end(), tag.begin(), tag.end());
			Put32(out, sum);
			Put32(out, offsets[tag]);
			Put32(out, static_cast<std::uint32_t>(data.size()));
		}
		out.insert(out.end(), body.begin(), body.end());

		// head.checkSumAdjustment = 0xB1B0AFBA - checksum of the whole file
		std::uint32_t total = 0;
		for (std::size_t i = 0; i + 4 <= out.size(); i += 4) {
			total += U32(out.data() + i);
		}
		const auto adjustment = 0xB1B0AFBAu - total;
		const auto headOff = offsets["head"];
		out[headOff + 8] = static_cast<std::uint8_t>(adjustment >> 24);
		out[headOff + 9] = static_cast<std::uint8_t>(adjustment >> 16);
		out[headOff + 10] = static_cast<std::uint8_t>(adjustment >> 8);
		out[headOff + 11] = static_cast<std::uint8_t>(adjustment);

		// what the PDF writer needs
		a_out.file = std::move(out);
		a_out.upem = f.upem;
		a_out.ascent = f.ascent * 1000 / f.upem;
		a_out.descent = f.descent * 1000 / f.upem;
		for (int i = 0; i < 4; ++i) {
			a_out.bbox[i] = f.bbox[i] * 1000 / f.upem;
		}
		a_out.widths.assign(count, 0);
		a_out.glyphOf.assign(count, 0);
		for (auto cp : a_codePoints) {
			auto it = f.cmap.find(cp);
			if (it == f.cmap.end()) {
				continue;
			}
			auto n2 = newGid.find(it->second);
			if (n2 == newGid.end()) {
				continue;
			}
			a_out.gid[cp] = n2->second;
			if (a_out.glyphOf[n2->second] == 0) {
				a_out.glyphOf[n2->second] = static_cast<std::uint16_t>(cp);
			}
			a_out.widths[n2->second] = f.advance(it->second) * 1000 / f.upem;
		}
		a_out.ok = !a_out.gid.empty() && !a_out.file.empty();
		return a_out.ok;
	}
}
