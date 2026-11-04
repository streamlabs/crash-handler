/******************************************************************************
    Copyright (C) 2016-2020 by Streamlabs (General Workings Inc)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_map>

#include "socket.hpp"
#include "process.hpp"
#include "logger.hpp"
#include "util.hpp"

struct ThreadData {
	std::thread *worker = nullptr;

	bool should_stop = false;
	std::condition_variable_any stop_event;
	std::recursive_mutex stop_mutex;
	void send_stop();
	bool wait_or_stop();
};

class ProcessManager {
public:
	ProcessManager();
	~ProcessManager();

	void runWatcher();
	bool m_applicationCrashed;
	bool m_criticalCrash;
	std::atomic<bool> m_shutdownIntent{false};

	void handleCrash(std::wstring path);
	bool sendExitMessage(bool appCrashed);

private:
	ThreadData *watcher = nullptr;
	ThreadData *monitor = nullptr;
	std::vector<std::unique_ptr<Process>> processes;
	std::mutex mtx;
	std::unordered_map<uint32_t, bool> outputActive;
	std::unique_ptr<Socket> socket;

	void watcher_fnc();
	void monitor_fnc();

	void startMonitoring();
	void stopMonitoring();

	size_t registerProcess(bool isCritical, uint32_t PID);
	void unregisterProcess(uint32_t PID);
	void registerProcessMemoryDump(uint32_t PID, const std::wstring &eventName_Start, const std::wstring &eventName_Fail,
				       const std::wstring &eventName_Success, const std::wstring &dumpPath, const std::wstring &dumpName);

	void terminateAll(void);
	void terminateNonCritical(void);
	bool criticalOutputsInactive();
};