#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include <gst/gst.h>

#include "capture/sources.h"
#include "capture/wgc_capture.h"
#include "common/gst_util.h"

namespace streamline {

// appsrc "vsrc"/"asrc" -> encode (AV1 via AMF, VP8 fallback) + Opus -> "video_tee" / "audio_tee".
// The tees feed a local RTP pay/depay/decode preview rendered into `preview`,
// and HostSession hangs one webrtcbin per viewer off them.
// Capture runs outside that chain (WGC thread or small slave pipelines pushing into the
// appsrcs), so the source can be swapped while viewers stay connected.
class HostPipeline {
 public:
  struct Config {
    CaptureTarget target;
    int max_width = 1920;
    int max_height = 1080;
    int fps = 60;
    int video_kbps = 8000;
    int audio_kbps = 128;
  };

  struct Stats {
    double fps = 0;         // frames rendered by the preview
    double video_kbps = 0;  // encoder output
    double audio_db = -100;
  };

  HostPipeline() = default;
  ~HostPipeline() { stop(); }
  HostPipeline(const HostPipeline&) = delete;
  HostPipeline& operator=(const HostPipeline&) = delete;

  // Returns false and fills `error` on failure.
  bool start(const Config& config, HWND preview, std::string& error);
  // Captures `target` (video and audio) from now on; encoder, tees and viewers are untouched.
  // If the new source can't be opened the current one keeps streaming.
  bool switch_source(const CaptureTarget& target, std::string& error);
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  // Drains bus messages; returns a non-empty error message if the pipeline failed.
  std::string poll();
  Stats stats();

  GstElement* pipeline() const { return pipeline_; }
  bool video_is_av1() const { return av1_; }

  // Human-readable description of what is running, e.g. "AV1 (AMF) 1920x1080@60".
  const std::string& description() const { return description_; }

 private:
  // What feeds "vsrc" and "asrc"; replaced as a whole on a switch.
  struct Source {
    std::unique_ptr<WgcCapture> wgc;  // window video
    GstElement* screen = nullptr;     // monitor video: d3d11screencapturesrc ! appsink
    GstElement* audio = nullptr;      // wasapi2src ! ... ! appsink
    std::string description;
  };

  // Prepares without capturing yet, so a failure leaves the running source alone.
  bool open_source(const CaptureTarget& target, Source& source, std::string& error);
  bool start_source(Source& source, std::string& error);
  void close_source(Source& source);

  GstElement* pipeline_ = nullptr;
  GstD3D11Device* device_ = nullptr;  // shared by capture, convert, encoder and preview
  Config config_;
  Source source_;
  bool av1_ = true;
  std::string encoder_description_;
  std::string description_;
  FlowMeter meter_;
  double audio_db_ = -100;
};

}  // namespace streamline
