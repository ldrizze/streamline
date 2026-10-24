#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include <gst/gst.h>

#include "common/glib_thread.h"
#include "common/gst_util.h"
#include "common/signaling.h"

namespace streamline {

// Joins a room as viewer, answers the host's offer and renders what arrives:
// video into `video_window` (D3D11), audio to the default output device.
class ViewerSession {
 public:
  struct Callbacks {
    std::function<void(const std::string&)> log;  // called from the session thread
  };
  struct Stats {
    double fps = 0;
    double video_kbps = 0;  // RTP payload received
    double audio_db = -100;
  };

  ~ViewerSession() { stop(); }

  bool start(const std::string& url, const std::string& room, HWND video_window,
             Callbacks callbacks, std::string& error);
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  std::string poll() { return drain_bus(pipeline_, audio_db_); }
  Stats stats();

 private:
  void on_signal(const json::Message& m);
  void on_offer(const std::string& from, const std::string& sdp);

  static void on_pad_added(GstElement* webrtc, GstPad* pad, gpointer self);
  static void on_ice_candidate(GstElement* webrtc, guint mline, gchar* candidate, gpointer self);
  static void on_remote_set(GstPromise* promise, gpointer self);
  static void on_answer_created(GstPromise* promise, gpointer self);

  void log(const std::string& s) {
    if (callbacks_.log) callbacks_.log(s);
  }

  GlibThread thread_;
  SignalingClient signaling_;
  Callbacks callbacks_;
  GstElement* pipeline_ = nullptr;
  GstElement* webrtc_ = nullptr;
  HWND video_window_ = nullptr;
  std::string room_;
  std::string host_id_;  // session thread only
  FlowMeter meter_;
  double audio_db_ = -100;
};

}  // namespace streamline
