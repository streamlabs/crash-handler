# Unit tests for crash-handler-process (Windows only).
# The tested sources are compiled directly into the test executable, so the tests
# don't need the AWS SDK, Boost or zlib that the full crash-handler-process links.

set(CRASH_HANDLER_PROCESS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../crash-handler-process")

add_executable(crash_handler_unit_tests
	"${CMAKE_CURRENT_SOURCE_DIR}/socket-win-tests.cpp"
	"${CRASH_HANDLER_PROCESS_DIR}/platforms/socket-win.cpp"
	"${CRASH_HANDLER_PROCESS_DIR}/logger.cpp"
)

set_target_properties(crash_handler_unit_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(crash_handler_unit_tests PRIVATE UNICODE _UNICODE NOCRYPT)
target_include_directories(crash_handler_unit_tests PRIVATE "${CRASH_HANDLER_PROCESS_DIR}")
target_link_libraries(crash_handler_unit_tests PRIVATE Catch2::Catch2WithMain)

# Use the static MSVC runtime (/MT) like the shipped crash-handler-process.
set_target_properties(crash_handler_unit_tests Catch2 Catch2WithMain
	PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# No install() rule: the test executable must never end up in the release package.

include(Catch)
catch_discover_tests(crash_handler_unit_tests)
