#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace streamline {

struct WindowSource {
  HWND hwnd = nullptr;
  DWORD pid = 0;          // owning app process (resolved through UWP frame hosts)
  std::wstring title;
  std::wstring process;   // exe file name, e.g. L"chrome.exe"
  int width = 0;          // visible frame size in physical pixels
  int height = 0;
  bool minimized = false; // WGC can't capture minimized windows
};

struct MonitorSource {
  HMONITOR handle = nullptr;  // pass to d3d11screencapturesrc monitor-handle
  std::wstring name;          // friendly name, e.g. L"DELL U2720Q"
  std::wstring device;        // GDI name, e.g. L"\\\\.\\DISPLAY1"
  RECT rect{};
  bool primary = false;
};

// What the user picked to stream.
struct CaptureTarget {
  enum class Kind { Monitor, Window } kind = Kind::Monitor;
  HMONITOR monitor = nullptr;
  HWND hwnd = nullptr;
  DWORD pid = 0;  // window only: audio is captured from this process tree
  std::wstring label;
  bool exclude_discord = true;  // monitor only: leave Discord's voice out of the system audio
};

// Capturable top-level windows in Z-order, excluding our own process.
std::vector<WindowSource> list_windows();

// Monitors with the primary one first.
std::vector<MonitorSource> list_monitors();

// Root process of a running Discord client (stable, PTB or Canary), or 0. Its children
// (voice/audio utility processes) belong to its process tree.
DWORD find_discord_pid();

std::string to_utf8(const std::wstring& s);

}  // namespace streamline
