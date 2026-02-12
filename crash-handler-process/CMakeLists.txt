CMAKE_MINIMUM_REQUIRED(VERSION 3.21)

set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

IF(APPLE)
	cmake_policy(SET CMP0025 NEW)
ENDIF()

PROJECT(crash-handler-process VERSION 0.0.1)

IF(WIN32)
	include(FetchContent)
	# Nlohmann JSON (modern JSON for C++)
	FetchContent_Declare(nlohmannjson URL "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp" DOWNLOAD_NO_EXTRACT true)
	FetchContent_MakeAvailable(nlohmannjson)

	set(Boost_USE_STATIC_LIBS ON)
	set(Boost_USE_STATIC_RUNTIME ON)
	set(Boost_USE_MULTITHREADED ON)
	set(Boost_DEBUG OFF)
	set(Boost_RELEASE ON)
	
	find_package(Boost
		1.79.0 EXACT
		REQUIRED
		COMPONENTS
			iostreams
			system
			locale
	)
	INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})
ENDIF()

#############################
# Source, Libraries & Directories
#############################
SET(PROJECT_SOURCE
	"${PROJECT_SOURCE_DIR}/process.hpp"
	"${PROJECT_SOURCE_DIR}/process-manager.cpp" "${PROJECT_SOURCE_DIR}/process-manager.hpp"
	"${PROJECT_SOURCE_DIR}/brief-crash-info-uploader.cpp" "${PROJECT_SOURCE_DIR}/brief-crash-info-uploader.hpp"
	"${PROJECT_SOURCE_DIR}/message.cpp" "${PROJECT_SOURCE_DIR}/message.hpp"
	"${PROJECT_SOURCE_DIR}/http-helper.hpp"
	"${PROJECT_SOURCE_DIR}/socket.hpp"
	"${PROJECT_SOURCE_DIR}/logger.cpp" "${PROJECT_SOURCE_DIR}/logger.hpp"
	"${PROJECT_SOURCE_DIR}/main.cpp"
	"${PROJECT_SOURCE_DIR}/util.hpp"
)

IF(WIN32)
	SET(WINDOWS_SOURCE
		"${PROJECT_SOURCE_DIR}/platforms/util-win.cpp"
		"${PROJECT_SOURCE_DIR}/platforms/socket-win.cpp" "${PROJECT_SOURCE_DIR}/platforms/socket-win.hpp"
		"${PROJECT_SOURCE_DIR}/platforms/process-win.cpp" "${PROJECT_SOURCE_DIR}/platforms/process-win.hpp"
		"${PROJECT_SOURCE_DIR}/platforms/upload-window-win.cpp" "${PROJECT_SOURCE_DIR}/platforms/upload-window-win.hpp"
		"${PROJECT_SOURCE_DIR}/platforms/httprequest_i.c" "${PROJECT_SOURCE_DIR}/platforms/httprequest.h"
		"${PROJECT_SOURCE_DIR}/platforms/http-helper-win.cpp" "${PROJECT_SOURCE_DIR}/platforms/http-helper-win.hpp"
		"${PROJECT_SOURCE_DIR}/minizip/zip.c" "${PROJECT_SOURCE_DIR}/minizip/zip.h"
		"${PROJECT_SOURCE_DIR}/minizip/ioapi.c" "${PROJECT_SOURCE_DIR}/minizip/ioapi.h"
		"${PROJECT_SOURCE_DIR}/minizip/iowin32.c" "${PROJECT_SOURCE_DIR}/minizip/iowin32.h"
	)
ELSE(APPLE)
	SET(APPLE_SOURCE
		"${PROJECT_SOURCE_DIR}/platforms/util-osx.mm"
		"${PROJECT_SOURCE_DIR}/platforms/socket-osx.cpp" "${PROJECT_SOURCE_DIR}/platforms/socket-osx.hpp"
		"${PROJECT_SOURCE_DIR}/platforms/process-osx.mm" "${PROJECT_SOURCE_DIR}/platforms/process-osx.hpp"
		"${PROJECT_SOURCE_DIR}/platforms/http-helper-osx.mm" "${PROJECT_SOURCE_DIR}/platforms/http-helper-osx.hpp"
	)
	find_library(COCOA Cocoa)
	# Prepare gettext
	FetchContent_Declare(
		gettext
		URL "https://obs-studio-deployment.s3.us-west-2.amazonaws.com/gettext-0.21.1-osx-universal.zip"
		URL_HASH SHA256=fe0a85235d130d6b892de8c2370893eac115471dbd1dc1b0c55b3b41f9949d3f)
	FetchContent_MakeAvailable(gettext)		
	SET(gettext_INCLUDE_DIR ${CMAKE_BINARY_DIR}/_deps/gettext-src/include)
	SET(gettext_LIBRARIES
		${CMAKE_BINARY_DIR}/_deps/gettext-src/lib/libintl.a
		libiconv.a) # libiconv.a is available on macOS; the gettext package was compiled with the default libiconv.a
	include_directories(${gettext_INCLUDE_DIR})
ENDIF()



#############################
# Building
#############################
IF(WIN32)
	ADD_DEFINITIONS(-DUNICODE -DNOCRYPT)
ENDIF()

IF(WIN32)
	ADD_EXECUTABLE(crash-handler-process ${PROJECT_SOURCE} ${WINDOWS_SOURCE})
ELSE(APPLE)
	ADD_EXECUTABLE(crash-handler-process ${PROJECT_SOURCE} ${APPLE_SOURCE})
	set_property (TARGET crash-handler-process  PROPERTY XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
ENDIF()

IF(WIN32)
	target_compile_options(crash-handler-process PRIVATE $<IF:$<CONFIG:Debug>,-MTd,-MT> )

	find_package(ZLIB)

	IF(ZLIB_FOUND)
		message(STATUS "Found binary: " ${ZLIB_LIBRARIES})
		message(STATUS "Found include directory: " ${ZLIB_INCLUDE_DIRS})	

		include(FindZLIB_Bin)

		add_custom_command(
				TARGET crash-handler-process POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy
						${ZLIB_BIN_FILES}
						${CMAKE_CURRENT_BINARY_DIR}/$<CONFIGURATION>)
				
	ELSE()
		message(FATAL_ERROR "ZLIB not found, refer to DepsPath for OBS")
	ENDIF()

	list(APPEND CMAKE_PREFIX_PATH "${PROJECT_SOURCE_DIR}/../aws-sdk/awsi/")
	#Set the location of where Windows can find the installed libraries of the SDK.
	if(MSVC)
		string(REPLACE ";" "/aws-cpp-sdk-all;" SYSTEM_MODULE_PATH "${CMAKE_SYSTEM_PREFIX_PATH}/aws-cpp-sdk-all")
		list(APPEND CMAKE_PREFIX_PATH ${SYSTEM_MODULE_PATH})
	endif()

	#AWSSDK gives linking error if PkgCOnfig not found explicitly before awssdk find_package
	find_package(PkgConfig QUIET)

	find_package(AWSSDK REQUIRED COMPONENTS s3 sts cognito-identity identity-management)

	if(MSVC AND BUILD_SHARED_LIBS)
		add_definitions(-DUSE_IMPORT_EXPORT)

		list(APPEND SERVICE_LIST s3 sts cognito-identity identity-management)

		set(CMAKE_BUILD_TYPE Release) 
		
		#AWSSDK_CPY_DYN_LIBS(SERVICE_LIST "" ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_BUILD_TYPE})  #Choose this line if your executables are in /build/Debug
		AWSSDK_CPY_DYN_LIBS(SERVICE_LIST "" ${CMAKE_CURRENT_BINARY_DIR})  #Choose this line for Visual Studio and possibly other IDEs
		
		message(STATUS ">>CMAKE_CURRENT_BINARY_DIR: ${CMAKE_CURRENT_BINARY_DIR}")
		message(STATUS ">>CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
	endif()


	# Include/link crash manager dependencies
	target_include_directories(crash-handler-process PUBLIC
		"${CMAKE_CURRENT_BINARY_DIR}/awsi/include/"
		"${nlohmannjson_SOURCE_DIR}/" 
		"${ZLIB_INCLUDE_DIRS}")

	target_sources(crash-handler-process PUBLIC "${PROJECT_SOURCE_DIR}/platforms/resource-win.rc")

	target_link_libraries(crash-handler-process ${AWSSDK_LINK_LIBRARIES})
	target_link_libraries(crash-handler-process ${ZLIB_LIBRARIES} Boost::boost Boost::locale Boost::system)


	# Compare current linked libs with prev
	FetchContent_Declare(deps_checker URL "https://raw.githubusercontent.com/stream-labs/obs-studio-node/staging/dependency_checker/check_dependencies.cmd" DOWNLOAD_NO_EXTRACT true)
	FetchContent_MakeAvailable(deps_checker)

	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND ${deps_checker_SOURCE_DIR}/check_dependencies.cmd $<TARGET_FILE:crash-handler-process> ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} $<CONFIG> )
ELSE()
	target_link_libraries(crash-handler-process ${COCOA} ${gettext_LIBRARIES})
ENDIF()

message(status "${CMAKE_CURRENT_BINARY_DIR}/locale/")
#############################
# Distribute
#############################
INSTALL(TARGETS crash-handler-process RUNTIME DESTINATION "./" COMPONENT Runtime )
IF(WIN32)
	INSTALL(FILES $<TARGET_PDB_FILE:crash-handler-process> DESTINATION "./" OPTIONAL)
	INSTALL(FILES "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIGURATION>/zlib.dll" DESTINATION "./" OPTIONAL)
ELSE(APPLE)
	INSTALL(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/locale" DESTINATION "./" )
ENDIF()
INSTALL(DIRECTORY ${PROJECT_DATA} DESTINATION "./" OPTIONAL)
