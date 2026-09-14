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

#include "logger.hpp"
#include "process-manager.hpp"
#include "util.hpp"
#include <codecvt>
#include <filesystem>

#include "brief-crash-info-uploader.hpp"

#include "http-helper.hpp"

#if defined(WIN32)
#include "platforms/upload-window-win.hpp"
const std::string log_file_name = "\\crash-handler.log";
#else // for __APPLE__ and other
const std::wstring log_file_name = L"/crash-handler.log";
#endif

int main(int argc, char **argv)
{
#if defined(_WIN32)
	CoInitialize(NULL);
#endif
	Util::setupLocale();

	std::string pid_path(Util::get_temp_directory());
	pid_path.append("crash-handler.pid");
	Util::check_pid_file(pid_path);
	Util::write_pid_file(pid_path);

	std::wstring path;
	std::wstring version;
	std::wstring isDevEnv;
	std::wstring ipc_path;
	std::string appData_path;

	std::cout << "Launched with number of arguments = " << argc << std::endl;
#if defined(WIN32)
	LPWSTR *szArglist;
	int nArgs;

	szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	std::cout << "Launched with number of arguments = " << nArgs << std::endl;
	static thread_local std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

	// Frontend passes as non-unicode
	if (nArgs == 1) {
		// No arguments - show the UI for development.
		UploadWindow::getInstance()->createWindow();
		UploadWindow::getInstance()->crashCaught();
		UploadWindow::getInstance()->waitForUserChoise();
		return 0;
	}
	if (nArgs >= 1)
		path = szArglist[0];
	if (nArgs >= 3)
		version = szArglist[2];
	if (nArgs >= 4)
		isDevEnv = szArglist[3];
	if (nArgs >= 5) {
		std::string cache_path = converter.to_bytes(szArglist[4]);
		std::string log_path = cache_path + log_file_name;
		std::wstring w_log_path = converter.from_bytes(log_path.c_str());
		std::wstring w_cache_path = converter.from_bytes(cache_path.c_str());
		logging_start(w_log_path);
		log_info << "=== Started CrashHandler ===" << std::endl;
		Util::setCachePath(w_cache_path);
		appData_path = cache_path;
	}
	if (nArgs >= 6) {
		ipc_path = szArglist[5];
		log_info << "ipc path option recieve : " << converter.to_bytes(ipc_path) << std::endl;
		Socket::set_ipc_path(ipc_path);
	}
#else // for __APPLE__ and other
	static thread_local std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	// Frontend passes as non-unicode
	if (argc >= 1)
		path = converter.from_bytes(argv[0]);
	if (argc >= 3)
		version = converter.from_bytes(argv[2]);
	if (argc >= 4)
		isDevEnv = converter.from_bytes(argv[3]);
	if (argc >= 5) {
		std::wstring cache_path = converter.from_bytes(argv[4]);
		std::wstring log_path = cache_path + log_file_name;
		std::cout << "Path for logging = " << std::string(log_path.begin(), log_path.end()) << std::endl;
		logging_start(log_path);
		log_info << "=== Started CrashHandler ===" << std::endl;
		Util::setCachePath(cache_path);
		appData_path = argv[4];
	}
	if (argc >= 6) {
		ipc_path = converter.from_bytes(argv[5]);
		log_info << "ipc path option recieve : " << std::string(ipc_path.begin(), ipc_path.end()) << std::endl;
		Socket::set_ipc_path(ipc_path);
	}
#endif

	ProcessManager *pm = new ProcessManager();
	pm->runWatcher();

	if (pm->m_applicationCrashed) {
		pm->handleCrash(path);
		BriefCrashInfoUploader(appData_path).Run();
	}

	delete pm;

	log_info << "=== Terminating CrashHandler ===" << std::endl;
	logging_end();

#if defined(_WIN32)
	CoUninitialize();
#endif

	return 0;
}
