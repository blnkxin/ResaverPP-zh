// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim Special Edition / Anniversary Edition .ess save file.
#pragma once

#include "Papyrus.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ess
{
	enum class Compression : std::uint16_t
	{
		kNone = 0,
		kZlib = 1,
		kLZ4 = 2
	};

	struct Header
	{
		std::string               magic;  // "TESV_SAVEGAME"
		std::uint32_t             version = 0;
		std::uint32_t             saveNumber = 0;
		std::string               playerName;
		std::uint32_t             playerLevel = 0;
		std::string               location;
		std::string               gameDate;
		std::string               race;
		std::uint16_t             sex = 0;
		float                     currentXP = 0, neededXP = 0;
		std::uint64_t             fileTime = 0;
		std::uint32_t             shotWidth = 0, shotHeight = 0;
		Compression               compression = Compression::kLZ4;
		std::vector<std::uint8_t> screenshot;  // SE: RGBA (4 bytes a pixel); LE: RGB (3)

		bool          isLE() const { return version < 12; }  // Skyrim Legendary Edition (save version 9)
		void          read(Reader& r);
		void          write(Writer& w) const;
		std::uint32_t partialSize() const;  // the value stored in the header-size field
		std::uint32_t totalSize() const;    // magic + size field + header + screenshot
	};

	struct PluginInfo
	{
		std::vector<std::string> full;
		std::vector<std::string> lite;
		bool                     hasLite = false;
	};

	struct FileLocationTable
	{
		std::uint32_t formIDArrayCountOffset = 0, unknownTable3Offset = 0;
		std::uint32_t table1Offset = 0, table2Offset = 0, changeFormsOffset = 0, table3Offset = 0;
		std::uint32_t table1Count = 0, table2Count = 0, table3Count = 0, changeFormCount = 0;
		std::uint32_t unused[15]{};
	};

	struct GlobalData
	{
		std::uint32_t             type = 0;
		std::vector<std::uint8_t> raw;      // every block except Papyrus
		std::unique_ptr<Papyrus>  papyrus;  // type 1001
	};

	enum class ChangeFormType : std::uint8_t
	{
		REFR = 0, ACHR, PMIS, PGRE, PBEA, PFLA, CELL, INFO, QUST, NPC_, ACTI, TACT, ARMO, BOOK, CONT, DOOR, INGR, LIGH, MISC, APPA, STAT, MSTT, FURN, WEAP, AMMO, KEYM, ALCH, IDLM, NOTE, ECZN, CLAS, FACT, PACK, NAVM, WOOP, MGEF, SMQN, SCEN, LCTN, RELA, PHZD, PBAR, PCON, FLST, LVLN, LVLI, LVSP, PARW, ENCH
	};
	const char* ChangeFormTypeName(std::uint8_t a_code);

	struct ChangeForm
	{
		RefID                     refID;
		std::uint32_t             changeFlags = 0;
		std::uint8_t              typeField = 0;  // low 6 bits type, high 2 bits length width
		std::uint8_t              version = 0;
		std::uint32_t             length1 = 0;  // stored data length
		std::uint32_t             length2 = 0;  // uncompressed length (0 = not compressed)
		std::vector<std::uint8_t> data;

		std::uint8_t typeCode() const { return typeField & 0x3F; }
	};

	class SaveFile
	{
	public:
		static std::unique_ptr<SaveFile> load(const std::filesystem::path& a_path);
		void save(const std::filesystem::path& a_path) const;  // also writes the .skse co-save next to it

		// Writes the save (and its .skse co-save) safely:
		//  1. both files are written to temporary files and the new save is read back (a save that cannot
		//     be read is never put in place);
		//  2. with a_backup, the files already there are copied to a matching backup generation first
		//     (name.ess.bak + name.skse.bak, name.ess.bak2 + name.skse.bak2, ...); a failed copy stops
		//     everything before anything is replaced;
		//  3. the save is put in place, then the co-save; if the co-save cannot be, the save is rolled
		//     back to the previous file (or removed, if there was none).
		// Throws std::runtime_error on any failure; then nothing but the temporary files was changed.
		struct WriteResult
		{
			std::filesystem::path backup, cosaveBackup;  // empty when none was made
		};
		WriteResult write(const std::filesystem::path& a_path, bool a_backup) const;
		std::vector<std::uint8_t> serialize() const;  // the whole .ess file

		std::vector<std::uint8_t> buildBody() const;  // uncompressed body, offsets recomputed

		std::filesystem::path     path;
		Header                    header;
		std::uint8_t              formVersion = 0;
		bool                      isLE() const { return header.isLE(); }
		PluginInfo                plugins;
		FileLocationTable         flt;
		std::vector<GlobalData>   table1, table2, table3;
		std::vector<ChangeForm>   changeForms;
		std::vector<std::uint32_t> formIDs;
		std::vector<std::uint32_t> visitedWorldspaces;
		std::vector<std::uint8_t> unknown3;
		std::vector<std::uint8_t> cosave;  // .skse file contents (empty if none)
		std::size_t               originalBodySize = 0;
		// Structural problems found while reading (layout that does not add up, bad string indices,
		// duplicate ids...). A save with problems may not be written back faithfully: open it read-only.
		std::vector<std::string>  problems;

		Papyrus*       papyrus();
		const Papyrus* papyrus() const { return const_cast<SaveFile*>(this)->papyrus(); }

		// Resolves a RefID to a full form id (0xFF...... for created forms). 0 if unknown.
		std::uint32_t formIDOf(const RefID& a_id) const;
		// Which plugin a form id comes from ("Created" for FF forms, "?" if out of range).
		std::string   pluginOf(std::uint32_t a_formID) const;
		ChangeForm*   findChangeForm(const RefID& a_id);
		void          reindexChangeForms();

	private:
		std::unordered_map<std::uint32_t, std::size_t> _cfIdx;
	};

	// A name for an edited copy of a save that the game still reads correctly. The game takes the
	// character's ID and other details from the name's fields (Save<n>_<character id>_..._<date>_<level>_<n>);
	// text added at the end ("_edited") breaks that, and the saves made after it lose the character ID
	// (00000000) and drop out of the character's group in the load menu. So the tag goes right after the
	// 14-digit date, as letters and digits only ("..._20261025075454edited_66_1.ess"). Names not in the
	// game's format get "_<tag>" at the end. a_unique: the first name not taken (tag, tag2, tag3...).
	std::filesystem::path EditedSaveName(const std::filesystem::path& a_save, const std::string& a_tag, bool a_unique = true);

	std::vector<std::uint8_t> DecompressLZ4(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen);
	std::vector<std::uint8_t> CompressLZ4(const std::vector<std::uint8_t>& a_src);
	std::vector<std::uint8_t> DecompressZlib(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen);
	std::vector<std::uint8_t> CompressZlib(const std::vector<std::uint8_t>& a_src);
}
