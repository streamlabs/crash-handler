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

#include "socket-win.hpp"
#include <iostream>
#include <mutex>
#include <algorithm>
#include <vector>
#include "../logger.hpp"

std::wstring Socket_WIN::ipc_path;

void Socket::set_ipc_path(const std::wstring &new_ipc_path)
{
	Socket_WIN::ipc_path = new_ipc_path;
}

BOOL Socket_WIN::ConnectToNewClient(HANDLE hPipe, LPOVERLAPPED lpo)
{
	BOOL fConnected, fPendingIO = FALSE;
	fConnected = ConnectNamedPipe(hPipe, lpo);

	if (fConnected) {
		return 0;
	}

	switch (GetLastError()) {
	case ERROR_IO_PENDING:
		fPendingIO = TRUE;
		break;
	case ERROR_PIPE_CONNECTED:
		if (SetEvent(lpo->hEvent))
			break;
	default: {
		return 0;
	}
	}

	return fPendingIO;
}

void Socket_WIN::DisconnectAndReconnect(DWORD i)
{
	log_info << "DisconnectAndReconnect start for " << i << std::endl;
	if (!DisconnectNamedPipe(Pipe[i].hPipeInst)) {
		return;
	}

	Pipe[i].fPendingIO = ConnectToNewClient(Pipe[i].hPipeInst, &(Pipe[i].oOverlap));

	Pipe[i].dwState = Pipe[i].fPendingIO ? CONNECTING_STATE : READING_STATE;
}

Socket_WIN::Socket_WIN()
{
	this->name = ipc_path;
	this->name_exit_legacy = L"\\\\.\\pipe\\exit-slobs-crash-handler";
	const std::wstring pipe_prefix = L"\\\\.\\pipe\\";
	if (ipc_path.compare(0, pipe_prefix.size(), pipe_prefix) == 0)
		this->name_exit = pipe_prefix + L"exit-" + ipc_path.substr(pipe_prefix.size());
	else
		this->name_exit = this->name_exit_legacy;
	int retries = 5;

	for (int i = 0; i < INSTANCES; i++) {
		if (!initPipe(i)) {
			retries--;
			if (retries) {
				i--;
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			} else {
				initialization_failed = true;
				break;
			}
		}
	}

	log_info << "Pipes initialization " << (initialization_failed ? "failed" : "successfull") << std::endl;
}

bool Socket_WIN::initPipe(int &i)
{
	hEvents[i] = CreateEvent(NULL, TRUE, TRUE, NULL);

	if (hEvents[i] == NULL) {
		DWORD dwErr = GetLastError();
		log_error << "Failed CreateEvent with code " << dwErr << std::endl;
		return false;
	}
	memset(&Pipe[i].oOverlap, 0x00, sizeof(OVERLAPPED));
	Pipe[i].oOverlap.hEvent = hEvents[i];

	Pipe[i].hPipeInst = CreateNamedPipe(const_cast<LPWSTR>(this->name.c_str()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
					    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, INSTANCES, BUFSIZE, BUFSIZE, PIPE_TIMEOUT, NULL);

	if (Pipe[i].hPipeInst == INVALID_HANDLE_VALUE) {
		DWORD dwErr = GetLastError();
		log_error << "Failed CreateNamedPipe with code " << dwErr << std::endl;
		CloseHandle(hEvents[i]);
		return false;
	}

	Pipe[i].fPendingIO = ConnectToNewClient(Pipe[i].hPipeInst, &(Pipe[i].oOverlap));

	Pipe[i].dwState = Pipe[i].fPendingIO ? CONNECTING_STATE : READING_STATE;

	Pipe[i].chRequest.resize(BUFSIZE);

	return true;
}

Socket_WIN::~Socket_WIN() {}

std::unique_ptr<Socket> Socket::create()
{
	return std::make_unique<Socket_WIN>();
}

std::vector<char> Socket_WIN::read()
{
	std::vector<char> buffer;

	DWORD i, dwWait, cbRet = 0, dwErr;
	BOOL fSuccess;

	dwWait = WaitForMultipleObjects(INSTANCES, hEvents, FALSE, 500);

	i = dwWait - WAIT_OBJECT_0;
	if (i < 0 || i > (INSTANCES - 1))
		return buffer;

	log_info << "Socket::read instance_" << i << " pendingIO state " << (Pipe[i].fPendingIO ? 1 : 0) << std::endl;
	if (Pipe[i].fPendingIO) {
		fSuccess = GetOverlappedResult(Pipe[i].hPipeInst, &Pipe[i].oOverlap, &cbRet, FALSE);
		dwErr = GetLastError();
		log_debug << "Socket::read instance_" << i << " GetOverlappedResult read " << cbRet << ", dwErr " << dwErr << "\n";

		switch (Pipe[i].dwState) {
		case CONNECTING_STATE: {
			if (!fSuccess)
				return buffer;

			Pipe[i].dwState = READING_STATE;
			Pipe[i].cbRead = 0;
			break;
		}
		case READING_STATE: {
			if (!fSuccess && (dwErr == ERROR_IO_PENDING)) {
				log_debug << "Socket::read instance_" << i << " pending " << dwErr << "\n";
				return buffer;
			}
			if (!fSuccess || cbRet == 0) {
				log_error << "Socket::read instance_" << i << " pending check failed for  " << dwErr << "\n";
				DisconnectAndReconnect(i);
				return buffer;
			}
			Pipe[i].cbRead = cbRet;
			break;
		}
		default: {
			return buffer;
		}
		}
	}

	switch (Pipe[i].dwState) {
	case READING_STATE: {
		if (cbRet == 0) {
			fSuccess = ReadFile(Pipe[i].hPipeInst, Pipe[i].chRequest.data(), BUFSIZE, &Pipe[i].cbRead, &Pipe[i].oOverlap);
			dwErr = GetLastError();
			log_debug << "Socket::read instance_" << i << " read size " << Pipe[i].cbRead << ", fSuccess " << fSuccess << "\n";
		}

		// The read operation completed successfully.
		if (Pipe[i].cbRead > 0 && fSuccess) {
			Pipe[i].fPendingIO = FALSE;
			return std::vector<char>(Pipe[i].chRequest.begin(), Pipe[i].chRequest.begin() + std::min<DWORD>(Pipe[i].cbRead, BUFSIZE));
		} else {
			log_error << "Socket::read instance_" << i << " failed with error code " << dwErr << "\n";
			if (!fSuccess && (dwErr == ERROR_IO_PENDING)) {
				Pipe[i].fPendingIO = TRUE;
				return buffer;
			}
			DisconnectAndReconnect(i);
		}
		Pipe[i].dwState = WRITING_STATE;
		Pipe[i].fPendingIO = false;

		break;
	}
	case WRITING_STATE: {
		log_error << "Socket::read instance_" << i << " ready to write and disconnect \n";
		const char finish_data[4] = {};
		DWORD sent_bytes = 0;
		fSuccess = WriteFile(Pipe[i].hPipeInst, finish_data, 4, &sent_bytes, &Pipe[i].oOverlap);

		if (fSuccess && sent_bytes == 4) {
			Pipe[i].fPendingIO = false;
			Pipe[i].dwState = READING_STATE;
		} else {
			dwErr = GetLastError();
			if (!fSuccess && (dwErr == ERROR_IO_PENDING)) {
				Pipe[i].fPendingIO = true;
			} else {
				DisconnectAndReconnect(i);
			}
		}
		break;
	}
	}
	return buffer;
}

int Socket_WIN::write(bool exit, std::vector<char> buffer)
{
	return writeToPipe(exit ? this->name_exit : this->name, buffer);
}

int Socket_WIN::writeLegacyExit(std::vector<char> buffer)
{
	if (this->name_exit == this->name_exit_legacy)
		return 0;

	return writeToPipe(this->name_exit_legacy, buffer);
}

int Socket_WIN::writeToPipe(const std::wstring &pipe_name, const std::vector<char> &buffer)
{
	HANDLE hPipe = CreateFile(const_cast<LPWSTR>(pipe_name.c_str()), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

	if (hPipe == INVALID_HANDLE_VALUE)
		return 0;

	if (GetLastError() == ERROR_PIPE_BUSY)
		return 0;

	DWORD bytesWritten = 0;

	WriteFile(hPipe, buffer.data(), buffer.size(), &bytesWritten, NULL);

	CloseHandle(hPipe);
	return (int)bytesWritten;
}

void Socket_WIN::disconnect()
{
	for (int i = 0; i < INSTANCES; i++) {
		if (Pipe[i].hPipeInst == INVALID_HANDLE_VALUE)
			continue;

		DisconnectNamedPipe(Pipe[i].hPipeInst);
		CloseHandle(Pipe[i].hPipeInst);
		CloseHandle(hEvents[i]);
	}
}