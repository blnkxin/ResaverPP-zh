#include "PCH.h"
#include "Game.h"
#include "Protocol.h"
#include "Server.h"
#include "Settings.h"

namespace
{
	void SetupLog()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "ReSaverHelper.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("ReSaverHelper", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Server::Start();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Game::SetInGame(false);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			if (a_msg->data) {  // true when the load succeeded
				Game::OnGameLoaded();
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
			Game::SetInGame(true);
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	spdlog::info("ReSaverHelper version {} - game {}", proto::kHelperVersion, REL::Module::get().version().string());
	Settings::Load();
	if (auto messaging = SKSE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
		spdlog::error("SKSE messaging unavailable; the helper cannot start");
		return false;
	}
	return true;
}
