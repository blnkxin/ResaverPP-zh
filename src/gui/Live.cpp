// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Live.h"
#include "LiveProtocol.h"

#include <chrono>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

LiveLink::~LiveLink()
{
	_stop = true;
#ifdef _WIN32
	if (_pipe) {
		CancelIoEx(static_cast<HANDLE>(_pipe), nullptr);  // ends a read or write the worker waits in
	}
#endif
	if (_thread.joinable()) {
		_thread.join();
	}
	disconnect();
}

void LiveLink::start()
{
	if (_thread.joinable()) {
		return;
	}
#ifdef _WIN32
	_thread = std::thread([this] { run(); });
#endif
}

LiveLink::Info LiveLink::info() const
{
	std::lock_guard l(_m);
	return _info;
}

void LiveLink::request(std::vector<std::string> a_command, Done a_done)
{
	std::lock_guard l(_m);
	if (!_info.connected) {
		_finished.push_back({ std::move(a_done), Reply{ "ERR", "the game is not running with ReSaverHelper.dll" } });
		return;
	}
	_queue.push_back({ std::move(a_command), std::move(a_done) });
}

void LiveLink::poll()
{
	std::vector<std::pair<Done, Reply>> done;
	{
		std::lock_guard l(_m);
		done.swap(_finished);
	}
	for (auto& [fn, reply] : done) {
		if (fn) {
			fn(reply);
		}
	}
}

void LiveLink::disconnect()
{
#ifdef _WIN32
	if (_pipe) {
		CloseHandle(static_cast<HANDLE>(_pipe));
		_pipe = nullptr;
	}
#endif
	_readBuf.clear();
	std::lock_guard l(_m);
	_info = Info{};
	for (auto& p : _queue) {
		_finished.push_back({ std::move(p.done), Reply{ "ERR", "the connection to the game was lost" } });
	}
	_queue.clear();
}

bool LiveLink::exchange(const std::vector<std::string>& a_command, Reply& a_reply)
{
#ifdef _WIN32
	const auto       id = std::to_string(_nextId++);
	std::vector<std::string> fields{ id };
	fields.insert(fields.end(), a_command.begin(), a_command.end());
	const auto line = proto::Join(fields) + "\n";
	DWORD      wrote = 0;
	if (!WriteFile(static_cast<HANDLE>(_pipe), line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr)) {
		return false;
	}
	// one reply line per request
	while (true) {
		const auto nl = _readBuf.find('\n');
		if (nl != std::string::npos) {
			auto r = proto::Split(_readBuf.substr(0, nl));
			_readBuf.erase(0, nl + 1);
			if (!r.empty() && r[0] == id) {
				a_reply.assign(r.begin() + 1, r.end());
				return true;
			}
			continue;  // a stale reply
		}
		char  buf[4096];
		DWORD got = 0;
		if (!ReadFile(static_cast<HANDLE>(_pipe), buf, sizeof(buf), &got, nullptr) || !got) {
			return false;
		}
		_readBuf.append(buf, got);
	}
#else
	(void)a_command;
	(void)a_reply;
	return false;
#endif
}

void LiveLink::run()
{
#ifdef _WIN32
	using clock = std::chrono::steady_clock;
	auto lastHello = clock::now() - std::chrono::seconds(10);
	while (!_stop) {
		if (!_pipe) {
			if (!_enabled) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}
			HANDLE h = CreateFileW(proto::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				for (int i = 0; i < 20 && !_stop; ++i) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				continue;
			}
			_pipe = h;
			lastHello = clock::now() - std::chrono::seconds(10);
		}
		if (!_enabled) {
			disconnect();
			continue;
		}
		// keep the link status fresh (the game may load a save, go back to the menu...)
		if (clock::now() - lastHello > std::chrono::seconds(3)) {
			Reply r;
			if (!exchange({ "HELLO" }, r) || !Ok(r)) {
				disconnect();
				continue;
			}
			std::lock_guard l(_m);
			_info.connected = true;
			_info.helperVersion = Field(r, 1);
			_info.gameVersion = Field(r, 2);
			_info.runtime = Field(r, 3);
			_info.inGame = Field(r, 4) == "1";
			lastHello = clock::now();
		}
		Pending p;
		{
			std::lock_guard l(_m);
			if (!_queue.empty()) {
				p = std::move(_queue.front());
				_queue.pop_front();
			}
		}
		if (p.command.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		_busy = true;
		Reply r;
		const bool ok = exchange(p.command, r);
		_busy = false;
		{
			std::lock_guard l(_m);
			_finished.push_back({ std::move(p.done), ok ? r : Reply{ "ERR", "the connection to the game was lost" } });
		}
		if (!ok) {
			disconnect();
		}
	}
#endif
}
