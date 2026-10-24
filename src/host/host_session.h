#pragma once

#include <functional>
#include <map>
#include <string>

#include <gst/gst.h>

#include "common/glib_thread.h"
#include "common/signaling.h"

namespace streamline {

class HostPipeline;

// Joins a signaling room as host and serves each viewer with its own webrtcbin
// fed from the HostPipeline's tees (sendonly; the host makes the offer).
class HostSession {
 public:
  struct Callbacks {
    std::function<void(const std::string&)> log;  // called from the session thread
    std::function<void(int)> viewers;             // viewer count changed
  };

  ~HostSession() { stop(); }

  // `pipeline` must be running and outlive this session's stop().
  void start(HostPipeline& pipeline, const std::string& url, const std::string& room,
             Callbacks callbacks);
  // Call before stopping the HostPipeline.
  void stop();

 private:
  struct Peer {
    std::string id;
    GstElement* webrtc = nullptr;
    GstElement* elements[4] = {};  // video queue, video pay, audio queue, audio pay
    GstPad* tee_pads[2] = {};      // video_tee / audio_tee request pads
  };

  void on_signal(const json::Message& m);
  void add_peer(const std::string& id);
  void remove_peer(const std::string& id);
  void on_answer(const std::string& id, const std::string& sdp);
  void on_remote_ice(const std::string& id, const std::string& candidate, int mline);
  static void disconnect_handlers(GstElement* webrtc);

  // Called from GStreamer threads; hop onto the session thread with the peer id.
  static void on_negotiation_needed(GstElement* webrtc, gpointer ctx);
  static void on_ice_candidate(GstElement* webrtc, guint mline, gchar* candidate, gpointer ctx);
  static void on_ice_state(GstElement* webrtc, GParamSpec*, gpointer ctx);
  static void on_offer_created(GstPromise* promise, gpointer ctx);

  void log(const std::string& s) {
    if (callbacks_.log) callbacks_.log(s);
  }

  HostPipeline* pipeline_ = nullptr;
  GlibThread thread_;
  SignalingClient signaling_;
  Callbacks callbacks_;
  std::string room_;
  std::map<std::string, Peer> peers_;  // touched only on thread_
};

}  // namespace streamline
