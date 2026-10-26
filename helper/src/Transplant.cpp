#include "PCH.h"
#include "Transplant.h"
#include "Protocol.h"
#include "Game.h"

// Windows.h defines GetObject as GetObjectW, which hides BGSDefaultObjectManager::GetObject
#ifdef GetObject
#	undef GetObject
#endif

#include <cctype>

namespace Transplant
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;
		using Record = std::vector<std::string>;

		constexpr std::pair<const char*, RE::ActorValue> kAVs[] = {
			{ "Health", RE::ActorValue::kHealth },
			{ "Magicka", RE::ActorValue::kMagicka },
			{ "Stamina", RE::ActorValue::kStamina },
			{ "CarryWeight", RE::ActorValue::kCarryWeight },
			{ "DragonSouls", RE::ActorValue::kDragonSouls },
		};
		constexpr int kSkills = 18;  // One-handed ... Enchanting: actor values 6..23

		RE::ActorValue SkillAV(int a_index) { return static_cast<RE::ActorValue>(static_cast<int>(RE::ActorValue::kOneHanded) + a_index); }

		std::string F(float a_v) { return std::format("{:.4f}", a_v); }

		bool IsDynamic(const RE::TESForm* a_form) { return a_form && (a_form->GetFormID() >> 24) == 0xFF; }

		std::string PluginOf(const RE::TESForm* a_form)
		{
			const auto* file = a_form ? a_form->GetFile(0) : nullptr;
			return file ? std::string(file->GetFilename()) : std::string();
		}

		// plugin, local id, runtime id
		void PutForm(Record& r, const RE::TESForm* a_form)
		{
			r.push_back(PluginOf(a_form));
			r.push_back(std::format("{:06X}", a_form ? a_form->GetLocalFormID() : 0));
			r.push_back(std::format("{:08X}", a_form ? a_form->GetFormID() : 0));
		}

		std::string NameOf(const RE::TESForm* a_form)
		{
			if (!a_form) {
				return {};
			}
			const char* n = a_form->GetName();
			if (n && *n) {
				return n;
			}
			const char* e = a_form->GetFormEditorID();
			return e ? std::string(e) : std::string();
		}

		std::uint32_t Hex(const std::string& s)
		{
			try {
				return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
			} catch (...) {
				return 0;
			}
		}
		float Num(const std::string& s)
		{
			try {
				return std::stof(s);
			} catch (...) {
				return 0.0f;
			}
		}

		RE::TESForm* Lookup(const std::string& a_plugin, const std::string& a_local)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh || a_plugin.empty()) {
				return nullptr;
			}
			return dh->LookupForm(Hex(a_local), a_plugin);
		}

		std::vector<std::string> Split(const std::string& s, char c)
		{
			std::vector<std::string> out;
			std::string              cur;
			for (char ch : s) {
				if (ch == c) {
					out.push_back(cur);
					cur.clear();
				} else {
					cur += ch;
				}
			}
			out.push_back(cur);
			return out;
		}

		// "<plugin>:<local>:<magnitude>:<area>:<duration>" for each effect, comma separated.
		// Plugin names cannot contain ':' (Windows file names), so it is a safe separator.
		std::string Effects(const RE::MagicItem* a_item)
		{
			std::string out;
			if (!a_item) {
				return out;
			}
			for (const auto* e : a_item->effects) {
				if (!e || !e->baseEffect) {
					continue;
				}
				out += std::format("{}{}:{:06X}:{}:{}:{}", out.empty() ? "" : ",", PluginOf(e->baseEffect), e->baseEffect->GetLocalFormID(),
					e->effectItem.magnitude, e->effectItem.area, e->effectItem.duration);
			}
			return out;
		}

		bool MakeEffects(const std::string& a_text, RE::BSTArray<RE::Effect>& a_out, std::string& a_why)
		{
			for (const auto& one : Split(a_text, ',')) {
				const auto p = Split(one, ':');
				if (p.size() < 5) {
					continue;
				}
				auto* mgef = Lookup(p[0], p[1]);
				auto* setting = mgef ? mgef->As<RE::EffectSetting>() : nullptr;
				if (!setting) {
					a_why = "its magic effect " + p[1] + " from " + p[0] + " is not in this game";
					return false;
				}
				auto& e = a_out.emplace_back();
				e.baseEffect = setting;
				e.effectItem.magnitude = Num(p[2]);
				e.effectItem.area = static_cast<std::uint32_t>(Num(p[3]));
				e.effectItem.duration = static_cast<std::uint32_t>(Num(p[4]));
				e.cost = 0.0f;
			}
			if (a_out.empty()) {
				a_why = "no effects";
				return false;
			}
			return true;
		}

		// an enchantment or a poison on an item
		std::string MagicField(const RE::MagicItem* a_item, char a_kind, std::int64_t a_amount)
		{
			if (!a_item) {
				return {};
			}
			if (IsDynamic(a_item)) {
				return std::format("C:{}:{}:{}", a_kind, a_amount, Effects(a_item));
			}
			return std::format("F:{}:{:06X}:{}", PluginOf(a_item), a_item->GetLocalFormID(), a_amount);
		}

		class Later : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Later(std::function<void(const RE::BSScript::Variable&)> a_fn) :
				_fn(std::move(a_fn))
			{}
			void operator()(RE::BSScript::Variable a_result) override
			{
				if (_fn) {
					_fn(a_result);
				}
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::function<void(const RE::BSScript::Variable&)> _fn;
		};
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeLater(std::function<void(const RE::BSScript::Variable&)> a_fn)
		{
			return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Later(std::move(a_fn)) };
		}

		// the left or right hand equip slot
		const RE::BGSEquipSlot* HandSlot(bool a_left)
		{
			auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
			if (!dom) {
				return nullptr;
			}
			const auto which = static_cast<RE::BGSDefaultObjectManager::DefaultObject>(a_left ? RE::DEFAULT_OBJECTS::kLeftHandEquip : RE::DEFAULT_OBJECTS::kRightHandEquip);
			auto*      form = dom->GetObject(which);
			return form ? form->As<RE::BGSEquipSlot>() : nullptr;
		}

		void Console(const std::string& a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto*      script = factory ? factory->Create() : nullptr;
			if (!script) {
				return;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
			spdlog::info("console: {}", a_command);
		}

		void Papyrus(const char* a_class, const char* a_fn, RE::TESForm* a_arg)
		{
			auto* vm = VM::GetSingleton();
			if (!vm) {
				return;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			if (auto* word = a_arg ? a_arg->As<RE::TESWordOfPower>() : nullptr) {
				vm->DispatchStaticCall(a_class, a_fn, RE::MakeFunctionArguments(std::move(word)), none);
			}
		}
	}

	// ---- snapshot ---------------------------------------------------------------------------------

	std::filesystem::path Snapshot(std::string& a_error, std::size_t& a_lines)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* dh = RE::TESDataHandler::GetSingleton();
		spdlog::info("Snapshot: starting");
		if (!player || !dh) {
			a_error = "no player (load a save first)";
			return {};
		}
		std::vector<Record> out;
		// the character's own name (the display name can be changed by mods, e.g. to "The Narrator")
		std::string         playerName = NameOf(player->GetActorBase());
		if (playerName.empty()) {
			const char* dn = player->GetDisplayFullName();
			playerName = dn ? dn : "";
		}
		std::string         safe;
		for (char c : playerName) {
			safe += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
		}
		if (safe.empty()) {
			safe = "player";
		}
		const auto stamp = std::format("{:%Y%m%d_%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
		out.push_back({ "RSH-SNAPSHOT", "1", REL::Module::get().version().string(), playerName });
		// the load order, so the other side can tell which mods this game has
		{
			const auto* mods = dh->GetLoadedMods();
			for (std::uint8_t i = 0; mods && i < dh->GetLoadedModCount(); ++i) {
				if (mods[i]) {
					out.push_back({ "plugin", std::string(mods[i]->GetFilename()) });
				}
			}
			const auto* light = dh->GetLoadedLightMods();
			for (std::uint16_t i = 0; light && i < dh->GetLoadedLightModCount(); ++i) {
				if (light[i]) {
					out.push_back({ "plugin", std::string(light[i]->GetFilename()) });
				}
			}
		}

		// level, skills, attributes
		auto* skills = player->GetInfoRuntimeData().skills;
		auto* sd = skills ? skills->data : nullptr;
		out.push_back({ "player", std::to_string(player->GetLevel()), F(sd ? sd->xp : 0.0f), F(sd ? sd->levelThreshold : 0.0f),
			std::to_string(player->GetGameStatsData().perkCount) });
		auto* av = player->AsActorValueOwner();
		for (int i = 0; i < kSkills; ++i) {
			out.push_back({ "skill", std::to_string(i), F(av->GetBaseActorValue(SkillAV(i))), F(sd ? sd->skills[i].xp : 0.0f),
				F(sd ? sd->skills[i].levelThreshold : 0.0f), std::to_string(sd ? sd->legendaryLevels[i] : 0) });
		}
		for (const auto& [name, value] : kAVs) {
			out.push_back({ "av", name, F(av->GetBaseActorValue(value)) });
		}

		// perks, spells, shouts, words
		for (auto* perk : dh->GetFormArray<RE::BGSPerk>()) {
			if (perk && player->HasPerk(perk)) {
				Record r{ "perk" };
				PutForm(r, perk);
				r.push_back(NameOf(perk));
				out.push_back(std::move(r));
			}
		}
		for (auto* spell : dh->GetFormArray<RE::SpellItem>()) {
			if (!spell) {
				continue;
			}
			const auto t = spell->GetSpellType();
			if ((t == RE::MagicSystem::SpellType::kSpell || t == RE::MagicSystem::SpellType::kPower || t == RE::MagicSystem::SpellType::kLesserPower) &&
				player->HasSpell(spell)) {
				Record r{ "spell" };
				PutForm(r, spell);
				r.push_back(NameOf(spell));
				out.push_back(std::move(r));
			}
		}
		if (auto* npc = player->GetActorBase(); npc && npc->actorEffects) {
			const auto* fx = npc->actorEffects;
			for (std::uint32_t i = 0; i < fx->numShouts; ++i) {
				if (auto* shout = fx->shouts[i]) {
					Record r{ "shout" };
					PutForm(r, shout);
					r.push_back(NameOf(shout));
					out.push_back(std::move(r));
				}
			}
		}
		for (auto* word : dh->GetFormArray<RE::TESWordOfPower>()) {
			if (!word) {
				continue;
			}
			const bool known = (word->formFlags & RE::TESForm::RecordFlags::kKnown) != 0;
			const bool unlocked = (word->formFlags & RE::TESForm::RecordFlags::kUnlocked) != 0;
			if (known || unlocked) {
				Record r{ "word" };
				PutForm(r, word);
				r.push_back(NameOf(word));
				r.push_back(known ? "1" : "0");
				r.push_back(unlocked ? "1" : "0");
				out.push_back(std::move(r));
			}
		}

		// factions
		player->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
			if (a_faction && !IsDynamic(a_faction)) {
				Record r{ "faction" };
				PutForm(r, a_faction);
				r.push_back(NameOf(a_faction));
				r.push_back(std::to_string(a_rank));
				out.push_back(std::move(r));
			}
			return false;
		});

		// map markers the player has found
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSReadLockGuard guard{ lock };
			if (map) {
				for (const auto& [id, form] : *map) {
					if (!form || form->GetFormType() != RE::FormType::Reference || IsDynamic(form)) {
						continue;
					}
					auto* ref = form->As<RE::TESObjectREFR>();
					auto* xm = ref ? ref->extraList.GetByType<RE::ExtraMapMarker>() : nullptr;
					if (!xm || !xm->mapData) {
						continue;
					}
					const auto flags = static_cast<int>(xm->mapData->flags.underlying()) & 3;
					if (flags) {
						Record r{ "marker" };
						PutForm(r, ref);
						const char* n = xm->mapData->locationName.GetFullName();
						r.push_back(n ? n : "");
						r.push_back(std::to_string(flags));
						out.push_back(std::move(r));
					}
				}
			}
		}

		// quests: every quest, so a quest started only in the other save can still be found here
		for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest) {
				continue;
			}
			// objectives the journal shows or has shown (index:state; 1 shown, 2 completed, 3 completed and shown, 4 failed, 5 failed and shown)
			std::string objectives;
			for (auto* o : quest->objectives) {
				if (o && o->state.underlying() != 0) {
					objectives += std::format("{}{}:{}", objectives.empty() ? "" : ",", o->index, static_cast<int>(o->state.underlying()));
				}
			}
			const std::string done;  // (the executed stage list could not be read reliably; left empty)
			Record r{ "quest" };
			PutForm(r, quest);
			const char* edid = quest->GetFormEditorID();
			r.push_back(std::string(edid ? edid : "") + "|" + NameOf(quest));
			r.push_back(Game::Running(quest) ? "1" : "0");
			r.push_back(quest->IsCompleted() ? "1" : "0");
			r.push_back(std::to_string(quest->GetCurrentStageID()));
			r.push_back(done);
			r.push_back(objectives);
			r.push_back(std::to_string(static_cast<int>(quest->GetType())));
			out.push_back(std::move(r));
		}

		// followers: every character following the player (a teammate, in the follower faction, or held by the
		// vanilla follower quest), with the quest aliases that hold them (their mod's own follower system) and
		// what they carry. Only placed characters (from a plugin): they are the same character in every save.
		{
			auto*      followerFaction = Lookup("Skyrim.esm", "05C84E") ? Lookup("Skyrim.esm", "05C84E")->As<RE::TESFaction>() : nullptr;
			auto*      dialogueFollower = Lookup("Skyrim.esm", "0750BA");
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			const RE::BSReadLockGuard guard{ lock };
			if (map) {
				for (const auto& [id, form] : *map) {
					if (!form || form->GetFormType() != RE::FormType::ActorCharacter || IsDynamic(form) || form == player) {
						continue;
					}
					auto* actor = form->As<RE::Actor>();
					if (!actor || actor->IsDead()) {
						continue;
					}
					const int rank = followerFaction ? actor->GetFactionRank(followerFaction, false) : -1;
					std::string aliases;
					bool        inVanilla = false;
					if (auto* xa = actor->extraList.GetByType<RE::ExtraAliasInstanceArray>()) {
						const RE::BSReadLockGuard aguard{ xa->lock };
						for (auto* inst : xa->aliases) {
							if (!inst || !inst->quest || !inst->alias || IsDynamic(inst->quest)) {
								continue;
							}
							inVanilla = inVanilla || inst->quest == dialogueFollower;
							aliases += std::format("{}{}*{:06X}*{}", aliases.empty() ? "" : "|", PluginOf(inst->quest), inst->quest->GetLocalFormID(), inst->alias->aliasID);
						}
					}
					if (!actor->IsPlayerTeammate() && rank < 0 && !inVanilla) {
						continue;
					}
					std::string items;
					for (const auto& [obj, cnt] : actor->GetInventoryCounts()) {
						if (obj && cnt > 0 && !IsDynamic(obj) && !PluginOf(obj).empty()) {
							items += std::format("{}{}*{:06X}*{}", items.empty() ? "" : "|", PluginOf(obj), obj->GetLocalFormID(), cnt);
						}
					}
					Record r{ "follower" };
					PutForm(r, actor);
					r.push_back(NameOf(actor->GetActorBase()).empty() ? NameOf(actor) : NameOf(actor->GetActorBase()));
					r.push_back(actor->IsPlayerTeammate() ? "1" : "0");
					r.push_back(std::to_string(rank));
					r.push_back(aliases);
					r.push_back(items);
					out.push_back(std::move(r));
				}
			}
		}

		// the journal: every objective the player's journal lists, with the state it shows. This is what the
		// journal really holds (a radiant quest done and started again since keeps its old entry only here).
		for (const auto& io : player->GetPlayerRuntimeData().objectives) {
			auto* o = io.Objective;
			auto* q = o ? o->ownerQuest : nullptr;
			if (!q) {
				continue;
			}
			Record r{ "jobj" };
			PutForm(r, q);
			r.push_back(std::to_string(o->index));
			r.push_back(std::to_string(static_cast<int>(io.InstanceState)));
			out.push_back(std::move(r));
		}

		// inventory: each item with its kind, so it can be listed in groups
		auto kindOf = [](const RE::TESBoundObject* o) -> std::string {
			if (!o) {
				return "Other";
			}
			if (o->GetFormID() == 0xF) {
				return "Gold";
			}
			switch (o->GetFormType()) {
			case RE::FormType::Weapon: return "Weapons";
			case RE::FormType::Armor: return "Armor and clothing";
			case RE::FormType::Ammo: return "Arrows and bolts";
			case RE::FormType::Book: return "Books and spell tomes";
			case RE::FormType::AlchemyItem: return "Potions and food";
			case RE::FormType::Ingredient: return "Ingredients";
			case RE::FormType::SoulGem: return "Soul gems";
			case RE::FormType::KeyMaster: return "Keys";
			case RE::FormType::Scroll: return "Scrolls";
			case RE::FormType::Light: return "Torches and lights";
			case RE::FormType::Misc: return "Miscellaneous";
			default: return "Other";
			}
		};
		for (auto& [obj, data] : player->GetInventory()) {
			const auto& [count, entry] = data;
			if (!obj || count <= 0) {
				continue;
			}
			if (IsDynamic(obj)) {
				// potions and poisons the player made: written as their effects
				if (auto* alch = obj->As<RE::AlchemyItem>()) {
					out.push_back({ "potion", NameOf(alch), std::to_string(count), alch->IsPoison() ? "1" : "0", Effects(alch) });
				}
				continue;
			}
			const bool  questItem = entry && entry->IsQuestObject();
			std::int32_t special = 0;
			if (entry && entry->extraLists) {
				for (auto* xl : *entry->extraLists) {
					if (!xl) {
						continue;
					}
					auto* xName = xl->GetByType<RE::ExtraTextDisplayData>();
					auto* xHealth = xl->GetByType<RE::ExtraHealth>();
					auto* xCharge = xl->GetByType<RE::ExtraCharge>();
					auto* xEnch = xl->GetByType<RE::ExtraEnchantment>();
					auto* xPoison = xl->GetByType<RE::ExtraPoison>();
					if (!xName && !xHealth && !xCharge && !xEnch && !xPoison) {
						continue;
					}
					auto*      xCount = xl->GetByType<RE::ExtraCount>();
					const auto n = xCount ? static_cast<std::int32_t>(xCount->count) : 1;
					special += n;
					Record r{ "item" };
					PutForm(r, obj);
					r.push_back(NameOf(obj));
					r.push_back(std::to_string(n));
					r.push_back(xName && !xName->displayNameText && xName->displayName.c_str() ? xName->displayName.c_str() : "");
					r.push_back(xHealth ? F(xHealth->health) : "");
					r.push_back(xCharge ? F(xCharge->charge) : "");
					r.push_back(xEnch ? MagicField(xEnch->enchantment, obj->IsWeapon() ? 'w' : 'a', xEnch->charge) : "");
					r.push_back(xPoison ? MagicField(xPoison->poison, 'p', xPoison->count) : "");
					r.push_back(questItem ? "1" : "0");
					r.push_back(kindOf(obj));
					out.push_back(std::move(r));
				}
			}
			if (count - special > 0) {
				Record r{ "item" };
				PutForm(r, obj);
				r.push_back(NameOf(obj));
				r.push_back(std::to_string(count - special));
				r.insert(r.end(), { "", "", "", "", "", questItem ? "1" : "0", kindOf(obj) });
				out.push_back(std::move(r));
			}
		}

		// what the player wears and holds
		for (auto& [obj, data] : player->GetInventory()) {
			const auto& entry = data.second;
			if (!obj || IsDynamic(obj) || !entry || !entry->extraLists) {
				continue;
			}
			for (auto* xl : *entry->extraLists) {
				if (!xl) {
					continue;
				}
				const bool right = xl->HasType(RE::ExtraDataType::kWorn), left = xl->HasType(RE::ExtraDataType::kWornLeft);
				if (!right && !left) {
					continue;
				}
				auto* xName = xl->GetByType<RE::ExtraTextDisplayData>();
				Record r{ "equip" };
				PutForm(r, obj);
				r.push_back(NameOf(obj));
				r.push_back(left ? "left" : "right");
				r.push_back(xName && !xName->displayNameText && xName->displayName.c_str() ? xName->displayName.c_str() : "");
				out.push_back(std::move(r));
			}
		}
		for (const bool left : { false, true }) {
			auto* f = player->GetEquippedObject(left);
			if (auto* spell = f ? f->As<RE::SpellItem>() : nullptr; spell && !IsDynamic(spell)) {
				Record r{ "equipspell" };
				PutForm(r, spell);
				r.push_back(NameOf(spell));
				r.push_back(left ? "left" : "right");
				out.push_back(std::move(r));
			}
		}
		if (auto* power = player->GetActorRuntimeData().selectedPower; power && !IsDynamic(power)) {
			Record r{ power->Is(RE::FormType::Shout) ? "equipshout" : "equipspell" };
			PutForm(r, power);
			r.push_back(NameOf(power));
			r.push_back("voice");
			out.push_back(std::move(r));
		}

		// the look: RaceMenu saves it as a preset (face, body sliders, overlays...), loaded again by "look"
		if (GetModuleHandleW(L"skee64.dll") || GetModuleHandleW(L"skeevr.dll")) {
			const auto preset = std::format("ReSaverPP_{}_{}", safe, stamp);
			if (auto* vm = VM::GetSingleton()) {
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
				vm->DispatchStaticCall("CharGen", "SavePreset", RE::MakeFunctionArguments(RE::BSFixedString(preset)), none);
				out.push_back({ "look", preset, "RaceMenu" });
			}
		}

		// write
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			a_error = "no folder for the snapshot";
			return {};
		}
		const auto folder = *dir / "ReSaverHelper";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const auto path = folder / std::format("snapshot_{}_{}.txt", safe, stamp);
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		if (!f) {
			a_error = "cannot write " + path.string();
			spdlog::error("Snapshot: {}", a_error);
			return {};
		}
		for (const auto& r : out) {
			f << proto::Join(r) << "\n";
		}
		a_lines = out.size();
		spdlog::info("Snapshot: {} record(s) written to {}", out.size(), path.string());
		return path;
	}

	// ---- apply ------------------------------------------------------------------------------------

	std::string Apply(const Record& a)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || a.empty()) {
			return "no player";
		}
		auto        arg = [&](std::size_t i) { return i < a.size() ? a[i] : std::string(); };
		const auto& kind = a[0];
		auto*       av = player->AsActorValueOwner();

		if (kind == "player") {
			const int level = static_cast<int>(Num(arg(1)));
			if (level > player->GetLevel()) {
				Console(std::format("player.setlevel {}", level));
			}
			// experience and perk points only when the snapshot has them (one read from a save file has the level only)
			if (auto* sk = player->GetInfoRuntimeData().skills; sk && sk->data && !arg(2).empty()) {
				sk->data->xp = Num(arg(2));
				if (Num(arg(3)) > 0) {
					sk->data->levelThreshold = Num(arg(3));
				}
			}
			if (!arg(4).empty()) {
				const auto points = static_cast<std::uint8_t>(std::clamp(static_cast<int>(Num(arg(4))), 0, 255));
				auto&      gs = player->GetGameStatsData();
				if (points > gs.perkCount) {
					gs.perkCount = points;
				}
			}
			return {};
		}
		if (kind == "skill") {
			const int i = static_cast<int>(Num(arg(1)));
			if (i < 0 || i >= kSkills) {
				return "bad skill";
			}
			const float level = Num(arg(2));
			if (level > av->GetBaseActorValue(SkillAV(i))) {
				av->SetBaseActorValue(SkillAV(i), level);
			}
			if (auto* sk = player->GetInfoRuntimeData().skills; sk && sk->data) {
				sk->data->skills[i].xp = Num(arg(3));
				if (Num(arg(4)) > 0) {
					sk->data->skills[i].levelThreshold = Num(arg(4));
				}
				sk->data->legendaryLevels[i] = std::max(sk->data->legendaryLevels[i], static_cast<std::uint32_t>(Num(arg(5))));
			}
			return {};
		}
		if (kind == "av") {
			for (const auto& [name, value] : kAVs) {
				if (arg(1) == name) {
					const float v = Num(arg(2));
					if (v > av->GetBaseActorValue(value)) {
						av->SetBaseActorValue(value, v);
					}
					return {};
				}
			}
			return "unknown value " + arg(1);
		}

		if (kind == "potion") {
			RE::BSTArray<RE::Effect> effs;
			std::string              why;
			if (!MakeEffects(arg(4), effs, why)) {
				return why;
			}
			auto*      mgr = RE::BGSCreatedObjectManager::GetSingleton();
			const int  count = std::max(1, static_cast<int>(Num(arg(2))));
			RE::AlchemyItem* made = nullptr;
			if (arg(3) == "1") {
				RE::BSTSmartPointer<RE::AlchemyItem> p;
				made = mgr ? mgr->AddPoison(p, effs) : nullptr;
				if (!made) {
					made = p.get();
				}
			} else {
				RE::BSTSmartPointer<RE::AlchemyItem, RE::BGSCreatedObjectManager::BSTCreatedObjectSmartPointerPolicy> p;
				if (mgr) {
					mgr->AddPotion(p, effs);
				}
				made = p.get();
			}
			if (!made) {
				return "the game did not make the potion";
			}
			if (!arg(1).empty()) {
				made->fullName = arg(1).c_str();
			}
			player->AddObjectToContainer(made, nullptr, count, nullptr);
			return {};
		}

		if (kind == "follower") {
			// brings a follower back: alive and enabled, next to the player, in the same quest aliases as before
			// (the follower mod's own system), a teammate in the follower faction, carrying at least what they
			// carried. Nothing is taken from them.
			auto* form = Lookup(arg(1), arg(2));
			auto* actor = form ? form->As<RE::Actor>() : nullptr;
			if (!actor) {
				return "not a character in this game";
			}
			auto*       vm = VM::GetSingleton();
			auto*       policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			const auto  h = policy ? policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::Character::FORMTYPE), actor) : 0;
			if (!vm || !h) {
				return "no script object for this character";
			}
			std::string notes;
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			if (actor->IsDisabled()) {
				vm->DispatchMethodCall(h, "Actor", "Enable", RE::MakeFunctionArguments(false), none);
				notes += "enabled; ";
			}
			if (actor->IsDead()) {
				vm->DispatchMethodCall(h, "Actor", "Resurrect", RE::MakeFunctionArguments(), none);
				notes += "brought back to life; ";
			}
			// what they carry
			int added = 0;
			{
				auto have = actor->GetInventoryCounts();
				for (const auto& one : Split(arg(7), '|')) {
					const auto p = Split(one, '*');
					if (p.size() != 3) {
						continue;
					}
					auto* item = Lookup(p[0], p[1]);
					auto* obj = item ? item->As<RE::TESBoundObject>() : nullptr;
					if (!obj) {
						continue;
					}
					const int want = static_cast<int>(Num(p[2]));
					const int now = have.contains(obj) ? have[obj] : 0;
					if (want > now) {
						actor->AddObjectToContainer(obj, nullptr, want - now, nullptr);
						added += want - now;
					}
				}
			}
			if (added) {
				notes += std::format("{} item(s) given back; ", added);
			}
			// the follower faction, and the quest aliases that made them follow
			if (const int rank = static_cast<int>(Num(arg(6))); rank >= 0) {
				if (auto* f = Lookup("Skyrim.esm", "05C84E")) {
					if (auto* fac = f->As<RE::TESFaction>()) {
						actor->AddToFaction(fac, static_cast<std::int8_t>(rank));
					}
				}
			}
			int forced = 0;
			for (const auto& one : Split(arg(5), '|')) {
				const auto p = Split(one, '*');
				if (p.size() != 3) {
					continue;
				}
				auto* qf = Lookup(p[0], p[1]);
				auto* q = qf ? qf->As<RE::TESQuest>() : nullptr;
				if (!q) {
					continue;
				}
				if (!Game::Running(q)) {
					notes += std::format("{}'s quest {} is not running; ", NameOf(actor), q->GetFormEditorID());
					continue;
				}
				q->ForceRefIntoAlias(static_cast<std::uint32_t>(Num(p[2])), actor);
				++forced;
				if (q->GetFormID() == 0x000750BA) {  // the vanilla follower quest: one follower taken
					if (auto* g = Lookup("Skyrim.esm", "0BCCAE")) {
						if (auto* glob = g->As<RE::TESGlobal>()) {
							glob->value = 1.0f;
						}
					}
				}
			}
			if (arg(4) == "1") {
				vm->DispatchMethodCall(h, "Actor", "SetPlayerTeammate", RE::MakeFunctionArguments(true, true), none);
			}
			vm->DispatchMethodCall(h, "Actor", "MoveTo",
				RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(player), 0.0f, 0.0f, 0.0f, true), none);
			vm->DispatchMethodCall(h, "Actor", "EvaluatePackage", RE::MakeFunctionArguments(), none);
			spdlog::info("follower {:08X} ({}): {}{} alias(es) set", actor->GetFormID(), NameOf(actor), notes, forced);
			return {};
		}
		if (kind == "controls") {
			// after the quests: a quest's stage script can leave the player's controls switched off (a scene
			// that never plays here); they come back on a reload only, so they are switched on again now
			Console("EnablePlayerControls");
			return {};
		}
		if (kind == "questobj") {
			// shows the quest's objectives in the journal the way the other save had them (displayed, completed
			// or failed), so a quest brought over appears in the journal. Written straight into the journal list:
			// no Papyrus objective calls, so no on-screen notification for each one (they piled up, doubled
			// and kept the HUD busy for minutes). "start" in field 3: the quest is started first when it is
			// not running (objectives of a quest that is not running are left alone).
			auto* quest = Lookup(arg(1), arg(2));
			auto* q = quest ? quest->As<RE::TESQuest>() : nullptr;
			if (!q) {
				return "not a quest in this game";
			}
			struct Step
			{
				std::int32_t index;
				int          state;
			};
			auto steps = std::make_shared<std::vector<Step>>();
			for (const auto& one : Split(arg(4), ',')) {
				const auto p = Split(one, ':');
				if (p.size() == 2) {
					steps->push_back({ static_cast<std::int32_t>(Num(p[0])), static_cast<int>(Num(p[1])) });
				}
			}
			if (steps->empty()) {
				return {};
			}
			const auto id = q->GetFormID();
			auto       handleOf = [](RE::TESQuest* a_q) -> RE::VMHandle {
                auto* vm = VM::GetSingleton();
                auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
                return policy && a_q ? policy->GetHandleForObject(RE::TESQuest::FORMTYPE, a_q) : 0;
			};
			// at the end: the player's journal list is put right directly (the list the Journal menu reads), so
			// it shows each objective exactly as the other save did. Papyrus cannot do it for a quest that has
			// stopped (a completed one): its objectives end up "completed, not shown" and the journal skips it.
			auto report = [id, steps]() {
				auto*       qq = RE::TESForm::LookupByID<RE::TESQuest>(id);
				auto*       pl = RE::PlayerCharacter::GetSingleton();
				std::string listed;
				int         fixed = 0, added = 0, skipped = 0;
				// only a running quest, or a completed one (its finished objectives), may be listed: a quest that
				// did not start has no people or places set up, and showing its objectives crashes the game when
				// the journal or the compass looks for them
				const bool live = Game::Running(qq);
				const bool finished = qq && qq->IsCompleted();
				if (pl && qq) {
					auto& list = pl->GetPlayerRuntimeData().objectives;
					for (const auto& st : *steps) {
						if (!(st.state & 1)) {
							continue;  // never shown in the other save
						}
						if (!live && !(finished && st.state != 1)) {
							++skipped;
							continue;
						}
						const auto want = static_cast<RE::QUEST_OBJECTIVE_STATE>(st.state);
						RE::BGSQuestObjective* obj = nullptr;
						for (auto* o : qq->objectives) {
							if (o && static_cast<std::int32_t>(o->index) == st.index) {
								obj = o;
								break;
							}
						}
						if (!obj) {
							continue;
						}
						obj->state = want;
						// the quest's saved record must carry its objectives, or the save forgets them (a stage
						// script that shows the same objective afterwards changes nothing, so it does not mark it)
						qq->AddChange(RE::TESQuest::ChangeFlags::kQuestObjectives);
						bool found = false;
						for (auto& io : list) {
							if (io.Objective == obj) {
								found = true;
								if (io.InstanceState != want) {
									io.InstanceState = want;
									++fixed;
								}
							}
						}
						if (!found) {
							list.push_back(RE::BGSInstancedQuestObjective{ obj, qq->currentInstanceID, want });
							++added;
						}
					}
				}
				if (pl && qq) {
					for (const auto& io : pl->GetPlayerRuntimeData().objectives) {
						if (io.Objective && io.Objective->ownerQuest == qq) {
							listed += std::format("{}{}:{}", listed.empty() ? "" : ",", io.Objective->index, static_cast<int>(io.InstanceState));
						}
					}
				}
				spdlog::info("questobj {:08X} ({}): running {}, completed {}, stage {}; journal lists [{}] ({} put right, {} added, {} left out: the quest is not running)", id,
					qq ? qq->GetFormEditorID() : "?", live, finished, qq ? qq->GetCurrentStageID() : 0, listed, fixed, added, skipped);
			};
			if (arg(3) == "start" && !Game::Running(q)) {
				auto* vm = VM::GetSingleton();
				const auto h = handleOf(q);
				auto cb = MakeLater([id, report](const RE::BSScript::Variable& a_r) {
					spdlog::info("questobj {:08X}: Start() returned {}", id, a_r.IsBool() ? (a_r.GetBool() ? "true" : "false") : "?");
					report();  // when it did not start, its objectives are left alone
				});
				if (vm && h && vm->DispatchMethodCall(h, "Quest", "Start", RE::MakeFunctionArguments(), cb)) {
					return {};
				}
				spdlog::info("questobj {:08X}: could not call Start()", id);
				report();
				return {};
			}
			report();
			return {};
		}
		if (kind == "look") {
			if (!GetModuleHandleW(L"skee64.dll") && !GetModuleHandleW(L"skeevr.dll")) {
				return "RaceMenu is not installed in this game";
			}
			auto* vm = VM::GetSingleton();
			if (!vm) {
				return "no script engine";
			}
			// RaceMenu's own preset loading, then the player's 3D is rebuilt so the new look shows
			auto cb = MakeLater([](const RE::BSScript::Variable&) {
				auto* vm2 = VM::GetSingleton();
				auto* pl = RE::PlayerCharacter::GetSingleton();
				auto* policy = vm2 ? vm2->GetObjectHandlePolicy() : nullptr;
				const auto h = policy && pl ? policy->GetHandleForObject(RE::PlayerCharacter::FORMTYPE, pl) : 0;
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
				if (vm2 && h) {
					vm2->DispatchMethodCall(h, "Actor", "QueueNiNodeUpdate", RE::MakeFunctionArguments(), none);
				}
			});
			vm->DispatchStaticCall("CharGen", "LoadPreset", RE::MakeFunctionArguments(RE::BSFixedString(arg(1))), cb);
			return {};
		}

		// everything else names a form
		auto* form = Lookup(arg(1), arg(2));
		if (!form) {
			return arg(2) + " from " + arg(1) + " is not in this game (is the mod installed?)";
		}

		if (kind == "perk") {
			auto* perk = form->As<RE::BGSPerk>();
			if (!perk) {
				return "not a perk";
			}
			if (!player->HasPerk(perk)) {
				player->AddPerk(perk, 0);
			}
			return {};
		}
		if (kind == "spell") {
			auto* spell = form->As<RE::SpellItem>();
			if (!spell) {
				return "not a spell";
			}
			if (!player->HasSpell(spell)) {
				player->AddSpell(spell);
			}
			return {};
		}
		if (kind == "shout") {
			auto* shout = form->As<RE::TESShout>();
			if (!shout) {
				return "not a shout";
			}
			player->AddShout(shout);
			return {};
		}
		if (kind == "word") {
			auto* word = form->As<RE::TESWordOfPower>();
			if (!word) {
				return "not a word of power";
			}
			if (arg(5) == "1" && !(word->formFlags & RE::TESForm::RecordFlags::kKnown)) {
				Papyrus("Game", "TeachWord", word);
			}
			if (arg(6) == "1" && !(word->formFlags & RE::TESForm::RecordFlags::kUnlocked)) {
				player->UnlockWord(word);
			}
			return {};
		}
		if (kind == "faction") {
			auto* faction = form->As<RE::TESFaction>();
			if (!faction) {
				return "not a faction";
			}
			const auto rank = static_cast<std::int8_t>(Num(arg(5)));
			if (player->GetFactionRank(faction, true) < rank) {
				player->AddToFaction(faction, rank);
			}
			return {};
		}
		if (kind == "marker") {
			auto* ref = form->As<RE::TESObjectREFR>();
			auto* xm = ref ? ref->extraList.GetByType<RE::ExtraMapMarker>() : nullptr;
			if (!xm || !xm->mapData) {
				return "not a map marker";
			}
			const int flags = static_cast<int>(Num(arg(5)));
			if (flags & 1) {
				xm->mapData->flags.set(RE::MapMarkerData::Flag::kVisible);
			}
			if (flags & 2) {
				xm->mapData->flags.set(RE::MapMarkerData::Flag::kCanTravelTo);
			}
			return {};
		}
		if (kind == "equip") {
			auto* obj = form->As<RE::TESBoundObject>();
			auto* mgr = RE::ActorEquipManager::GetSingleton();
			if (!obj || !mgr) {
				return "not an item";
			}
			// the copy with the same custom name, when it has one
			RE::ExtraDataList* which = nullptr;
			bool               have = false;
			for (auto& [o, data] : player->GetInventory()) {
				if (o != obj) {
					continue;
				}
				have = data.first > 0;
				if (!arg(6).empty() && data.second && data.second->extraLists) {
					for (auto* xl : *data.second->extraLists) {
						auto* xName = xl ? xl->GetByType<RE::ExtraTextDisplayData>() : nullptr;
						if (xName && xName->displayName.c_str() && arg(6) == xName->displayName.c_str()) {
							which = xl;
						}
					}
				}
			}
			if (!have) {
				return "not in the inventory (bring the item over first)";
			}
			const RE::BGSEquipSlot* slot = nullptr;
			if (obj->IsWeapon()) {
				slot = HandSlot(arg(5) == "left");
			}
			mgr->EquipObject(player, obj, which, 1, slot, true, false, false, false);
			return {};
		}
		if (kind == "equipspell") {
			auto* spell = form->As<RE::SpellItem>();
			auto* mgr = RE::ActorEquipManager::GetSingleton();
			if (!spell || !mgr) {
				return "not a spell";
			}
			if (!player->HasSpell(spell)) {
				return "the spell is not known (bring it over first)";
			}
			const RE::BGSEquipSlot* slot = nullptr;
			if (arg(5) != "voice") {
				slot = HandSlot(arg(5) == "left");
			}
			mgr->EquipSpell(player, spell, slot);
			return {};
		}
		if (kind == "equipshout") {
			auto* shout = form->As<RE::TESShout>();
			auto* mgr = RE::ActorEquipManager::GetSingleton();
			if (!shout || !mgr) {
				return "not a shout";
			}
			mgr->EquipShout(player, shout);
			return {};
		}
		if (kind == "item") {
			auto* obj = form->As<RE::TESBoundObject>();
			if (!obj) {
				return "not an item";
			}
			const int  count = std::max(1, static_cast<int>(Num(arg(5))));
			const auto name = arg(6), temper = arg(7), charge = arg(8), ench = arg(9), poison = arg(10);
			if (name.empty() && temper.empty() && charge.empty() && ench.empty() && poison.empty()) {
				player->AddObjectToContainer(obj, nullptr, count, nullptr);
				return {};
			}
			// one at a time: placed at the player's feet, given its extra data, then picked up (the game moves
			// a picked-up object's extra data into the inventory, the way it does for anything picked up)
			for (int n = 0; n < count; ++n) {
				auto placed = player->PlaceObjectAtMe(obj, false);
				if (!placed) {
					return "the game could not create the item";
				}
				auto* xl = &placed->extraList;
				if (!ench.empty()) {
					const auto p = Split(ench, ':');
					RE::EnchantmentItem* e = nullptr;
					std::uint16_t        amount = 0;
					if (p.size() >= 4 && p[0] == "C") {
						RE::BSTArray<RE::Effect> effs;
						std::string              why;
						const auto               effText = ench.substr(p[0].size() + p[1].size() + p[2].size() + 3);
						if (!MakeEffects(effText, effs, why)) {
							placed->Disable();
							placed->SetDelete(true);
							return "its enchantment: " + why;
						}
						auto* mgr = RE::BGSCreatedObjectManager::GetSingleton();
						e = mgr ? (p[1] == "w" ? mgr->AddWeaponEnchantment(effs) : mgr->AddArmorEnchantment(effs)) : nullptr;
						amount = static_cast<std::uint16_t>(Num(p[2]));
					} else if (p.size() >= 3 && p[0] == "F") {
						auto* f = Lookup(p[1], p[2]);
						e = f ? f->As<RE::EnchantmentItem>() : nullptr;
						amount = p.size() >= 4 ? static_cast<std::uint16_t>(Num(p[3])) : 0;
					}
					if (!e) {
						placed->Disable();
						placed->SetDelete(true);
						return "its enchantment could not be made";
					}
					xl->Add(new RE::ExtraEnchantment(e, amount, false));
				}
				if (!poison.empty()) {
					const auto       p = Split(poison, ':');
					RE::AlchemyItem* pz = nullptr;
					std::int32_t     doses = 1;
					if (p.size() >= 4 && p[0] == "C") {
						RE::BSTArray<RE::Effect> effs;
						std::string              why;
						const auto               effText = poison.substr(p[0].size() + p[1].size() + p[2].size() + 3);
						if (MakeEffects(effText, effs, why)) {
							RE::BSTSmartPointer<RE::AlchemyItem> sp;
							auto*                                mgr = RE::BGSCreatedObjectManager::GetSingleton();
							pz = mgr ? mgr->AddPoison(sp, effs) : nullptr;
							if (!pz) {
								pz = sp.get();
							}
						}
						doses = static_cast<std::int32_t>(Num(p[2]));
					} else if (p.size() >= 3 && p[0] == "F") {
						auto* f = Lookup(p[1], p[2]);
						pz = f ? f->As<RE::AlchemyItem>() : nullptr;
						doses = p.size() >= 4 ? static_cast<std::int32_t>(Num(p[3])) : 1;
					}
					if (pz) {
						xl->Add(new RE::ExtraPoison(pz, std::max(1, doses)));
					}
				}
				if (!temper.empty()) {
					xl->Add(new RE::ExtraHealth(Num(temper)));
				}
				if (!charge.empty()) {
					auto* xc = new RE::ExtraCharge();
					xc->charge = Num(charge);
					xl->Add(xc);
				}
				if (!name.empty()) {
					xl->Add(new RE::ExtraTextDisplayData(name.c_str()));
				}
				player->PickUpObject(placed.get(), 1, false, false);
			}
			return {};
		}
		return "unknown record " + kind;
	}
}
