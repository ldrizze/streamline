#pragma once

#include <windows.h>

#include <string>

#include <gst/gst.h>

#include "capture/sources.h"
#include "capture/wgc_capture.h"
#include "common/gst_util.h"

namespace streamline {

// Capture -> encode (AV1 via AMF, VP8 fallback) + Opus -> "video_tee" / "audio_tee".
// The tees feed a local RTP pay/depay/decode preview rendered into `preview`,
// and HostSession hangs one webrtcbin per viewer off them.
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
  void stop();
  bool running() const { return pipeline_ != nullptr; }

  // Drains bus messages; returns a non-empty error message if the pipeline failed.
  std::string poll() { return drain_bus(pipeline_, audio_db_); }
  Stats stats();

  GstElement* pipeline() const { return pipeline_; }
  bool video_is_av1() const { return av1_; }

  // Human-readable description of what is running, e.g. "AV1 (AMF) 1920x1080@60".
  const std::string& description() const { return description_; }

 private:
  GstElement* pipeline_ = nullptr;
  GstD3D11Device* device_ = nullptr;  // shared by capture, convert, encoder and preview
  WgcCapture capture_;                // window mode only
  bool av1_ = true;
  std::string description_;
  FlowMeter meter_;
  double audio_db_ = -100;
};

}  // namespace streamline
