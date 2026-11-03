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

#include "process-manager.hpp"

#include <chrono>
#include <iostream>

void ThreadData::send_stop()
{
	{
		const std::lock_guard<std::recursive_mutex> lock(stop_mutex);
		should_stop = true;
	}
	stop_event.notify_one();
}

bool ThreadData::wait_or_stop()
{
	std::unique_lock<std::recursive_mutex> lck(stop_mutex);
	if (should_stop)
		return true;
	else
		return stop_event.wait_for(lck, std::chrono::milliseconds(50), [&flag = this->should_stop] { return flag; });
}

ProcessManager::ProcessManager()
{
	m_applicationCrashed = false;
	m_criticalCrash = false;
}

ProcessManager::~ProcessManager()
{
	this->processes.clear();
	this->socket->disconnect();
	this->socket.reset();
}

void ProcessManager::runWatcher()
{
	this->watcher = new ThreadData();
	this->watcher->should_stop = false;
	this->watcher->worker = new std::thread(&ProcessManager::watcher_fnc, this);

	if (this->watcher->worker->joinable())
		this->watcher->worker->join();
}

void ProcessManager::watcher_fnc()
{
	log_info << "Start Watcher" << std::endl;
	this->socket = Socket::create();
	if (this->socket->initialization_failed)
		return;

	while (!this->watcher->wait_or_stop()) {
		std::vector<char> buffer = this->socket->read();
		while (buffer.size()) {
			Message msg(buffer);
			switch (static_cast<Action>(msg.readUInt8())) {
			case Action::REGISTER: {
				bool isCritical = msg.readBool();
				uint32_t pid = msg.readUInt32();
				if (!msg.isValid())
					break;
				size_t size = registerProcess(isCritical, pid);
				if (size == 1)
					startMonitoring();

				break;
			}
			case Action::UNREGISTER: {
				uint32_t pid = msg.readUInt32();
				if (!msg.isValid())
					break;
				unregisterProcess(pid);
				break;
			}
			case Action::REGISTERMEMORYDUMP: {
				uint32_t pid = msg.readUInt32();
				std::wstring eventName_Start = msg.readWstring();
				std::wstring eventName_Fail = msg.readWstring();
				std::wstring eventName_Success = msg.readWstring();
				std::wstring dumpPath = msg.readWstring();
				std::wstring dumpName = msg.readWstring();
				if (!msg.isValid())
					break;
				registerProcessMemoryDump(pid, eventName_Start, eventName_Fail, eventName_Success, dumpPath, dumpName);
				break;
			}
			case Action::CRASHED_MODULE_INFO: {
				const auto moduleName = msg.readString();
				const auto modulePath = msg.readString();
				if (!msg.isValid())
					break;
				log_info << "crashed_module_info " << moduleName << " (" << modulePath << ")" << std::endl;
				break;
			}
			case Action::OUTPUT_STATE: {
				uint32_t pid = msg.readUInt32();
				bool active = msg.readBool();
				if (!msg.isValid())
					break;
				log_info << "output state pid " << pid << " active " << active << std::endl;
				this->outputActive[pid] = active;
				break;
			}
			case Action::SHUTDOWN_INTENT: {
				uint32_t pid = msg.readUInt32();
				if (!msg.isValid())
					break;
				log_info << "shutdown intent pid " << pid << std::endl;
				this->m_shutdownIntent = true;
				break;
			}
			default:
				break;
			}
			if (!msg.isValid())
				log_error << "Dropped invalid message, size " << buffer.size() << std::endl;
			buffer = this->socket->read();
		}
	}
	log_info << "End Watcher" << std::endl;
}

void ProcessManager::monitor_fnc()
{
	log_info << "Start monitoring" << std::endl;
	bool criticalCrash = false;
	bool unresponsiveMarked = false;
	uint32_t last_responsive_check = 0;
	std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();
	bool validatedProcesses = false;

	while (!this->monitor->wait_or_stop()) {
		bool detectedUnresponsive = false;
		if (this->mtx.try_lock()) {
			if (++last_responsive_check % 100 == 0)
				last_responsive_check = 0;

			bool willValidateProcesses = false;
			if (!validatedProcesses) {
				std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
				auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
				if (diff >= 7) { // check in 7 seconds after start
					willValidateProcesses = true;
					validatedProcesses = true;
				}
			}

			for (auto &process : this->processes) {
				if (!process->isAlive() || (willValidateProcesses && !process->isValid())) {
					// Log information about the process that just crashed
					if (!process->isValid()) {
						log_info << "process handle not valid" << std::endl;
					} else {
						log_info << "process died" << std::endl;
					}
					log_info << "process.pid: " << process->getPID() << std::endl;
					log_info << "process.isCritical: " << process->isCritical() << std::endl;

					m_criticalCrash |= process->isCritical();
					m_applicationCrashed = true;
				} else if (last_responsive_check == 0) {
					detectedUnresponsive |= process->isUnResponsive();
				}
			}

			this->mtx.unlock();
			if (unresponsiveMarked && !detectedUnresponsive) {
				log_info << "Unresponsive window not detected anymore " << std::endl;
				Util::updateAppState(Util::AppState::Responsive);
				unresponsiveMarked = false;
			} else if (!unresponsiveMarked && detectedUnresponsive) {
				log_info << "Unresponsive window detected " << std::endl;
				Util::updateAppState(Util::AppState::Unresponsive);
				unresponsiveMarked = true;
			}
		}
		if (m_applicationCrashed)
			break;
	}

	if (m_applicationCrashed && !m_criticalCrash && !m_shutdownIntent) {
		log_info << "Non critical crash detected. save it to app state file" << std::endl;
		Util::updateAppState(Util::AppState::NoncriticallyDead);
	}

	this->watcher->send_stop();
#ifdef __APPLE__
	if (m_applicationCrashed) {
		std::vector<char> buffer;
		buffer.push_back('-1');
		this->socket->write(false, buffer);
	}
#endif
	log_info << "End monitoring" << std::endl;
}

void ProcessManager::startMonitoring()
{
	this->monitor = new ThreadData();
	this->monitor->should_stop = false;

	this->monitor->worker = new std::thread(&ProcessManager::monitor_fnc, this);
}

void ProcessManager::stopMonitoring()
{
	log_info << "stop monitoring" << std::endl;
	this->monitor->send_stop();

	if (this->monitor->worker->joinable())
		this->monitor->worker->join();
}

size_t ProcessManager::registerProcess(bool isCritical, uint32_t PID)
{
	log_info << "register process" << std::endl;
	log_info << "pid " << PID << std::endl;
	log_info << "isCritical " << isCritical << std::endl;

	const std::lock_guard<std::mutex> lock(this->mtx);

	auto it = std::find_if(this->processes.begin(), this->processes.end(), [&PID](std::unique_ptr<Process> &p) { return p->getPID() == PID; });
	if (it == this->processes.end()) {
		this->processes.push_back(Process::create(PID, isCritical));
	}
	log_info << "Processes size: " << this->processes.size() << std::endl;
	return this->processes.size();
}

void ProcessManager::unregisterProcess(uint32_t PID)
{
	const std::lock_guard<std::mutex> lock(this->mtx);

	auto it = std::find_if(this->processes.begin(), this->processes.end(), [&PID](std::unique_ptr<Process> &p) { return p->getPID() == PID; });

	if (it == this->processes.end())
		return;

	log_info << "unregister process" << std::endl;
	log_info << "pid " << PID << std::endl;
	log_info << "isCritical " << (*it)->isCritical() << std::endl;

	if ((*it)->isCritical()) {
		this->watcher->send_stop();
		this->stopMonitoring();
		this->sendExitMessage(false);
	}

	this->processes.erase(it);
}

void ProcessManager::registerProcessMemoryDump(uint32_t PID, const std::wstring &eventName_Start, const std::wstring &eventName_Fail,
					       const std::wstring &eventName_Success, const std::wstring &dumpPath, const std::wstring &dumpName)
{
	const std::lock_guard<std::mutex> lock(this->mtx);

	log_info << "requested memory dump on crash for pid = " << PID << std::endl;
	auto it = std::find_if(this->processes.begin(), this->processes.end(), [&PID](std::unique_ptr<Process> &p) { return p->getPID() == PID; });

	if (it == this->processes.end())
		return;

	log_info << "register for memory dump" << std::endl;
	(*it)->startMemoryDumpMonitoring(eventName_Start, eventName_Fail, eventName_Success, dumpPath, dumpName);
}

void ProcessManager::handleCrash(std::wstring path)
{
	log_info << "Handling crash - processes state: " << std::endl;
	for (auto &process : this->processes) {
		log_info << "----" << std::endl;
		if (process->isAlive()) {
			log_info << "process.pid: " << process->getPID() << std::endl;
		} else {
			log_info << "process.pid: " << process->getPID() << " (not alive)" << std::endl;
			if (process->isCritical())
				m_criticalCrash = true;
		}
		log_info << "process.isCritical: " << process->isCritical() << std::endl;
	}
	log_info << "----" << std::endl;

	bool shouldRestart = false;
	if (m_criticalCrash) {
		terminateAll();
	} else {
		log_info << "Terminate non critical processes" << std::endl;
		terminateNonCritical();
		if (m_shutdownIntent) {
			log_info << "Shutdown was requested, send exit message" << std::endl;
			this->sendExitMessage(true);
		} else {
			bool exitSent = false;
			if (criticalOutputsInactive()) {
				log_info << "No active outputs, send exit message" << std::endl;
				this->sendExitMessage(true);
				exitSent = true;
			}
			// Blocking operation that will return once the user
			// decides to terminate the application
			Util::runTerminateWindow(shouldRestart);
			if (!exitSent) {
				log_info << "Send exit message" << std::endl;
				this->sendExitMessage(true);
			}
		}
	}

	if (shouldRestart)
		Util::restartApp(path);
}

bool ProcessManager::criticalOutputsInactive()
{
	for (auto &process : this->processes) {
		if (!process->isCritical())
			continue;
		auto it = this->outputActive.find(process->getPID());
		if (it != this->outputActive.end() && !it->second)
			return true;
	}
	return false;
}

void ProcessManager::sendExitMessage(bool appCrashed)
{
	std::vector<char> buffer;
	buffer.push_back(appCrashed);

	if (this->socket->write(true, buffer) > 0)
		return;

	// Only an obs64 that never sent OUTPUT_STATE waits on the shared legacy pipe
	if (this->outputActive.empty() && this->socket->writeLegacyExit(buffer) > 0)
		return;

	log_info << "Failed to send exit message" << std::endl;
}

void ProcessManager::terminateAll(void)
{

	std::vector<std::thread> workers;

	// What if the head process is busy? Do we really want the other processes to keep running?
	// Instead, seperate these out into workers so that non-busy processes are killed asap
	for (auto &process : this->processes) {
		workers.push_back(std::thread([](Process *ptr) { ptr->terminate(); }, process.get()));
	}

	for (auto &itr : workers) {
		if (itr.joinable())
			itr.join();
	}
}

void ProcessManager::terminateNonCritical(void)
{
	for (auto &process : this->processes) {
		if (!process->isCritical())
			process->terminate();
	}
}