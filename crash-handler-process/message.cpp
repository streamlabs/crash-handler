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

#include "message.hpp"

Message::Message(std::vector<char> buffer)
{
	m_buffer = buffer;
}

Message::~Message() {}

bool Message::ensureAvailable(uint64_t size)
{
	if (!m_valid || size > m_buffer.size() - index) {
		m_valid = false;
		return false;
	}
	return true;
}

bool Message::readBool()
{
	if (!ensureAvailable(sizeof(bool)))
		return false;
	bool value = reinterpret_cast<bool &>(m_buffer[index]);
	index++;
	return value;
}

uint8_t Message::readUInt8()
{
	if (!ensureAvailable(sizeof(uint8_t)))
		return 0;
	uint8_t value = reinterpret_cast<uint8_t &>(m_buffer[index]);
	index += sizeof(uint8_t);
	return value;
}

uint64_t Message::readUInt64()
{
	if (!ensureAvailable(sizeof(uint64_t)))
		return 0;
	uint64_t value = reinterpret_cast<uint64_t &>(m_buffer[index]);
	index += sizeof(uint64_t);
	return value;
}

uint32_t Message::readUInt32()
{
	if (!ensureAvailable(sizeof(uint32_t)))
		return 0;
	uint32_t value = reinterpret_cast<uint32_t &>(m_buffer[index]);
	index += sizeof(uint32_t);
	return value;
}

template<typename T> T Message::readStringImpl()
{
	const uint32_t string_size = readUInt32();
	if (string_size < sizeof(typename T::value_type) || string_size % sizeof(typename T::value_type) != 0) {
		m_valid = false;
		return T();
	}
	if (!ensureAvailable(string_size))
		return T();
	T value;
	value.resize(string_size / sizeof(typename T::value_type));
	memcpy(&value[0], &m_buffer[index], string_size);
	index += string_size;
	value.resize(string_size / sizeof(typename T::value_type) - 1);
	return value;
}

std::wstring Message::readWstring()
{
	return readStringImpl<std::wstring>();
}

std::string Message::readString()
{
	return readStringImpl<std::string>();
}