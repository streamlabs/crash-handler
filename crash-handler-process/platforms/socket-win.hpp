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

#include "../socket.hpp"
#include <vector>
#include <windows.h>
#include <tchar.h>

#define CONNECTING_STATE 0
#define READING_STATE 1
#define WRITING_STATE 2
#define INSTANCES 8
#define PIPE_TIMEOUT 5000
#define BUFSIZE 512

typedef struct {
	OVERLAPPED oOverlap;
	HANDLE hPipeInst;
	std::vector<char> chRequest;
	DWORD cbRead;
	TCHAR chReply[BUFSIZE];
	DWORD cbToWrite;
	DWORD dwState;
	BOOL fPendingIO;
} PIPEINST, *LPPIPEINST;

class Socket_WIN : public Socket {
private:
	PIPEINST Pipe[INSTANCES];
	HANDLE hEvents[INSTANCES];
	std::wstring name;
	std::wstring name_exit;
	std::wstring name_exit_legacy;
	static std::wstring ipc_path;

	BOOL ConnectToNewClient(HANDLE hPipe, LPOVERLAPPED lpo);
	void DisconnectAndReconnect(DWORD i);
	bool initPipe(int &i);
	int writeToPipe(const std::wstring &pipe_name, const std::vector<char> &buffer);

public:
	Socket_WIN();
	virtual ~Socket_WIN();

public:
	virtual std::vector<char> read() override;
	virtual int write(bool exit, std::vector<char> buffer) override;
	virtual int writeLegacyExit(std::vector<char> buffer) override;
	virtual void disconnect() override;
	friend void Socket::set_ipc_path(const std::wstring &new_ipc_path);
};