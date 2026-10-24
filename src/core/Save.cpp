// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "Save.h"

#include "lz4.h"
#include "miniz.h"

#include <fstream>
#include <stdexcept>

namespace ess
{
	const char* ChangeFormTypeName(std::uint8_t a_code)
	{
		static const char* names[] = {
			"REFR", "ACHR", "PMIS", "PGRE", "PBEA", "PFLA", "CELL", "INFO", "QUST", "NPC_", "ACTI", "TACT", "ARMO", "BOOK", "CONT", "DOOR",
			"INGR", "LIGH", "MISC", "APPA", "STAT", "MSTT", "FURN", "WEAP", "AMMO", "KEYM", "ALCH", "IDLM", "NOTE", "ECZN", "CLAS", "FACT",
			"PACK", "NAVM", "WOOP", "MGEF", "SMQN", "SCEN", "LCTN", "RELA", "PHZD", "PBAR", "PCON", "FLST", "LVLN", "LVLI", "LVSP", "PARW", "ENCH"
		};
		if (a_code < std::size(names)) {
			return names[a_code];
		}
		return a_code == 63 ? "LAND" : "????";
	}

	// ---- compression -------------------------------------------------------------------

	std::vector<std::uint8_t> DecompressLZ4(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen)
	{
		std::vector<std::uint8_t> out(a_dstLen);
		const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(a_src), reinterpret_cast<char*>(out.data()),
			static_cast<int>(a_srcLen), static_cast<int>(a_dstLen));
		if (n < 0 || static_cast<std::size_t>(n) != a_dstLen) {
			throw std::runtime_error("LZ4 解压失败（存档已损坏，或不是天际 SE 存档）");
		}
		return out;
	}

	std::vector<std::uint8_t> CompressLZ4(const std::vector<std::uint8_t>& a_src)
	{
		std::vector<std::uint8_t> out(LZ4_compressBound(static_cast<int>(a_src.size())));
		const int n = LZ4_compress_default(reinterpret_cast<const char*>(a_src.data()), reinterpret_cast<char*>(out.data()),
			static_cast<int>(a_src.size()), static_cast<int>(out.size()));
		if (n <= 0) {
			throw std::runtime_error("LZ4 压缩失败");
		}
		out.resize(n);
		return out;
	}

	std::vector<std::uint8_t> DecompressZlib(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen)
	{
		std::vector<std::uint8_t> out(a_dstLen);
		mz_ulong                  len = static_cast<mz_ulong>(a_dstLen);
		if (mz_uncompress(out.data(), &len, a_src, static_cast<mz_ulong>(a_srcLen)) != MZ_OK || len != a_dstLen) {
			throw std::runtime_error("zlib 解压失败");
		}
		return out;
	}

	std::vector<std::uint8_t> CompressZlib(const std::vector<std::uint8_t>& a_src)
	{
		mz_ulong                  len = mz_compressBound(static_cast<mz_ulong>(a_src.size()));
		std::vector<std::uint8_t> out(len);
		if (mz_compress(out.data(), &len, a_src.data(), static_cast<mz_ulong>(a_src.size())) != MZ_OK) {
			throw std::runtime_error("zlib 压缩失败");
		}
		out.resize(len);
		return out;
	}

	// ---- header -------------------------------------------------------------------------

	void Header::read(Reader& r)
	{
		magic.assign(reinterpret_cast<const char*>(r.here()), std::min<std::size_t>(13, r.remaining()));
		if (magic != "TESV_SAVEGAME") {
			throw FormatError("不是天际存档（魔数错误）", 0);
		}
		r.seek(13);
		const auto headerSize = r.u32();
		const auto start = r.pos();
		version = r.u32();
		if (version < 12) {
			throw FormatError("这是天际传奇版存档；仅支持特别版/周年纪念版存档", 13);
		}
		saveNumber = r.u32();
		playerName = r.wstring();
		playerLevel = r.u32();
		location = r.wstring();
		gameDate = r.wstring();
		race = r.wstring();
		sex = r.u16();
		currentXP = r.f32();
		neededXP = r.f32();
		fileTime = r.u64();
		shotWidth = r.u32();
		shotHeight = r.u32();
		compression = static_cast<Compression>(r.u16());
		if (r.pos() - start != headerSize) {
			throw FormatError("头部大小不匹配", r.pos());
		}
		screenshot = r.bytes(static_cast<std::size_t>(shotWidth) * shotHeight * 4);
	}

	std::uint32_t Header::partialSize() const
	{
		return static_cast<std::uint32_t>(4 + 4 + (2 + playerName.size()) + 4 + (2 + location.size()) + (2 + gameDate.size()) +
										  (2 + race.size()) + 2 + 4 + 4 + 8 + 4 + 4 + 2);
	}

	std::uint32_t Header::totalSize() const
	{
		return static_cast<std::uint32_t>(13 + 4 + partialSize() + screenshot.size());
	}

	void Header::write(Writer& w) const
	{
		w.raw(magic.data(), 13);
		w.u32(partialSize());
		w.u32(version);
		w.u32(saveNumber);
		w.wstring(playerName);
		w.u32(playerLevel);
		w.wstring(location);
		w.wstring(gameDate);
		w.wstring(race);
		w.u16(sex);
		w.f32(currentXP);
		w.f32(neededXP);
		w.u64(fileTime);
		w.u32(shotWidth);
		w.u32(shotHeight);
		w.u16(static_cast<std::uint16_t>(compression));
		w.bytes(screenshot);
	}

	// ---- body ---------------------------------------------------------------------------

	namespace
	{
		std::vector<std::uint8_t> ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream f(a_path, std::ios::binary);
			if (!f) {
				throw std::runtime_error("无法打开 " + a_path.string());
			}
			f.seekg(0, std::ios::end);
			const auto n = static_cast<std::size_t>(f.tellg());
			f.seekg(0);
			std::vector<std::uint8_t> v(n);
			f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n));
			return v;
		}

		void WriteFile(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_data)
		{
			std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
			if (!f) {
				throw std::runtime_error("无法写入 " + a_path.string());
			}
			f.write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
			if (!f) {
				throw std::runtime_error("写入失败： " + a_path.string());
			}
		}

		GlobalData ReadGlobal(Reader& r)
		{
			GlobalData g;
			g.type = r.u32();
			const auto size = r.u32();
			r.need(size);
			if (g.type == 1001) {
				Reader sub(r.here(), size);
				g.papyrus = std::make_unique<Papyrus>();
				try {
					g.papyrus->read(sub);
				} catch (const FormatError& e) {
					throw FormatError(std::string("Papyrus 区段： ") + e.what(), r.pos() + e.pos);
				}
				r.seek(r.pos() + size);
			} else {
				g.raw = r.bytes(size);
			}
			return g;
		}

		void WriteGlobal(Writer& w, const GlobalData& g)
		{
			w.u32(g.type);
			if (g.papyrus) {
				Writer pw;
				g.papyrus->write(pw);
				w.u32(static_cast<std::uint32_t>(pw.size()));
				w.bytes(pw.buf);
			} else {
				w.u32(static_cast<std::uint32_t>(g.raw.size()));
				w.bytes(g.raw);
			}
		}

		ChangeForm ReadChangeForm(Reader& r)
		{
			ChangeForm c;
			c.refID = RefID::read(r);
			c.changeFlags = r.u32();
			c.typeField = r.u8();
			c.version = r.u8();
			switch (c.typeField >> 6) {
			case 0:
				c.length1 = r.u8();
				c.length2 = r.u8();
				break;
			case 1:
				c.length1 = r.u16();
				c.length2 = r.u16();
				break;
			case 2:
				c.length1 = r.u32();
				c.length2 = r.u32();
				break;
			default:
				throw FormatError("变更表单长度字段宽度无效", r.pos());
			}
			c.data = r.bytes(c.length1);
			return c;
		}

		void WriteChangeForm(Writer& w, const ChangeForm& c)
		{
			c.refID.write(w);
			w.u32(c.changeFlags);
			w.u8(c.typeField);
			w.u8(c.version);
			switch (c.typeField >> 6) {
			case 0:
				w.u8(static_cast<std::uint8_t>(c.data.size()));
				w.u8(static_cast<std::uint8_t>(c.length2));
				break;
			case 1:
				w.u16(static_cast<std::uint16_t>(c.data.size()));
				w.u16(static_cast<std::uint16_t>(c.length2));
				break;
			default:
				w.u32(static_cast<std::uint32_t>(c.data.size()));
				w.u32(c.length2);
				break;
			}
			w.bytes(c.data);
		}

		std::filesystem::path CosavePath(const std::filesystem::path& a_save)
		{
			auto p = a_save;
			p.replace_extension(".skse");
			return p;
		}
	}

	std::unique_ptr<SaveFile> SaveFile::load(const std::filesystem::path& a_path)
	{
		auto save = std::make_unique<SaveFile>();
		save->path = a_path;

		const auto file = ReadFile(a_path);
		Reader     hr(file.data(), file.size());
		save->header.read(hr);
		const std::size_t headerEnd = hr.pos();

		std::vector<std::uint8_t> bodyStore;
		const std::uint8_t*       body = nullptr;
		std::size_t               bodyLen = 0;
		if (save->header.compression == Compression::kNone) {
			body = file.data() + headerEnd;
			bodyLen = file.size() - headerEnd;
		} else {
			const auto uncompressed = hr.u32();
			const auto compressed = hr.u32();
			hr.need(compressed);
			if (hr.remaining() > compressed) {
				save->problems.push_back(std::to_string(hr.remaining() - compressed) + " byte(s) after the compressed data");
			}
			if (save->header.compression == Compression::kLZ4) {
				bodyStore = DecompressLZ4(hr.here(), compressed, uncompressed);
			} else if (save->header.compression == Compression::kZlib) {
				bodyStore = DecompressZlib(hr.here(), compressed, uncompressed);
			} else {
				throw std::runtime_error("未知的压缩类型 " + std::to_string(static_cast<int>(save->header.compression)));
			}
			body = bodyStore.data();
			bodyLen = bodyStore.size();
			if (bodyLen != uncompressed) {
				save->problems.push_back("the data unpacks to " + std::to_string(bodyLen) + " bytes, the header says " + std::to_string(uncompressed));
			}
		}
		save->originalBodySize = bodyLen;

		// File-location-table offsets count from the start of the file, as if the body were
		// stored uncompressed right after the header.
		const std::size_t base = save->header.totalSize();
		Reader            r(body, bodyLen);
		auto              at = [&](std::uint32_t a_fileOffset) {
            if (a_fileOffset < base) {
                throw FormatError("文件位置表的偏移指向了头部内部", r.pos());
            }
            r.seek(a_fileOffset - base);
		};

		save->formVersion = r.u8();
		{
			const auto infoSize = r.u32();
			const auto start = r.pos();
			const auto full = r.u8();
			for (unsigned i = 0; i < full; ++i) {
				save->plugins.full.push_back(r.wstring());
			}
			if (save->formVersion >= 78) {
				save->plugins.hasLite = true;
				const auto lite = r.u16();
				for (unsigned i = 0; i < lite; ++i) {
					save->plugins.lite.push_back(r.wstring());
				}
			}
			if (r.pos() - start != infoSize) {
				throw FormatError("插件列表大小不匹配", r.pos());
			}
		}

		auto& t = save->flt;
		t.formIDArrayCountOffset = r.u32();
		t.unknownTable3Offset = r.u32();
		t.table1Offset = r.u32();
		t.table2Offset = r.u32();
		t.changeFormsOffset = r.u32();
		t.table3Offset = r.u32();
		t.table1Count = r.u32();
		t.table2Count = r.u32();
		t.table3Count = r.u32() + 1;  // Skyrim under-reports table 3 by one
		t.changeFormCount = r.u32();
		for (auto& u : t.unused) {
			u = r.u32();
		}

		at(t.formIDArrayCountOffset);
		const auto formIDCount = r.u32();
		save->formIDs.reserve(formIDCount);
		for (std::uint32_t i = 0; i < formIDCount; ++i) {
			save->formIDs.push_back(r.u32());
		}

		// each section must end exactly where the file location table says the next one starts:
		// otherwise bytes in between would be lost (or invented) when the save is written again
		auto expect = [&](std::uint32_t a_fileOffset, const char* a_what) {
			if (a_fileOffset >= base && r.pos() != a_fileOffset - base) {
				save->problems.push_back(std::string(a_what) + " does not start where the previous section ends (" +
										 std::to_string(static_cast<long long>(a_fileOffset - base) - static_cast<long long>(r.pos())) +
										 " byte(s) unaccounted for)");
			}
		};
		at(t.table1Offset);
		for (std::uint32_t i = 0; i < t.table1Count; ++i) {
			save->table1.push_back(ReadGlobal(r));
		}
		expect(t.table2Offset, "global data table 2");
		for (std::uint32_t i = 0; i < t.table2Count; ++i) {
			save->table2.push_back(ReadGlobal(r));
		}
		expect(t.changeFormsOffset, "the change forms");
		save->changeForms.reserve(t.changeFormCount);
		for (std::uint32_t i = 0; i < t.changeFormCount; ++i) {
			save->changeForms.push_back(ReadChangeForm(r));
		}
		expect(t.table3Offset, "global data table 3");
		for (std::uint32_t i = 0; i < t.table3Count; ++i) {
			save->table3.push_back(ReadGlobal(r));
		}
		expect(t.formIDArrayCountOffset, "the form ID array");

		r.seek(t.formIDArrayCountOffset - base + 4 + 4 * static_cast<std::size_t>(formIDCount));
		const auto wsCount = r.u32();
		for (std::uint32_t i = 0; i < wsCount; ++i) {
			save->visitedWorldspaces.push_back(r.u32());
		}
		save->unknown3 = r.bytes(r.remaining());

		std::error_code ec;
		const auto      co = CosavePath(a_path);
		if (std::filesystem::exists(co, ec)) {
			save->cosave = ReadFile(co);
		}

		save->reindexChangeForms();
		if (auto* p = save->papyrus()) {
			p->reindex();
			for (auto& msg : p->check()) {
				save->problems.push_back("Papyrus: " + msg);
			}
		}
		return save;
	}

	std::vector<std::uint8_t> SaveFile::buildBody() const
	{
		const std::size_t base = header.totalSize();

		Writer w;
		w.u8(formVersion);
		{
			Writer pw;
			pw.u8(static_cast<std::uint8_t>(plugins.full.size()));
			for (const auto& p : plugins.full) {
				pw.wstring(p);
			}
			if (plugins.hasLite) {
				pw.u16(static_cast<std::uint16_t>(plugins.lite.size()));
				for (const auto& p : plugins.lite) {
					pw.wstring(p);
				}
			}
			w.u32(static_cast<std::uint32_t>(pw.size()));
			w.bytes(pw.buf);
		}

		const std::size_t fltAt = w.size();
		for (int i = 0; i < 25; ++i) {
			w.u32(0);  // patched below
		}

		const auto t1 = static_cast<std::uint32_t>(base + w.size());
		for (const auto& g : table1) {
			WriteGlobal(w, g);
		}
		const auto t2 = static_cast<std::uint32_t>(base + w.size());
		for (const auto& g : table2) {
			WriteGlobal(w, g);
		}
		const auto cf = static_cast<std::uint32_t>(base + w.size());
		for (const auto& c : changeForms) {
			WriteChangeForm(w, c);
		}
		const auto t3 = static_cast<std::uint32_t>(base + w.size());
		for (const auto& g : table3) {
			WriteGlobal(w, g);
		}
		const auto fid = static_cast<std::uint32_t>(base + w.size());
		w.u32(static_cast<std::uint32_t>(formIDs.size()));
		for (auto id : formIDs) {
			w.u32(id);
		}
		w.u32(static_cast<std::uint32_t>(visitedWorldspaces.size()));
		for (auto id : visitedWorldspaces) {
			w.u32(id);
		}
		const auto u3 = static_cast<std::uint32_t>(base + w.size());
		w.bytes(unknown3);

		std::uint32_t vals[10] = { fid, u3, t1, t2, cf, t3,
			static_cast<std::uint32_t>(table1.size()), static_cast<std::uint32_t>(table2.size()),
			static_cast<std::uint32_t>(table3.size() - 1), static_cast<std::uint32_t>(changeForms.size()) };
		for (int i = 0; i < 10; ++i) {
			w.patch32(fltAt + 4 * i, vals[i]);
		}
		for (int i = 0; i < 15; ++i) {
			w.patch32(fltAt + 40 + 4 * i, flt.unused[i]);
		}
		return std::move(w.buf);
	}

	std::vector<std::uint8_t> SaveFile::serialize() const
	{
		const auto body = buildBody();
		Writer     out;
		header.write(out);
		switch (header.compression) {
		case Compression::kNone:
			out.bytes(body);
			break;
		case Compression::kLZ4:
			{
				const auto c = CompressLZ4(body);
				out.u32(static_cast<std::uint32_t>(body.size()));
				out.u32(static_cast<std::uint32_t>(c.size()));
				out.bytes(c);
				break;
			}
		case Compression::kZlib:
			{
				const auto c = CompressZlib(body);
				out.u32(static_cast<std::uint32_t>(body.size()));
				out.u32(static_cast<std::uint32_t>(c.size()));
				out.bytes(c);
				break;
			}
		}
		return std::move(out.buf);
	}

	void SaveFile::save(const std::filesystem::path& a_path) const { write(a_path, false); }

	SaveFile::WriteResult SaveFile::write(const std::filesystem::path& a_path, bool a_backup) const
	{
		WriteResult     res;
		std::error_code ec;
		const auto      co = CosavePath(a_path);
		auto            tmp = a_path;
		tmp += ".tmp";
		auto tmpCo = co;
		tmpCo += ".tmp";
		auto cleanup = [&] {
			std::error_code e;
			std::filesystem::remove(tmp, e);
			std::filesystem::remove(tmpCo, e);
		};

		// 1. stage both files and check the new save reads back
		try {
			WriteFile(tmp, serialize());
			if (!cosave.empty()) {
				WriteFile(tmpCo, cosave);
			}
			auto check = SaveFile::load(tmp);
			if (!check->papyrus()) {
				throw std::runtime_error("the written save has no Papyrus section");
			}
		} catch (const std::exception& e) {
			cleanup();
			throw std::runtime_error(std::string("could not write the new save (nothing was changed): ") + e.what());
		}

		// 2. back up what is there, as one generation for both files
		const bool hadEss = std::filesystem::exists(a_path, ec);
		const bool hadCo = std::filesystem::exists(co, ec);
		if (a_backup && (hadEss || hadCo)) {
			std::string suffix = ".bak";
			for (int n = 2;; ++n) {
				auto b1 = a_path;
				b1 += suffix;
				auto b2 = co;
				b2 += suffix;
				if (!std::filesystem::exists(b1, ec) && !std::filesystem::exists(b2, ec)) {
					break;
				}
				suffix = ".bak" + std::to_string(n);
			}
			if (hadEss) {
				res.backup = a_path;
				res.backup += suffix;
				if (!std::filesystem::copy_file(a_path, res.backup, ec) || ec) {
					cleanup();
					throw std::runtime_error("could not back up " + a_path.filename().string() + " (" + ec.message() + "); nothing was changed");
				}
			}
			if (hadCo) {
				res.cosaveBackup = co;
				res.cosaveBackup += suffix;
				if (!std::filesystem::copy_file(co, res.cosaveBackup, ec) || ec) {
					cleanup();
					throw std::runtime_error("could not back up " + co.filename().string() + " (" + ec.message() + "); nothing was changed");
				}
			}
		}

		// 3. put them in place: the save, then the co-save; roll the save back if the co-save fails
		std::filesystem::rename(tmp, a_path, ec);
		if (ec) {
			cleanup();
			throw std::runtime_error("could not replace " + a_path.filename().string() + " (" + ec.message() + "); nothing was changed");
		}
		if (!cosave.empty()) {
			std::filesystem::rename(tmpCo, co, ec);
			if (ec) {
				const auto      why = ec.message();
				std::error_code e2;
				bool            restored = false;
				if (!res.backup.empty()) {
					restored = std::filesystem::copy_file(res.backup, a_path, std::filesystem::copy_options::overwrite_existing, e2) && !e2;
				} else if (!hadEss) {
					restored = std::filesystem::remove(a_path, e2) && !e2;
				}
				cleanup();
				throw std::runtime_error("could not write the co-save " + co.filename().string() + " (" + why + "); " +
										 (restored ? "the save was put back as it was" : "the save could not be put back: restore it from its .bak copy"));
			}
		}
		return res;
	}

	Papyrus* SaveFile::papyrus()
	{
		for (auto& g : table3) {
			if (g.papyrus) {
				return g.papyrus.get();
			}
		}
		return nullptr;
	}

	std::uint32_t SaveFile::formIDOf(const RefID& a_id) const
	{
		switch (a_id.type()) {
		case RefID::Type::kDefault:
			return a_id.value();
		case RefID::Type::kCreated:
			return 0xFF000000u | a_id.value();
		case RefID::Type::kFormIdx:
			{
				const auto idx = a_id.value();
				if (idx == 0 || idx > formIDs.size()) {
					return 0;
				}
				return formIDs[idx - 1];
			}
		default:
			return 0;
		}
	}

	std::string SaveFile::pluginOf(std::uint32_t a_formID) const
	{
		const std::uint32_t top = a_formID >> 24;
		if (top == 0xFF) {
			return "Created";
		}
		if (top == 0xFE) {
			const std::uint32_t lite = (a_formID >> 12) & 0xFFF;
			return lite < plugins.lite.size() ? plugins.lite[lite] : "?";
		}
		return top < plugins.full.size() ? plugins.full[top] : "?";
	}

	void SaveFile::reindexChangeForms()
	{
		_cfIdx.clear();
		_cfIdx.reserve(changeForms.size());
		for (std::size_t i = 0; i < changeForms.size(); ++i) {
			_cfIdx[changeForms[i].refID.data] = i;
		}
	}

	ChangeForm* SaveFile::findChangeForm(const RefID& a_id)
	{
		auto it = _cfIdx.find(a_id.data);
		return it == _cfIdx.end() ? nullptr : &changeForms[it->second];
	}
}
