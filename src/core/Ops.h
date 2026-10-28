// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save editing operations and analysis. Semantics follow ReSaver's Papyrus/ESS operations.
#pragma once

#include "Save.h"

#include <set>
#include <string>
#include <vector>

namespace ess::ops
{
	// ---- element classification (same rules as ReSaver) ---------------------------------
	bool IsNativeScriptName(const std::string& a_name);
	// Engine object types (Form, Actor, Quest, ReferenceAlias, ...). An "instance" of one of these
	// is only the game's link between a form and Papyrus, not a mod script that runs.
	bool IsEngineType(const std::string& a_name);
	bool IsScriptUndefined(const Papyrus& p, const Script& s);
	bool IsInstanceUndefined(const Papyrus& p, TStr a_script);
	bool IsUnattached(const ScriptInstance& s);
	bool IsFrameNative(const StackFrame& f);
	bool IsFrameZeroed(const StackFrame& f);
	bool IsThreadSuspended(const Papyrus& p, ThreadID id);
	bool IsThreadTerminated(const Papyrus& p, const ActiveScript& a);
	bool IsThreadUndefined(const Papyrus& p, const ActiveScript& a);
	bool IsCreatedWithoutForm(SaveFile& s, const ScriptInstance& i);
	// ReSaver's extra filters
	std::vector<std::string> ExtendedMembers(const Papyrus& p, TStr a_script);  // parents' members first
	bool                     IsMemberless(const Papyrus& p, const ScriptInstance& i);  // script has members, instance has no data
	bool                     HasCanary(const Papyrus& p, const ScriptInstance& i);     // ::iPapyrusDataVerification_var
	int                      CanaryValue(const Papyrus& p, const ScriptInstance& i);
	bool                     IsNoParent(const Papyrus& p, const Script& s);       // empty parent, not a base script
	bool                     IsMissingParent(const Papyrus& p, const Script& s);  // parent not defined in the save

	// ---- edits (each returns how many elements it changed) -------------------------------
	std::size_t DeleteInstances(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteReferences(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteArrays(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteThreads(Papyrus& p, const std::set<ThreadID>& a_ids);
	std::size_t TerminateThreads(Papyrus& p, const std::set<ThreadID>& a_ids);
	// Deletes threads together with their suspended stacks and queued function messages.
	std::size_t DeleteThreadsFully(Papyrus& p, const std::set<ThreadID>& a_ids);
	std::size_t DeleteSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids);
	std::size_t DeleteFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices);
	std::size_t DeleteUnbinds(Papyrus& p, const std::set<EID>& a_ids);
	std::size_t DeleteScripts(Papyrus& p, const std::set<TStr>& a_names);  // also deletes their instances
	std::size_t DeleteChangeForms(SaveFile& s, const std::set<std::uint32_t>& a_refIDs);

	// ReSaver "Clean" menu
	std::size_t RemoveUnattachedInstances(Papyrus& p);
	std::size_t RemoveUndefinedElements(Papyrus& p);  // scripts, instances, references; zeroes undefined threads
	std::size_t TerminateUndefinedThreads(Papyrus& p);
	std::size_t RemoveNonexistentCreated(SaveFile& s);

	// Removes every trace of a script that is known to loop forever: its threads (any frame
	// running that script or running on one of its instances), their suspended stacks and
	// function messages, queued calls that pass one of its instances, its instances, and
	// optionally the created references those instances were attached to.
	// a_keepInstances: only remove the running/queued work (threads, stacks, messages) and keep
	// the instances with their data, so properties other scripts hold to them stay valid. Use
	// this for a mod you keep installed (e.g. a stuck MCM registration).
	struct PurgeResult
	{
		std::size_t threads = 0, stacks = 0, messages = 0, instances = 0, forms = 0;
	};
	PurgeResult PurgeScript(SaveFile& s, const std::string& a_scriptName, bool a_deleteCreatedForms, bool a_keepInstances = false);

	// SkyUI MCM registration jam: removes every thread that is registering a menu (any frame in
	// OnConfigManagerReady / OnConfigManagerReset) or running inside SKI_ConfigManager, their
	// suspended stacks and queued calls, and puts SKI_ConfigManager back into its normal state if
	// the save caught it in its "BUSY" state. Menus that were already registered stay registered;
	// SkyUI sends "config manager ready" again on the next load and the rest register then.
	struct McmFixResult
	{
		std::size_t threads = 0, stacks = 0, messages = 0;
		bool        managerFound = false, stateReset = false;
		std::size_t registered = 0;  // menus already registered (non-empty _modConfigs entries)
		std::string text() const;
	};
	std::size_t  CountMcmJam(const Papyrus& p);  // threads that a fix would remove
	McmFixResult FixMcmJam(Papyrus& p, bool a_dryRun = false);

	// ReSaver's plugin actions: delete a plugin's change forms and/or the script instances on its
	// forms (threads running on those instances go with them).
	struct PluginPurge
	{
		std::set<std::uint32_t> changeForms;  // refID.data
		std::set<EID>           instances;
		std::set<ThreadID>      threads;
	};
	PluginPurge SelectPluginElements(const SaveFile& s, const std::set<std::string>& a_plugins, bool a_changeForms, bool a_instances);
	void        ApplyPluginPurge(SaveFile& s, const PluginPurge& a_sel);

	// ---- queries ---------------------------------------------------------------------------
	struct Referrer
	{
		enum class Kind
		{
			kInstance,
			kReference,
			kArray,
			kThread,
			kSuspended,
			kMessage
		} kind;
		std::uint64_t id = 0;
		std::string   where;
	};
	std::vector<Referrer> FindReferrers(const Papyrus& p, EID a_target);

	// ---- analysis ------------------------------------------------------------------------
	struct Count
	{
		std::string name;
		std::size_t n = 0;
	};
	struct Report
	{
		std::size_t scripts = 0, instances = 0, references = 0, arrays = 0, threads = 0, functionMessages = 0, suspended = 0, changeForms = 0, strings = 0;
		std::size_t unattached = 0, undefinedScripts = 0, undefinedInstances = 0, undefinedThreads = 0, terminatedThreads = 0, createdWithoutForm = 0;
		std::vector<Count> threadsByTopScript;      // script of the event frame (frame 0)
		std::vector<Count> threadsByCurrentFrame;   // innermost frame: what the thread is waiting in
		std::vector<Count> suspendedByFunction;
		std::vector<Count> messagesByFunction;
		std::vector<Count> instancesByScript;
		std::vector<Count> changeFormsByType;
		std::vector<std::string> warnings;          // plain-language findings
		std::vector<std::string> loopingScripts;    // scripts with 20+ threads parked in Utility.Wait
		bool                     mcmJam = false;    // SkyUI registration was mid-way (FixMcmJam applies)
	};
	Report Analyze(SaveFile& s);
	std::string FormatReport(const Report& r, std::size_t a_top = 20);

	// Points the save at a plugin that was renamed (Mod.esp -> Mod.esl, ...): the save's plugin list entry is
	// renamed, so the game finds the plugin again by its new name and keeps everything the save holds from it.
	// Only for a rename: if the plugin's records were renumbered (compacted), the save's ids no longer match.
	// Returns "" when done, else why not.
	std::string RenamePlugin(SaveFile& s, const std::string& a_from, const std::string& a_to);
	// Installed plugins that look like a renamed copy of a_missing (same name, other extension).
	std::vector<std::string> RenameCandidates(const SaveFile& s, const std::string& a_missing, const std::vector<std::string>& a_installed);
}
