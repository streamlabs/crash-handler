/******************************************************************************
	Copyright (C) 2016-2026 by Streamlabs (General Workings Inc)

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

#include <catch2/catch_test_macros.hpp>

#include "platforms/socket-win.hpp"

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// A pipe name unique to this test process, so the test never talks to a real crash-handler.
std::wstring uniquePipeName(const wchar_t *suffix)
{
	return L"\\\\.\\pipe\\crash-handler-unit-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix;
}

// Sends one message the same way real clients (obs-studio-node, crash-handler-module) do:
// open the pipe, write one message, close it. Retries while all pipe instances are busy.
bool sendMessage(const std::wstring &pipeName, const std::string &payload, Clock::time_point deadline)
{
	while (Clock::now() < deadline) {
		HANDLE hPipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (hPipe == INVALID_HANDLE_VALUE) {
			if (GetLastError() == ERROR_PIPE_BUSY)
				WaitNamedPipeW(pipeName.c_str(), 100);
			else
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		DWORD bytesWritten = 0;
		// Include the terminating NUL so the receiver can read the payload as a C string.
		const BOOL written = WriteFile(hPipe, payload.c_str(), (DWORD)payload.size() + 1, &bytesWritten, NULL);
		CloseHandle(hPipe);
		return written && bytesWritten == payload.size() + 1;
	}
	return false;
}

std::string messageId(int i)
{
	return "test-message-" + std::to_string(i);
}

// Starts a Socket_WIN server, sends `count` messages from another thread, and returns
// the distinct payloads the server received before `timeout` ran out.
std::set<std::string> sendAndReceive(const wchar_t *pipeSuffix, int count, std::chrono::seconds timeout, int &sentOk)
{
	const std::wstring pipeName = uniquePipeName(pipeSuffix);
	Socket::set_ipc_path(pipeName);
	Socket_WIN server;
	REQUIRE_FALSE(server.initialization_failed);

	const auto deadline = Clock::now() + timeout;
	std::atomic<int> sent{0};
	std::thread client([&] {
		for (int i = 0; i < count; i++) {
			if (sendMessage(pipeName, messageId(i), deadline))
				sent++;
		}
	});

	std::set<std::string> received;
	while (Clock::now() < deadline && (int)received.size() < count) {
		std::vector<char> buffer = server.read();
		if (!buffer.empty())
			received.insert(std::string(buffer.data()));
	}

	client.join();
	server.disconnect();
	sentOk = sent;
	return received;
}

} // namespace

TEST_CASE("Socket_WIN receives a single message", "[socket-win]")
{
	int sentOk = 0;
	const auto received = sendAndReceive(L"single", 1, std::chrono::seconds(5), sentOk);

	REQUIRE(sentOk == 1);
	REQUIRE(received == std::set<std::string>{messageId(0)});
}

// Regression test: once a pipe instance had handled a message, the next message routed to
// that instance was dropped. With only 8 instances, any session sending more than 8 messages
// lost roughly every other message, including obs-server's "unregister" at shutdown.
TEST_CASE("Socket_WIN receives every message when pipe instances are reused", "[socket-win]")
{
	constexpr int count = 50; // well above the 8 pipe instances, so every instance is reused
	int sentOk = 0;
	const auto received = sendAndReceive(L"reuse", count, std::chrono::seconds(15), sentOk);

	REQUIRE(sentOk == count);

	std::vector<std::string> missing;
	for (int i = 0; i < count; i++) {
		if (received.count(messageId(i)) == 0)
			missing.push_back(messageId(i));
	}
	INFO("received " << received.size() << " of " << count << " messages");
	CHECK(missing.empty());
	REQUIRE(received.size() == count);
}
