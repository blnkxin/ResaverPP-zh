// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Decoding of change form bodies (REFR, ACHR, NPC_, QUST, FLST, LVLN/LVLI, RELA), following
// ReSaver's ChangeForm* classes, plus the edits built on it: Reset Havok and Purify FormLists.
#pragma once
#include <set>

#include "Save.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ess::cf
{
	struct Node
	{
		std::string       label;
		std::string       value;      // text for plain values
		bool              isRef = false;
		std::uint32_t     refRaw = 0;  // the 3-byte RefID as stored
		std::uint32_t     formID = 0;  // resolved runtime form ID (0 if none / unknown)
		std::vector<Node> kids;
	};

	// An inventory entry that carries an enchantment and/or a charge in its extra data (weapons and
	// staves enchanted at a table or by a script, or partly used). Offsets are in the uncompressed body.
	struct InvCharge
	{
		std::uint32_t item = 0;     // runtime form ID of the base item
		std::int32_t  count = 0;
		bool          hasEnchant = false;
		std::uint32_t enchRaw = 0;  // ENCH RefID as stored (0: none)
		std::uint32_t ench = 0;     // runtime form ID (0 if it does not resolve)
		std::uint16_t maxCharge = 0;
		std::size_t   maxChargeOffset = 0;
		bool          hasCharge = false;
		float         charge = 0;
		std::size_t   chargeOffset = 0;
	};

	struct ExtraEntry
	{
		std::string name;  // as shown ("Ownership", "PackageData"...)
		std::size_t start = 0, end = 0;
	};

	struct Decoded
	{
		bool        supported = false;  // the type has a decoder
		bool        complete = false;   // the whole body was decoded with nothing left over
		std::string error;              // why decoding stopped (if it did)
		std::size_t bodySize = 0, consumed = 0;
		Node        root;

		bool          hasHavok = false;
		std::size_t   havokOffset = 0, havokSize = 0;  // in the uncompressed body
		std::uint32_t baseObject = 0;                  // runtime form ID, when the body names one

		// ReSaver filter facts
		bool          hasFormFlags = false;
		std::uint32_t formFlags = 0;      // CHANGE_FORM_FLAGS block, when present
		bool          isEmptyRef = false;  // "empty REFR": no change flags 0-7 and nothing but empty initial data
		bool          inCellMinus1 = false;  // moved to the "-1" cell ("cell(-1) change forms")

		// form lists
		bool                       isFormList = false;
		std::vector<std::uint32_t> listRaw;  // the entries' RefIDs (raw 3-byte values)

		// inventory entries with an enchantment or charge (REFR containers, ACHR actors incl. the player)
		std::vector<InvCharge> charged;

		// where each change flag's data sits in the uncompressed body: bit -> [start, end) (NPC_, ACHR)
		std::map<int, std::pair<std::size_t, std::size_t>> sections;

		// a reference's own extra data list: its count (a vsval) and each entry [start, end)
		bool                    hasExtras = false;
		std::size_t             extrasCountAt = 0, extrasCountEnd = 0;
		std::vector<ExtraEntry> extras;

		// an actor's AI: the process level field, the AI process data (absent at level -1), and the
		// movement and combat controllers (each its "has" byte and data)
		// values that can be edited in place (offsets in the body)
		struct InvAt
		{
			std::uint32_t item = 0;  // runtime form ID
			std::size_t   entryAt = 0, countAt = 0, endAt = 0;
		};
		struct ValueAt
		{
			std::uint32_t id = 0;  // actor value / faction form ID
			std::size_t   at = 0;
		};
		std::size_t              inventoryCountAt = 0;  // the inventory's count (a vsval)
		std::vector<InvAt>       inventory;
		std::vector<ValueAt>     baseAVs;         // an actor's changed base actor values: id, value offset (f32)
		std::vector<std::size_t> playerSkillsAt;  // the player's 18 skills: level, XP, XP for next level (f32 each)
		std::vector<ValueAt>     crimeGold;       // the player's bounties: faction, offset of 4 i32
		std::size_t              baseAVListAt = 0, avModListAt = 0;  // the two actor value lists' counts (vsval)
		std::vector<ValueAt>     avMods;          // actor value modifiers: id, offset of permanent, temporary, damage (f32)
		std::size_t              addedSpellsAt = 0;  // the added spells' count (u32)
		std::vector<ValueAt>     addedSpells;     // spell form ID, offset of its RefID
		std::size_t              playerXPAt = 0;          // XP, XP for next level (f32 each)
		std::size_t              playerPerkPointsAt = 0;  // u8

		bool                                hasActorData = false;
		std::int32_t                        processLevel = -1;
		std::size_t                         levelAt = 0;
		std::pair<std::size_t, std::size_t> aiProcess{ 0, 0 }, movement{ 0, 0 }, combat{ 0, 0 };
	};

	// Every inventory entry with an enchantment or charge in the save, with the change form holding it.
	struct ChargedItem
	{
		std::uint32_t holderRefID = 0;  // the change form's RefID (raw)
		std::uint32_t holder = 0;       // its runtime form ID
		InvCharge     inv;
	};
	std::vector<ChargedItem> FindChargedItems(const SaveFile& a_save);

	// Fixes enchanted inventory items that cannot be recharged: max charge 0 (no "EAMT") is set to
	// a_maxCharge, and a current charge above the max is lowered to it. Only fixed-size values are
	// rewritten in place, nothing is added or removed. a_only: change form RefIDs (raw) to touch (empty:
	// all) and item form IDs (empty: all). Returns how many items were fixed.
	std::size_t FixCharges(SaveFile& a_save, std::uint16_t a_maxCharge, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a_only);
	bool        NeedsChargeFix(const InvCharge& a_item);  // has an enchantment with max charge 0, or charge > max

	// Optional: record type of a form ("EXPL", ...), from the plugin database. Needed only to
	// decode the explosion data of spawned explosion references.
	using TypeLookup = std::function<std::string(std::uint32_t)>;

	std::vector<std::uint8_t> Body(const ChangeForm& a_cf);                        // decompressed
	void                      SetBody(ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body);  // recompresses if it was compressed
	Decoded                   Decode(const SaveFile& a_save, const ChangeForm& a_cf, const TypeLookup& a_types = {});
	std::vector<std::string>  FlagNames(std::uint8_t a_type, std::uint32_t a_flags);

	// Removes one part of a change form: clears its change flag and cuts out the data that flag holds,
	// keeping everything else (e.g. an NPC's changed face, so the game uses the plugin's face again,
	// while its factions, outfits and spells stay). For change forms whose body is fully decoded and
	// whose parts are known (NPC_, ACHR). Checks the result decodes completely, else changes nothing.
	// Returns "" when done, else why not.
	// ACHR (an actor reference): REFR_MOVE (back to its editor position, when the position is saved for
	// that flag alone), REFR_SCALE, REFR_INVENTORY (when not shared with the leveled inventory) and
	// REFR_ANIMATION. The player: only REFR_SCALE and REFR_ANIMATION.
	bool        CanRemoveFlag(const ChangeForm& a_cf, int a_bit);
	std::string RemoveFlagEffect(std::uint8_t a_type, int a_bit);  // what removing it does, for the user
	std::string RemoveFlag(const SaveFile& a_save, ChangeForm& a_cf, int a_bit);

	// Removes one entry from an actor's (ACHR) own extra data list, keeping the rest and the change
	// flags: the game falls back to the plugin's value, or none, for that entry. Only entries that are
	// safe to drop are offered (a package also takes its package data with it, and the reverse).
	// Checks the result decodes completely, else changes nothing. Returns "" when done, else why not.
	bool        CanRemoveExtra(const std::string& a_name);
	bool        CanRemoveExtraFrom(const ChangeForm& a_cf, const std::string& a_name);  // the player only gets a few
	std::string RemoveExtraEffect(const std::string& a_name);
	std::string RemoveExtra(const SaveFile& a_save, ChangeForm& a_cf, std::size_t a_index);

	// ---- the player's character: read and change gold, skills and bounties in place ----
	struct PlayerBounty
	{
		std::uint32_t faction = 0;  // form ID
		std::int32_t  violent = 0, nonViolent = 0, nonViolentTotal = 0, violentTotal = 0;
	};
	struct PlayerAV
	{
		std::uint32_t id = 0;
		bool          hasBase = false, hasMod = false;
		float         base = 0, permanent = 0, temporary = 0, damage = 0;
		float         current() const { return base + permanent + temporary + damage; }
	};
	struct PlayerItem
	{
		std::uint32_t item = 0;  // form ID
		std::int32_t  count = 0;
		bool          hasExtra = false;  // enchanted, tempered, worn...
	};
	struct PlayerValues
	{
		std::string               error;  // empty when read
		bool                      hasGold = false;
		std::int32_t              gold = 0;
		float                     skills[18]{};  // base skill values (One-handed ... Enchanting)
		bool                      hasSkill[18]{};
		std::int32_t              legendary[18]{};
		bool                      hasLegendary = false;
		std::vector<PlayerBounty> bounties;
		bool                      hasLevel = false;
		int                       level = 0;
		float                     xp = 0, xpNext = 0;
		int                       perkPoints = 0;
		std::vector<PlayerAV>     avs;  // every actor value with a saved base value or modifier
		std::vector<PlayerItem>   items;
		std::vector<std::uint32_t> spells;  // spells and powers added in game
	};
	extern const char* const kSkillNames[18];
	const char*  ActorValueName(std::uint32_t a_av);  // "Health", "DragonSouls"...
	// 界面显示用的中文名；CLI 的 --set-skill / --set-av 仍按上面的英文名匹配
	const char*  SkillNameZh(int a_index);
	const char*  ActorValueNameZh(std::uint32_t a_av);  // 没有译文的回退英文名
	PlayerValues ReadPlayer(const SaveFile& a_save);
	std::string  SetPlayerGold(SaveFile& a_save, std::int32_t a_gold);  // adds a gold entry when there is none
	std::string  SetPlayerSkill(SaveFile& a_save, int a_skill, float a_level);
	std::string  SetPlayerLegendary(SaveFile& a_save, int a_skill, std::int32_t a_count);
	std::string  SetPlayerBounty(SaveFile& a_save, std::uint32_t a_faction, std::int32_t a_violent, std::int32_t a_nonViolent);
	std::string  ClearPlayerCrimes(SaveFile& a_save, std::uint32_t a_faction);  // bounty and crime totals to 0
	std::string  SetPlayerLevel(SaveFile& a_save, int a_level);  // also sets the XP needed for the next level
	std::string  SetPlayerXP(SaveFile& a_save, float a_xp);
	std::string  SetPlayerPerkPoints(SaveFile& a_save, int a_points);
	std::string  SetPlayerDragonSouls(SaveFile& a_save, int a_souls);
	// actor values: set the base value; reset it (the saved value and its permanent and damage modifiers are
	// dropped, so the race and plugins give it again: not for skills, Health, Magicka, Stamina or dragon souls,
	// whose saved values hold your progress);
	// clear modifiers (a_which: 1 permanent, 2 temporary, 4 damage)
	std::string  SetPlayerAV(SaveFile& a_save, std::uint32_t a_av, float a_base);
	bool         CanResetPlayerAV(std::uint32_t a_av);
	std::string  ResetPlayerAV(SaveFile& a_save, std::uint32_t a_av);
	std::string  ClearPlayerAVModifiers(SaveFile& a_save, std::uint32_t a_av, int a_which);
	// items: set the count (a stack without extra data), or remove every stack of the item
	std::string  SetPlayerItemCount(SaveFile& a_save, std::uint32_t a_item, std::int32_t a_count);
	std::string  RemovePlayerItem(SaveFile& a_save, std::uint32_t a_item);
	std::string  RemovePlayerSpell(SaveFile& a_save, std::uint32_t a_spell);

	// Resets an actor's (ACHR) saved AI: the process level becomes -1 (the state the game saves for
	// actors it has no AI loaded for) and the AI process, movement controller and combat controller
	// data are dropped, so the game builds fresh AI when the actor loads. Its stats, spells, inventory,
	// factions and position stay. Never the player. Returns "" when done, else why not.
	bool        CanResetAI(const ChangeForm& a_cf, const Decoded& a_decoded);
	std::string ResetAI(const SaveFile& a_save, ChangeForm& a_cf);

	// Where every actor (ACHR) is, as the save has it. Actors the game never moved have no position in
	// the save (they stand where their plugin places them): hasPosition false.
	struct ActorPlace
	{
		std::uint32_t refRaw = 0;    // the change form's RefID (raw)
		std::uint32_t formID = 0;    // the actor reference's form id
		std::uint32_t baseObject = 0;  // set for actors created in game (their base NPC)
		bool          hasPosition = false;
		std::uint32_t cell = 0;       // the cell (interior) or worldspace (exterior) it is in
		std::uint32_t startCell = 0;  // where its plugin put it, when it changed cell
		float         pos[3]{}, rot[3]{};
		bool          isPlayer = false;
	};
	std::vector<ActorPlace> ListActors(const SaveFile& a_save);
	// Moves an actor next to the player (same cell or worldspace, a little in front of them) and resets its
	// saved AI so it does not keep walking an old path. Only actors the save already has a position for.
	// Checks the result decodes completely, else changes nothing. Returns "" when done, else why not.
	std::string MoveActorToPlayer(SaveFile& a_save, ChangeForm& a_cf);

	// Every inventory entry in the save (the player, NPCs, containers): who holds what. For finding items
	// left by mods that are gone.
	struct InventoryEntry
	{
		std::uint32_t holderRefRaw = 0;  // the change form's RefID (raw)
		std::uint32_t holder = 0;        // its form id
		std::uint8_t  holderType = 0;    // ChangeFormType (REFR or ACHR)
		std::uint32_t item = 0;          // the item's form id
		std::int32_t  count = 0;
	};
	std::vector<InventoryEntry> ListInventories(const SaveFile& a_save);
	// Removes every entry of the given items from one change form's inventory. Checks the result decodes
	// completely, else changes nothing. Returns "" when done (a_removed: entries removed), else why not.
	std::string RemoveInventoryItems(const SaveFile& a_save, ChangeForm& a_cf, const std::set<std::uint32_t>& a_items, std::size_t* a_removed = nullptr);

	// ReSaver "Reset Havok": zeroes the saved physics state of every object reference whose body
	// decodes completely. Returns {reset, could not be decoded}.
	struct HavokResult
	{
		std::size_t reset = 0, failed = 0;
	};
	HavokResult ResetHavok(SaveFile& a_save, const TypeLookup& a_types = {}, bool a_dryRun = false);

	// ReSaver "Purify FormLists": removes null entries from form lists.
	struct PurifyResult
	{
		std::size_t entries = 0, forms = 0, failed = 0;
	};
	PurifyResult PurifyFormLists(SaveFile& a_save, bool a_dryRun = false, std::uint32_t a_onlyRefID = 0);  // 0: every list

	// ReSaver's change form content filter: terms separated by ";" must all match, alternatives
	// separated by "," need one match, "!" negates. A term is a path of field names separated by
	// "/" ending in a value, e.g. "EXTRADATA/Ownership" or "INITIAL/CELL/0000003C" or
	// "INVENTORY/ITEM/0001F4" (hex form IDs match as prefixes). Field names ignore case and counts.
	bool ContentMatches(const Node& a_root, const std::string& a_expression);
	std::size_t  NullEntries(const SaveFile& a_save, const ChangeForm& a_cf);  // null entries in a form list (0 if none / not a list)
}
