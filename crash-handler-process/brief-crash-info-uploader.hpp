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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class BriefCrashInfoUploader final {
public:
	BriefCrashInfoUploader(const std::string &appDataPath);
	~BriefCrashInfoUploader();

	void Run(std::int64_t maxFileWaitTimeMs = 10000);

private:
	std::string ProcessBriefCrashInfoJson(std::int64_t maxFileWaitTimeMs);
	std::string LoadBriefCrashInfoJson();
	void UploadJson(const std::string &json);

	std::string m_filename;
};
