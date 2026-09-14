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

#include <codecvt>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "logger.hpp"
#include "util.hpp"

#include "http-helper.hpp"
#include "brief-crash-info-uploader.hpp"

#if defined(_WIN32)
std::string_view g_pathSeparator("\\");
#else
std::string_view g_pathSeparator("/");
#endif

std::string_view g_briefCrashDataFilename("brief-crash-info.json");

BriefCrashInfoUploader::BriefCrashInfoUploader(const std::string &appDataPath) : m_filename(appDataPath)
{
	if (*m_filename.rbegin() != '/' && *m_filename.rbegin() != '\\') {
		m_filename += g_pathSeparator;
	}
	m_filename += g_briefCrashDataFilename;
}

BriefCrashInfoUploader::~BriefCrashInfoUploader() {}

void BriefCrashInfoUploader::Run(std::int64_t maxFileWaitTimeMs)
{
	std::string json = ProcessBriefCrashInfoJson(maxFileWaitTimeMs);
	if (json.size()) {
		UploadJson(json);
	}
}

std::string BriefCrashInfoUploader::ProcessBriefCrashInfoJson(std::int64_t maxFileWaitTimeMs)
{
	log_info << "Waiting for brief crash info file: " << m_filename << std::endl;
	std::string json;
	std::filesystem::path briefCrashInfoPath = std::filesystem::u8path(m_filename);
	std::int64_t realFileWaitTimeMs = 0;
	while (realFileWaitTimeMs < maxFileWaitTimeMs) {
		if (std::filesystem::exists(briefCrashInfoPath) && std::filesystem::is_regular_file(briefCrashInfoPath)) {
			json = LoadBriefCrashInfoJson();
			std::filesystem::remove(briefCrashInfoPath);
			log_info << "The brief crash info file has been loaded." << std::endl;
			break;
		}
		// Obviously the sleep can take more than 100ms but not much so we ignore the difference
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		realFileWaitTimeMs += 100;
	}
	log_info << "Waited for the brief crash info file (ms): " << realFileWaitTimeMs << std::endl;
	return json;
}

std::string BriefCrashInfoUploader::LoadBriefCrashInfoJson()
{
#if defined(_WIN32)
	std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
	std::ifstream file(converter.from_bytes(m_filename));
#else
	std::ifstream file(m_filename);
#endif
	file.seekg(0, std::ios::end);
	size_t fileSize = file.tellg();

	std::string buffer(fileSize, ' ');
	file.seekg(0);
	file.read(&buffer.front(), fileSize);
	file.close();

	return buffer;
}

void BriefCrashInfoUploader::UploadJson(const std::string &json)
{
	log_info << "Uploading the brief crash info..." << std::endl;

	HttpHelper::Headers requestHeaders = {{"Content-Type", "application/json; Charset=UTF-8"}};
	HttpHelper::Headers responseHeaders;
	std::string response;

	std::uint32_t statusCode = 0;

	auto httpHelper = HttpHelper::Create();
	HttpHelper::Result result = httpHelper->PostRequest("https://httpbin.org/post", requestHeaders, json, &statusCode, &responseHeaders, &response);

	log_info << "Brief crash info upload result (0 means SUCCESS): " << static_cast<int>(result) << std::endl;
	log_info << "Brief crash info upload HTTP status code: " << statusCode << std::endl;
	log_info << "Brief crash info upload response: " << response << std::endl;

	for (const auto &[header, value] : responseHeaders) {
		log_info << "Brief crash info upload response header: " << header << " = " << value << std::endl;
	}
}
