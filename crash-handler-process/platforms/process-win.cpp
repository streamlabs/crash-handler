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

#include "process-win.hpp"
#include "../util.hpp"
#include "upload-window-win.hpp"
#include <iomanip>
#include <ctime>
#include <filesystem>
#include "Shlobj.h"
#pragma comment(lib, "Shell32.lib")

struct handle_data {
	unsigned long process_id;
	HWND window_handle;
};

BOOL CALLBACK enum_windows_callback(HWND handle, LPARAM lParam)
{
	handle_data &data = *(handle_data *)lParam;
	unsigned long process_id = 0;
	GetWindowThreadProcessId(handle, &process_id);
	if (data.process_id != process_id || !(GetWindow(handle, GW_OWNER) == (HWND)0))
		return TRUE;

	data.window_handle = handle;
	return FALSE;
}

std::unique_ptr<Process> Process::create(int32_t pid, bool isCritical)
{
	return std::make_unique<Process_WIN>(pid, isCritical);
}

Process_WIN::Process_WIN(int32_t pid, bool isCritical)
{
	this->PID = pid;
	this->critical = isCritical;
	this->alive = true;
	this->handle_OpenProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, getPIDDWORD());
	if (this->handle_OpenProcess != NULL && this->handle_OpenProcess != INVALID_HANDLE_VALUE) {
		this->checker = new std::thread(&Process_WIN::worker, this);
		this->isValidHandle = true;
	} else {
		log_info << "OpenProcess failed: " << GetLastError() << ", pid: " << pid << ", handle: " << this->handle_OpenProcess << std::endl;
	}
}

Process_WIN::~Process_WIN()
{
	if (isValidHandle) {
		safeCloseHandle(this->handle_OpenProcess);
	} else {
		std::wstring taskkill_command(L"taskkill /F /T /PID ");
		taskkill_command += std::to_wstring(this->PID);
		int retval = _wsystem(taskkill_command.data());
		log_info << "taskkill retval: " << retval << std::endl;
	}

	if (this->checker && this->checker->joinable())
		this->checker->join();

	if (memorydump != nullptr && memorydump->joinable())
		memorydump->join();

	safeCloseHandle(this->handle_event_Start);
	safeCloseHandle(this->handle_event_Fail);
	safeCloseHandle(this->handle_event_Success);
}

int32_t Process_WIN::getPID(void)
{
	return PID;
}

bool Process_WIN::isValid(void)
{
	return isValidHandle;
}

bool Process_WIN::isCritical(void)
{
	return critical;
}

void Process_WIN::startMemoryDumpMonitoring(const std::wstring &eventName_Start, const std::wstring &eventName_Fail, const std::wstring &eventName_Success,
					    const std::wstring &dumpPath, const std::wstring &dumpName)
{
	if (memorydump && memorydump->joinable()) {
		return;
	}

	// Open event from the other process
	if (!isValidHandleValue(handle_event_Start = OpenEvent(EVENT_ALL_ACCESS, FALSE, eventName_Start.c_str()))) {
		log_info << "Failed to open start event for memory dump " << GetLastError() << std::endl;
		return;
	}

	auto initEventByName = [](const std::wstring &eventName) {
		HANDLE result = NULL;
		PSECURITY_DESCRIPTOR securityDescriptor = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
		if (InitializeSecurityDescriptor(securityDescriptor, SECURITY_DESCRIPTOR_REVISION)) {
			if (SetSecurityDescriptorDacl(securityDescriptor, TRUE, NULL, FALSE)) {
				SECURITY_ATTRIBUTES eventSecurityAttr = {0};
				eventSecurityAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
				eventSecurityAttr.lpSecurityDescriptor = securityDescriptor;
				eventSecurityAttr.bInheritHandle = FALSE;
				result = CreateEvent(&eventSecurityAttr, TRUE, FALSE, eventName.c_str());
			}
		}

		LocalFree(securityDescriptor);
		return result;
	};

	if (!isValidHandleValue(handle_event_Fail = initEventByName(eventName_Fail)) ||
	    !isValidHandleValue(handle_event_Success = initEventByName(eventName_Success))) {
		log_info << "Failed to create events for memory dump " << GetLastError() << std::endl;
		safeCloseHandle(handle_event_Fail);
		safeCloseHandle(handle_event_Success);
		return;
	}

	memorydumpName = dumpName;
	memorydumpPath = dumpPath;
	memorydump = new std::thread(&Process_WIN::memorydump_worker, this);
}

void Process_WIN::memorydump_worker()
{
	log_info << "Memory dump worker started" << std::endl;
	HANDLE handles[] = {handle_OpenProcess, handle_event_Start};
	DWORD ret = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	if (ret - WAIT_OBJECT_0 == 1) {
		recievedDmpEvent = true;
		alive = false;
		log_info << "Memory dump worker event recieved" << std::endl;
		bool successful_upload = false;
		if (std::filesystem::exists(memorydumpPath) && UploadWindow::getInstance()->createWindow()) {
			UploadWindow::getInstance()->crashCaught();
			log_info << "Window created. Waiting for user decision" << std::endl;
			if (UploadWindow::getInstance()->waitForUserChoise() == IDYES) {
				log_info << "User selected OK for saving a dump" << std::endl;
				UploadWindow::getInstance()->setDumpPath(memorydumpPath);
				UploadWindow::getInstance()->setDumpFileName(memorydumpName);
				UploadWindow::getInstance()->savingStarted();

				const std::wstring archiveName = memorydumpName + L".zip";
				const std::wstring fullArchivePath = memorydumpPath + L"/" + archiveName;
				const std::wstring fullDumpPath = memorydumpPath + L"/" + memorydumpName;

				// Before any writing is done, register these paths to make sure that whatever happens below, they get removed
				UploadWindow::getInstance()->registerRemoveFile(fullArchivePath);
				UploadWindow::getInstance()->registerRemoveFile(fullDumpPath);

				bool dump_saved = Util::saveMemoryDump(PID, memorydumpPath, memorydumpName);

				if (dump_saved && !UploadWindow::getInstance()->userWantsToClose()) {
					UploadWindow::getInstance()->setDumpFileName(archiveName);
					UploadWindow::getInstance()->zippingStarted();
					dump_saved = Util::archiveFile(fullDumpPath, fullArchivePath, "MiniDumpWriteDump.dmp");
				}

				UploadWindow::getInstance()->popRemoveFile(fullDumpPath);

				if (dump_saved && !UploadWindow::getInstance()->userWantsToClose()) {
					UploadWindow::getInstance()->setTotalBytes(std::filesystem::file_size(fullArchivePath));
					UploadWindow::getInstance()->setUploadProgress(0);

					if (!UploadWindow::getInstance()->userWantsToClose()) {
						if (Util::uploadToAWS(memorydumpPath, archiveName)) {
							successful_upload = true;
							SetEvent(handle_event_Success);
						} else if (UploadWindow::getInstance()->waitForUserChoise() == IDYES) {
							UploadWindow::getInstance()->unregisterRemoveFile(fullArchivePath);
						}
					}

					UploadWindow::getInstance()->popRemoveFiles();
					UploadWindow::getInstance()->waitForUserChoise();

				} else {
					UploadWindow::getInstance()->popRemoveFiles();
					UploadWindow::getInstance()->savingFailed();
					UploadWindow::getInstance()->waitForUserChoise();
				}

			} else {
				log_info << "User selected Cancel for saving a dump" << std::endl;
			}
		}

		if (!successful_upload) {
			SetEvent(handle_event_Fail);
		}

		UploadWindow::shutdownInstance();
	} else {
		DWORD last_error = GetLastError();
		log_error << "Memory dump worker exited wait ret = " << ret << ", last error = " << last_error << std::endl;
	}
}

void Process_WIN::worker()
{
	if (!this->handle_OpenProcess)
		return;

	if (!WaitForSingleObject(this->handle_OpenProcess, INFINITE)) {
		std::unique_lock<std::mutex> ul(this->mtx);
		this->alive = false;
		this->handle_OpenProcess = NULL;
	}
}

bool Process_WIN::isAlive(void)
{
	std::unique_lock<std::mutex> ul(this->mtx);
	return this->alive;
}

bool Process_WIN::isUnResponsive(void)
{
	std::unique_lock<std::mutex> ul(this->mtx);
	HWND window = getTopWindow();
	if (window)
		return IsHungAppWindow(window);
	else
		return false;
}

void Process_WIN::terminate(void)
{
	// As a note, when a memorydump thread is 'activated' by the Start signal, it sets in motion a series of events that will 'terminateAll' (every other process), and then this terminate will wait at the join() below
	safeCloseHandle(handle_event_Start);

	if (memorydump != nullptr && memorydump->joinable())
		memorydump->join();

	safeCloseHandle(handle_event_Fail);
	safeCloseHandle(handle_event_Success);

	if (this->handle_OpenProcess && this->handle_OpenProcess != INVALID_HANDLE_VALUE) {
		// Do not terminate a process that wanted a .dmp to be uploaded, because it still needs to make its sentry report
		// Normally, we wouldn't end up here until after the crashing process submitted the sentry report, however, as stated above, when a memorydump thread is 'activated' by the Start signal, it sets in motion a series of events that will 'terminateAll' (every other process)
		if (recievedDmpEvent == false) {
			TerminateProcess(handle_OpenProcess, 1);
		}

		safeCloseHandle(handle_OpenProcess);
	}

	if (this->checker && this->checker->joinable())
		this->checker->join();

	log_debug << "Terminated pid : " << PID << std::endl;
}

DWORD Process_WIN::getPIDDWORD(void)
{
	return static_cast<DWORD>(PID);
}

HWND Process_WIN::getTopWindow()
{
	handle_data data;
	data.process_id = PID;
	data.window_handle = 0;
	EnumWindows(enum_windows_callback, (LPARAM)&data);
	return data.window_handle;
}

bool Process_WIN::isValidHandleValue(const HANDLE h)
{
	return h != NULL && h != INVALID_HANDLE_VALUE;
}

void Process_WIN::safeCloseHandle(HANDLE &h)
{
	if (isValidHandleValue(h)) {
		CloseHandle(h);
		h = NULL;
	}
}
