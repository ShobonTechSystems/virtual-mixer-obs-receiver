# SPDX-License-Identifier: GPL-2.0-or-later
# 仮想ミキサーの音（OBS の音声ソース、試作）。README.md
#   Windows：OBS_SOURCE_DIR（obs-studio のソース。ヘッダーだけ使う）と OBS_IMPORT_LIB（obs.dll から作る obs.lib、tools/make-obs-lib.ps1）
#   Linux：Ubuntu の libobs-dev（ビルドの確認だけ。Linux の OBS は対象ではない）
cmake_minimum_required(VERSION 3.20)
project(vmx_obs_receiver LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(VMX_OBS_RECEIVER_VERSION "0.1.0" CACHE STRING "プラグインの版")
set(OBS_SOURCE_DIR "" CACHE PATH "Windows：obs-studio のソース")
set(OBS_IMPORT_LIB "" CACHE FILEPATH "Windows：obs.dll から作った obs.lib")

set(VMX_OBS_TARGET "")
if(OBS_SOURCE_DIR AND OBS_IMPORT_LIB)
  add_library(vmx_libobs SHARED IMPORTED)
  # obs-config.h が読む obsconfig.h は OBS のビルドで作られるので、プラグインに要る分だけを作る
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obsconfig/obsconfig.h"
    "#pragma once\n#define OBS_DATA_PATH \"data\"\n#define OBS_PLUGIN_PATH \"obs-plugins\"\n"
    "#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
  set_target_properties(vmx_libobs PROPERTIES IMPORTED_IMPLIB "${OBS_IMPORT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${CMAKE_CURRENT_BINARY_DIR}/obsconfig")
  set(VMX_OBS_TARGET vmx_libobs)
else()
  find_package(libobs CONFIG QUIET)
  if(TARGET OBS::libobs)
    set(VMX_OBS_TARGET OBS::libobs)
  else()
    find_path(VMX_LIBOBS_INCLUDE obs-module.h PATH_SUFFIXES obs)
    find_library(VMX_LIBOBS_LIB obs)
    if(VMX_LIBOBS_INCLUDE AND VMX_LIBOBS_LIB)
      add_library(vmx_libobs UNKNOWN IMPORTED)
      set_target_properties(vmx_libobs PROPERTIES IMPORTED_LOCATION "${VMX_LIBOBS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${VMX_LIBOBS_INCLUDE}")
      set(VMX_OBS_TARGET vmx_libobs)
    endif()
  endif()
endif()
if(NOT VMX_OBS_TARGET)
  message(FATAL_ERROR "libobs が見つかりません。Windows は OBS_SOURCE_DIR と OBS_IMPORT_LIB を指定してください（README.md）")
endif()

add_library(vmx-obs-receiver MODULE src/vmx-obs-receiver.cpp)
target_include_directories(vmx-obs-receiver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared/vmx-ipc")
target_compile_definitions(vmx-obs-receiver PRIVATE VMX_OBS_RECEIVER_VERSION="${VMX_OBS_RECEIVER_VERSION}")
target_link_libraries(vmx-obs-receiver PRIVATE ${VMX_OBS_TARGET} Threads::Threads)
set_target_properties(vmx-obs-receiver PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(vmx-obs-receiver PRIVATE /utf-8 /W4 /permissive-)
  # 利用者の PC に Visual C++ ランタイムが無くても動くよう静的リンクする
  set_property(TARGET vmx-obs-receiver PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(vmx-obs-receiver PRIVATE winmm)
else()
  target_compile_options(vmx-obs-receiver PRIVATE -Wall -Wextra -Wconversion -Wshadow)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(vmx-obs-receiver PRIVATE rt)
  endif()
endif()

# Linux の簡単な試し（Windows の OBS の確認の代わりにはならない）
if(NOT WIN32)
  enable_testing()
  add_executable(vmx_obs_receiver_smoke tests/smoke.cpp)
  target_include_directories(vmx_obs_receiver_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared/vmx-ipc")
  target_link_libraries(vmx_obs_receiver_smoke PRIVATE ${VMX_OBS_TARGET} Threads::Threads rt)
  add_test(NAME vmx_obs_receiver_smoke COMMAND vmx_obs_receiver_smoke $<TARGET_FILE:vmx-obs-receiver>)
endif()
