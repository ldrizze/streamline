#include "host/host_pipeline.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <format>

#include <gst/d3d11/gstd3d11.h>
#include <gst/video/videooverlay.h>

namespace streamline {

namespace {

// Scale (w, h) down to fit max_w x max_h, keeping aspect; encoders want even sizes.
void fit(int w, int h, int max_w, int max_h, int& out_w, int& out_h) {
  double scale = std::min({1.0, double(max_w) / w, double(max_h) / h});
  out_w = std::max(64, int(std::lround(w * scale)) & ~1);
  out_h = std::max(64, int(std::lround(h * scale)) & ~1);
}

}  // namespace

bool HostPipeline::start(const Config& config, HWND preview, std::string& error) {
  stop();

  device_ = gst_d3d11_device_new(0, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
  if (!device_) {
    error = "Falha ao criar o dispositivo Direct3D 11.";
    return false;
  }

  // Video source, producing D3D11 memory at config.fps.
  bool is_window = config.target.kind == CaptureTarget::Kind::Window;
  std::string source;
  int src_w = 0, src_h = 0;
  if (is_window) {
    if (!IsWindow(config.target.hwnd)) {
      error = "A janela foi fechada.";
      stop();
      return false;
    }
    if (!capture_.open(config.target.hwnd, device_, src_w, src_h, error)) {
      stop();
      return false;
    }
    // Caps (BGRA, size) are set by WgcCapture and follow window resizes.
    source = "appsrc name=wgcsrc is-live=true format=time max-buffers=2 leaky-type=downstream";
  } else {
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(config.target.monitor, &mi)) {
      error = "Monitor não encontrado.";
      stop();
      return false;
    }
    src_w = mi.rcMonitor.right - mi.rcMonitor.left;
    src_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    source = std::format(
        "d3d11screencapturesrc show-cursor=true monitor-handle={} "
        "! video/x-raw(memory:D3D11Memory),framerate={}/1",
        reinterpret_cast<uintptr_t>(config.target.monitor), config.fps);
  }

  int w = 0, h = 0;
  fit(src_w, src_h, config.max_width, config.max_height, w, h);
  av1_ = has_element("amfav1enc");
  if (av1_) {
    // AMF AV1 pads the coded size (width to 64, height roughly to 16: 1080 -> 1082,
    // 900 -> 960) and decoders show the padding as garbage. Use an aligned canvas;
    // d3d11convert letterboxes into it without rescaling (1080p -> 1920x1088).
    w = (w + 63) & ~63;
    h = (h + 15) & ~15;
  }
  int gop = config.fps * 2;

  // Fixed encode size; d3d11convert scales whatever the source sends. Square pixels force
  // it to letterbox on window resizes (a non-square PAR would be lost over RTP).
  std::string video = std::format(
      "{} ! queue max-size-buffers=2 leaky=downstream ! d3d11convert "
      "! video/x-raw(memory:D3D11Memory),format=NV12,width={},height={},pixel-aspect-ratio=1/1 ! ",
      source, w, h);
  std::string preview_branch;
  if (av1_) {
    // Latency-constrained VBR with a peak cap: plain "cbr" swung 0.1–11 Mbps.
    video += std::format(
        "amfav1enc name=venc usage=low-latency rate-control=lcvbr bitrate={} max-bitrate={} "
        "gop-size={} ! av1parse ! tee name=video_tee allow-not-linked=true ",
        config.video_kbps, config.video_kbps * 5 / 4, gop);
    preview_branch = std::format("video_tee. ! queue ! rtpav1pay ! rtpav1depay ! av1parse ! {} ",
                                 has_element("d3d11av1dec") ? "d3d11av1dec"
                                                            : "dav1ddec ! d3d11upload");
    description_ = std::format("AV1 (AMF) {}x{}@{}", w, h, config.fps);
  } else {
    video += std::format(
        "d3d11download ! videoconvert ! video/x-raw,format=I420 "
        "! vp8enc name=venc deadline=1 cpu-used=8 end-usage=cbr target-bitrate={} "
        "keyframe-max-dist={} threads=8 ! tee name=video_tee allow-not-linked=true ",
        config.video_kbps * 1000, gop);
    preview_branch = "video_tee. ! queue ! rtpvp8pay ! rtpvp8depay ! vp8dec ! d3d11upload ";
    description_ = std::format("VP8 (software) {}x{}@{}", w, h, config.fps);
  }
  preview_branch += "! d3d11videosink name=vsink sync=false ";

  // Audio: window -> only that process tree; monitor -> whole system except ourselves,
  // so a local viewer/preview never feeds back into the capture.
  std::string audio = std::format(
      "wasapi2src loopback=true low-latency=true loopback-mode={} loopback-target-pid={} "
      "! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 "
      "! opusenc bitrate={} frame-size=10 ! tee name=audio_tee allow-not-linked=true "
      "audio_tee. ! queue ! rtpopuspay ! rtpopusdepay ! opusdec "
      "! level name=alevel interval=200000000 ! fakesink sync=false ",
      is_window ? "include-process-tree" : "exclude-process-tree",
      is_window ? config.target.pid : GetCurrentProcessId(), config.audio_kbps * 1000);
  description_ += is_window ? std::format(" · janela (WGC) + Opus (PID {})", config.target.pid)
                            : std::string(" · tela + Opus (sistema)");

  GError* parse_error = nullptr;
  std::string launch = video + preview_branch + audio;
  pipeline_ = gst_parse_launch(launch.c_str(), &parse_error);
  if (parse_error) {
    error = std::string("Erro no pipeline: ") + parse_error->message;
    g_error_free(parse_error);
    stop();
    return false;
  }

  // Every D3D11 element uses our device, so WGC textures reach the encoder without copies.
  GstContext* context = gst_d3d11_context_new(device_);
  gst_element_set_context(pipeline_, context);
  gst_context_unref(context);

  if (GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "vsink")) {
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),
                                        reinterpret_cast<guintptr>(preview));
    gst_object_unref(sink);
  }
  meter_.count_frames(pipeline_, "vsink", "sink");
  meter_.count_bytes(pipeline_, "venc", "src");
  meter_.reset();
  audio_db_ = -100;

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    error = poll();
    if (error.empty()) error = "Falha ao iniciar o pipeline.";
    stop();
    return false;
  }
  if (is_window) {
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline_), "wgcsrc");
    capture_.start(appsrc, config.fps);
    gst_object_unref(appsrc);
  }
  return true;
}

void HostPipeline::stop() {
  capture_.stop();  // stop pushing before the pipeline goes away
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }
  if (device_) {
    gst_object_unref(device_);
    device_ = nullptr;
  }
}

HostPipeline::Stats HostPipeline::stats() {
  Stats s;
  meter_.take(s.fps, s.video_kbps);
  s.audio_db = audio_db_;
  return s;
}

}  // namespace streamline
