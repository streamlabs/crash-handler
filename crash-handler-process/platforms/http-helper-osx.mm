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

#import <CoreServices/CoreServices.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <CFNetwork/CFHTTPStream.h>
#import <CFNetwork/CFHTTPMessage.h>

#include <thread>

#include "../util.hpp"
#include "../logger.hpp"

#include "http-helper-osx.hpp"

std::unique_ptr<HttpHelper> HttpHelper::Create()
{
	return std::make_unique<HttpHelper_OSX>();
}

HttpHelper_OSX::HttpHelper_OSX() {}

HttpHelper_OSX::~HttpHelper_OSX() {}

HttpHelper_OSX::Result HttpHelper_OSX::Request(Method method, std::string_view url, const Headers &requestHeaders, std::string_view body,
					       std::uint32_t *statusCode, Headers *responseHeaders, std::string *response)
{
	dispatch_semaphore_t completionSemaphore = dispatch_semaphore_create(0);

	NSString *u = [NSString stringWithUTF8String:url.data()];
	NSMutableURLRequest *urlRequest = [[NSMutableURLRequest alloc] initWithURL:[NSURL URLWithString:u]];
	switch (method) {
	case Method::Get:
		[urlRequest setHTTPMethod:@"GET"];
		break;
	case Method::Post:
		[urlRequest setHTTPMethod:@"POST"];
		break;
	default:
		return Result::InvalidParam;
	}

	for (const auto &[header, value] : requestHeaders) {
		NSString *h = [NSString stringWithUTF8String:header.data()];
		NSString *v = [NSString stringWithUTF8String:value.data()];
		[urlRequest setValue:v forHTTPHeaderField:h];
	}

	if (method == Method::Post) {
		NSData *b = [NSData dataWithBytes:body.data() length:static_cast<NSUInteger>(body.size())];
		[urlRequest setHTTPBody:b];
	}

	NSURLSession *session = [NSURLSession sharedSession];

	std::string errorDescription;
	std::string *errorDescriptionPtr = &errorDescription;

	NSURLSessionDataTask *dataTask = [session
		dataTaskWithRequest:urlRequest
		  completionHandler:^(NSData *data, NSURLResponse *resp, NSError *error) {
			  if (error) {
				  *errorDescriptionPtr = [[error localizedDescription] UTF8String];
				  return;
			  }

			  NSHTTPURLResponse *httpResponse = (NSHTTPURLResponse *)resp;

			  if (statusCode) {
				  *statusCode = static_cast<std::uint32_t>(httpResponse.statusCode);
			  }

			  if (responseHeaders) {
				  if ([httpResponse respondsToSelector:@selector(allHeaderFields)]) {
					  NSDictionary *dict = [httpResponse allHeaderFields];
					  for (id key in dict) {
						  id value = [dict objectForKey:key];
						  if ([key isKindOfClass:[NSString class]]) {
							  if ([value isKindOfClass:[NSString class]]) {
								  responseHeaders->emplace(std::string([key UTF8String]), std::string([value UTF8String]));
							  }
						  }
					  }
				  }
			  }

			  if (response) {
				  response->clear();
				  NSString *responseString = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
				  if (responseString) {
					  response->append([responseString UTF8String]);
				  }
			  }

			  dispatch_semaphore_signal(completionSemaphore);
		  }];

	[dataTask resume];

	dispatch_semaphore_wait(completionSemaphore, DISPATCH_TIME_FOREVER);

	if (errorDescription.size()) {
		log_error << "HTTP request error: " << errorDescription << std::endl;
		return Result::RequestFailed;
	}

	return Result::Success;
}

HttpHelper_OSX::Result HttpHelper_OSX::GetRequest(std::string_view url, const Headers &requestHeaders, std::uint32_t *statusCode, Headers *responseHeaders,
						  std::string *response)
{
	return Request(Method::Get, url, requestHeaders, "", statusCode, responseHeaders, response);
}

HttpHelper_OSX::Result HttpHelper_OSX::PostRequest(std::string_view url, const Headers &requestHeaders, std::string_view body, std::uint32_t *statusCode,
						   Headers *responseHeaders, std::string *response)
{
	return Request(Method::Post, url, requestHeaders, body, statusCode, responseHeaders, response);
}