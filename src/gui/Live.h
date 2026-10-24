// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Connection to ReSaverHelper.dll in the running game (local named pipe, see LiveProtocol.h). A
// background thread connects when the game is running, keeps the link alive and sends the requests
// one at a time; replies are handed back on the UI thread by poll().
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LiveLink
{
public:
	struct Info
	{
		bool        connected = false;
		bool        inGame = false;  // a save is loaded in the game
		std::string helperVersion, gameVersion, runtime;
	};
	// reply fields after the request id: "OK" or "ERR", then the command's fields
	using Reply = std::vector<std::string>;
	using Done = std::function<void(const Reply&)>;

	LiveLink() = default;
	~LiveLink();

	void start();  // starts the background thread (Windows only; elsewhere it never connects)
	void setEnabled(bool a_enabled) { _enabled = a_enabled; }
	Info info() const;
	bool busy() const { return _busy; }

	// queues a request; a_done runs on the UI thread from poll(). Not connected: a_done gets ERR.
	void request(std::vector<std::string> a_command, Done a_done);
	void poll();

	static bool Ok(const Reply& r) { return !r.empty() && r[0] == "OK"; }
	static std::string Field(const Reply& r, std::size_t i) { return i + 1 < r.size() ? r[i + 1] : std::string(); }
	static std::string Error(const Reply& r) { return r.size() > 1 ? r[1] : std::string("no reply"); }

private:
	void run();
	bool exchange(const std::vector<std::string>& a_command, Reply& a_reply);  // worker thread
	void disconnect();

	struct Pending
	{
		std::vector<std::string> command;
		Done                     done;
	};

	mutable std::mutex                       _m;
	Info                                     _info;
	std::deque<Pending>                      _queue;
	std::vector<std::pair<Done, Reply>>      _finished;
	std::thread                              _thread;
	std::atomic<bool>                        _stop{ false }, _enabled{ true }, _busy{ false };
	void*                                    _pipe = nullptr;  // HANDLE
	unsigned                                 _nextId = 1;
	std::string                              _readBuf;
};
