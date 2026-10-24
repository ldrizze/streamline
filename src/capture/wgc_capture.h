#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include <gst/gst.h>

typedef struct _GstD3D11Device GstD3D11Device;

namespace streamline {

// Captures one window with Windows.Graphics.Capture and feeds its frames, as
// D3D11 textures on `device` (no CPU copy), into an appsrc at a fixed rate.
// The last frame is repeated while the window doesn't change (or is minimized),
// so the encoder can always produce a keyframe for a new viewer.
class WgcCapture {
 public:
  ~WgcCapture() { stop(); }

  // Creates the capture item for `hwnd` and reports its current size.
  bool open(HWND hwnd, GstD3D11Device* device, int& width, int& height, std::string& error);
  // Starts pushing BGRA D3D11 buffers into `appsrc` at `fps`.
  void start(GstElement* appsrc, int fps);
  void stop();

 private:
  struct State;
  void run(State* state);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};
  State* state_ = nullptr;  // owned by the capture thread
};

}  // namespace streamline
