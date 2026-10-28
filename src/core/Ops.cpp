// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// 本地化修改：界面、提示与报告文字改为简体中文（详见 README.zh-CN.md）。
#include "Ops.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ess::ops
{
	// ---- classification -------------------------------------------------------------------

	bool IsNativeScriptName(const std::string& a_name)
	{
		static const char* natives[] = { "ActiveMagicEffect", "Alias", "Debug", "Form", "Game", "Input", "Math", "ModEvent", "SKSE",
			"StringUtil", "UI", "Utility", "CommonArrayFunctions", "ScriptObject", "InputEnableLayer" };
		for (const char* n : natives) {
			if (EqualsNoCase(a_name, n)) {
				return true;
			}
		}
		return false;
	}

	bool IsEngineType(const std::string& a_name)
	{
		static const char* types[] = { "Action", "Activator", "ActiveMagicEffect", "Actor", "ActorBase", "ActorValueInfo", "Alias", "Ammo",
			"Apparatus", "Armor", "ArmorAddon", "Art", "AssociationType", "Book", "Cell", "Class", "ColorForm", "CombatStyle",
			"ConstructibleObject", "Container", "DefaultObjectManager", "Door", "EffectShader", "Enchantment", "EncounterZone", "EquipSlot",
			"Explosion", "Faction", "Flora", "Form", "FormList", "Furniture", "GlobalVariable", "Hazard", "HeadPart", "Idle", "ImageSpaceModifier",
			"ImpactDataSet", "Ingredient", "Key", "Keyword", "LeveledActor", "LeveledItem", "LeveledSpell", "Light", "Location", "LocationAlias",
			"LocationRefType", "MagicEffect", "Message", "MiscObject", "MusicType", "NetImmerse", "ObjectReference", "Outfit", "Package", "Perk",
			"Potion", "Projectile", "Quest", "Race", "ReferenceAlias", "Scene", "Scroll", "ShaderParticleGeometry", "Shout", "SoulGem", "Sound",
			"SoundCategory", "SoundDescriptor", "Spell", "Static", "TalkingActivator", "TextureSet", "Topic", "TopicInfo", "TreeObject",
			"VisualEffect", "VoiceType", "Weapon", "Weather", "WordOfPower", "WorldSpace", "TESModPlatform", "FootstepSet", "Lightning" };
		for (const char* t : types) {
			if (EqualsNoCase(a_name, t)) {
				return true;
			}
		}
		return false;
	}

	bool IsScriptUndefined(const Papyrus& p, const Script& s)
	{
		// The game writes an empty parent type for scripts whose .pex it could not load.
		if (!p.str(s.parent).empty()) {
			return false;
		}
		return !IsNativeScriptName(p.str(s.name));
	}

	bool IsInstanceUndefined(const Papyrus& p, TStr a_script)
	{
		if (const auto* s = p.findScript(a_script)) {
			return IsScriptUndefined(p, *s);
		}
		return !IsNativeScriptName(p.str(a_script));
	}

	bool IsUnattached(const ScriptInstance& s) { return s.refID.isZero(); }

	bool IsFrameNative(const StackFrame& f) { return (f.fnFlags & 0x02) != 0; }

	bool IsFrameZeroed(const StackFrame& f)
	{
		if (IsFrameNative(f) || f.code.empty()) {
			return false;
		}
		return std::all_of(f.code.begin(), f.code.end(), [](const Opcode& o) { return o.op == 0 && o.params.empty(); });
	}

	bool IsThreadSuspended(const Papyrus& p, ThreadID id)
	{
		for (const auto& s : p.suspended1) {
			if (s.id == id) {
				return true;
			}
		}
		for (const auto& s : p.suspended2) {
			if (s.id == id) {
				return true;
			}
		}
		return false;
	}

	static bool ThreadHasMessage(const Papyrus& p, ThreadID id)
	{
		for (const auto& m : p.functionMessages) {
			if (m.id && *m.id == id) {
				return true;
			}
		}
		return false;
	}

	bool IsThreadTerminated(const Papyrus& p, const ActiveScript& a)
	{
		if (!a.data || a.data->frames.empty() || IsThreadSuspended(p, a.id) || ThreadHasMessage(p, a.id)) {
			return false;
		}
		const auto& first = a.data->frames.front();
		if (IsFrameNative(first) || !IsFrameZeroed(first)) {
			return false;
		}
		return std::all_of(a.data->frames.begin(), a.data->frames.end(), [](const StackFrame& f) { return IsFrameZeroed(f) || IsFrameNative(f); });
	}

	bool IsThreadUndefined(const Papyrus& p, const ActiveScript& a)
	{
		if (!a.data) {
			return false;
		}
		for (const auto& f : a.data->frames) {
			if (IsFrameNative(f)) {
				continue;
			}
			if (const auto* s = p.findScript(f.scriptName); s && IsScriptUndefined(p, *s)) {
				return true;
			}
		}
		return false;
	}

	bool IsCreatedWithoutForm(SaveFile& s, const ScriptInstance& i)
	{
		return i.refID.type() == RefID::Type::kCreated && s.findChangeForm(i.refID) == nullptr;
	}

	// ---- edits -----------------------------------------------------------------------------

	namespace
	{
		template <class Vec, class Pred>
		std::size_t EraseIf(Vec& v, Pred pred)
		{
			const auto before = v.size();
			v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
			return before - v.size();
		}

		template <class ID>
		void PruneOrder(std::vector<ID>& order, const auto& items)
		{
			std::unordered_set<ID> live;
			live.reserve(items.size());
			for (const auto& it : items) {
				live.insert(it.id);
			}
			EraseIf(order, [&](ID id) { return !live.contains(id); });
		}
	}

	std::size_t DeleteInstances(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.instances, [&](const ScriptInstance& s) { return a_ids.contains(s.id); });
		PruneOrder(p.instanceDataOrder, p.instances);
		p.reindex();
		return n;
	}

	std::size_t DeleteReferences(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.references, [&](const Reference& s) { return a_ids.contains(s.id); });
		PruneOrder(p.referenceDataOrder, p.references);
		p.reindex();
		return n;
	}

	std::size_t DeleteArrays(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.arrays, [&](const ArrayInfo& s) { return a_ids.contains(s.id); });
		PruneOrder(p.arrayDataOrder, p.arrays);
		p.reindex();
		return n;
	}

	std::size_t DeleteThreads(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		const auto n = EraseIf(p.activeScripts, [&](const ActiveScript& s) { return a_ids.contains(s.id); });
		PruneOrder(p.activeDataOrder, p.activeScripts);
		p.reindex();
		return n;
	}

	std::size_t TerminateThreads(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		// ReSaver's "terminate": every opcode of every frame becomes NOP, so the thread
		// finishes the moment the game resumes it.
		std::size_t n = 0;
		for (auto& a : p.activeScripts) {
			if (!a_ids.contains(a.id) || !a.data) {
				continue;
			}
			for (auto& f : a.data->frames) {
				for (auto& o : f.code) {
					o.op = 0;
					o.params.clear();
				}
			}
			++n;
		}
		return n;
	}

	std::size_t DeleteThreadsFully(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		std::set<std::size_t> msgs;
		for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
			if (p.functionMessages[i].id && a_ids.contains(*p.functionMessages[i].id)) {
				msgs.insert(i);
			}
		}
		DeleteFunctionMessages(p, msgs);
		DeleteSuspendedStacks(p, a_ids);
		return DeleteThreads(p, a_ids);
	}

	std::size_t DeleteSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		return EraseIf(p.suspended1, [&](const SuspendedStack& s) { return a_ids.contains(s.id); }) +
		       EraseIf(p.suspended2, [&](const SuspendedStack& s) { return a_ids.contains(s.id); });
	}

	std::size_t DeleteFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices)
	{
		std::size_t                  n = 0;
		std::vector<FunctionMessage> keep;
		keep.reserve(p.functionMessages.size());
		for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
			if (a_indices.contains(i)) {
				++n;
			} else {
				keep.push_back(std::move(p.functionMessages[i]));
			}
		}
		p.functionMessages = std::move(keep);
		return n;
	}

	std::size_t DeleteUnbinds(Papyrus& p, const std::set<EID>& a_ids)
	{
		return EraseIf(p.unbinds, [&](const QueuedUnbind& u) { return a_ids.contains(u.id); });
	}

	std::size_t DeleteScripts(Papyrus& p, const std::set<TStr>& a_names)
	{
		std::size_t n = EraseIf(p.scripts, [&](const Script& s) { return a_names.contains(s.name); });
		n += EraseIf(p.instances, [&](const ScriptInstance& s) { return a_names.contains(s.script); });
		PruneOrder(p.instanceDataOrder, p.instances);
		p.reindex();
		return n;
	}

	std::size_t DeleteChangeForms(SaveFile& s, const std::set<std::uint32_t>& a_refIDs)
	{
		const auto n = EraseIf(s.changeForms, [&](const ChangeForm& c) { return a_refIDs.contains(c.refID.data); });
		s.reindexChangeForms();
		return n;
	}

	std::size_t RemoveUnattachedInstances(Papyrus& p)
	{
		std::set<EID> ids;
		for (const auto& i : p.instances) {
			if (IsUnattached(i)) {
				ids.insert(i.id);
			}
		}
		return DeleteInstances(p, ids);
	}

	std::size_t TerminateUndefinedThreads(Papyrus& p)
	{
		std::set<ThreadID> ids;
		for (const auto& a : p.activeScripts) {
			if (IsThreadUndefined(p, a) && !IsThreadTerminated(p, a)) {
				ids.insert(a.id);
			}
		}
		return TerminateThreads(p, ids);
	}

	std::size_t RemoveUndefinedElements(Papyrus& p)
	{
		// Threads first: their "undefined" test needs the script definitions still present.
		std::size_t n = TerminateUndefinedThreads(p);

		std::set<TStr> scripts;
		for (const auto& s : p.scripts) {
			if (IsScriptUndefined(p, s)) {
				scripts.insert(s.name);
			}
		}
		std::set<EID> insts, refs;
		for (const auto& i : p.instances) {
			if (IsInstanceUndefined(p, i.script)) {
				insts.insert(i.id);
			}
		}
		for (const auto& r : p.references) {
			if (IsInstanceUndefined(p, r.script)) {
				refs.insert(r.id);
			}
		}
		n += DeleteInstances(p, insts);
		n += DeleteReferences(p, refs);
		n += DeleteScripts(p, scripts);
		return n;
	}

	std::size_t RemoveNonexistentCreated(SaveFile& s)
	{
		auto* p = s.papyrus();
		if (!p) {
			return 0;
		}
		std::set<EID> ids;
		for (const auto& i : p->instances) {
			if (IsCreatedWithoutForm(s, i)) {
				ids.insert(i.id);
			}
		}
		return DeleteInstances(*p, ids);
	}

	namespace
	{
		bool VarHitsAny(const Variable& v, const std::set<EID>& ids)
		{
			const Variable* x = &v;
			if (x->type == VType::kVariant && x->inner) {
				x = x->inner.get();
			}
			return x->type == VType::kRef && ids.contains(x->value);
		}
		bool VarsHitAny(const std::vector<Variable>& vars, const std::set<EID>& ids)
		{
			for (const auto& v : vars) {
				if (VarHitsAny(v, ids)) {
					return true;
				}
			}
			return false;
		}
	}

	PurgeResult PurgeScript(SaveFile& s, const std::string& a_scriptName, bool a_deleteCreatedForms, bool a_keepInstances)
	{
		PurgeResult r;
		auto*       p = s.papyrus();
		if (!p) {
			return r;
		}
		auto matches = [&](TStr t) { return EqualsNoCase(p->str(t), a_scriptName); };

		std::set<EID>           insts;
		std::set<std::uint32_t> forms;
		for (const auto& i : p->instances) {
			if (matches(i.script)) {
				insts.insert(i.id);
				if (a_deleteCreatedForms && i.refID.type() == RefID::Type::kCreated) {
					forms.insert(i.refID.data);
				}
			}
		}

		// threads running the script, or running any function on one of its instances
		std::set<ThreadID> threads;
		for (const auto& a : p->activeScripts) {
			if (!a.data) {
				continue;
			}
			for (const auto& f : a.data->frames) {
				if (matches(f.scriptName) || VarHitsAny(f.owner, insts)) {
					threads.insert(a.id);
					break;
				}
			}
		}
		// suspended calls: belonging to those threads, of the script itself, called on one of its
		// instances, or (when the instances go) passing one of them as an argument. Queued calls the
		// same way. A thread whose pending or queued call goes is removed with it (it would wait
		// forever), and its own pending and queued calls with it: repeated until nothing changes.
		std::set<std::size_t> msgs;
		for (bool changed = true; changed;) {
			changed = false;
			for (const auto* list : { &p->suspended1, &p->suspended2 }) {
				for (const auto& st : *list) {
					if (threads.contains(st.id)) {
						continue;
					}
					if (st.data && (matches(st.data->scriptName) || VarHitsAny(st.data->unknownVar, insts) ||
									   (!a_keepInstances && VarsHitAny(st.data->vars, insts)))) {
						threads.insert(st.id);
						changed = true;
					}
				}
			}
			for (std::size_t i = 0; i < p->functionMessages.size(); ++i) {
				if (msgs.contains(i)) {
					continue;
				}
				const auto& m = p->functionMessages[i];
				if ((m.id && threads.contains(*m.id)) ||
					(m.data && (matches(m.data->scriptName) || VarHitsAny(m.data->unknownVar, insts) ||
								   (!a_keepInstances && VarsHitAny(m.data->vars, insts))))) {
					msgs.insert(i);
					if (m.id && !threads.contains(*m.id)) {
						threads.insert(*m.id);
						changed = true;
					}
				}
			}
		}

		r.messages = DeleteFunctionMessages(*p, msgs);
		r.stacks = DeleteSuspendedStacks(*p, threads);
		r.threads = DeleteThreads(*p, threads);
		if (!a_keepInstances) {
			r.instances = DeleteInstances(*p, insts);
			if (a_deleteCreatedForms) {
				r.forms = DeleteChangeForms(s, forms);
			}
		}
		return r;
	}

	// ---- ReSaver filters ---------------------------------------------------------------------

	std::vector<std::string> ExtendedMembers(const Papyrus& p, TStr a_script)
	{
		std::vector<const Script*> chain;
		for (const Script* s = p.findScript(a_script); s && chain.size() < 32; s = p.findScript(s->parent)) {
			chain.push_back(s);
			if (p.str(s->parent).empty()) {
				break;
			}
		}
		std::vector<std::string> names;
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			for (const auto& m : (*it)->members) {
				names.push_back(p.str(m.name));
			}
		}
		return names;
	}

	bool IsMemberless(const Papyrus& p, const ScriptInstance& i)
	{
		const bool noVars = !i.data || i.data->vars.empty();
		return noVars && !ExtendedMembers(p, i.script).empty();
	}

	bool HasCanary(const Papyrus& p, const ScriptInstance& i)
	{
		for (const auto& n : ExtendedMembers(p, i.script)) {
			if (n == "::iPapyrusDataVerification_var") {
				return true;
			}
		}
		return false;
	}

	int CanaryValue(const Papyrus& p, const ScriptInstance& i)
	{
		const auto names = ExtendedMembers(p, i.script);
		for (std::size_t k = 0; k < names.size(); ++k) {
			if (names[k] == "::iPapyrusDataVerification_var") {
				if (i.data && k < i.data->vars.size() && i.data->vars[k].type == VType::kInt) {
					return static_cast<std::int32_t>(i.data->vars[k].value);
				}
				return 0;
			}
		}
		return 0;
	}

	bool IsNoParent(const Papyrus& p, const Script& s) { return p.str(s.parent).empty() && !IsNativeScriptName(p.str(s.name)); }

	bool IsMissingParent(const Papyrus& p, const Script& s)
	{
		if (IsNativeScriptName(p.str(s.name))) {
			return false;
		}
		return p.str(s.parent).empty() || !p.findScript(s.parent);
	}

	// ---- plugin actions ----------------------------------------------------------------------

	PluginPurge SelectPluginElements(const SaveFile& s, const std::set<std::string>& a_plugins, bool a_changeForms, bool a_instances)
	{
		PluginPurge sel;
		std::set<std::string> lower;
		for (const auto& n : a_plugins) {
			std::string l = n;
			for (auto& c : l) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			lower.insert(l);
		}
		auto mine = [&](const RefID& id) {
			if (id.isZero() || id.type() == RefID::Type::kCreated) {
				return false;
			}
			auto n = s.pluginOf(s.formIDOf(id));
			for (auto& c : n) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return lower.contains(n);
		};
		if (a_changeForms) {
			for (const auto& c : s.changeForms) {
				if (mine(c.refID)) {
					sel.changeForms.insert(c.refID.data);
				}
			}
		}
		if (const auto* p = s.papyrus(); p && a_instances) {
			for (const auto& i : p->instances) {
				if (mine(i.refID)) {
					sel.instances.insert(i.id);
				}
			}
			for (const auto& a : p->activeScripts) {
				if (!a.data) {
					continue;
				}
				for (const auto& f : a.data->frames) {
					if ((f.owner.type == VType::kRef) && sel.instances.contains(f.owner.value)) {
						sel.threads.insert(a.id);
						break;
					}
				}
			}
		}
		return sel;
	}

	void ApplyPluginPurge(SaveFile& s, const PluginPurge& a_sel)
	{
		if (auto* p = s.papyrus()) {
			DeleteThreadsFully(*p, a_sel.threads);
			DeleteInstances(*p, a_sel.instances);
		}
		DeleteChangeForms(s, a_sel.changeForms);
	}

	// ---- SkyUI MCM registration jam ------------------------------------------------------------

	namespace
	{
		bool IsMcmEvent(const std::string& e) { return EqualsNoCase(e, "OnConfigManagerReady") || EqualsNoCase(e, "OnConfigManagerReset"); }
		bool IsManager(const std::string& s) { return EqualsNoCase(s, "SKI_ConfigManager"); }

		struct McmSel
		{
			std::set<ThreadID>    threads;
			std::set<ThreadID>    stacks;
			std::set<std::size_t> messages;
		};

		McmSel SelectMcm(const Papyrus& p)
		{
			McmSel s;
			for (const auto& a : p.activeScripts) {
				if (!a.data) {
					continue;
				}
				for (const auto& f : a.data->frames) {
					if (IsMcmEvent(p.str(f.event)) || IsManager(p.str(f.scriptName)) || IsManager(p.str(f.baseName))) {
						s.threads.insert(a.id);
						break;
					}
				}
			}
			auto dataHit = [&](const FunctionMessageData& d) { return IsMcmEvent(p.str(d.event)) || IsManager(p.str(d.scriptName)); };
			// a thread whose pending call goes is removed too, or it would wait forever
			for (bool changed = true; changed;) {
				changed = false;
				for (const auto* list : { &p.suspended1, &p.suspended2 }) {
					for (const auto& st : *list) {
						if (!s.stacks.contains(st.id) && (s.threads.contains(st.id) || (st.data && dataHit(*st.data)))) {
							s.stacks.insert(st.id);
							if (p.findActive(st.id) && s.threads.insert(st.id).second) {
								changed = true;
							}
						}
					}
				}
				for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
					const auto& m = p.functionMessages[i];
					if (s.messages.contains(i)) {
						continue;
					}
					if ((m.id && (s.threads.contains(*m.id) || s.stacks.contains(*m.id))) || (m.data && dataHit(*m.data))) {
						s.messages.insert(i);
						if (m.id && p.findActive(*m.id) && s.threads.insert(*m.id).second) {
							changed = true;
						}
					}
				}
			}
			return s;
		}
	}

	std::size_t CountMcmJam(const Papyrus& p) { return SelectMcm(p).threads.size(); }

	McmFixResult FixMcmJam(Papyrus& p, bool a_dryRun)
	{
		McmFixResult r;
		const auto   sel = SelectMcm(p);
		for (auto& i : p.instances) {
			if (!IsManager(p.str(i.script)) || !i.data) {
				continue;
			}
			r.managerFound = true;
			// _modConfigs is the only Object[] array member; count its filled entries
			for (const auto& v : i.data->vars) {
				if (v.type == VType::kRefArray) {
					if (const auto* a = p.findArray(v.value)) {
						for (const auto& e : a->vars) {
							r.registered += e.value != 0;
						}
					}
				}
			}
			if (EqualsNoCase(p.str(i.data->state), "BUSY")) {
				r.stateReset = true;
				if (!a_dryRun) {
					i.data->state = p.internString("");
				}
			}
		}
		if (a_dryRun) {
			r.threads = sel.threads.size();
			r.stacks = sel.stacks.size();
			r.messages = sel.messages.size();
			return r;
		}
		r.messages = DeleteFunctionMessages(p, sel.messages);
		std::set<ThreadID> st = sel.stacks;
		st.insert(sel.threads.begin(), sel.threads.end());
		r.stacks = DeleteSuspendedStacks(p, st);
		r.threads = DeleteThreads(p, sel.threads);
		return r;
	}

	std::string McmFixResult::text() const
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), "%zu 个注册线程、%zu 个挂起堆栈、%zu 个排队调用%s%s", threads, stacks, messages,
			stateReset ? "；SKI_ConfigManager 已从卡住的 BUSY 状态中解除" : "",
			managerFound ? "" : "（这个存档里没有找到 SKI_ConfigManager）");
		return buf;
	}

	// ---- queries -----------------------------------------------------------------------------

	static bool VarRefersTo(const Variable& v, EID target)
	{
		if (v.type == VType::kVariant) {
			return v.inner && VarRefersTo(*v.inner, target);
		}
		return (v.type == VType::kRef || v.type == VType::kStruct || IsArrayType(v.type)) && v.value == target;
	}

	std::vector<Referrer> FindReferrers(const Papyrus& p, EID a_target)
	{
		std::vector<Referrer> out;
		auto                  scanVars = [&](const std::vector<Variable>& vars, Referrer::Kind k, std::uint64_t id, const std::string& owner) {
            for (std::size_t i = 0; i < vars.size(); ++i) {
                if (VarRefersTo(vars[i], a_target)) {
                    out.push_back({ k, id, owner + " 变量[" + std::to_string(i) + "]" });
                }
            }
		};
		for (const auto& i : p.instances) {
			if (i.id != a_target && i.data) {
				scanVars(i.data->vars, Referrer::Kind::kInstance, i.id, p.str(i.script));
			}
		}
		for (const auto& r : p.references) {
			if (r.id != a_target && r.data) {
				scanVars(r.data->vars, Referrer::Kind::kReference, r.id, p.str(r.script));
			}
		}
		for (const auto& a : p.arrays) {
			if (a.id != a_target) {
				scanVars(a.vars, Referrer::Kind::kArray, a.id, std::string("数组 ") + VTypeName(a.type));
			}
		}
		for (const auto& t : p.activeScripts) {
			if (!t.data) {
				continue;
			}
			for (const auto& f : t.data->frames) {
				const std::string where = p.str(f.baseName) + "." + p.str(f.event);
				if (VarRefersTo(f.owner, a_target)) {
					out.push_back({ Referrer::Kind::kThread, t.id, where + "（所属对象）" });
				}
				scanVars(f.vars, Referrer::Kind::kThread, t.id, where);
			}
		}
		return out;
	}

	// ---- analysis ---------------------------------------------------------------------------

	namespace
	{
		std::vector<Count> Sorted(const std::unordered_map<std::string, std::size_t>& m)
		{
			std::vector<Count> v;
			v.reserve(m.size());
			for (const auto& [k, n] : m) {
				v.push_back({ k, n });
			}
			std::sort(v.begin(), v.end(), [](const Count& a, const Count& b) { return a.n != b.n ? a.n > b.n : a.name < b.name; });
			return v;
		}
		std::string FrameName(const Papyrus& p, const StackFrame& f) { return p.str(f.baseName) + "." + p.str(f.event); }
		std::string MsgName(const Papyrus& p, const FunctionMessageData& d) { return p.str(d.scriptName) + "." + p.str(d.event); }
	}

	Report Analyze(SaveFile& s)
	{
		Report r;
		r.changeForms = s.changeForms.size();
		std::unordered_map<std::string, std::size_t> cfTypes;
		for (const auto& c : s.changeForms) {
			cfTypes[ChangeFormTypeName(c.typeCode())]++;
		}
		r.changeFormsByType = Sorted(cfTypes);

		const auto* pp = s.papyrus();
		if (!pp) {
			r.warnings.push_back("这个存档里没有 Papyrus 段。");
			return r;
		}
		const auto& p = *pp;
		r.scripts = p.scripts.size();
		r.instances = p.instances.size();
		r.references = p.references.size();
		r.arrays = p.arrays.size();
		r.threads = p.activeScripts.size();
		r.functionMessages = p.functionMessages.size();
		r.suspended = p.suspended1.size() + p.suspended2.size();
		r.strings = p.strings.size();

		for (const auto& sc : p.scripts) {
			if (IsScriptUndefined(p, sc)) {
				++r.undefinedScripts;
			}
		}

		std::unordered_map<std::string, std::size_t> inst, top, cur, susp, msg;
		for (const auto& i : p.instances) {
			inst[p.str(i.script)]++;
			if (IsUnattached(i)) {
				++r.unattached;
			}
			if (IsInstanceUndefined(p, i.script)) {
				++r.undefinedInstances;
			}
			if (IsCreatedWithoutForm(s, i)) {
				++r.createdWithoutForm;
			}
		}

		std::size_t waitLoops = 0;
		std::unordered_map<std::string, std::size_t> waitByScript;
		for (const auto& a : p.activeScripts) {
			if (!a.data || a.data->frames.empty()) {
				top["<无栈帧>"]++;
				continue;
			}
			top[p.str(a.data->frames.front().scriptName)]++;
			const auto& inner = a.data->frames.back();
			cur[FrameName(p, inner)]++;
			if (IsThreadUndefined(p, a)) {
				++r.undefinedThreads;
			}
			if (IsThreadTerminated(p, a)) {
				++r.terminatedThreads;
			}
			const auto innerName = FrameName(p, inner);
			if (EqualsNoCase(innerName, "utility.Wait") || EqualsNoCase(innerName, "utility.WaitGameTime") || EqualsNoCase(innerName, "utility.WaitMenuMode")) {
				++waitLoops;
				waitByScript[p.str(a.data->frames.front().scriptName)]++;
			}
		}
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& st : *list) {
				susp[st.data ? MsgName(p, *st.data) : "<无数据>"]++;
			}
		}
		for (const auto& m : p.functionMessages) {
			msg[m.data ? MsgName(p, *m.data) : "<无数据>"]++;
		}

		r.instancesByScript = Sorted(inst);
		r.threadsByTopScript = Sorted(top);
		r.threadsByCurrentFrame = Sorted(cur);
		r.suspendedByFunction = Sorted(susp);
		r.messagesByFunction = Sorted(msg);

		// ---- plain-language findings ----
		char buf[512];
		for (const auto& c : Sorted(waitByScript)) {
			if (c.n >= 20) {
				r.loopingScripts.push_back(c.name);
				std::snprintf(buf, sizeof(buf),
					"%zu 个 '%s' 的线程正睡在 Utility.Wait 循环里。同一个脚本攒出这么多份，通常意味着"
					"这个脚本根本不会退出（每一份每秒都会醒来好几次）。建议清除 '%s'。",
					c.n, c.name.c_str(), c.name.c_str());
				r.warnings.push_back(buf);
			}
		}
		std::size_t mcmThreads = 0;
		for (const auto& a : p.activeScripts) {
			if (a.data && !a.data->frames.empty() && EqualsNoCase(p.str(a.data->frames.front().event), "OnConfigManagerReady")) {
				++mcmThreads;
			}
		}
		std::size_t registerMod = 0;
		for (const auto& c : r.suspendedByFunction) {
			if (EqualsNoCase(c.name, "SKI_ConfigManager.RegisterMod")) {
				registerMod = c.n;
			}
		}
		bool busy = false;
		for (const auto& i : p.instances) {
			if (i.data && EqualsNoCase(p.str(i.script), "SKI_ConfigManager") && EqualsNoCase(p.str(i.data->state), "BUSY")) {
				busy = true;
			}
		}
		r.mcmJam = mcmThreads > 0 && (mcmThreads >= 10 || registerMod >= 10 || busy);
		if (mcmThreads >= 10 || registerMod >= 10) {
			std::snprintf(buf, sizeof(buf),
				"做这个存档的时候，SkyUI MCM 的注册还没跑完：有 %zu 个菜单卡在 OnConfigManagerReady 里，另外还有 "
				"%zu 个 RegisterMod 调用在排队%s。读档时这些会全部重放一遍。修复办法：清理 > 修复 SkyUI MCM 注册卡死"
				"（它会清掉卡住的注册工作；下次读档时 SkyUI 会重新注册剩下的菜单）。",
				mcmThreads, registerMod, busy ? "，而且 SKI_ConfigManager 存档时正处于 BUSY 状态" : "");
			r.warnings.push_back(buf);
		}
		if (r.suspended >= 50) {
			std::snprintf(buf, sizeof(buf), "有 %zu 个挂起堆栈在排队（正常只有几个）。", r.suspended);
			r.warnings.push_back(buf);
		}
		if (r.threads >= 300) {
			std::snprintf(buf, sizeof(buf), "存档里保存了 %zu 个活动脚本线程（健康的存档一般在 150 个以下）。", r.threads);
			r.warnings.push_back(buf);
		}
		if (r.undefinedInstances || r.undefinedThreads) {
			std::snprintf(buf, sizeof(buf),
				"有 %zu 个脚本实例和 %zu 个线程属于已经不再安装的脚本。用“清理 > 移除未定义元素”可以修复。",
				r.undefinedInstances, r.undefinedThreads);
			r.warnings.push_back(buf);
		}
		if (r.unattached >= 100) {
			std::snprintf(buf, sizeof(buf), "有 %zu 个脚本实例没有附加到任何对象上（清理 > 移除未附加实例）。", r.unattached);
			r.warnings.push_back(buf);
		}
		if (r.instances >= 150000) {
			std::snprintf(buf, sizeof(buf),
				"%zu 个脚本实例非常多；每次读档都得把它们重建一遍，导致脚本引擎读档后要很久才能稳定下来。",
				r.instances);
			r.warnings.push_back(buf);
		}
		return r;
	}

	std::string FormatReport(const Report& r, std::size_t a_top)
	{
		std::string out;
		char        buf[512];
		std::snprintf(buf, sizeof(buf),
			"脚本 %zu | 实例 %zu | 引用 %zu | 数组 %zu | 线程 %zu | 函数消息 %zu | 挂起堆栈 %zu | 变更表单 %zu | 字符串 %zu\n"
			"未附加实例 %zu | 未定义脚本 %zu | 未定义实例 %zu | 未定义线程 %zu | 已终止线程 %zu | 位于缺失生成对象上的实例 %zu\n",
			r.scripts, r.instances, r.references, r.arrays, r.threads, r.functionMessages, r.suspended, r.changeForms, r.strings, r.unattached,
			r.undefinedScripts, r.undefinedInstances, r.undefinedThreads, r.terminatedThreads, r.createdWithoutForm);
		out += buf;
		if (!r.warnings.empty()) {
			out += "\n诊断结论\n";
			for (const auto& w : r.warnings) {
				out += "  * " + w + "\n";
			}
		}
		auto section = [&](const char* title, const std::vector<Count>& v) {
			if (v.empty()) {
				return;
			}
			out += std::string("\n") + title + "\n";
			for (std::size_t i = 0; i < v.size() && i < a_top; ++i) {
				std::snprintf(buf, sizeof(buf), "  %7zu  %s\n", v[i].n, v[i].name.c_str());
				out += buf;
			}
		};
		section("按事件脚本统计的线程（第 0 帧）", r.threadsByTopScript);
		section("按当前栈帧统计的线程（它们正等在哪里）", r.threadsByCurrentFrame);
		section("挂起堆栈", r.suspendedByFunction);
		section("函数消息", r.messagesByFunction);
		section("按脚本统计的脚本实例", r.instancesByScript);
		section("按类型统计的变更表单", r.changeFormsByType);
		return out;
	}

	namespace
	{
		std::string LowerName(std::string a)
		{
			for (auto& c : a) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a;
		}
		std::string Stem(const std::string& a)
		{
			const auto dot = a.find_last_of('.');
			return LowerName(dot == std::string::npos ? a : a.substr(0, dot));
		}
		bool PluginExt(const std::string& a)
		{
			const auto l = LowerName(a);
			return l.size() > 4 && (l.ends_with(".esp") || l.ends_with(".esm") || l.ends_with(".esl"));
		}
	}

	std::string RenamePlugin(SaveFile& s, const std::string& a_from, const std::string& a_to)
	{
		if (!PluginExt(a_to) || a_to.size() > 255 || a_to.find_first_of("\\/:*?\"<>|") != std::string::npos) {
			return "「" + a_to + "」不是插件文件名（.esp、.esm 或 .esl）";
		}
		std::string* entry = nullptr;
		for (auto* list : { &s.plugins.full, &s.plugins.lite }) {
			for (auto& p : *list) {
				if (LowerName(p) == LowerName(a_to) && LowerName(p) != LowerName(a_from)) {
					return "存档里已经有一个名为「" + a_to + "」的插件：没有做任何修改";
				}
				if (LowerName(p) == LowerName(a_from)) {
					entry = &p;
				}
			}
		}
		if (!entry) {
			return "存档里没有名为「" + a_from + "」的插件";
		}
		*entry = a_to;
		return {};
	}

	std::vector<std::string> RenameCandidates(const SaveFile& s, const std::string& a_missing, const std::vector<std::string>& a_installed)
	{
		std::set<std::string> inSave;
		for (const auto* list : { &s.plugins.full, &s.plugins.lite }) {
			for (const auto& p : *list) {
				inSave.insert(LowerName(p));
			}
		}
		std::vector<std::string> out;
		const auto stem = Stem(a_missing);
		for (const auto& p : a_installed) {
			if (!inSave.contains(LowerName(p)) && LowerName(p) != LowerName(a_missing) && Stem(p) == stem && PluginExt(p)) {
				out.push_back(p);
			}
		}
		return out;
	}
}
