# GP4 Ultimate: dinput8.dll and d3d8.dll proxies for Grand Prix 4, plus tools.
# GP4 is a 32-bit game: configure for x86.
#   Visual Studio:  cmake -S . -B build -A Win32 && cmake --build build --config Release
#   MinGW (any OS): cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/i686-mingw.cmake
#   Native tests:   cmake -S . -B build-tests -DGP4U_TESTS_ONLY=ON && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.16)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION GP4U_VERSION LIMIT_COUNT 1)
project(gp4ultimate VERSION ${GP4U_VERSION} LANGUAGES C CXX)
configure_file(src/core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GP4U_TESTS_ONLY "Build only the platform-independent core and its tests" OFF)

add_library(gp4u_core STATIC
    src/core/ini.cpp
    src/core/mapping.cpp
    src/core/car_locator.cpp
    src/core/ffb_model.cpp
    src/core/projection.cpp
    src/core/ini_doc.cpp
    src/core/schema.cpp
    src/core/net_stats.cpp)
target_include_directories(gp4u_core PUBLIC src)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core gp4u_core)
add_test(NAME core COMMAND test_core)
add_executable(test_schema tests/test_schema.cpp)
target_link_libraries(test_schema gp4u_core)
add_test(NAME schema COMMAND test_schema ${CMAKE_CURRENT_SOURCE_DIR})
if (MINGW)
    target_link_options(test_core PRIVATE -static)
    target_link_options(test_schema PRIVATE -static)
endif()

if (GP4U_TESTS_ONLY)
    return()
endif()

if (NOT WIN32)
    message(FATAL_ERROR "The DLL targets Windows; use the MinGW toolchain file or -DGP4U_TESTS_ONLY=ON")
endif()
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GP4 is 32-bit: configure with -A Win32 (Visual Studio) or an i686 toolchain")
endif()

add_library(dinput8 SHARED
    src/win/dllmain.cpp
    src/win/di.cpp
    src/win/log.cpp
    src/win/device_manager.cpp
    src/win/effect_proxy.cpp
    src/win/ffb_engine.cpp
    src/win/cockpit_patch.cpp
    src/win/net_hook.cpp
    src/win/net_sync.cpp
    src/win/net_vtidx.c
    src/win/physics_hook.cpp
    src/win/dinput8.def)
target_link_libraries(dinput8 PRIVATE gp4u_core dxguid winmm)
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
if (MSVC)
    target_compile_options(dinput8 PRIVATE /W3 /EHsc)
    set_property(TARGET dinput8 gp4u_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(dinput8 PRIVATE -Wall -Wno-unused-parameter -Wno-missing-exception-spec)
    target_link_options(dinput8 PRIVATE -static -Wl,--kill-at)
endif()

add_library(d3d8 SHARED
    src/d3d8/gp4u_d3d8.cpp
    src/d3d8/vtidx.c
    src/win/log.cpp
    src/d3d8/d3d8.def)
target_link_libraries(d3d8 PRIVATE gp4u_core)
set_target_properties(d3d8 PROPERTIES PREFIX "" OUTPUT_NAME "d3d8")
if (MSVC)
    set_property(TARGET d3d8 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_options(d3d8 PRIVATE -static -Wl,--kill-at)
endif()

add_executable(gp4ultimate-devices tools/gp4ultimate-devices.cpp)
target_include_directories(gp4ultimate-devices PRIVATE src tools)
# The tool links against the SYSTEM dinput8, not our proxy target of the same name,
# so name the import library explicitly.
if (MSVC)
    target_link_libraries(gp4ultimate-devices PRIVATE dinput8.lib dxguid)
    set_property(TARGET gp4ultimate-devices PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(gp4ultimate-devices PRIVATE -ldinput8 dxguid)
    target_link_options(gp4ultimate-devices PRIVATE -static)
endif()

# gp4ultimate-config.exe: graphical configurator (Dear ImGui, vendored in third_party/imgui, MIT).
enable_language(RC)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(GP4UCONFIG_ICON ${CMAKE_CURRENT_SOURCE_DIR}/tools/config/gp4ultimate-config.ico)
set(GP4UCONFIG_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/tools/config/gp4ultimate-config.manifest)
set(GP4U_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/config/gp4ultimate_schema.ini)
configure_file(tools/config/gp4ultimate-config.rc.in ${CMAKE_CURRENT_BINARY_DIR}/gp4ultimate-config.rc @ONLY)
add_executable(gp4ultimate-config WIN32
    tools/config/main.cpp
    tools/config/app.cpp
    tools/config/input.cpp
    tools/config/selftest.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/gp4ultimate-config.rc)
target_include_directories(gp4ultimate-config PRIVATE src tools/config ${IMGUI_DIR})
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/gp4ultimate-config.rc PROPERTIES
    OBJECT_DEPENDS "${GP4UCONFIG_ICON};${GP4UCONFIG_MANIFEST};${GP4U_SCHEMA}")
if (MSVC)
    target_include_directories(gp4ultimate-config PRIVATE tools/config)
    target_link_libraries(gp4ultimate-config PRIVATE gp4u_core d3d9 dxguid imm32 dwmapi)
    target_link_options(gp4ultimate-config PRIVATE /ENTRY:mainCRTStartup)
    set_property(TARGET gp4ultimate-config PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(gp4ultimate-config PRIVATE gp4u_core -ld3d9 -ldinput8 dxguid imm32 dwmapi gdi32)
    target_link_options(gp4ultimate-config PRIVATE -static -mwindows)
    set_target_properties(gp4ultimate-config PROPERTIES WIN32_EXECUTABLE ON)
endif()
# The resource compiler needs resource.h next to the generated .rc.
target_include_directories(gp4ultimate-config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/config)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/tools/config")

# End-to-end test: test_proxy plays GP4, loads our dinput8.dll from its folder, which chains
# to mock_dinput.dll (fake wheel + pedals). Everything goes to <build>/e2e.
set(E2E_DIR ${CMAKE_BINARY_DIR}/e2e)
add_library(mock_dinput SHARED tests/win/mock_dinput.cpp tests/win/mock_dinput.def)
target_link_libraries(mock_dinput PRIVATE dxguid)
set_target_properties(mock_dinput PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
add_executable(test_proxy tests/win/test_proxy.cpp)
target_include_directories(test_proxy PRIVATE src)
set_target_properties(test_proxy PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
if (MSVC)
    target_link_libraries(test_proxy PRIVATE dinput8.lib dxguid)
    set_property(TARGET mock_dinput test_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(test_proxy PRIVATE -ldinput8 dxguid)
    target_link_options(test_proxy PRIVATE -static)
    target_link_options(mock_dinput PRIVATE -static -Wl,--kill-at)
endif()
add_dependencies(test_proxy dinput8 mock_dinput)
# Copy whenever the DLL itself is rebuilt (copying after the test exe only refreshed it
# when the test changed, so the e2e tests could run against a stale DLL).
add_custom_command(TARGET dinput8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${E2E_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:dinput8> ${E2E_DIR}/dinput8.dll)
add_test(NAME proxy_telemetry COMMAND test_proxy telemetry WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME proxy_game COMMAND test_proxy game WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME proxy_gp4 COMMAND test_proxy gp4 WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME proxy_aligning COMMAND test_proxy aligning WORKING_DIRECTORY ${E2E_DIR})

# Network log: a GP4-like importer of CoCreateInstance driving the system's DirectPlay.
add_executable(test_net tests/win/test_net.cpp)
set_target_properties(test_net PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
if (MSVC)
    target_link_libraries(test_net PRIVATE dinput8.lib dxguid ole32)
    set_property(TARGET test_net PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(test_net PRIVATE -ldinput8 dxguid ole32)
    target_link_options(test_net PRIVATE -static)
endif()
add_dependencies(test_net dinput8)
add_test(NAME net COMMAND test_net WORKING_DIRECTORY ${E2E_DIR})

# Cockpit wheel patch: patches a copy of the signature in its own memory and calls through
# it like GP4 does; with GP4_EXE set it also runs the ORIGINAL function, read at test time
# from the user's GP4.exe (never committed), and compares bit for bit.
add_executable(test_cockpit tests/win/test_cockpit.cpp src/win/cockpit_patch.cpp src/win/physics_hook.cpp
               src/win/net_sync.cpp src/win/log.cpp)
target_include_directories(test_cockpit PRIVATE src)
set_target_properties(test_cockpit PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
if (MSVC)
    set_property(TARGET test_cockpit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_options(test_cockpit PRIVATE -static)
endif()
add_test(NAME cockpit COMMAND test_cockpit WORKING_DIRECTORY ${E2E_DIR})

# d3d8 proxy test: test_d3d8 -> our d3d8.dll -> mock_d3d8.dll (no GPU needed).
add_library(mock_d3d8 SHARED tests/win/mock_d3d8.c tests/win/mock_d3d8.def)
set_target_properties(mock_d3d8 PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
add_executable(test_d3d8 tests/win/test_d3d8.cpp)
set_target_properties(test_d3d8 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${E2E_DIR}
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${E2E_DIR} RUNTIME_OUTPUT_DIRECTORY_DEBUG ${E2E_DIR})
if (MSVC)
    target_link_libraries(test_d3d8 PRIVATE d3d8.lib)
    set_property(TARGET mock_d3d8 test_d3d8 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_libraries(test_d3d8 PRIVATE -ld3d8)
    target_link_options(test_d3d8 PRIVATE -static)
    target_link_options(mock_d3d8 PRIVATE -static -Wl,--kill-at)
endif()
add_dependencies(test_d3d8 d3d8 mock_d3d8)
# Copy whenever the DLL itself is rebuilt (copying after the test exe only refreshed it
# when the test changed, so the e2e tests could run against a stale DLL).
add_custom_command(TARGET d3d8 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${E2E_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:d3d8> ${E2E_DIR}/d3d8.dll)
add_test(NAME d3d8_fix COMMAND test_d3d8 fix WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME d3d8_nofix COMMAND test_d3d8 nofix WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME d3d8_window COMMAND test_d3d8 window WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME d3d8_fullscreen COMMAND test_d3d8 fullscreen WORKING_DIRECTORY ${E2E_DIR})
add_test(NAME d3d8_fullscreen_plain COMMAND test_d3d8 fullscreen_plain WORKING_DIRECTORY ${E2E_DIR})
