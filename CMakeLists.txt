cmake_minimum_required(VERSION 3.25)
project(streamline VERSION 0.6.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Version shown by the app: project(VERSION), unless CI passes one derived from the
# git tag or run number (not a cache default, so bumping project() always takes effect).
set(STREAMLINE_VERSION_OVERRIDE "" CACHE STRING "Override the app version (CI)")
if(STREAMLINE_VERSION_OVERRIDE)
  set(STREAMLINE_VERSION "${STREAMLINE_VERSION_OVERRIDE}")
else()
  set(STREAMLINE_VERSION "${PROJECT_VERSION}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0
  gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-d3d11-1.0)
pkg_check_modules(SIGNAL REQUIRED IMPORTED_TARGET libsoup-3.0 json-glib-1.0 libsodium)
pkg_get_variable(GST_PLUGINS_DIR gstreamer-1.0 pluginsdir)
pkg_get_variable(GIO_MODULES_DIR gio-2.0 giomoduledir)
find_package(wxWidgets 3.3 REQUIRED CONFIG COMPONENTS core base)

# Shared code: GStreamer runtime setup and helpers, encrypted MQTT signaling.
add_library(streamline_common STATIC
  src/common/runtime.cpp
  src/common/gst_util.cpp
  src/common/mqtt.cpp
  src/common/room_channel.cpp)
target_include_directories(streamline_common PUBLIC src)
target_link_libraries(streamline_common PUBLIC PkgConfig::GST PkgConfig::SIGNAL)
# libgstwebrtc is marked unstable API; we use it deliberately.
target_compile_definitions(streamline_common PUBLIC GST_USE_UNSTABLE_API)

# Capture: window/monitor enumeration and Windows.Graphics.Capture (WinRT ABI headers).
add_library(streamline_capture STATIC
  src/capture/sources.cpp
  src/capture/wgc_capture.cpp)
target_include_directories(streamline_capture PUBLIC src)
target_link_libraries(streamline_capture PUBLIC streamline_common dwmapi d3d11 dxgi runtimeobject)

# Host side: capture -> encode -> tees -> local preview + one webrtcbin per viewer.
add_library(streamline_host STATIC
  src/host/host_pipeline.cpp
  src/host/host_session.cpp)
target_link_libraries(streamline_host PUBLIC streamline_common streamline_capture)

# Viewer side: webrtcbin -> decode -> render.
add_library(streamline_viewer STATIC
  src/viewer/viewer_session.cpp)
target_link_libraries(streamline_viewer PUBLIC streamline_common)

# GUI app: stream or watch.
add_executable(streamline WIN32
  src/app/main.cpp
  src/app/source_picker.cpp
  src/app/streamline.rc)
# streamline.rc pulls the app icon from resources/icon.
set_property(SOURCE src/app/streamline.rc APPEND PROPERTY INCLUDE_DIRECTORIES
  ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon)
set_property(SOURCE src/app/streamline.rc APPEND PROPERTY OBJECT_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/streamline.ico)
target_link_libraries(streamline PRIVATE streamline_host streamline_viewer wx::core wx::base)
target_compile_definitions(streamline PRIVATE STREAMLINE_VERSION="${STREAMLINE_VERSION}")

include(cmake/deploy.cmake)
