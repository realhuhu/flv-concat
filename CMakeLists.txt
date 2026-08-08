cmake_minimum_required(VERSION 3.20)

project(
  FLVConcat
  VERSION 1.1.3
  DESCRIPTION "Repair broken live-recording FLV timestamps and losslessly merge them into MP4"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

option(FLVCONCAT_BUILD_TESTS "Build FLVConcat tests" ${BUILD_TESTING})
option(FLVCONCAT_FFMPEG_AOM_STUB "Provide a compatibility symbol for a minimal FFmpeg 7.1 build" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(FFMPEG REQUIRED COMPONENTS avformat avcodec avutil)

add_library(flvconcat_codecs
  src/codecs/aac.cpp
  src/codecs/h264.cpp
  src/codecs/hevc.cpp
  src/codecs/registry.cpp)
target_include_directories(flvconcat_codecs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(flvconcat_codecs PRIVATE FFMPEG::avcodec)

add_library(flvconcat_core
  src/flv_scanner.cpp
  src/timeline.cpp)
target_include_directories(flvconcat_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(flvconcat_core PUBLIC flvconcat_codecs)

add_library(flvconcat_media
  src/flv/probe.cpp
  src/media.cpp
  src/media_muxer.cpp)
if(FLVCONCAT_FFMPEG_AOM_STUB)
  target_sources(flvconcat_media PRIVATE src/ffmpeg_aom_stub.cpp)
endif()
target_include_directories(flvconcat_media PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(flvconcat_media PUBLIC flvconcat_core FFMPEG::avformat FFMPEG::avcodec FFMPEG::avutil)
if(WIN32)
  # Static FFmpeg builds may include Schannel and Media Foundation objects even
  # when FLVConcat only uses local files. Their Windows SDK imports are private
  # pkg-config dependencies and therefore need to be stated explicitly.
  target_link_libraries(flvconcat_media PUBLIC
    advapi32 bcrypt crypt32 mf mfplat mfuuid ncrypt ole32 psapi
    secur32 shell32 shlwapi strmiids user32 vfw32 ws2_32)
endif()

add_executable(flvconcat src/main.cpp)
target_link_libraries(flvconcat PRIVATE flvconcat_media)
target_compile_definitions(flvconcat PRIVATE FLVCONCAT_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(flvconcat_codecs PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(flvconcat_core PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(flvconcat_media PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(flvconcat PRIVATE /W4 /permissive- /utf-8)
  set_property(TARGET flvconcat_codecs PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET flvconcat PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET flvconcat_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET flvconcat_media PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(flvconcat_codecs PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(flvconcat_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(flvconcat_media PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(flvconcat PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FLVCONCAT_BUILD_TESTS)
  add_executable(flvconcat_tests tests/test_core.cpp)
  target_link_libraries(flvconcat_tests PRIVATE flvconcat_media)
  if(MSVC)
    target_compile_options(flvconcat_tests PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET flvconcat_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    target_compile_options(flvconcat_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME flvconcat_core_tests COMMAND flvconcat_tests)
endif()

install(TARGETS flvconcat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md README_EN.md LICENSE CHANGELOG.md THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
