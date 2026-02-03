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

#include <windows.h>
#include <CommCtrl.h>
#include <thread>
#include <filesystem>

#include "../util.hpp"
#include "../logger.hpp"
#include "upload-window-win.hpp"
#include <boost/locale.hpp>

std::unique_ptr<UploadWindow> UploadWindow::instance = nullptr;

#define CUSTOM_CLOSE_MSG (WM_USER + 1)
#define CUSTOM_PROGRESS_MSG (WM_USER + 2)

#define CUSTOM_CAUGHT_CRASH (WM_USER + 3)
#define CUSTOM_SAVE_STARTED (WM_USER + 4)
#define CUSTOM_SAVED_DUMP (WM_USER + 5)
#define CUSTOM_SAVING_DUMP_FAILED (WM_USER + 6)
#define CUSTOM_UPLOAD_STARTED (WM_USER + 7)
#define CUSTOM_UPLOAD_FINISHED (WM_USER + 8)
#define CUSTOM_UPLOAD_FAILED (WM_USER + 9)
#define CUSTOM_UPLOAD_CANCELED (WM_USER + 10)
#define CUSTOM_ZIPPING_STARTED (WM_USER + 11)
#define CUSTOM_REQUESTED_CLICK (WM_USER + 12)
constexpr int IDC_HYPERLINK = 1001;

std::wstring from_utf8_to_utf16_wide(const char *from, size_t length = -1);

LRESULT CALLBACK FrameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE: {
		UploadWindow::getInstance()->onUserWantsToClose();
		SetCursor(LoadCursor(NULL, IDC_WAIT));

		// Aws/File operations tied to WM_CLOSE instead of onUserWantsToClose to avoid future risk of misusing onUserWantsToClose
		Util::abortUploadAWS();
		while (UploadWindow::getInstance()->hasRemoveFilesQueued()) {
			using namespace std::chrono_literals;
			UploadWindow::getInstance()->popRemoveFiles();
			std::this_thread::sleep_for(50ms);
		}
		return 0;
	}
	case WM_NCCREATE:
	case WM_CREATE: {
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_NCDESTROY:
		break;
	default: {
		return UploadWindow::getInstance()->WndProc(hwnd, msg, wParam, lParam);
	}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void UploadWindow::showButtons(const DialogButtonsState &state)
{
	ShowWindow(ok_button_hwnd, state.ok ? SW_SHOW : SW_HIDE);
	ShowWindow(yes_button_hwnd, state.yes ? SW_SHOW : SW_HIDE);
	ShowWindow(cancel_button_hwnd, state.cancel ? SW_SHOW : SW_HIDE);
	ShowWindow(no_button_hwnd, state.no ? SW_SHOW : SW_HIDE);
}

void UploadWindow::enableButtons(const DialogButtonsState &state)
{
	EnableWindow(ok_button_hwnd, state.ok);
	EnableWindow(yes_button_hwnd, state.yes);
	EnableWindow(cancel_button_hwnd, state.cancel);
	EnableWindow(no_button_hwnd, state.no);
}

LRESULT UploadWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (upload_window_hwnd == NULL) {
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	if (blockedMessageDueToShutdown(msg))
		return DefWindowProc(hwnd, msg, wParam, lParam);

	switch (msg) {
	case CUSTOM_CLOSE_MSG:
		log_info << "UploadWindow close message recieved" << std::endl;
		DestroyWindow(upload_window_hwnd);
		upload_window_hwnd = NULL;
		break;
	case CUSTOM_CAUGHT_CRASH: {
		std::string message_boost = boost::locale::translate("The application just crashed.").str() + std::string("\r\n\r\n") +
					    boost::locale::translate("Would you like to send a report to the developers?").str();
		std::wstring caught_crash_message = from_utf8_to_utf16_wide(message_boost.c_str());

		SetWindowText(upload_label_hwnd, caught_crash_message.c_str());
		showButtons({.ok = false, .cancel = true, .yes = true, .no = false});
		enableButtons({.ok = false, .cancel = true, .yes = true, .no = false});
		SendMessage(progresss_bar_hwnd, PBM_SETBARCOLOR, 0, RGB(49, 195, 162));
		SendMessage(progresss_bar_hwnd, PBM_SETRANGE32, 0, 100);
		SendMessage(progresss_bar_hwnd, PBM_SETPOS, 0, 0);
		ShowWindow(progresss_bar_hwnd, SW_SHOW);
		ShowWindow(hyperlink_hwnd, SW_SHOW);
		break;
	}
	case CUSTOM_PROGRESS_MSG: {
		double progress = ((double)bytes_sent) / ((double)total_bytes_to_send / 100.0);
		PostMessage(progresss_bar_hwnd, PBM_SETPOS, (int)progress, 0);
		std::wstring upload_message = from_utf8_to_utf16_wide(boost::locale::translate("Uploading... %.2f%%\r\n%.1f / %.1fmb").str().c_str());
		swprintf(upload_progress_message, upload_message_len, upload_message.c_str(), static_cast<float>(progress),
			 static_cast<float>(bytes_sent) / (1024.f * 1024.f), static_cast<float>(total_bytes_to_send) / (1024.f * 1024.f));
		SetWindowText(upload_label_hwnd, upload_progress_message);
		break;
	}
	case CUSTOM_SAVE_STARTED: {
		std::wstring save_message = from_utf8_to_utf16_wide(boost::locale::translate("Saving... to \"%s\"").str().c_str());
		swprintf(upload_progress_message, upload_message_len, save_message.c_str(), file_name.c_str());
		SetWindowText(upload_label_hwnd, upload_progress_message);
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_ZIPPING_STARTED: {
		PostMessage(progresss_bar_hwnd, PBM_SETPOS, (int)25, 0);
		std::wstring zipping_message = from_utf8_to_utf16_wide(boost::locale::translate("Zipping... to \"%s\"").str().c_str());
		swprintf(upload_progress_message, upload_message_len, zipping_message.c_str(), file_name.c_str());
		SetWindowText(upload_label_hwnd, upload_progress_message);
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_SAVING_DUMP_FAILED: {
		std::wstring dump_failed_message =
			from_utf8_to_utf16_wide(boost::locale::translate("Failed to save the local debug information.").str().c_str());
		SetWindowText(upload_label_hwnd, dump_failed_message.c_str());
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_UPLOAD_STARTED: {
		SendMessage(progresss_bar_hwnd, PBM_SETRANGE32, 0, 100);
		std::wstring upload_start_message = from_utf8_to_utf16_wide(boost::locale::translate("Initializing upload...").str().c_str());
		SetWindowText(upload_label_hwnd, upload_start_message.c_str());
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_UPLOAD_FINISHED: {
		auto message_boost = boost::locale::translate("Successfully uploaded the debug information.\r\n"
							      "Please provide this file name to the support.\r\n"
							      "File: \"%s\".");
		std::wstring upload_finished_message = from_utf8_to_utf16_wide(message_boost.str().c_str());
		swprintf(upload_progress_message, upload_message_len, upload_finished_message.c_str(), file_name.c_str());
		SetWindowText(upload_label_hwnd, upload_progress_message);
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = true, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_UPLOAD_CANCELED: {
		std::wstring upload_upload_canceled = from_utf8_to_utf16_wide(boost::locale::translate("Upload cancleled.\r\n%s removed.").str().c_str());
		swprintf(upload_progress_message, upload_message_len, upload_upload_canceled.c_str(), file_name.c_str());
		SetWindowText(upload_label_hwnd, upload_progress_message);
		showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
		enableButtons({.ok = true, .cancel = false, .yes = false, .no = false});
		break;
	}
	case CUSTOM_UPLOAD_FAILED: {
		std::wstring upload_upload_failed = from_utf8_to_utf16_wide(boost::locale::translate("Upload failed. Save a copy?\r\n\"%s/%s\"").str().c_str());
		swprintf(upload_progress_message, upload_message_len, upload_upload_failed.c_str(), dump_path.c_str(), file_name.c_str());
		SetWindowText(upload_label_hwnd, upload_progress_message);
		showButtons({.ok = false, .cancel = false, .yes = true, .no = true});
		enableButtons({.ok = false, .cancel = false, .yes = true, .no = true});
		break;
	}
	case WM_COMMAND: {
		if ((HWND)lParam == ok_button_hwnd) {
			enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
			SetWindowText(upload_label_hwnd, L"");
			button_clicked = IDOK;
			upload_window_choose_variable.notify_one();
			break;
		}
		if ((HWND)lParam == yes_button_hwnd) {
			enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
			SetWindowText(upload_label_hwnd, L"");
			button_clicked = IDYES;
			upload_window_choose_variable.notify_one();
			break;
		}
		if ((HWND)lParam == cancel_button_hwnd) {
			enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
			SetWindowText(upload_label_hwnd, L"");
			button_clicked = IDCANCEL;
			upload_window_choose_variable.notify_one();
			break;
		}
		if ((HWND)lParam == no_button_hwnd) {
			enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});
			SetWindowText(upload_label_hwnd, L"");
			button_clicked = IDNO;
			upload_window_choose_variable.notify_one();
			break;
		}
		// Extract notification code and control ID
		int notificationCode = HIWORD(wParam);
		int controlId = LOWORD(wParam);
		if (controlId == IDC_HYPERLINK && notificationCode == STN_CLICKED) {
			ShellExecuteW(nullptr, L"open", L"https://streamlabs.com/content-hub/post/streamlabs-desktop-crash-troubleshooting-guide", nullptr,
				      nullptr, SW_SHOWNORMAL);
			break;
		}
		break;
	}
	case CUSTOM_REQUESTED_CLICK: {
		if (!upload_window_hwnd) {
			button_clicked = IDOK;
			upload_window_choose_variable.notify_one();
		} else {
			if (button_clicked != 0) {
				upload_window_choose_variable.notify_one();
			}
		}
		break;
	}
	case WM_CTLCOLORSTATIC: {
		if ((HWND)lParam == hyperlink_hwnd) {
			HDC hdc = (HDC)wParam;
			SetTextColor(hdc, RGB(0, 0, 255));
			SetBkMode(hdc, TRANSPARENT);
			return (LRESULT)GetStockObject(NULL_BRUSH);
		}
		[[fallthrough]];
	}
	case WM_CTLCOLOREDIT: {
		if ((HWND)lParam == upload_label_hwnd) {
			HDC hdc = (HDC)wParam;
			return (LRESULT)GetStockObject(CTLCOLOR_MSGBOX);
		}
		break;
	}
	}

	return DefWindowProc(hwnd, msg, wParam, lParam);
}

UploadWindow::UploadWindow() {}

UploadWindow::~UploadWindow()
{
	popRemoveFiles();

	if (hasRemoveFilesQueued())
		log_error << "UploadWindow failed to auto remove all queued files " << std::endl;

	if (window_thread) {
		PostMessage(upload_window_hwnd, CUSTOM_CLOSE_MSG, 0, 0);
		if (window_thread->joinable()) {
			window_thread->join();
		}
		window_thread = nullptr;
	}
};

UploadWindow *UploadWindow::getInstance()
{
	static std::unique_ptr<UploadWindow> instance = std::make_unique<UploadWindow>();
	return instance.get();
}

void UploadWindow::shutdownInstance()
{
	instance.reset();
}

void UploadWindow::onUserWantsToClose()
{
	user_wants_to_close = true;
	button_clicked = IDABORT;
	upload_window_choose_variable.notify_one();
	SetWindowText(upload_label_hwnd, L"Closing...");
}

void UploadWindow::registerRemoveFile(const std::wstring &fullPath)
{
	std::lock_guard<std::mutex> grd(upload_remove_file_mutex);
	filesForRemoval.insert(fullPath);
}

void UploadWindow::unregisterRemoveFile(const std::wstring &fullPath)
{
	std::lock_guard<std::mutex> grd(upload_remove_file_mutex);
	filesForRemoval.erase(fullPath);
}

bool UploadWindow::hasRemoveFilesQueued()
{
	std::lock_guard<std::mutex> grd(upload_remove_file_mutex);

	for (auto &itr : filesForRemoval) {
		if (std::filesystem::exists(itr))
			return true;
	}

	return false;
}

void UploadWindow::popRemoveFile(const std::wstring &fullPath)
{
	std::lock_guard<std::mutex> grd(upload_remove_file_mutex);

	auto itr = filesForRemoval.find(fullPath);

	if (itr == filesForRemoval.end())
		return;

	if (std::filesystem::exists(*itr)) {
		try {
			std::filesystem::remove(*itr);
			filesForRemoval.erase(itr);
		} catch (...) {
			log_error << "UploadWindow failed to auto remove file " << std::endl;
		}
	}
}

void UploadWindow::popRemoveFiles()
{
	std::lock_guard<std::mutex> grd(upload_remove_file_mutex);

	auto itr = filesForRemoval.begin();

	while (itr != filesForRemoval.end()) {
		if (std::filesystem::exists(*itr)) {
			try {
				std::filesystem::remove(*itr);
				itr = filesForRemoval.erase(itr);
			} catch (...) {
				++itr;
			}
		} else {
			itr = filesForRemoval.erase(itr);
		}
	}
}

void UploadWindow::windowThread()
{
	log_info << "UploadWindow windowThread started" << std::endl;
	hInstance = GetModuleHandle(NULL);

	WNDCLASSEX wc;
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = 0;
	wc.lpfnWndProc = FrameWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(CTLCOLOR_MSGBOX);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = L"uploaderwindowclass";
	wc.hIcon = LoadIcon(NULL, IDI_ERROR);
	if (!RegisterClassEx(&wc)) {
		log_error << "Failed to create a class for uploader window " << GetLastError() << std::endl;
		upload_window_choose_variable.notify_one();
		return;
	}

	/* We only care about the main display */
	int screen_width = GetSystemMetrics(SM_CXSCREEN);
	int screen_height = GetSystemMetrics(SM_CYSCREEN);
	std::wstring upload_window_title =
		from_utf8_to_utf16_wide(boost::locale::translate("Streamlabs Desktop has encountered a critical error").str().c_str());

	upload_window_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, L"uploaderwindowclass", upload_window_title.c_str(),
					    WS_OVERLAPPED | WS_MINIMIZEBOX | WS_SYSMENU | WS_EX_TOPMOST, (screen_width - width) / 2,
					    (screen_height - height) / 2, width, height, NULL, NULL, hInstance, NULL);
	if (!upload_window_hwnd) {
		log_error << "Failed to create an uploader window " << GetLastError() << std::endl;
		upload_window_choose_variable.notify_one();
		return;
	}

	int x_pos = 10;
	int y_pos = 10;
	int x_size = 470;
	int y_size = height;
	RECT client_rect;
	if (GetClientRect(upload_window_hwnd, &client_rect)) {
		x_size = client_rect.right - client_rect.left;
		y_size = client_rect.bottom - client_rect.top;
	}

	progresss_bar_hwnd = CreateWindow(PROGRESS_CLASS, TEXT("ProgressWorker"), WS_CHILD | PBS_SMOOTH, x_pos, y_pos, x_size - 20, 40, upload_window_hwnd,
					  NULL, NULL, NULL);

	const int label_height = 90;
	const int label_ypos = y_pos + 50;
	upload_label_hwnd = CreateWindow(WC_EDIT, TEXT(""), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, x_pos, label_ypos,
					 x_size - 20, label_height, upload_window_hwnd, NULL, NULL, NULL);

	std::wstring link_text = from_utf8_to_utf16_wide(boost::locale::translate("Troubleshoot here").str().c_str());
	const HMENU hyperlinkId = reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HYPERLINK));
	hyperlink_hwnd = CreateWindow(WC_STATIC, link_text.c_str(), SS_LEFT | WS_CHILD | SS_NOTIFY, x_pos, y_pos + (label_ypos + label_height), x_size - 20, 20,
				      upload_window_hwnd, hyperlinkId, NULL, NULL);

	std::wstring yes_button_title = from_utf8_to_utf16_wide(boost::locale::translate("Yes").str().c_str());
	std::wstring no_button_title = from_utf8_to_utf16_wide(boost::locale::translate("No").str().c_str());
	std::wstring cancel_button_title = from_utf8_to_utf16_wide(boost::locale::translate("Cancel").str().c_str());
	std::wstring ok_button_title = from_utf8_to_utf16_wide(boost::locale::translate("OK").str().c_str());

	ok_button_hwnd = CreateWindow(WC_BUTTON, ok_button_title.c_str(), WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, x_size - 220, y_size - 50, 100,
				      40, upload_window_hwnd, NULL, NULL, NULL);

	yes_button_hwnd = CreateWindow(WC_BUTTON, yes_button_title.c_str(), WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, x_size - 220, y_size - 50,
				       100, 40, upload_window_hwnd, NULL, NULL, NULL);

	cancel_button_hwnd = CreateWindow(WC_BUTTON, cancel_button_title.c_str(), WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, x_size - 110,
					  y_size - 50, 100, 40, upload_window_hwnd, NULL, NULL, NULL);

	no_button_hwnd = CreateWindow(WC_BUTTON, no_button_title.c_str(), WS_TABSTOP | WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, x_size - 110, y_size - 50, 100,
				      40, upload_window_hwnd, NULL, NULL, NULL);

	ShowWindow(upload_window_hwnd, SW_SHOWNORMAL);
	UpdateWindow(upload_window_hwnd);
	showButtons({.ok = true, .cancel = true, .yes = false, .no = false});
	enableButtons({.ok = false, .cancel = false, .yes = false, .no = false});

	HFONT main_font = CreateFont(0, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				     DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	if (main_font) {
		SendMessage(upload_label_hwnd, WM_SETFONT, WPARAM(main_font), TRUE);
		SendMessage(ok_button_hwnd, WM_SETFONT, WPARAM(main_font), TRUE);
		SendMessage(yes_button_hwnd, WM_SETFONT, WPARAM(main_font), TRUE);
		SendMessage(cancel_button_hwnd, WM_SETFONT, WPARAM(main_font), TRUE);
		SendMessage(no_button_hwnd, WM_SETFONT, WPARAM(main_font), TRUE);
	}
	HFONT underline_font = CreateFont(0, 0, 0, 0, FW_NORMAL, FALSE, TRUE /* underline */, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
					  DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
	if (hyperlink_hwnd && underline_font) {
		SendMessage(hyperlink_hwnd, WM_SETFONT, WPARAM(underline_font), TRUE);
	}
	SendMessage(upload_label_hwnd, EM_SETREADONLY, TRUE, 0);

	upload_window_choose_variable.notify_one();
	MSG msg;

	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	if (main_font) {
		DeleteObject(main_font);
	}
	if (underline_font) {
		DeleteObject(underline_font);
	}
	log_info << "UploadWindow windowThread at finish" << std::endl;
}

bool UploadWindow::createWindow()
{
	window_thread = new std::thread(&UploadWindow::windowThread, this);

	std::unique_lock<std::mutex> lock(upload_window_choose_mutex);
	upload_window_choose_variable.wait(lock);

	if (!window_thread || !window_thread->joinable()) {
		return false;
	}

	if (window_thread && window_thread->joinable() && !upload_window_hwnd) {
		return false;
	}

	return true;
}

void UploadWindow::uploadFinished()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_UPLOAD_FINISHED, NULL, NULL);
}

void UploadWindow::uploadStarted()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_UPLOAD_STARTED, NULL, NULL);
}

void UploadWindow::uploadFailed()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_UPLOAD_FAILED, NULL, NULL);
}

void UploadWindow::uploadCanceled()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_UPLOAD_CANCELED, NULL, NULL);
}

void UploadWindow::savingStarted()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_SAVE_STARTED, NULL, NULL);
}

void UploadWindow::zippingStarted()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_ZIPPING_STARTED, NULL, NULL);
}

void UploadWindow::savingFailed()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_SAVING_DUMP_FAILED, NULL, NULL);
}

void UploadWindow::crashCaught()
{
	if (user_wants_to_close) {
		return;
	}

	button_clicked = 0;
	PostMessage(upload_window_hwnd, CUSTOM_CAUGHT_CRASH, NULL, NULL);
}

int UploadWindow::waitForUserChoise()
{
	if (button_clicked != 0) {
		return button_clicked;
	}

	if (user_wants_to_close) {
		return IDABORT;
	}

	std::unique_lock<std::mutex> lock(upload_window_choose_mutex);
	PostMessage(upload_window_hwnd, CUSTOM_REQUESTED_CLICK, NULL, NULL);
	upload_window_choose_variable.wait(lock);

	return button_clicked;
}

void UploadWindow::setDumpFileName(const std::wstring &new_file_name)
{
	file_name = new_file_name;
}

void UploadWindow::setDumpPath(const std::wstring &new_path)
{
	dump_path = new_path;
}

void UploadWindow::setTotalBytes(long long new_total)
{
	total_bytes_to_send = new_total;
}

void UploadWindow::setUploadProgress(long long sent_amount)
{
	bytes_sent = sent_amount;
	PostMessage(upload_window_hwnd, CUSTOM_PROGRESS_MSG, NULL, NULL);
}

bool UploadWindow::blockedMessageDueToShutdown(const INT msg) const
{
	if (!user_wants_to_close) {
		return false;
	}

	switch (msg) {
	case CUSTOM_PROGRESS_MSG:
	case CUSTOM_SAVE_STARTED:
	case CUSTOM_SAVED_DUMP:
	case CUSTOM_SAVING_DUMP_FAILED:
	case CUSTOM_UPLOAD_STARTED:
	case CUSTOM_UPLOAD_FINISHED:
	case CUSTOM_UPLOAD_FAILED:
	case CUSTOM_UPLOAD_CANCELED:
	case CUSTOM_ZIPPING_STARTED:
	case CUSTOM_REQUESTED_CLICK:
		return true;
	}

	return false;
}