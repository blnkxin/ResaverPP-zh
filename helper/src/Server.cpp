#include "PCH.h"
#include "Server.h"
#include "Game.h"
#include "Protocol.h"
#include "Settings.h"

namespace Server
{
	namespace
	{
		std::string RuntimeName()
		{
			if (REL::Module::IsVR()) {
				return "VR";
			}
			return REL::Module::IsAE() ? "AE" : "SE";
		}

		// how long a request waits for the game's main thread (a heavily modded game can stall for many seconds)
		constexpr auto kWait = 30s;

		std::uint32_t ParseHex(const std::string& s)
		{
			try {
				return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
			} catch (...) {
				return 0;
			}
		}

		std::vector<std::string> Handle(const std::vector<std::string>& f)
		{
			const auto& cmd = f.size() > 1 ? f[1] : std::string();
			auto        arg = [&](std::size_t i) { return f.size() > i + 2 ? f[i + 2] : std::string(); };
			auto        err = [](std::string m) { return std::vector<std::string>{ "ERR", std::move(m) }; };

			if (cmd == "HELLO") {
				return { "OK", "ReSaverHelper", std::to_string(proto::kHelperVersion), REL::Module::get().version().string(), RuntimeName(),
					Game::InGame() ? "1" : "0" };
			}
			if (cmd == "GETOPTION") {
				const auto v = Settings::GetOption(arg(0));
				return v.empty() ? err("unknown option " + arg(0)) : std::vector<std::string>{ "OK", v };
			}
			if (cmd == "SETOPTION") {
				if (!Settings::SetOption(arg(0), arg(1))) {
					return err("invalid option or value: " + arg(0) + "=" + arg(1));
				}
				if (!Settings::Save()) {
					return err("could not write " + Settings::IniPath().string());
				}
				spdlog::info("Option {} set to {} by ReSaver++", arg(0), arg(1));
				return { "OK" };
			}
			if (cmd == "NOTIFY") {
				Game::Notify(arg(0), arg(1));
				return { "OK" };
			}
			if (cmd == "PROGRESS") {
				const auto p = Game::RefreshProgress();
				return { "OK", std::to_string(p.done), std::to_string(p.total), p.running ? "1" : "0" };
			}
			if (!Game::InGame()) {
				return err("no game loaded yet (load a save first)");
			}
			// Everything a main-thread task touches is owned by the task (values and shared_ptr copies),
			// never by this function's stack: when the game is busy the task can run long after this
			// request has given up waiting (version 1 crashed that way after a long stall).
			if (cmd == "QUEST") {
				struct State
				{
					Game::QuestInfo q;
				};
				auto       st = std::make_shared<State>();
				const auto formID = ParseHex(arg(0));
				if (!Game::RunOnMain([st, formID] { st->q = Game::GetQuest(formID); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				const auto& q = st->q;
				if (!q.found) {
					return err("no quest " + arg(0) + " in this game");
				}
				std::string objs;
				for (const auto& [i, o] : q.objectives) {
					objs += (objs.empty() ? "" : ",") + std::to_string(i) + ":" + std::to_string(o);
				}
				return { "OK", q.editorID, std::format("{:04X}", q.flags), std::to_string(q.currentStage), q.running ? "1" : "0",
					q.completed ? "1" : "0", objs };
			}
			if (cmd == "SETSTAGE") {
				const auto formID = ParseHex(arg(0));
				int        stage = -1;
				try {
					stage = std::stoi(arg(1));
				} catch (...) {
				}
				if (!formID || stage < 0 || stage > 0xFFFF) {
					return err("bad quest or stage");
				}
				struct Outcome
				{
					bool          ok = false, running = false, completed = false;
					std::uint16_t stage = 0;
				};
				auto       result = std::make_shared<std::promise<Outcome>>();
				auto       fut = result->get_future();
				const auto notify = arg(2), message = arg(3);
				const auto st16 = static_cast<std::uint16_t>(stage);
				const bool ran = Game::RunOnMain(
					[formID, st16, result, notify, message] {
						Game::SetStage(formID, st16, [formID, result, notify, message](bool ok) {
							Game::Notify(notify, ok ? (message.empty() ? "ReSaverHelper: quest stage set" : message) : "ReSaverHelper: the game refused that stage");
							Outcome o;
							o.ok = ok;
							if (auto* q = RE::TESForm::LookupByID<RE::TESQuest>(formID)) {
								o.running = q->IsRunning();
								o.completed = q->IsCompleted();
								o.stage = q->GetCurrentStageID();
							}
							try {
								result->set_value(o);
							} catch (...) {
							}
						});
					},
					kWait);
				if (!ran) {
					return { "OK", "?" };  // queued: the game is busy and does it as soon as it can
				}
				if (fut.wait_for(15s) != std::future_status::ready) {
					return { "OK", "?" };  // still running (a long stage script): it continues in the game
				}
				const auto o = fut.get();
				return { "OK", o.ok ? "1" : "0", o.running ? "1" : "0", o.completed ? "1" : "0", std::to_string(o.stage) };
			}
			if (cmd == "COMPLETE") {
				const auto formID = ParseHex(arg(0));
				if (!formID) {
					return err("bad quest");
				}
				auto       result = std::make_shared<std::promise<bool>>();
				auto       fut = result->get_future();
				const auto notify = arg(1), message = arg(2);
				const bool ran = Game::RunOnMain(
					[formID, result, notify, message] {
						Game::Complete(formID, [result, notify, message](bool ok) {
							Game::Notify(notify, ok ? (message.empty() ? "ReSaverHelper: quest marked completed" : message) : "ReSaverHelper: the quest could not be marked completed");
							try {
								result->set_value(ok);
							} catch (...) {
							}
						});
					},
					kWait);
				if (!ran || fut.wait_for(15s) != std::future_status::ready) {
					return { "OK", "?" };
				}
				return { "OK", fut.get() ? "1" : "0" };
			}
			if (cmd == "REFRESH") {
				auto       n = std::make_shared<std::atomic<std::size_t>>(0);
				const auto notify = arg(0);
				if (!Game::RunOnMain([n, notify] { n->store(Game::StartRefresh(notify)); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds); the refresh starts when it can");
				}
				return { "OK", std::to_string(n->load()) };
			}
			return err("unknown command " + cmd);
		}

		// security attributes: only the current user (and SYSTEM) may open the pipe
		bool OwnerOnly(SECURITY_ATTRIBUTES& a_sa)
		{
			HANDLE token = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
				return false;
			}
			std::vector<std::uint8_t> buf(512);
			DWORD                     len = 0;
			const bool                ok = GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &len);
			CloseHandle(token);
			if (!ok) {
				return false;
			}
			LPWSTR sid = nullptr;
			if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
				return false;
			}
			const std::wstring   sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
			LocalFree(sid);
			PSECURITY_DESCRIPTOR sd = nullptr;
			if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
				return false;
			}
			a_sa.nLength = sizeof(a_sa);
			a_sa.lpSecurityDescriptor = sd;  // kept for the life of the process
			a_sa.bInheritHandle = FALSE;
			return true;
		}

		void Serve()
		{
			SECURITY_ATTRIBUTES sa{};
			if (!OwnerOnly(sa)) {
				spdlog::error("Server: could not build owner-only pipe security; not listening");
				return;
			}
			spdlog::info("Server: listening on \\\\.\\pipe\\ReSaverHelper");
			while (true) {
				if (!Settings::Get().listen) {
					std::this_thread::sleep_for(2s);
					continue;
				}
				HANDLE pipe = CreateNamedPipeW(proto::kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
					64 * 1024, 64 * 1024, 0, &sa);
				if (pipe == INVALID_HANDLE_VALUE) {
					spdlog::error("Server: CreateNamedPipe failed ({})", GetLastError());
					std::this_thread::sleep_for(5s);
					continue;
				}
				const bool connected = ConnectNamedPipe(pipe, nullptr) ? true : (GetLastError() == ERROR_PIPE_CONNECTED);
				if (connected) {
					spdlog::info("Server: ReSaver++ connected");
					std::string pending;
					char        buf[4096];
					DWORD       got = 0;
					while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) {
						pending.append(buf, got);
						for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
							const auto line = pending.substr(0, nl);
							pending.erase(0, nl + 1);
							const auto fields = proto::Split(line);
							auto       reply = Handle(fields);
							reply.insert(reply.begin(), fields.empty() ? std::string("0") : fields[0]);
							const auto out = proto::Join(reply) + "\n";
							DWORD      wrote = 0;
							WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
						}
						if (pending.size() > 1024 * 1024) {
							break;  // not a ReSaver++ client
						}
					}
					spdlog::info("Server: ReSaver++ disconnected");
				}
				DisconnectNamedPipe(pipe);
				CloseHandle(pipe);
			}
		}
	}

	void Start()
	{
		std::thread(Serve).detach();
	}
}
