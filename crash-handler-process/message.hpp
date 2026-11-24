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

#ifndef MESSAGE_H
#define MESSAGE_H

#include <vector>
#include <string>

enum class Action : uint8_t {
	REGISTER = 0,
	UNREGISTER = 1,
	REGISTERMEMORYDUMP = 2,
	CRASHWITHCODE = 3,
	CRASHED_MODULE_INFO = 4,
	OUTPUT_STATE = 6,
	SHUTDOWN_INTENT = 7,
};

class Message {
public:
	Message(std::vector<char> buffer);
	~Message();

private:
	std::vector<char> m_buffer;
	uint64_t index = 0;
	bool m_valid = true;

	bool ensureAvailable(uint64_t size);
	template<typename T> T readStringImpl();

public:
	bool isValid() const { return m_valid; }
	bool readBool();
	uint64_t readUInt64();
	uint32_t readUInt32();
	uint8_t readUInt8();
	std::wstring readWstring();
	std::string readString();
};

#endif