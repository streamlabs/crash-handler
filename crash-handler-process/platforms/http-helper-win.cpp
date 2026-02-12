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

#include <Windows.h>
#include <stdio.h>
#include <objbase.h>
#include <comutil.h>
#include <wrl/client.h>

#include <memory>
#include <sstream>

#include "../util.hpp"
#include "../logger.hpp"

#include "httprequest.h"
#include "http-helper-win.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comsuppw.lib")

// IID for IWinHttpRequest.
//const IID IID_IWinHttpRequest =
//{
//  0x06f29373,
//  0x5c5a,
//  0x4b54,
//  {0xb0, 0x25, 0x6e, 0xf1, 0xbf, 0x8a, 0xbf, 0x0e}
//};

std::string from_utf16_wide_to_utf8(const wchar_t *from, size_t length = -1);
std::wstring from_utf8_to_utf16_wide(const char *from, size_t length = -1);

std::unique_ptr<HttpHelper> HttpHelper::Create()
{
	return std::make_unique<HttpHelper_WIN>();
}

HttpHelper_WIN::HttpHelper_WIN() {}

HttpHelper_WIN::~HttpHelper_WIN() {}

HttpHelper::Result HttpHelper_WIN::Request(Method method, std::string_view url, const Headers &requestHeaders, std::string_view body, std::uint32_t *statusCode,
					   Headers *responseHeaders, std::string *response)
{
	VARIANT varFalse;
	VariantInit(&varFalse);
	V_VT(&varFalse) = VT_BOOL;
	V_BOOL(&varFalse) = VARIANT_FALSE;

	VARIANT varEmpty;
	VariantInit(&varEmpty);
	V_VT(&varEmpty) = VT_ERROR;

	CLSID clsid;
	HRESULT hr = CLSIDFromProgID(L"WinHttp.WinHttpRequest.5.1", &clsid);
	if (FAILED(hr)) {
		return Result::CouldNotInit;
	}

	_bstr_t bstrMethod;
	switch (method) {
	case Method::Get:
		bstrMethod = L"GET";
		break;
	case Method::Post:
		bstrMethod = L"POST";
		break;
	default:
		return Result::InvalidParam;
	}

	std::wstring wsUrl = from_utf8_to_utf16_wide(url.data());
	_bstr_t bstrUrl(wsUrl.data());

	std::wstring wsBody = from_utf8_to_utf16_wide(body.data());
	_bstr_t bstrBody(wsBody.data());

	VARIANT varBody;
	VariantInit(&varBody);
	V_VT(&varBody) = VT_BSTR;
	varBody.bstrVal = bstrBody.GetBSTR();

	Microsoft::WRL::ComPtr<IWinHttpRequest> winHttpRequest;
	hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IWinHttpRequest, &winHttpRequest);
	if (FAILED(hr)) {
		return Result::CouldNotInit;
	}

	hr = winHttpRequest->Open(bstrMethod, bstrUrl, varFalse);
	if (FAILED(hr)) {
		return Result::ConnectFailed;
	}

	for (const auto &[header, value] : requestHeaders) {
		std::wstring wsHeader = from_utf8_to_utf16_wide(header.data());
		_bstr_t bstrHeader(wsHeader.data());
		std::wstring wsValue = from_utf8_to_utf16_wide(value.data());
		_bstr_t bstrValue(wsValue.data());

		hr = winHttpRequest->SetRequestHeader(bstrHeader, bstrValue);
		if (FAILED(hr)) {
			log_error << "Incorrectly formatted header: " << header << " = " << value << std::endl;
			return Result::InvalidParam;
		}
	}

	hr = winHttpRequest->Send((method == Method::Post) ? varBody : varEmpty);
	if (FAILED(hr)) {
		return Result::RequestFailed;
	}

	if (statusCode) {
		hr = winHttpRequest->get_Status(static_cast<long *>(static_cast<void *>(statusCode)));
		if (FAILED(hr)) {
			return Result::RequestFailed;
		}
	}

	if (responseHeaders) {
		BSTR bstrTmp = NULL;
		hr = winHttpRequest->GetAllResponseHeaders(&bstrTmp);
		if (FAILED(hr)) {
			return Result::ResponseFailed;
		}

		_bstr_t bstrResponseHeaders(bstrTmp, false);
		std::wstring wsResponseHeaders(bstrResponseHeaders, SysStringLen(bstrResponseHeaders));
		std::wstringstream wss(wsResponseHeaders);
		std::wstring token;
		while (std::getline(wss, token, L'\n')) {
			std::string::size_type pos = token.find(L':');
			if (pos != std::string::npos) {
				std::wstring wsHeader = token.substr(0, pos);
				_bstr_t bstrHeader(wsHeader.data());
				hr = winHttpRequest->GetResponseHeader(bstrHeader, &bstrTmp);
				if (SUCCEEDED(hr)) {
					_bstr_t bstrValue(bstrTmp, false);
					std::wstring wsValue(bstrValue, SysStringLen(bstrValue));
					std::string value = from_utf16_wide_to_utf8(wsValue.data());
					std::string header = from_utf16_wide_to_utf8(wsHeader.data());
					responseHeaders->emplace(header, value);
				}
			}
		}
	}

	if (response) {
		BSTR bstrTmp = NULL;
		hr = winHttpRequest->get_ResponseText(&bstrTmp);
		if (FAILED(hr)) {
			return Result::ResponseFailed;
		}

		_bstr_t bstrResponse(bstrTmp, false);
		std::wstring wsResponse(bstrResponse, SysStringLen(bstrResponse));

		*response = from_utf16_wide_to_utf8(wsResponse.data());
	}

	return Result::Success;
}

HttpHelper::Result HttpHelper_WIN::GetRequest(std::string_view url, const Headers &requestHeaders, std::uint32_t *statusCode, Headers *responseHeaders,
					      std::string *response)
{
	return Request(Method::Get, url, requestHeaders, "", statusCode, responseHeaders, response);
}

HttpHelper::Result HttpHelper_WIN::PostRequest(std::string_view url, const Headers &requestHeaders, std::string_view body, std::uint32_t *statusCode,
					       Headers *responseHeaders, std::string *response)
{
	return Request(Method::Post, url, requestHeaders, body, statusCode, responseHeaders, response);
}
