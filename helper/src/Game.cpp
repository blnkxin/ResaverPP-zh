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
		struct Item
		{
			RE::FormID    quest = 0;
			std::uint16_t objective = 0;
			std::uint8_t  state = 0;  // 1 shown, 3 completed + shown, 5 failed + shown
		};
		std::vector<Item>         g_items;
		std::atomic<std::size_t>  g_done{ 0 }, g_failedCalls{ 0 };
		std::atomic<int>          g_workersLeft{ 0 };
		std::atomic<bool>         g_refreshing{ false };
		std::string               g_refreshNotify;
		std::size_t               g_refreshQuests = 0;
		constexpr std::size_t     kWorkers = 8;

		void Finish()
		{
			g_refreshing = false;
			const auto msg = std::format("ReSaverHelper: quest journal refreshed ({} objective(s) in {} quest(s))", g_done.load(), g_refreshQuests);
			spdlog::info("{} ({} call(s) failed)", msg, g_failedCalls.load());
			Notify(g_refreshNotify, msg);
		}

		void Step(std::size_t a_index, int a_phase);

		// calls Quest.<fn>(args) on the item's quest, then continues with the next phase
		template <class... Args>
		void CallThen(std::size_t a_index, int a_nextPhase, const char* a_fn, Args... a_args)
		{
			auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(g_items[a_index].quest);
			auto* vm = VM::GetSingleton();
			const auto handle = HandleOf(quest);
			auto       cb = MakeCallback([a_index, a_nextPhase](const RE::BSScript::Variable&) { Step(a_index, a_nextPhase); });
			if (!vm || !handle || !vm->DispatchMethodCall(handle, "Quest", a_fn, RE::MakeFunctionArguments(std::move(a_args)...), cb)) {
				++g_failedCalls;
				Step(a_index, 99);  // skip the rest of this objective
			}
		}

		// phase 0 hide, 1 show, 2 put completed / failed back, 3 next objective of this worker
		void Step(std::size_t a_index, int a_phase)
		{
			if (a_index >= g_items.size()) {
				if (--g_workersLeft == 0) {
					Finish();
				}
				return;
			}
			const auto& it = g_items[a_index];
			const auto  idx = static_cast<std::int32_t>(it.objective);
			switch (a_phase) {
			case 0:
				CallThen(a_index, 1, "SetObjectiveDisplayed", idx, false, false);
				return;
			case 1:
				CallThen(a_index, 2, "SetObjectiveDisplayed", idx, true, false);
				return;
			case 2:
				if (it.state == 3) {
					CallThen(a_index, 3, "SetObjectiveCompleted", idx, true);
					return;
				}
				if (it.state == 5) {
					CallThen(a_index, 3, "SetObjectiveFailed", idx, true);
					return;
				}
				[[fallthrough]];
			default:
				++g_done;
				Step(a_index + kWorkers, 0);
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
			fn();
			done->set_value();
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
		info.running = quest->IsRunning();
		info.completed = quest->IsCompleted();
		for (auto* o : quest->objectives) {
			if (o) {
				info.objectives.push_back({ o->index, static_cast<std::uint8_t>(o->state.underlying()) });
			}
		}
		return info;
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
                q && q->IsRunning(), q && q->IsCompleted(), q ? q->GetCurrentStageID() : 0);
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
		spdlog::info("Complete {:08X} ({}): running {}, completed {}, current stage {}", a_formID, quest->GetFormEditorID(), quest->IsRunning(), quest->IsCompleted(),
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
		g_items.clear();
		g_refreshQuests = 0;
		for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest || !quest->IsRunning()) {
				continue;
			}
			bool any = false;
			for (auto* o : quest->objectives) {
				if (!o) {
					continue;
				}
				const auto st = static_cast<std::uint8_t>(o->state.underlying());
				if (st == 1 || st == 3 || st == 5) {
					g_items.push_back({ quest->GetFormID(), o->index, st });
					any = true;
				}
			}
			g_refreshQuests += any ? 1 : 0;
		}
		if (g_items.empty()) {
			Notify(a_notify, "ReSaverHelper: no journal objectives to refresh");
			return 0;
		}
		g_done = 0;
		g_failedCalls = 0;
		g_refreshNotify = a_notify;
		g_refreshing = true;
		const auto workers = std::min<std::size_t>(kWorkers, g_items.size());
		g_workersLeft = static_cast<int>(workers);
		spdlog::info("Journal refresh: {} objective(s) in {} quest(s), {} worker(s)", g_items.size(), g_refreshQuests, workers);
		Notify(a_notify, std::format("ReSaverHelper: refreshing the quest journal ({} objectives)...", g_items.size()));
		for (std::size_t w = 0; w < workers; ++w) {
			Step(w, 0);
		}
		return g_items.size();
	}

	Progress RefreshProgress() { return { g_done.load(), g_items.size(), g_refreshing.load() }; }

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
