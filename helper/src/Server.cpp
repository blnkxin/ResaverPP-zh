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
			if (cmd == "QUEST") {
				Game::QuestInfo q;
				if (!Game::RunOnMain([&] { q = Game::GetQuest(ParseHex(arg(0))); })) {
					return err("the game did not answer in time");
				}
				if (!q.found) {
					return err("no quest " + arg(0) + " in this game");
				}
				std::string objs;
				for (const auto& [i, s] : q.objectives) {
					objs += (objs.empty() ? "" : ",") + std::to_string(i) + ":" + std::to_string(s);
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
				auto       result = std::make_shared<std::promise<bool>>();
				auto       fut = result->get_future();
				const auto notify = arg(2), message = arg(3);
				if (!Game::RunOnMain([&] {
						Game::SetStage(formID, static_cast<std::uint16_t>(stage), [result, notify, message](bool ok) {
							Game::Notify(notify, ok ? (message.empty() ? "ReSaverHelper: quest stage set" : message) : "ReSaverHelper: the game refused that stage");
							try {
								result->set_value(ok);
							} catch (...) {
							}
						});
					})) {
					return err("the game did not answer in time");
				}
				if (fut.wait_for(15s) != std::future_status::ready) {
					return { "OK", "?" };  // still running (a long stage script): it continues in the game
				}
				return { "OK", fut.get() ? "1" : "0" };
			}
			if (cmd == "REFRESH") {
				std::size_t n = 0;
				const auto  notify = arg(0);
				if (!Game::RunOnMain([&] { n = Game::StartRefresh(notify); })) {
					return err("the game did not answer in time");
				}
				return { "OK", std::to_string(n) };
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
