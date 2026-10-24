#include "host/host_pipeline.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

namespace streamline {

namespace {

// Scale (w, h) down to fit max_w x max_h, keeping aspect; encoders want even sizes.
void fit(int w, int h, int max_w, int max_h, int& out_w, int& out_h) {
  double scale = std::min({1.0, double(max_w) / w, double(max_h) / h});
  out_w = std::max(64, int(std::lround(w * scale)) & ~1);
  out_h = std::max(64, int(std::lround(h * scale)) & ~1);
}

// Pushes every sample of a slave pipeline's "sink" appsink into the main pipeline's appsrc.
// Both pipelines share clock and base time, so timestamps need no translation.
struct Forward {
  GstElement* appsrc = nullptr;  // ref held
  GstCaps* caps = nullptr;
  bool first = true;
};

GstFlowReturn forward_sample(GstAppSink* sink, gpointer data) {
  auto* f = static_cast<Forward*>(data);
  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_EOS;
  GstCaps* caps = gst_sample_get_caps(sample);
  if (caps && (!f->caps || !gst_caps_is_equal(caps, f->caps))) {
    gst_caps_replace(&f->caps, caps);
    gst_app_src_set_caps(GST_APP_SRC(f->appsrc), caps);
  }
  GstBuffer* buf = gst_buffer_ref(gst_sample_get_buffer(sample));
  gst_sample_unref(sample);
  if (f->first) {  // the previous source's stream ended somewhere else
    buf = gst_buffer_make_writable(buf);
    GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DISCONT);
    f->first = false;
  }
  gst_app_src_push_buffer(GST_APP_SRC(f->appsrc), buf);
  return GST_FLOW_OK;  // a flushing main pipeline is not this pipeline's error
}

void free_forward(gpointer data) {
  auto* f = static_cast<Forward*>(data);
  gst_clear_caps(&f->caps);
  gst_object_unref(f->appsrc);
  delete f;
}

GstElement* launch(const std::string& description, std::string& error) {
  GError* parse_error = nullptr;
  GstElement* pipeline = gst_parse_launch(description.c_str(), &parse_error);
  if (parse_error) {
    error = std::string("Erro no pipeline: ") + parse_error->message;
    g_error_free(parse_error);
    if (pipeline) gst_object_unref(pipeline);
    return nullptr;
  }
  return pipeline;
}

void connect_forward(GstElement* slave, GstElement* main, const char* appsrc_name) {
  GstElement* sink = gst_bin_get_by_name(GST_BIN(slave), "sink");
  auto* f = new Forward;
  f->appsrc = gst_bin_get_by_name(GST_BIN(main), appsrc_name);
  GstAppSinkCallbacks callbacks{};
  callbacks.new_sample = forward_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, f, free_forward);
  gst_object_unref(sink);
}

void close_slave(GstElement*& slave) {
  if (!slave) return;
  gst_element_set_state(slave, GST_STATE_NULL);
  gst_object_unref(slave);
  slave = nullptr;
}

}  // namespace

bool HostPipeline::start(const Config& config, HWND preview, std::string& error) {
  stop();
  config_ = config;

  device_ = gst_d3d11_device_new(0, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
  if (!device_) {
    error = "Falha ao criar o dispositivo Direct3D 11.";
    return false;
  }

  // The encode size can't change mid-stream, and any window or monitor may be picked
  // later: size the canvas for the largest monitor; smaller sources are scaled up.
  int src_w = 0, src_h = 0;
  for (const auto& m : list_monitors()) {
    src_w = std::max<int>(src_w, m.rect.right - m.rect.left);
    src_h = std::max<int>(src_h, m.rect.bottom - m.rect.top);
  }
  if (src_w == 0 || src_h == 0) {
    src_w = config.max_width;
    src_h = config.max_height;
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

  // Caps (D3D11 BGRA, size) are set by whoever feeds vsrc and follow resizes and switches.
  // Fixed encode size; d3d11convert scales whatever the source sends. Square pixels force
  // it to letterbox (a non-square PAR would be lost over RTP).
  std::string video = std::format(
      "appsrc name=vsrc is-live=true format=time max-buffers=2 leaky-type=downstream "
      "! queue max-size-buffers=2 leaky=downstream ! d3d11convert "
      "! video/x-raw(memory:D3D11Memory),format=NV12,width={},height={},pixel-aspect-ratio=1/1 ! ",
      w, h);
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
    encoder_description_ = std::format("AV1 (AMF) {}x{}@{}", w, h, config.fps);
  } else {
    video += std::format(
        "d3d11download ! videoconvert ! video/x-raw,format=I420 "
        "! vp8enc name=venc deadline=1 cpu-used=8 end-usage=cbr target-bitrate={} "
        "keyframe-max-dist={} threads=8 ! tee name=video_tee allow-not-linked=true ",
        config.video_kbps * 1000, gop);
    preview_branch = "video_tee. ! queue ! rtpvp8pay ! rtpvp8depay ! vp8dec ! d3d11upload ";
    encoder_description_ = std::format("VP8 (software) {}x{}@{}", w, h, config.fps);
  }
  preview_branch += "! d3d11videosink name=vsink sync=false ";

  std::string audio = std::format(
      "appsrc name=asrc is-live=true format=time ! audioconvert "
      "! opusenc bitrate={} frame-size=10 ! tee name=audio_tee allow-not-linked=true "
      "audio_tee. ! queue ! rtpopuspay ! rtpopusdepay ! opusdec "
      "! level name=alevel interval=200000000 ! fakesink sync=false ",
      config.audio_kbps * 1000);

  pipeline_ = launch(video + preview_branch + audio, error);
  if (!pipeline_) {
    stop();
    return false;
  }

  // Every D3D11 element uses our device, so captured textures reach the encoder without copies.
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

  // Live sources only: this completes without preroll and fixes the base time the
  // capture pipelines are slaved to.
  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE ||
      gst_element_get_state(pipeline_, nullptr, nullptr, 5 * GST_SECOND) ==
          GST_STATE_CHANGE_FAILURE) {
    error = poll();
    if (error.empty()) error = "Falha ao iniciar o pipeline.";
    stop();
    return false;
  }
  if (!switch_source(config.target, error)) {
    stop();
    return false;
  }
  return true;
}

bool HostPipeline::switch_source(const CaptureTarget& target, std::string& error) {
  if (!pipeline_) return false;
  Source next;
  if (!open_source(target, next, error)) {
    close_source(next);
    return false;
  }
  close_source(source_);
  source_ = std::exchange(next, {});
  config_.target = target;
  description_ = encoder_description_ + source_.description;
  if (!start_source(source_, error)) {
    close_source(source_);
    return false;
  }
  return true;
}

bool HostPipeline::open_source(const CaptureTarget& target, Source& source, std::string& error) {
  bool is_window = target.kind == CaptureTarget::Kind::Window;
  if (is_window) {
    if (!IsWindow(target.hwnd)) {
      error = "A janela foi fechada.";
      return false;
    }
    int w = 0, h = 0;
    source.wgc = std::make_unique<WgcCapture>();
    if (!source.wgc->open(target.hwnd, device_, w, h, error)) return false;
  } else {
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(target.monitor, &mi)) {
      error = "Monitor não encontrado.";
      return false;
    }
    source.screen = launch(
        std::format("d3d11screencapturesrc show-cursor=true monitor-handle={} "
                    "! video/x-raw(memory:D3D11Memory),framerate={}/1 "
                    "! appsink name=sink sync=false max-buffers=2 drop=true",
                    reinterpret_cast<uintptr_t>(target.monitor), config_.fps),
        error);
    if (!source.screen) return false;
    GstContext* context = gst_d3d11_context_new(device_);
    gst_element_set_context(source.screen, context);
    gst_context_unref(context);
    connect_forward(source.screen, pipeline_, "vsrc");
  }

  // Audio: window -> only that process tree; monitor -> whole system minus one process tree.
  // The loopback API excludes a single tree, so with Discord open we drop Discord (call
  // partners would hear themselves) instead of ourselves; the host preview's audio ends in
  // a fakesink, so only a viewer on this same PC would then echo back.
  DWORD audio_pid = target.pid;
  DWORD discord_pid = 0;
  if (!is_window) {
    if (target.exclude_discord) discord_pid = find_discord_pid();
    audio_pid = discord_pid ? discord_pid : GetCurrentProcessId();
  }
  source.audio = launch(
      std::format("wasapi2src loopback=true low-latency=true loopback-mode={} "
                  "loopback-target-pid={} ! audioconvert ! audioresample "
                  "! audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2 "
                  "! appsink name=sink sync=false",
                  is_window ? "include-process-tree" : "exclude-process-tree", audio_pid),
      error);
  if (!source.audio) return false;
  connect_forward(source.audio, pipeline_, "asrc");

  if (is_window) {
    source.description = std::format(" · janela (WGC) + Opus (PID {})", target.pid);
  } else if (discord_pid) {
    source.description = std::format(" · tela + Opus (sistema sem Discord, PID {})", discord_pid);
  } else {
    source.description = " · tela + Opus (sistema)";
  }
  return true;
}

bool HostPipeline::start_source(Source& source, std::string& error) {
  if (source.wgc) {
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline_), "vsrc");
    source.wgc->start(appsrc, config_.fps);
    gst_object_unref(appsrc);
  }
  GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline_));
  GstClockTime base_time = gst_element_get_base_time(pipeline_);
  for (GstElement* slave : {source.screen, source.audio}) {
    if (!slave) continue;
    // Same clock and base time: the slave's buffer timestamps are main-pipeline running time.
    gst_pipeline_use_clock(GST_PIPELINE(slave), clock);
    gst_element_set_base_time(slave, base_time);
    gst_element_set_start_time(slave, GST_CLOCK_TIME_NONE);
    if (gst_element_set_state(slave, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      double unused = 0;
      error = drain_bus(slave, unused);
      if (error.empty()) error = "Falha ao iniciar a captura.";
      gst_object_unref(clock);
      return false;
    }
  }
  gst_object_unref(clock);

  // Start the new picture with a keyframe, so viewers recover from the gap right away.
  if (GstElement* venc = gst_bin_get_by_name(GST_BIN(pipeline_), "venc")) {
    gst_element_send_event(
        venc, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
    gst_object_unref(venc);
  }
  return true;
}

void HostPipeline::close_source(Source& source) {
  // Stop pushing before anything else goes away.
  if (source.wgc) source.wgc->stop();
  source.wgc.reset();
  close_slave(source.screen);
  close_slave(source.audio);
  source.description.clear();
}

void HostPipeline::stop() {
  close_source(source_);
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

std::string HostPipeline::poll() {
  std::string error = drain_bus(pipeline_, audio_db_);
  double unused = 0;
  for (GstElement* slave : {source_.screen, source_.audio}) {
    std::string e = drain_bus(slave, unused);
    if (error.empty()) error = e;
  }
  return error;
}

HostPipeline::Stats HostPipeline::stats() {
  Stats s;
  meter_.take(s.fps, s.video_kbps);
  s.audio_db = audio_db_;
  return s;
}

}  // namespace streamline
