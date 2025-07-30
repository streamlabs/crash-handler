cmake_minimum_required(VERSION 3.22...3.25)
PROJECT(crash-handler VERSION 0.0.1)

if(APPLE)
	if (NOT CMAKE_OSX_ARCHITECTURES)
		set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
	endif()
	if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
		if ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64")
			set(CMAKE_OSX_DEPOLYMENT_TARGET "11.0")
		else()
			set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15")
		endif()
	endif()
endif()

# Set Module Path
SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

#############################
# Configuration
#############################

# Installation
SET(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/distribute" CACHE PATH "Path to install files to")

ADD_SUBDIRECTORY(crash-handler-module)
ADD_SUBDIRECTORY(crash-handler-process)
