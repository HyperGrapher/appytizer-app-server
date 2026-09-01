cmake_minimum_required(VERSION 3.21)

# Make the documented `cmake -S . -B build` work in PowerShell, cmd, and Git Bash.
# Also recover from a literal, unexpanded "$env:VCPKG_ROOT/..." argument.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  if(DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" _appytizer_vcpkg_toolchain)
    if(EXISTS "${_appytizer_vcpkg_toolchain}")
      set(CMAKE_TOOLCHAIN_FILE "${_appytizer_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain" FORCE)
    endif()
  endif()
endif()

project(Appytizer VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

include(FetchContent)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fltk GIT_REPOSITORY https://github.com/fltk/fltk.git GIT_TAG release-1.4.5 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(fltk)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(appytizer_common
  common/config.cpp
  common/ipc_protocol.cpp
  common/windows_service.cpp
)
target_include_directories(appytizer_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(appytizer_common PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(appytizer_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_executable(AppytizerEngine
  src/app.rc
  engine/main.cpp
  engine/engine.cpp
  engine/dns/dns_server.cpp
  engine/ipc/pipe_server.cpp
  engine/tls/certificate_manager.cpp
  engine/services/detection_utils.cpp
  engine/services/providers.cpp
  engine/sites/folder_watcher.cpp
  engine/sites/site_registry.cpp
)
target_link_libraries(AppytizerEngine PRIVATE appytizer_common unofficial::sqlite3::sqlite3 spdlog::spdlog
  OpenSSL::Crypto advapi32 crypt32 dnsapi iphlpapi psapi ws2_32 shell32 version)

add_executable(Appytizer WIN32 appytizer-ui/main.cpp appytizer-ui/ipc/pipe_client.cpp src/app.rc)
target_link_libraries(Appytizer PRIVATE appytizer_common fltk::fltk fltk::images spdlog::spdlog
  comctl32 dwmapi shell32 user32 gdi32 ole32 advapi32)
add_custom_command(TARGET Appytizer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:Appytizer>/assets"
  COMMENT "Copying Appytizer UI assets")

foreach(target appytizer_common AppytizerEngine Appytizer)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()
endforeach()

set_target_properties(Appytizer PROPERTIES OUTPUT_NAME Appytizer VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Appytizer>")
set_target_properties(AppytizerEngine PROPERTIES OUTPUT_NAME AppytizerEngine)

include(CTest)
if(BUILD_TESTING)
  include(Catch)
  add_executable(appytizer_tests
    tests/tests.cpp
    engine/engine.cpp
    engine/dns/dns_server.cpp
    engine/ipc/pipe_server.cpp
    engine/sites/site_registry.cpp
    engine/tls/certificate_manager.cpp
    engine/services/detection_utils.cpp
    engine/services/providers.cpp
    engine/sites/folder_watcher.cpp
  )
  target_link_libraries(appytizer_tests PRIVATE appytizer_common unofficial::sqlite3::sqlite3 Catch2::Catch2WithMain spdlog::spdlog
    OpenSSL::Crypto ws2_32 advapi32 crypt32 dnsapi shell32 psapi version)
  catch_discover_tests(appytizer_tests PROPERTIES TIMEOUT 60)
endif()
