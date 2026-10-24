#include "common/runtime.h"

#include <windows.h>

#include <filesystem>

#include <gst/gst.h>

namespace fs = std::filesystem;

namespace streamline {

namespace {

fs::path exe_dir() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(std::wstring(buf, n)).parent_path();
}

void set_env(const wchar_t* name, const fs::path& value) {
  SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

}  // namespace

bool setup_gst_environment() {
  fs::path plugins = exe_dir().parent_path() / "lib" / "gstreamer-1.0";
  if (!fs::is_directory(plugins)) {
    return false;  // dev build: use the MSYS2 installation
  }

  set_env(L"GST_PLUGIN_SYSTEM_PATH_1_0", plugins);
  set_env(L"GST_PLUGIN_PATH_1_0", {});
  // Scan in-process: no gst-plugin-scanner.exe to ship, and we only bundle known plugins.
  SetEnvironmentVariableW(L"GST_REGISTRY_FORK", L"no");

  // Own registry so we never mix with another GStreamer install on the machine.
  wchar_t local[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
    fs::path dir = fs::path(local) / "streamline";
    std::error_code ec;
    fs::create_directories(dir, ec);
    set_env(L"GST_REGISTRY_1_0", dir / "gst-registry.bin");
  }
  return true;
}

const std::vector<std::string>& required_elements() {
  static const std::vector<std::string> elements = {
      // capture
      "d3d11screencapturesrc", "d3d11convert", "d3d11upload", "d3d11download", "appsrc",
      "wasapi2src",
      // encode / decode (hardware AV1 is optional, see optional_elements())
      "av1parse", "dav1ddec", "vp8enc", "vp8dec", "opusenc", "opusdec",
      // RTP / WebRTC
      "rtpav1pay", "rtpav1depay", "rtpvp8pay", "rtpvp8depay", "rtpopuspay", "rtpopusdepay",
      "webrtcbin", "rtpbin", "nicesrc", "nicesink", "dtlssrtpenc", "dtlssrtpdec", "srtpenc",
      "srtpdec",
      // output / plumbing
      "d3d11videosink", "wasapi2sink", "tee", "queue", "audioconvert", "audioresample",
      "videoconvert", "appsink", "level", "fakesink"};
  return elements;
}

const std::vector<std::string>& optional_elements() {
  // Registered only when the GPU supports them; VP8 / dav1d are the fallbacks.
  static const std::vector<std::string> elements = {"amfav1enc", "d3d11av1dec"};
  return elements;
}

namespace {

std::vector<std::string> missing_from(const std::vector<std::string>& names) {
  std::vector<std::string> missing;
  for (const auto& name : names) {
    GstElementFactory* f = gst_element_factory_find(name.c_str());
    if (f) {
      gst_object_unref(f);
    } else {
      missing.push_back(name);
    }
  }
  return missing;
}

}  // namespace

std::vector<std::string> missing_elements() { return missing_from(required_elements()); }

std::vector<std::string> missing_optional_elements() { return missing_from(optional_elements()); }

std::vector<std::string> failed_plugins(int& checked) {
  checked = 0;
  fs::path dir = exe_dir().parent_path() / "lib" / "gstreamer-1.0";
  if (!fs::is_directory(dir)) return {"not a packaged build (no lib/gstreamer-1.0)"};
  std::vector<std::string> failed;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.path().extension() != ".dll") continue;
    ++checked;
    GError* err = nullptr;
    std::string path = entry.path().string();
    GstPlugin* plugin = gst_plugin_load_file(path.c_str(), &err);
    if (plugin) {
      gst_object_unref(plugin);
    } else {
      failed.push_back(entry.path().filename().string() + ": " + (err ? err->message : "?"));
      g_clear_error(&err);
    }
  }
  return failed;
}

}  // namespace streamline
