// ReSaver++ unit and regression tests that need no save files: each builds the data it needs in memory.
// Run by CTest (rs_unit). Exit code 0 = all passed.
#include "core/ModRemove.h"
#include "core/Ops.h"
#include "core/Pex.h"
#include "core/Save.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace ess;

namespace
{
	int g_fail = 0, g_pass = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (a_ok) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	Variable Ref(EID a_id, VType a_type = VType::kRef)
	{
		Variable v;
		v.type = a_type;
		v.value = a_id;
		return v;
	}

	// A save with plugins Skyrim.esm, ModA.esp, ModB.esp and an empty Papyrus section.
	std::unique_ptr<SaveFile> MakeSave()
	{
		auto s = std::make_unique<SaveFile>();
		s->plugins.full = { "Skyrim.esm", "ModA.esp", "ModB.esp" };
		s->formIDs = { 0x01000800, 0x02000800 };  // RefID form index 1 -> ModA, 2 -> ModB
		GlobalData g;
		g.type = 1001;
		g.papyrus = std::make_unique<Papyrus>();
		s->table3.push_back(std::move(g));
		auto& p = *s->papyrus();
		p.strings = { "", "ScriptA", "ScriptB", "Quest", "OnUpdate", "x" };
		p.saveFileVersion = 4;  // always present in game saves
		for (TStr n : { 1u, 2u }) {
			Script sc;
			sc.name = n;
			sc.parent = 3;
			p.scripts.push_back(sc);
		}
		return s;
	}

	ScriptInstance Instance(EID a_id, TStr a_script, std::uint32_t a_refData)
	{
		ScriptInstance i;
		i.id = a_id;
		i.script = a_script;
		i.refID.data = a_refData;
		i.data = InstanceData{};
		return i;
	}

	ActiveScript Thread(ThreadID a_id, TStr a_script, const Variable& a_owner, std::vector<Variable> a_vars = {})
	{
		ActiveScript a;
		a.id = a_id;
		a.data = ActiveScriptData{};
		StackFrame f;
		f.scriptName = a_script;
		f.event = 4;
		f.owner = a_owner;
		f.vars = std::move(a_vars);
		a.data->frames.push_back(std::move(f));
		return a;
	}

	FunctionMessageData Call(TStr a_script, const Variable& a_receiver, std::vector<Variable> a_args = {})
	{
		FunctionMessageData d;
		d.scriptName = a_script;
		d.event = 4;
		d.unknownVar = a_receiver;
		d.vars = std::move(a_args);
		return d;
	}

	ArrayInfo Array(EID a_id)
	{
		ArrayInfo a;
		a.id = a_id;
		a.type = VType::kIntArray;
		a.length = 0;
		a.hasData = true;
		return a;
	}

	// ---- tests ------------------------------------------------------------------------------

	void TestFloatLiteral()
	{
		for (float f : { 0.0f, 1.0f, 0.1f, 3.25f, -2.5f, 1e-10f, 1.5e-38f, 1e30f, 123456789.0f, 1.4e-45f, 0.0000001f }) {
			const auto s = pex::FloatLiteral(f);
			char       what[160];
			std::snprintf(what, sizeof(what), "FloatLiteral(%g) = %s reads back exactly, no exponent", f, s.c_str());
			Check(std::strtof(s.c_str(), nullptr) == f && s.find_first_of("eE") == std::string::npos, what);
		}
	}

	void TestCreationClub()
	{
		Check(modremove::IsCreationClubPlugin("ccbgssse001-fish.esm"), "ccbgssse001-fish.esm is Creation Club");
		Check(modremove::IsCreationClubPlugin("ccqdrsse001-survivalmode.esl"), "ccqdrsse001-survivalmode.esl is Creation Club");
		Check(modremove::IsCreationClubPlugin("ccasvsse001-almsivi.esm"), "ccasvsse001-almsivi.esm is Creation Club");
		Check(!modremove::IsCreationClubPlugin("ccuserpatch.esp"), "ccuserpatch.esp is NOT Creation Club");
		Check(!modremove::IsCreationClubPlugin("cc.esp"), "cc.esp is NOT Creation Club");
	}

	void TestCheck()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		p.instances.push_back(Instance(10, 1, 1));
		p.instanceDataOrder = { 10 };
		p.reindex();
		Check(p.check().empty(), "a sound Papyrus section has no problems");
		p.instances[0].script = 999;
		Check(!p.check().empty(), "a bad string index is reported");
		p.instances[0].script = 1;
		p.instances.push_back(Instance(10, 2, 2));
		p.instanceDataOrder.push_back(10);
		Check(!p.check().empty(), "a duplicate instance id is reported");
	}

	// review item 3: a queued call removed with the purged script takes its waiting thread with it
	void TestPurgeKeepsThreadsConsistent()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		p.instances.push_back(Instance(10, 1, 1));  // ScriptA on a ModA object
		p.instanceDataOrder = { 10 };
		// thread 7 runs ScriptB and waits on a queued call to ScriptA
		p.activeScripts.push_back(Thread(7, 2, Ref(0)));
		p.activeDataOrder = { 7 };
		FunctionMessage m;
		m.flag = 0;
		m.id = 7;
		m.flags = 1;
		m.data = Call(1, Ref(10));
		p.functionMessages.push_back(m);
		p.reindex();
		const auto r = ops::PurgeScript(*s, "ScriptA", false);
		Check(r.messages == 1, "purge removes the queued call");
		Check(r.threads == 1 && p.activeScripts.empty(), "purge removes the thread waiting on that call (no orphaned thread)");
	}

	// review item 1: an array a surviving waiting/queued call still uses is not deleted
	void TestRemoveKeepsSharedArrays()
	{
		for (int where = 0; where < 2; ++where) {
			auto  s = MakeSave();
			auto& p = *s->papyrus();
			auto  inst = Instance(10, 1, 1);  // ScriptA on a ModA object, holding array 50
			inst.data->vars.push_back(Ref(50, VType::kIntArray));
			p.instances.push_back(inst);
			p.instanceDataOrder = { 10 };
			p.arrays.push_back(Array(50));
			p.arrayDataOrder = { 50 };
			// a ModB call that stays passes the same array
			p.instances.push_back(Instance(11, 2, 2));  // ScriptB on a ModB object
			p.instanceDataOrder.push_back(11);
			if (where == 0) {
				SuspendedStack st;
				st.id = 9;
				st.flag = 1;
				st.data = Call(2, Ref(11), { Ref(50, VType::kIntArray) });
				p.suspended1.push_back(st);
			} else {
				FunctionMessage m;
				m.flag = 3;
				m.flags = 1;
				m.data = Call(2, Ref(11), { Ref(50, VType::kIntArray) });
				p.functionMessages.push_back(m);
			}
			p.reindex();
			modremove::Options o;
			const auto plan = modremove::MakePlan(*s, "ModA.esp", nullptr, o);
			Check(plan.instances.contains(10) && !plan.instances.contains(11), "removing ModA removes its instance only");
			Check(!plan.arrays.contains(50), where == 0 ? "an array a surviving suspended call uses is kept"
														: "an array a surviving queued call uses is kept");
		}
		// and one nothing else uses is removed
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		auto  inst = Instance(10, 1, 1);
		inst.data->vars.push_back(Ref(50, VType::kIntArray));
		p.instances.push_back(inst);
		p.instanceDataOrder = { 10 };
		p.arrays.push_back(Array(50));
		p.arrayDataOrder = { 50 };
		p.reindex();
		const auto plan = modremove::MakePlan(*s, "ModA.esp", nullptr, modremove::Options{});
		Check(plan.arrays.contains(50), "an array only the removed instance used is removed");
	}

	// Papyrus section: write, read back, write again gives the same bytes
	void TestPapyrusRoundTrip()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		auto  inst = Instance(10, 1, 1);
		inst.data->vars.push_back(Ref(50, VType::kIntArray));
		p.instances.push_back(inst);
		p.instanceDataOrder = { 10 };
		p.arrays.push_back(Array(50));
		p.arrayDataOrder = { 50 };
		p.reindex();
		Writer w1;
		p.write(w1);
		Papyrus q;
		Reader  r(w1.buf.data(), w1.buf.size());
		q.read(r);
		Writer w2;
		q.write(w2);
		Check(w1.buf == w2.buf, "a Papyrus section writes back byte-identical after a read");
		Check(q.check().empty(), "the re-read section has no problems");
	}
}

int main()
{
	TestFloatLiteral();
	TestCreationClub();
	TestCheck();
	TestPurgeKeepsThreadsConsistent();
	TestRemoveKeepsSharedArrays();
	TestPapyrusRoundTrip();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
