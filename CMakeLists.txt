cmake_minimum_required(VERSION 3.24)
project(chrome_alt_tab VERSION 0.5.1 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
	message(FATAL_ERROR "chrome-alt-tab requires Windows, MSVC, and the Windows SDK.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_testing()
add_subdirectory(native)
