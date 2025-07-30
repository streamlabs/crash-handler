cmake_minimum_required(VERSION 3.22...3.25)
project(crash-handler)

SET(NODEJS_URL "https://artifacts.electronjs.org/headers/dist" CACHE STRING "Node.JS URL")
SET(NODEJS_NAME "iojs" CACHE STRING "Node.JS Name")
SET(NODEJS_VERSION "v29.4.3" CACHE STRING "Node.JS Version")

include(NodeJS)
include(CPack)

nodejs_init()

# Include N-API wrappers
execute_process(COMMAND node -p "require('node-addon-api').include"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/crash-handler-module
        OUTPUT_VARIABLE NODE_ADDON_API_DIR
        )
string(REPLACE "\n" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})
string(REPLACE "\"" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})

# Define NAPI_VERSION
add_definitions(-DNAPI_VERSION=4)

add_nodejs_module(${PROJECT_NAME} module.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_INCLUDE_PATHS} ${NODE_ADDON_API_DIR})
target_compile_definitions(${PROJECT_NAME} PRIVATE BUILDING_NODE_EXTENSION)

# Unicode support
ADD_DEFINITIONS(-DUNICODE)

set(CompilerFlags
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
		CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
		CMAKE_C_FLAGS_RELWITHDEBINFO
        )
IF(WIN32)
  foreach(CompilerFlag ${CompilerFlags})
    string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
  endforeach()
ELSE(APPLE)
  set_property (TARGET  ${PROJECT_NAME} PROPERTY XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
ENDIF()

#############################
# Distribute
#############################
install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION "./" COMPONENT Runtime
	LIBRARY DESTINATION "./" COMPONENT Runtime
)

include(FetchContent)

IF(WIN32)
  # Compare current linked libs with prev
  FetchContent_Declare(deps_checker URL "https://raw.githubusercontent.com/stream-labs/obs-studio-node/staging/dependency_checker/check_dependencies.cmd" DOWNLOAD_NO_EXTRACT true)
  FetchContent_MakeAvailable(deps_checker)

  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND ${deps_checker_SOURCE_DIR}/check_dependencies.cmd $<TARGET_FILE:${PROJECT_NAME}> ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} $<CONFIG> )
ENDIF()

IF(WIN32)
  install(FILES $<TARGET_PDB_FILE:${PROJECT_NAME}> DESTINATION "./" OPTIONAL)
ENDIF()
install(FILES "${PROJECT_SOURCE_DIR}/package.json" DESTINATION "./")
install(FILES "${PROJECT_SOURCE_DIR}/main.js" DESTINATION "./")
