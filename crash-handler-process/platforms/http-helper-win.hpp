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
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <map>

#include "../http-helper.hpp"

class HttpHelper_WIN : public HttpHelper {
public:
	HttpHelper_WIN();
	~HttpHelper_WIN() override;

	Result Request(Method method, std::string_view url, const Headers &requestHeaders, std::string_view body, std::uint32_t *statusCode,
		       Headers *responseHeaders, std::string *response) override;

	Result GetRequest(std::string_view url, const Headers &requestHeaders, std::uint32_t *statusCode, Headers *responseHeaders,
			  std::string *response) override;

	Result PostRequest(std::string_view url, const Headers &requestHeaders, std::string_view body, std::uint32_t *statusCode, Headers *responseHeaders,
			   std::string *response) override;
};