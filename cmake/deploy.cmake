# `cmake --install build --prefix dist` produces a self-contained folder:
#   dist/bin/                 streamline.exe and every DLL it needs
#   dist/lib/gstreamer-1.0/   only the GStreamer plugins we use
#   dist/lib/gio/modules/     the OpenSSL TLS backend (wss:// to the MQTT brokers)
# Keep STREAMLINE_PLUGINS in sync with kRequiredElements in src/common/runtime.cpp.

set(STREAMLINE_PLUGINS
  coreelements app audioconvert audioresample videoconvertscale
  d3d11 amfcodec videoparsersbad dav1d vpx opus wasapi2
  rtp rsrtp rtpmanager srtp dtls nice webrtc level)

set(_plugin_files "")
foreach(p IN LISTS STREAMLINE_PLUGINS)
  set(f "${GST_PLUGINS_DIR}/libgst${p}.dll")
  if(NOT EXISTS "${f}")
    message(FATAL_ERROR "GStreamer plugin not found: ${f}")
  endif()
  list(APPEND _plugin_files "${f}")
endforeach()

# Uses the Windows certificate store (CRYPT32), so no CA bundle has to be shipped.
set(_gio_tls "${GIO_MODULES_DIR}/libgioopenssl.dll")
if(NOT EXISTS "${_gio_tls}")
  message(FATAL_ERROR "GIO TLS module not found: ${_gio_tls} (install glib-networking)")
endif()

get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)

install(TARGETS streamline RUNTIME DESTINATION bin)
install(FILES ${_plugin_files} DESTINATION lib/gstreamer-1.0)
install(FILES "${_gio_tls}" DESTINATION lib/gio/modules)
# Our license + full GPL/LGPL texts; tools/collect-licenses.sh adds the per-package notices.
install(FILES LICENSE DESTINATION . RENAME LICENSE.txt)
install(DIRECTORY packaging/licenses/ DESTINATION licenses)

install(CODE "
  if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
  endif()
  file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES \"$<TARGET_FILE:streamline>\"
    MODULES ${_plugin_files} \"${_gio_tls}\"
    DIRECTORIES \"${_mingw_bin}\"
    RESOLVED_DEPENDENCIES_VAR _deps
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\"
    POST_EXCLUDE_REGEXES \"[Ss][Yy][Ss][Tt][Ee][Mm]32\" \"[Ss][Yy][Ss][Ww][Oo][Ww]64\")
  foreach(_d IN LISTS _unresolved)
    message(STATUS \"Unresolved (assumed system DLL): \${_d}\")
  endforeach()
  file(INSTALL \${_deps} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\" FOLLOW_SYMLINK_CHAIN)
")
