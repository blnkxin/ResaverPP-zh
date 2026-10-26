#include "PCH.h"
#include "Game.h"
#include "Settings.h"

namespace Game
{
	namespace
	{
		std::atomic<bool> g_inGame{ false };

		using VM = RE::BSScript::Internal::VirtualMachine;

		// calls a_fn with the Papyrus function's return value
		class Callback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Callback(std::function<void(const RE::BSScript::Variable&)> a_fn) :
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

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeCallback(std::function<void(const RE::BSScript::Variable&)> a_fn)
		{
			return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Callback(std::move(a_fn)) };
		}

		RE::VMHandle HandleOf(RE::TESQuest* a_quest)
		{
			auto* vm = VM::GetSingleton();
			if (!vm || !a_quest) {
				return 0;
			}
			auto* policy = vm->GetObjectHandlePolicy();
			return policy ? policy->GetHandleForObject(RE::TESQuest::FORMTYPE, a_quest) : 0;
		}

		// runs a console command (main thread)
		void Console(const std::string& a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto*      script = factory ? factory->Create() : nullptr;
			if (!script) {
				spdlog::error("console: could not create a script for '{}'", a_command);
				return;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
			spdlog::info("console: {}", a_command);
		}

		// ---- journal refresh ----------------------------------------------------------------------
		// One quest at a time: all of its shown objectives are hidden, then shown again, then their
		// completed / failed state is put back, before the next quest is touched. So at any moment at
		// most one quest is in flux, and nothing happens while a menu is open (the Journal remembers what
		// it showed; changing its list under it is what crashed in version 3 with some setups).
		struct Obj
		{
			std::uint16_t index = 0;
			std::uint8_t  state = 0;  // 1 shown, 3 completed + shown, 5 failed + shown
		};
		struct QuestItem
		{
			RE::FormID       quest = 0;
			std::vector<Obj> objs;
		};
		std::vector<QuestItem>    g_quests;
		std::size_t               g_total = 0;
		std::atomic<std::size_t>  g_done{ 0 }, g_failedCalls{ 0 };
		std::atomic<bool>         g_refreshing{ false };
		std::string               g_refreshNotify;

		void Finish()
		{
			g_refreshing = false;
			const auto msg = std::format("ReSaverHelper: quest journal refreshed ({} objective(s) in {} quest(s))", g_done.load(), g_quests.size());
			spdlog::info("{} ({} call(s) failed)", msg, g_failedCalls.load());
			Notify(g_refreshNotify, msg);
		}

		// a menu is open (Journal, pause menu, inventory...): wait
		bool MenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::JournalMenu::MENU_NAME));
		}

		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj);

		// runs Step later on the main thread (while a menu is open)
		void StepLater(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			std::thread([a_quest, a_phase, a_obj] {
				std::this_thread::sleep_for(400ms);
				if (auto* task = SKSE::GetTaskInterface()) {
					task->AddTask([a_quest, a_phase, a_obj] { Step(a_quest, a_phase, a_obj); });
				}
			}).detach();
		}

		template <class... Args>
		void CallThen(std::size_t a_quest, int a_phase, std::size_t a_nextObj, const char* a_fn, Args... a_args)
		{
			auto*      quest = RE::TESForm::LookupByID<RE::TESQuest>(g_quests[a_quest].quest);
			auto*      vm = VM::GetSingleton();
			const auto handle = HandleOf(quest);
			auto       cb = MakeCallback([a_quest, a_phase, a_nextObj](const RE::BSScript::Variable&) { Step(a_quest, a_phase, a_nextObj); });
			if (!vm || !handle || !vm->DispatchMethodCall(handle, "Quest", a_fn, RE::MakeFunctionArguments(std::move(a_args)...), cb)) {
				++g_failedCalls;
				Step(a_quest, a_phase, a_nextObj);
			}
		}

		// phase 0: hide each shown objective; 1: show each again; 2: put completed / failed back; then the next quest
		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			if (a_quest >= g_quests.size()) {
				Finish();
				return;
			}
			if (a_phase == 0 && a_obj == 0 && MenuOpen()) {
				StepLater(a_quest, a_phase, a_obj);  // a quest is only started with every menu closed
				return;
			}
			const auto& objs = g_quests[a_quest].objs;
			if (a_obj >= objs.size()) {
				if (a_phase < 2) {
					Step(a_quest, a_phase + 1, 0);
				} else {
					g_done += objs.size();
					Step(a_quest + 1, 0, 0);
				}
				return;
			}
			const auto idx = static_cast<std::int32_t>(objs[a_obj].index);
			switch (a_phase) {
			case 0:
				CallThen(a_quest, 0, a_obj + 1, "SetObjectiveDisplayed", idx, false, false);
				return;
			case 1:
				CallThen(a_quest, 1, a_obj + 1, "SetObjectiveDisplayed", idx, true, false);
				return;
			default:
				if (objs[a_obj].state == 3) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveCompleted", idx, true);
				} else if (objs[a_obj].state == 5) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveFailed", idx, true);
				} else {
					Step(a_quest, 2, a_obj + 1);
				}
				return;
			}
		}
	}

	void SetInGame(bool a_inGame) { g_inGame = a_inGame; }
	bool InGame() { return g_inGame; }

	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto task = SKSE::GetTaskInterface();
		if (!task) {
			return false;
		}
		auto done = std::make_shared<std::promise<void>>();
		auto fut = done->get_future();
		task->AddTask([fn = std::move(a_fn), done]() {
			try {
				fn();
			} catch (const std::exception& e) {
				spdlog::error("main thread task failed: {}", e.what());
			} catch (...) {
				spdlog::error("main thread task failed (unknown error)");
			}
			try {
				done->set_value();
			} catch (...) {
			}
		});
		return fut.wait_for(a_timeout) == std::future_status::ready;
	}

	void Notify(const std::string& a_mode, const std::string& a_text)
	{
		const auto mode = (a_mode.empty() || a_mode == "ini") ? Settings::Get().notify : a_mode;
		if (mode == "none") {
			return;
		}
		auto task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}
		task->AddTask([mode, a_text]() {
			auto* vm = VM::GetSingleton();
			if (!vm) {
				return;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			vm->DispatchStaticCall("Debug", mode == "box" ? "MessageBox" : "Notification", RE::MakeFunctionArguments(RE::BSFixedString(a_text)), none);
		});
	}

	QuestInfo GetQuest(std::uint32_t a_formID)
	{
		QuestInfo info;
		auto*     quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			return info;
		}
		info.found = true;
		info.editorID = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		info.flags = static_cast<std::uint16_t>(quest->data.flags.underlying());
		info.currentStage = quest->GetCurrentStageID();
		info.running = Running(quest);
		info.completed = quest->IsCompleted();
		for (auto* o : quest->objectives) {
			if (o) {
				info.objectives.push_back({ o->index, static_cast<std::uint8_t>(o->state.underlying()) });
			}
		}
		return info;
	}

	bool Running(const RE::TESQuest* a_quest)
	{
		return a_quest && a_quest->IsEnabled() && !a_quest->IsStopping();
	}

	void SetStage(std::uint32_t a_formID, std::uint16_t a_stage, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback([a_done, a_formID, a_stage](const RE::BSScript::Variable& a_result) {
            const bool ok = a_result.IsBool() ? a_result.GetBool() : true;
            auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
            spdlog::info("SetStage {:08X} stage {}: {} (running {}, completed {}, current stage {})", a_formID, a_stage, ok ? "accepted" : "refused",
                q && Running(q), q && q->IsCompleted(), q ? q->GetCurrentStageID() : 0);
            a_done(ok);
        });
		if (vm && handle && vm->DispatchMethodCall(handle, "Quest", "SetStage", RE::MakeFunctionArguments(static_cast<std::int32_t>(a_stage)), cb)) {
			spdlog::info("SetStage {:08X} ({}) stage {} dispatched", a_formID, quest->GetFormEditorID(), a_stage);
			return;
		}
		// no script object for this quest: the console command does the same
		const std::string edid = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		if (edid.empty()) {
			a_done(false);
			return;
		}
		Console(std::format("setstage {} {}", edid, a_stage));
		a_done(quest->GetCurrentStageID() == a_stage);
	}

	void Complete(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Complete {:08X} ({}): running {}, completed {}, current stage {}", a_formID, quest->GetFormEditorID(), Running(quest), quest->IsCompleted(),
			quest->GetCurrentStageID());
		// last resort, on the main thread: the console's CompleteQuest
		auto viaConsole = [a_formID, a_done] {
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				a_done(false);
				return;
			}
			task->AddTask([a_formID, a_done] {
				auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
				const auto edid = q && q->GetFormEditorID() ? std::string(q->GetFormEditorID()) : std::string();
				if (!edid.empty()) {
					Console("completequest " + edid);
				}
				const bool done = q && q->IsCompleted();
				spdlog::info("Complete {:08X}: {} after the console command", a_formID, done ? "completed" : "still not completed");
				a_done(done);
			});
		};
		auto finish = [a_formID, a_done, viaConsole](const RE::BSScript::Variable&) {
			auto* q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
			if (q && q->IsCompleted()) {
				spdlog::info("Complete {:08X}: completed", a_formID);
				a_done(true);
				return;
			}
			viaConsole();
		};
		auto second = [a_formID, finish](const RE::BSScript::Variable&) {
			auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
			auto*      vm2 = VM::GetSingleton();
			const auto h = HandleOf(q);
			auto       cb = MakeCallback(finish);
			if (!q || !vm2 || !h || !vm2->DispatchMethodCall(h, "Quest", "CompleteQuest", RE::MakeFunctionArguments(), cb)) {
				finish(RE::BSScript::Variable{});
			}
		};
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback(second);
		if (!vm || !handle || !vm->DispatchMethodCall(handle, "Quest", "CompleteAllObjectives", RE::MakeFunctionArguments(), cb)) {
			second(RE::BSScript::Variable{});
		}
	}

	std::size_t StartRefresh(const std::string& a_notify)
	{
		if (g_refreshing) {
			return 0;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return 0;
		}
		g_quests.clear();
		g_total = 0;
		for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest || !Running(quest)) {
				continue;
			}
			QuestItem qi;
			qi.quest = quest->GetFormID();
			for (auto* o : quest->objectives) {
				if (!o) {
					continue;
				}
				const auto st = static_cast<std::uint8_t>(o->state.underlying());
				if (st == 1 || st == 3 || st == 5) {
					qi.objs.push_back({ o->index, st });
				}
			}
			if (!qi.objs.empty()) {
				g_total += qi.objs.size();
				g_quests.push_back(std::move(qi));
			}
		}
		if (g_quests.empty()) {
			Notify(a_notify, "ReSaverHelper: no journal objectives to refresh");
			return 0;
		}
		g_done = 0;
		g_failedCalls = 0;
		g_refreshNotify = a_notify;
		g_refreshing = true;
		spdlog::info("Journal refresh: {} objective(s) in {} quest(s), one quest at a time", g_total, g_quests.size());
		Notify(a_notify, std::format("ReSaverHelper: refreshing the quest journal ({} objectives) - keep menus closed until it is done", g_total));
		Step(0, 0, 0);
		return g_total;
	}

	bool MenuBlocksRefresh() { return MenuOpen(); }

	Progress RefreshProgress() { return { g_done.load(), g_total, g_refreshing.load() }; }

	void OnGameLoaded()
	{
		SetInGame(true);
		const auto& s = Settings::Get();
		if (!s.refreshOnLoad) {
			return;
		}
		const int delay = s.refreshDelay;
		std::thread([delay]() {
			std::this_thread::sleep_for(std::chrono::seconds(delay));
			if (auto task = SKSE::GetTaskInterface()) {
				task->AddTask([]() { StartRefresh("ini"); });
			}
		}).detach();
	}
}
