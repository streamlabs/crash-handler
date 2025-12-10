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

#include "../util.hpp"
#include "../logger.hpp"
#include <windows.h>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>
#include <codecvt>
#include <psapi.h>
#include "json.hpp"
#include <filesystem>

#include "upload-window-win.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/cognito-identity/CognitoIdentityClient.h>
#include <aws/identity-management/auth/CognitoCachingCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/BucketLocationConstraint.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompletedPart.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")

#include "Dbghelp.h"
#pragma comment(lib, "Dbghelp.lib")

#include "..\minizip\zip.h"
#include "..\minizip\iowin32.h"

#include <boost/locale.hpp>

std::wstring from_utf8_to_utf16_wide(const char *from, size_t length = -1);

LONG CALLBACK unhandledHandler(EXCEPTION_POINTERS *e)
{
	if (log_output_file.is_open() && !log_output_path.empty()) {
		log_info << "!! Crashed !!" << std::endl;

		// Small file, ~50-100 kb, MiniDumpNormal will only have stack, always same name to overwrite on purpose (small footprint)
		HANDLE hFile =
			CreateFileW((log_output_path + L".dmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
			MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {GetCurrentThreadId(), e, FALSE};
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MINIDUMP_TYPE(MiniDumpNormal), e ? &exceptionInfo : nullptr,
					  nullptr, nullptr);
			CloseHandle(hFile);
		}
	}

	// If the lock inside is locked then the process will abort, so this comes after
	UploadWindow::getInstance()->popRemoveFiles();
	return EXCEPTION_CONTINUE_SEARCH;
}

struct unhandledHandlerObj {
	unhandledHandlerObj() { ::SetUnhandledExceptionFilter(unhandledHandler); }
};

unhandledHandlerObj unhandledHandlerObj_Impl;

const std::wstring appStateFileName = L"\\appState";
std::wstring appCachePath = L"";

void Util::restartApp(std::wstring path)
{
	STARTUPINFO info = {sizeof(info)};
	PROCESS_INFORMATION processInfo;

	memset(&info, 0, sizeof(info));
	memset(&processInfo, 0, sizeof(processInfo));

	const std::wstring crash_handler_subdir = L"\\resources\\app.asar.unpacked/node_modules/crash-handler/crash-handler-process";
	if (path.size() <= crash_handler_subdir.size())
		return;

	std::wstring slobs_path = path.substr(0, path.size() - crash_handler_subdir.size());
	slobs_path += L"\\Streamlabs OBS.exe";
	log_info << "Slobs path to restart: " << std::string(slobs_path.begin(), slobs_path.end()) << std::endl;
	CreateProcess(slobs_path.c_str(), NULL, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &info, &processInfo);
}

void Util::runTerminateWindow(bool &shouldRestart)
{
	std::wstring title = from_utf8_to_utf16_wide(boost::locale::translate("An error occurred").str().c_str());

	auto message_translated1 = boost::locale::translate("An error occurred which has caused Streamlabs Desktop to close. Don't worry! "
							    "If you were streaming or recording, that is still happening in the background.");
	std::wstring message1 = from_utf8_to_utf16_wide(message_translated1.str().c_str());
	auto message_translated2 = boost::locale::translate("Whenever you're ready, we can relaunch the application, however this will "
							    "end your stream / recording session.");
	std::wstring message2 = from_utf8_to_utf16_wide(message_translated2.str().c_str());
	auto message_translated3 = boost::locale::translate("Click the Yes button to keep streaming / recording.");
	std::wstring message3 = from_utf8_to_utf16_wide(message_translated3.str().c_str());
	auto message_translated4 = boost::locale::translate("Click the No button to stop streaming / recording.");
	std::wstring message4 = from_utf8_to_utf16_wide(message_translated4.str().c_str());

	std::wstring message = message1 + L"\n\n" + message2 + L"\n\n" + message3 + L"\n\n" + message4;

	int code = MessageBox(NULL, message.c_str(), title.c_str(), MB_YESNO | MB_SYSTEMMODAL);
	switch (code) {
	case IDYES: {
		title = from_utf8_to_utf16_wide(boost::locale::translate("Choose when to restart").str().c_str());
		auto message_translated = boost::locale::translate("Your stream / recording session is still running in the background. "
								   "Whenever you're ready, click the OK "
								   "button below to end your stream / recording and relaunch the application.");
		message = from_utf8_to_utf16_wide(message_translated.str().c_str());
		MessageBox(NULL, message.c_str(), title.c_str(), MB_OK | MB_SYSTEMMODAL);
		shouldRestart = true;
		break;
	}
	case IDNO:
	default:
		break;
	}
}

static thread_local std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;

std::string from_utf16_wide_to_utf8(const wchar_t *from, size_t length = -1)
{
	const wchar_t *from_end;
	if (length == 0)
		return {};
	else if (length != -1)
		from_end = from + length;
	else
		return converter.to_bytes(from);
	return converter.to_bytes(from, from_end);
}

std::wstring from_utf8_to_utf16_wide(const char *from, size_t length)
{
	const char *from_end;
	if (length == 0)
		return {};
	else if (length != -1)
		from_end = from + length;
	else
		return converter.from_bytes(from);
	return converter.from_bytes(from, from_end);
}

struct ProcessInfo {
	uint64_t handle;
	uint64_t id;
	ProcessInfo()
	{
		this->handle = 0;
		this->id = 0;
	};
	ProcessInfo(uint64_t h, uint64_t i)
	{
		this->handle = h;
		this->id = i;
	}
};

ProcessInfo open_process(uint64_t handle)
{
	ProcessInfo pi;
	DWORD flags = PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | PROCESS_VM_READ;
	pi.handle = (uint64_t)OpenProcess(flags, false, (DWORD)handle);
	return pi;
}

bool close_process(ProcessInfo pi)
{
	return !!CloseHandle((HANDLE)pi.handle);
}

std::string get_process_name(ProcessInfo pi)
{
	LPWSTR lpBuffer = NULL;
	DWORD dwBufferLength = 256;
	HANDLE hProcess = (HANDLE)pi.handle;
	HMODULE hModule;
	DWORD unused1;
	BOOL bSuccess;
	/* We rely on undocumented behavior here where
	* enumerating a process' modules will provide
	* the process HMODULE first every time. */
	bSuccess = EnumProcessModules(hProcess, &hModule, sizeof(hModule), &unused1);
	if (!bSuccess)
		return {};
	while (32768 >= dwBufferLength) {
		std::wstring lpBuffer(dwBufferLength, wchar_t());
		DWORD dwReturnLength = GetModuleFileNameExW(hProcess, hModule, &lpBuffer[0], dwBufferLength);
		if (!dwReturnLength)
			return {};
		if (dwBufferLength <= dwReturnLength) {
			/* Increased buffer exponentially.
			* Notice this will eventually match
			* a perfect 32768 which is the max
			* length of an NTFS file path. */
			dwBufferLength <<= 1;
			continue;
		}
		/* Notice that these are expensive
		* but they do shrink the buffer to
		* match the string */
		return from_utf16_wide_to_utf8(lpBuffer.data());
	}
	/* Path too long */
	return {};
}

std::fstream open_file(std::string &file_path, std::fstream::openmode mode)
{
	return std::fstream(from_utf8_to_utf16_wide(file_path.c_str()), mode);
}

bool kill(ProcessInfo pinfo, uint32_t code)
{
	return TerminateProcess(reinterpret_cast<HANDLE>(pinfo.handle), code);
}

void Util::check_pid_file(std::string &pid_path)
{
	std::fstream::openmode mode = std::fstream::in | std::fstream::binary;
	std::fstream pid_file(open_file(pid_path, mode));
	union {
		uint64_t pid;
		char pid_char[sizeof(uint64_t)];
	};
	if (!pid_file)
		return;
	pid_file.read(&pid_char[0], 8);
	pid_file.close();
	ProcessInfo pi = open_process(pid);
	if (pi.handle == 0)
		return;
	std::string name = get_process_name(pi);
	if (name.find("crash-handler-process.exe") != std::string::npos) {
		kill(pi, -1);
	}
	close_process(pi);
}

void Util::write_pid_file(std::string &pid_path)
{
	std::fstream::openmode mode = std::fstream::out | std::fstream::binary;
	std::fstream pid_file(open_file(pid_path, mode));
	union {
		uint64_t pid;
		char pid_char[sizeof(uint64_t)];
	};
	if (!pid_file)
		return;

	pid = GetCurrentProcessId();
	if (pid == 0)
		return;
	pid_file.write(&pid_char[0], 8);
	pid_file.close();
}

std::string Util::get_temp_directory()
{
	constexpr DWORD tmp_size = MAX_PATH + 1;
	std::wstring tmp(tmp_size, wchar_t());
	GetTempPathW(tmp_size, &tmp[0]);
	/* Here we resize an in-use string and then re-use it.
	* Note this is only okay because the long path name
	* will always be equal to or larger than the short
	* path name */
	DWORD tmp_len = GetLongPathNameW(&tmp[0], NULL, 0);
	tmp.resize(tmp_len);
	/* Note that this isn't a hack to use the same buffer,
	* it's explicitly meant to be used this way per MSDN. */
	GetLongPathNameW(&tmp[0], &tmp[0], tmp_len);
	return from_utf16_wide_to_utf8(tmp.data());
}

void Util::setCachePath(std::wstring path)
{
	appCachePath = path;
}

void Util::updateAppState(Util::AppState state)
{
	const std::string freez_flag = "window_unresponsive";
	const std::string noncritical_flag = "crashed_noncritically";
	const std::string flag_name = "detected";

	std::ifstream state_file(appCachePath + appStateFileName, std::ios::in);
	if (!state_file.is_open())
		return;

	std::ostringstream buffer;
	buffer << state_file.rdbuf();
	state_file.close();

	std::string current_status = buffer.str();
	if (current_status.size() == 0)
		return;

	std::string updated_status = "";
	std::string existing_flag_value = "";
	nlohmann::json jsonEntry = nlohmann::json::parse(current_status);
	try {
		existing_flag_value = jsonEntry.at(flag_name);
	} catch (...) {
	}
	if (state == AppState::Unresponsive) {
		if (existing_flag_value.empty())
			jsonEntry[flag_name] = freez_flag;
	} else if (state == AppState::Responsive) {
		if (existing_flag_value.compare(freez_flag) == 0)
			jsonEntry[flag_name] = "";
	} else if (state == AppState::NoncriticallyDead) {
		jsonEntry[flag_name] = noncritical_flag;
	}
	updated_status = jsonEntry.dump(-1);

	std::ofstream out_state_file;
	out_state_file.open(appCachePath + appStateFileName, std::ios::trunc | std::ios::out);
	if (!out_state_file.is_open())
		return;

	out_state_file << updated_status << "\n";
	out_state_file.flush();
	out_state_file.close();
}

bool Util::archiveFile(const std::wstring &fileFullPath, const std::wstring &archiveFullPath, const std::string &nameInsideArchive)
{
	zlib_filefunc64_def ffunc;
	fill_win32_filefunc64W(&ffunc);

	int options = APPEND_STATUS_ADDINZIP;

	if (!std::filesystem::exists(archiveFullPath))
		options = APPEND_STATUS_CREATE;

	zipFile zf = zipOpen2_64(archiveFullPath.c_str(), options, nullptr, &ffunc);

	if (zf == NULL)
		return false;

	bool result = false;

	std::fstream file(fileFullPath, std::ios::binary | std::ios::in);

	if (file.is_open()) {
		file.seekg(0, std::ios::end);
		uint64_t size = file.tellg();
		file.seekg(0, std::ios::beg);

#undef max
		if (size > uint64_t(std::numeric_limits<uint32_t>().max())) {
			log_info << "File is too large for minizip" << std::endl;
			zipClose(zf, NULL);
			file.close();
			return false;
		}

		std::vector<char> buffer(size);
		if (size == 0 || file.read(&buffer[0], size)) {
			zip_fileinfo zfi = {0};

			if (zipOpenNewFileInZip(zf, nameInsideArchive.c_str(), &zfi, NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_BEST_SPEED) == S_OK) {
				if (zipWriteInFileInZip(zf, size == 0 ? "" : &buffer[0], size) == S_OK) {
					if (zipCloseFileInZip(zf) == S_OK)
						result = true;
				}
			}
		}

		file.close();
	}

	// If we can't close the archive for some reason... does that mean we failed to archive the file??
	// Probably it's archived successfully?? and then the archive is read/write stuck or something?? imo still return true
	zipClose(zf, NULL);
	log_info << "Finished archiving file" << std::endl;
	return result;
}

bool Util::saveMemoryDump(uint32_t pid, const std::wstring &dumpPath, const std::wstring &dumpFileName)
{
	bool dumpSaved = false;

	std::filesystem::path memoryDumpFolder = dumpPath;
	std::filesystem::path memoryDumpFile = memoryDumpFolder;
	memoryDumpFile.append(dumpFileName);

	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
	if (hProcess == NULL || hProcess == INVALID_HANDLE_VALUE) {
		log_info << "Failed to open process to get memory dump." << std::endl;
		return false;
	}

	bool enoughDiskSpace = false;
	ULARGE_INTEGER diskBytesAvailable;
	if (GetDiskFreeSpaceEx(memoryDumpFolder.generic_wstring().c_str(), &diskBytesAvailable, NULL, NULL)) {
		PROCESS_MEMORY_COUNTERS pmc;
		if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
			log_info << "Disk available space " << diskBytesAvailable.QuadPart << " , process ram size " << pmc.WorkingSetSize << std::endl;

			// There is now way to know a size of memory dump.
			// On test crashes it was about two times bigger than a ram size used by a process.
			if (pmc.WorkingSetSize < diskBytesAvailable.QuadPart * 2) {
				enoughDiskSpace = true;
			}
		}
	}

	if (!enoughDiskSpace) {
		log_info << "Failed to create memory dump. Not enough disk space  available" << std::endl;

		CloseHandle(hProcess);
		return false;
	}

	HANDLE hFile = CreateFile(memoryDumpFile.generic_wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile && hFile != INVALID_HANDLE_VALUE) {
		const DWORD CD_Flags = MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData |
				       MiniDumpWithFullMemoryInfo | MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory;

		BOOL ret = MiniDumpWriteDump(hProcess, pid, hFile, (MINIDUMP_TYPE)CD_Flags, 0, 0, 0);
		CloseHandle(hFile);

		if (ret) {
			dumpSaved = true;
			log_info << "Memory dump saved. " << std::endl;
		} else {
			log_info << "Failed to save memory dump. err code = " << GetLastError() << std::endl;
		}

	} else {
		log_info << "Failed to create memory dump file \"" << memoryDumpFile.generic_string() << "\"" << std::endl;
	}
	CloseHandle(hProcess);

	return dumpSaved;
}

std::mutex s3_mutex;
std::mutex upload_mutex;
std::condition_variable upload_variable;
long long total_sent_amout = 0;
std::chrono::steady_clock::time_point last_progress_update;
std::unique_ptr<Aws::S3::S3Client> s3_client_ptr;
bool upload_aborted = false;

void PutObjectAsyncFinished(const Aws::S3::S3Client *s3Client, const Aws::S3::Model::PutObjectRequest &request, const Aws::S3::Model::PutObjectOutcome &outcome,
			    const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context)
{
	if (outcome.IsSuccess()) {
		log_info << "PutObjectAsyncFinished: Finished uploading '" << context->GetUUID() << "'." << std::endl;
		log_info << "RequestId=" << outcome.GetResult().GetRequestId() << ", ETag=" << outcome.GetResult().GetETag() << std::endl;
	} else {
		total_sent_amout = -1;
		const auto &err = outcome.GetError();
		log_error << "PutObjectAsyncFinished failed | Message=" << err.GetMessage() << ", ExceptionName=" << err.GetExceptionName()
			  << ", ResponseCode=" << (int)err.GetResponseCode() << ", Retryable=" << (err.ShouldRetry() ? "true" : "false") << std::endl;
	}

	upload_variable.notify_one();
}

bool UploadFileMultipart(const Aws::S3::S3Client &s3_client, const Aws::String &bucket_name, const std::wstring &file_path, const std::wstring &file_name)
{
	const size_t PART_SIZE = 10 * 1024 * 1024; // 10MB parts
	Aws::String key = Aws::String("crash_memory_dumps/") + Aws::String(std::string(file_name.begin(), file_name.end()));

	std::filesystem::path uploaded_file = file_path;
	uploaded_file.append(file_name);

	std::ifstream file(uploaded_file, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		log_error << "Failed to open file for multipart upload: " << uploaded_file.generic_string() << std::endl;
		return false;
	}

	size_t file_size = file.tellg();
	file.seekg(0, std::ios::beg);
	log_info << "Multipart upload: file size=" << file_size << " bytes, part size=" << PART_SIZE << std::endl;

	Aws::S3::Model::CreateMultipartUploadRequest create_request;
	create_request.SetBucket(bucket_name);
	create_request.SetKey(key);
	create_request.SetContentType("application/zip");

	auto create_outcome = s3_client.CreateMultipartUpload(create_request);
	if (!create_outcome.IsSuccess()) {
		log_error << "Failed to initiate multipart upload: " << create_outcome.GetError().GetMessage() << std::endl;
		file.close();
		return false;
	}

	Aws::String upload_id = create_outcome.GetResult().GetUploadId();
	log_info << "Multipart upload initiated: uploadId=" << std::string(upload_id.c_str()) << std::endl;

	Aws::Vector<Aws::S3::Model::CompletedPart> completed_parts;
	int part_number = 1;
	total_sent_amout = 0;

	while (file && !upload_aborted) {
		std::vector<char> buffer(PART_SIZE);
		file.read(buffer.data(), PART_SIZE);
		std::streamsize bytes_read = file.gcount();

		if (bytes_read == 0)
			break;

		log_info << "Uploading part " << part_number << ", size=" << bytes_read << " bytes" << std::endl;

		auto stream = Aws::MakeShared<Aws::StringStream>("UploadPartStream");
		stream->write(buffer.data(), bytes_read);

		Aws::S3::Model::UploadPartRequest part_request;
		part_request.SetBucket(bucket_name);
		part_request.SetKey(key);
		part_request.SetUploadId(upload_id);
		part_request.SetPartNumber(part_number);
		part_request.SetBody(stream);
		part_request.SetContentLength(bytes_read);

		auto part_outcome = s3_client.UploadPart(part_request);
		if (!part_outcome.IsSuccess()) {
			log_error << "Failed to upload part " << part_number << ": " << part_outcome.GetError().GetMessage() << std::endl;

			Aws::S3::Model::AbortMultipartUploadRequest abort_request;
			abort_request.SetBucket(bucket_name);
			abort_request.SetKey(key);
			abort_request.SetUploadId(upload_id);
			s3_client.AbortMultipartUpload(abort_request);

			file.close();
			return false;
		}

		Aws::S3::Model::CompletedPart completed_part;
		completed_part.SetPartNumber(part_number);
		completed_part.SetETag(part_outcome.GetResult().GetETag());
		completed_parts.push_back(completed_part);

		total_sent_amout += bytes_read;
		UploadWindow::getInstance()->setUploadProgress(total_sent_amout);

		part_number++;
	}

	file.close();

	if (upload_aborted) {
		log_info << "Upload aborted by user" << std::endl;
		Aws::S3::Model::AbortMultipartUploadRequest abort_request;
		abort_request.SetBucket(bucket_name);
		abort_request.SetKey(key);
		abort_request.SetUploadId(upload_id);
		s3_client.AbortMultipartUpload(abort_request);
		return false;
	}

	Aws::S3::Model::CompletedMultipartUpload completed_upload;
	completed_upload.SetParts(completed_parts);

	Aws::S3::Model::CompleteMultipartUploadRequest complete_request;
	complete_request.SetBucket(bucket_name);
	complete_request.SetKey(key);
	complete_request.SetUploadId(upload_id);
	complete_request.SetMultipartUpload(completed_upload);

	auto complete_outcome = s3_client.CompleteMultipartUpload(complete_request);
	if (!complete_outcome.IsSuccess()) {
		log_error << "Failed to complete multipart upload: " << complete_outcome.GetError().GetMessage() << std::endl;

		Aws::S3::Model::AbortMultipartUploadRequest abort_request;
		abort_request.SetBucket(bucket_name);
		abort_request.SetKey(key);
		abort_request.SetUploadId(upload_id);
		s3_client.AbortMultipartUpload(abort_request);

		return false;
	}

	log_info << "Multipart upload completed successfully, ETag=" << std::string(complete_outcome.GetResult().GetETag().c_str()) << std::endl;
	return true;
}

void Util::abortUploadAWS()
{
	std::lock_guard<std::mutex> grd(s3_mutex);
	upload_aborted = true;
	if (s3_client_ptr != nullptr)
		s3_client_ptr->DisableRequestProcessing();
}

bool Util::uploadToAWS(const std::wstring &wspath, const std::wstring &fileName)
{
	UploadWindow::getInstance()->uploadStarted();
	bool ret = false;
	upload_aborted = false;
	Aws::SDKOptions options;
	options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Warn;
	options.loggingOptions.logger_create_fn = []() {
		return Aws::MakeShared<Aws::Utils::Logging::ConsoleLogSystem>("AWSLog", Aws::Utils::Logging::LogLevel::Warn);
	};
	Aws::InitAPI(options);
	{
		const Aws::String bucket_name = "streamlabs-obs-user-cache";
		const Aws::String region = "us-west-2";
		const Aws::String identityPoolId = "us-west-2:d5badad4-ff41-4a80-8677-e2757ab32263";

		Aws::Client::ClientConfiguration config;

		if (!region.empty()) {
			config.region = region;
		}
		config.scheme = Aws::Http::Scheme::HTTPS;
		config.verifySSL = true;
		config.followRedirects = Aws::Client::FollowRedirectsPolicy::NEVER;
		config.enableTcpKeepAlive = true;
		config.maxConnections = 25;
		config.lowSpeedLimit = 1;
		config.connectTimeoutMs = 30000;
		config.requestTimeoutMs = 600000;
		config.httpLibOverride = Aws::Http::TransferLibType::WIN_HTTP_CLIENT;
		log_info << "AWS ClientConfig | region=" << std::string(config.region.c_str()) << ", connectTimeoutMs=" << config.connectTimeoutMs
			 << ", requestTimeoutMs=" << config.requestTimeoutMs
			 << ", httpLibOverride=WIN_HTTP, tcpKeepAlive=" << (config.enableTcpKeepAlive ? "true" : "false") << std::endl;

		auto cognitoClient = Aws::MakeShared<Aws::CognitoIdentity::CognitoIdentityClient>("CognitoClient", config);
		auto credentialsProvider =
			Aws::MakeShared<Aws::Auth::CognitoCachingAnonymousCredentialsProvider>("CognitoProvider", identityPoolId, cognitoClient);

		{
			std::lock_guard<std::mutex> grd(s3_mutex);

			s3_client_ptr = std::make_unique<Aws::S3::S3Client>(credentialsProvider, config,
									    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::RequestDependent, false);
		}

		log_info << "Upload to ASW ready to start multipart upload" << std::endl;
		if (!UploadFileMultipart(*s3_client_ptr, bucket_name, wspath, fileName)) {
			log_info << "Upload to ASW multipart upload failed" << std::endl;
			UploadWindow::getInstance()->uploadFailed();
		} else {
			log_info << "Upload to ASW multipart upload completed successfully." << std::endl;
			UploadWindow::getInstance()->uploadFinished();
			ret = true;
		}
	}

	Aws::ShutdownAPI(options);

	{
		std::lock_guard<std::mutex> grd(s3_mutex);
		s3_client_ptr.reset();
	}

	return ret;
}

std::vector<char> get_messages_callback(std::string const &file_name, std::string const &encoding)
{
	static std::unordered_map<std::string, int> locales_resources(
		{{"/ar_SA/LC_MESSAGES/messages.mo", 101}, {"/cs_CZ/LC_MESSAGES/messages.mo", 102}, {"/da_DK/LC_MESSAGES/messages.mo", 103},
		 {"/de_DE/LC_MESSAGES/messages.mo", 104}, {"/en_US/LC_MESSAGES/messages.mo", 105}, {"/es_ES/LC_MESSAGES/messages.mo", 106},
		 {"/fr_FR/LC_MESSAGES/messages.mo", 107}, {"/hu_HU/LC_MESSAGES/messages.mo", 108}, {"/id_ID/LC_MESSAGES/messages.mo", 109},
		 {"/it_IT/LC_MESSAGES/messages.mo", 110}, {"/ja_JP/LC_MESSAGES/messages.mo", 111}, {"/ko_KR/LC_MESSAGES/messages.mo", 112},
		 {"/mk_MK/LC_MESSAGES/messages.mo", 113}, {"/nl_NL/LC_MESSAGES/messages.mo", 114}, {"/pl_PL/LC_MESSAGES/messages.mo", 115},
		 {"/pt_BR/LC_MESSAGES/messages.mo", 116}, {"/pt_PT/LC_MESSAGES/messages.mo", 117}, {"/ru_RU/LC_MESSAGES/messages.mo", 118},
		 {"/sk_SK/LC_MESSAGES/messages.mo", 119}, {"/sl_SI/LC_MESSAGES/messages.mo", 120}, {"/sv_SE/LC_MESSAGES/messages.mo", 121},
		 {"/th_TH/LC_MESSAGES/messages.mo", 122}, {"/tr_TR/LC_MESSAGES/messages.mo", 123}, {"/vi_VN/LC_MESSAGES/messages.mo", 124},
		 {"/zh_CN/LC_MESSAGES/messages.mo", 125}, {"/zh_TW/LC_MESSAGES/messages.mo", 126}});
	std::vector<char> localization;

	HMODULE hmodule = GetModuleHandle(NULL);
	if (hmodule) {
		HRSRC res;
		auto res_id = locales_resources.at(file_name);
		res = FindResource(hmodule, MAKEINTRESOURCEW(res_id), L"BINARY");
		if (res) {
			HGLOBAL res_load;
			res_load = LoadResource(hmodule, res);
			if (res_load) {
				LPVOID res_memory;
				res_memory = LockResource(res_load);
				if (res_memory) {
					size_t file_size = SizeofResource(hmodule, res);
					localization.resize(file_size);
					memcpy(localization.data(), res_memory, file_size);
				}
			}
			FreeResource(res_load);
		}
	}

	return localization;
}

void Util::setupLocale()
{
	const char *current_locale = std::setlocale(LC_ALL, nullptr);
	if (current_locale == nullptr || std::strlen(current_locale) == 0) {
		std::setlocale(LC_ALL, "en_US.UTF-8");
	}

	namespace blg = boost::locale::gnu_gettext;
	blg::messages_info info;

	info.paths.push_back("");
	info.domains.push_back(blg::messages_info::domain("messages"));
	info.callback = get_messages_callback;

	boost::locale::generator gen;
	std::locale base_locale = gen("");

	boost::locale::info const &properties = std::use_facet<boost::locale::info>(base_locale);
	info.language = properties.language();
	info.country = properties.country();
	info.encoding = properties.encoding();
	info.variant = properties.variant();
	try {
		std::locale real_locale(base_locale, blg::create_messages_facet<char>(info));
		std::locale::global(real_locale);
	} catch (...) {
		log_error << "Failed to setup localizaiton for a current language: " << info.language << "-" << info.country << std::endl;
	}
}
