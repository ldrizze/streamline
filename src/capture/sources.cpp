#include "capture/sources.h"

#include <dwmapi.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace streamline {

namespace {

std::wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n, L'\0');
  if (n > 0) s.resize(GetWindowTextW(h, s.data(), n + 1));
  return s;
}

std::wstring class_name(HWND h) {
  wchar_t buf[256];
  int n = GetClassNameW(h, buf, 256);
  return std::wstring(buf, n);
}

bool is_cloaked(HWND h) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
}

std::wstring process_name(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return {};
  wchar_t buf[MAX_PATH];
  DWORD n = MAX_PATH;
  std::wstring name;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
    name = std::filesystem::path(std::wstring(buf, n)).filename().wstring();
  }
  CloseHandle(p);
  return name;
}

// UWP apps: the frame belongs to ApplicationFrameHost.exe, while the app (and its
// audio) runs in the process that owns the CoreWindow child.
DWORD app_pid(HWND h, DWORD pid) {
  if (class_name(h) != L"ApplicationFrameWindow") return pid;
  DWORD result = pid;
  EnumChildWindows(
      h,
      [](HWND child, LPARAM lp) -> BOOL {
        if (class_name(child) != L"Windows.UI.Core.CoreWindow") return TRUE;
        GetWindowThreadProcessId(child, reinterpret_cast<DWORD*>(lp));
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&result));
  return result;
}

bool is_capturable(HWND h) {
  if (!IsWindowVisible(h) || GetWindowTextLengthW(h) == 0 || is_cloaked(h)) return false;
  if (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) return false;
  if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
  std::wstring cls = class_name(h);
  return cls != L"Progman" && cls != L"WorkerW" && cls != L"Shell_TrayWnd" &&
         cls != L"Shell_SecondaryTrayWnd";
}

// GDI device name (\\.\DISPLAY1) -> monitor model name, via the DisplayConfig API.
std::map<std::wstring, std::wstring> monitor_friendly_names() {
  std::map<std::wstring, std::wstring> names;
  UINT32 path_count = 0, mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
    return names;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return names;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof source;
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;

    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof target;
    target.header.adapterId = paths[i].targetInfo.adapterId;
    target.header.id = paths[i].targetInfo.id;

    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
        target.monitorFriendlyDeviceName[0]) {
      names[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
    }
  }
  return names;
}

}  // namespace

std::vector<WindowSource> list_windows() {
  std::vector<WindowSource> windows;
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        if (!is_capturable(h)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;

        WindowSource w;
        w.hwnd = h;
        w.pid = app_pid(h, pid);
        w.title = window_text(h);
        w.process = process_name(w.pid);
        w.minimized = IsIconic(h);
        RECT r{};
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) ||
            GetWindowRect(h, &r)) {
          w.width = r.right - r.left;
          w.height = r.bottom - r.top;
        }
        reinterpret_cast<std::vector<WindowSource>*>(lp)->push_back(std::move(w));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));
  return windows;
}

std::vector<MonitorSource> list_monitors() {
  std::vector<MonitorSource> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof info;
        if (!GetMonitorInfoW(m, &info)) return TRUE;
        MonitorSource s;
        s.handle = m;
        s.device = info.szDevice;
        s.rect = info.rcMonitor;
        s.primary = info.dwFlags & MONITORINFOF_PRIMARY;
        reinterpret_cast<std::vector<MonitorSource>*>(lp)->push_back(std::move(s));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));

  auto names = monitor_friendly_names();
  for (size_t i = 0; i < monitors.size(); ++i) {
    auto it = names.find(monitors[i].device);
    monitors[i].name = it != names.end() ? it->second : L"Monitor " + std::to_wstring(i + 1);
  }
  std::stable_partition(monitors.begin(), monitors.end(),
                        [](const MonitorSource& m) { return m.primary; });
  return monitors;
}

std::string to_utf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

}  // namespace streamline
