cmake_minimum_required(VERSION 3.25)
project(streamline VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# CI overrides this from the git tag (v1.2.3 -> 1.2.3).
set(STREAMLINE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown by the app and installer")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0
  gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-d3d11-1.0)
pkg_check_modules(SIGNAL REQUIRED IMPORTED_TARGET libsoup-3.0 json-glib-1.0)
pkg_get_variable(GST_PLUGINS_DIR gstreamer-1.0 pluginsdir)
find_package(wxWidgets 3.3 REQUIRED CONFIG COMPONENTS core base)

# Shared code: GStreamer runtime setup and helpers, signaling client/protocol.
add_library(streamline_common STATIC
  src/common/runtime.cpp
  src/common/gst_util.cpp
  src/common/signaling.cpp)
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
target_link_libraries(streamline PRIVATE streamline_host streamline_viewer wx::core wx::base)
target_compile_definitions(streamline PRIVATE STREAMLINE_VERSION="${STREAMLINE_VERSION}")

# Signaling server (WebSocket, rooms, SDP/ICE relay). Needs only libsoup + json-glib.
add_executable(streamline-signal
  server/main.cpp)
target_include_directories(streamline-signal PRIVATE src)
target_link_libraries(streamline-signal PRIVATE PkgConfig::SIGNAL)

include(cmake/deploy.cmake)
