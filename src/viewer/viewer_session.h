#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include <gst/gst.h>

#include <vector>

#include "common/glib_thread.h"
#include "common/gst_util.h"
#include "common/room_channel.h"

namespace streamline {

// Joins an encrypted signaling room (RoomChannel) as viewer, answers the host's
// offer and renders what arrives: video into `video_window` (D3D11), audio to the
// default output device.
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

  bool start(const std::string& room, const std::vector<std::string>& brokers,
             HWND video_window, Callbacks callbacks, std::string& error);
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  std::string poll() { return drain_bus(pipeline_, audio_db_); }
  Stats stats();

 private:
  void on_signal(const json::Message& m);
  void on_offer(const std::string& from, const std::string& sdp);
  void send_join();
  void stop_join_timer();
  void add_remote_ice(const std::string& candidate, guint mline);

  static void on_pad_added(GstElement* webrtc, GstPad* pad, gpointer self);
  static void on_ice_candidate(GstElement* webrtc, guint mline, gchar* candidate, gpointer self);
  static void on_remote_set(GstPromise* promise, gpointer self);
  static void on_answer_created(GstPromise* promise, gpointer self);

  void log(const std::string& s) {
    if (callbacks_.log) callbacks_.log(s);
  }

  GlibThread thread_;
  RoomChannel channel_;
  Callbacks callbacks_;
  GstElement* pipeline_ = nullptr;
  GstElement* webrtc_ = nullptr;
  HWND video_window_ = nullptr;
  GSource* join_timer_ = nullptr;  // session thread only
  std::string host_id_;            // session thread only
  struct PendingIce {
    std::string from;
    std::string candidate;
    guint mline;
  };
  std::vector<PendingIce> pending_ice_;  // ICE that arrived before the offer
  FlowMeter meter_;
  double audio_db_ = -100;
};

}  // namespace streamline
