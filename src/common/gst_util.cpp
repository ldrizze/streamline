#include "common/gst_util.h"

#include <algorithm>
#include <format>

namespace streamline {

bool has_element(const char* name) {
  GstElementFactory* f = gst_element_factory_find(name);
  if (!f) return false;
  gst_object_unref(f);
  return true;
}

std::string drain_bus(GstElement* pipeline, double& audio_db) {
  if (!pipeline) return {};
  GstBus* bus = gst_element_get_bus(pipeline);
  std::string error;
  while (GstMessage* msg = gst_bus_pop(bus)) {
    switch (GST_MESSAGE_TYPE(msg)) {
      case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        if (error.empty()) error = std::format("{}: {}", GST_OBJECT_NAME(msg->src), err->message);
        g_error_free(err);
        g_free(debug);
        break;
      }
      case GST_MESSAGE_ELEMENT: {
        const GstStructure* s = gst_message_get_structure(msg);
        if (gst_structure_has_name(s, "level")) {
          G_GNUC_BEGIN_IGNORE_DEPRECATIONS
          auto* rms = static_cast<GValueArray*>(g_value_get_boxed(gst_structure_get_value(s, "rms")));
          double db = -100;
          for (guint i = 0; rms && i < rms->n_values; ++i) {
            db = std::max(db, g_value_get_double(g_value_array_get_nth(rms, i)));
          }
          G_GNUC_END_IGNORE_DEPRECATIONS
          audio_db = db;
        }
        break;
      }
      default:
        break;
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
  return error;
}

void FlowMeter::count_frames(GstPad* pad) {
  gst_pad_add_probe(
      pad, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo*, gpointer self) {
        static_cast<FlowMeter*>(self)->frames_.fetch_add(1, std::memory_order_relaxed);
        return GST_PAD_PROBE_OK;
      },
      this, nullptr);
}

void FlowMeter::count_bytes(GstPad* pad) {
  gst_pad_add_probe(
      pad, GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo* info, gpointer self) {
        static_cast<FlowMeter*>(self)->bytes_.fetch_add(
            gst_buffer_get_size(GST_PAD_PROBE_INFO_BUFFER(info)), std::memory_order_relaxed);
        return GST_PAD_PROBE_OK;
      },
      this, nullptr);
}

namespace {

template <typename Fn>
void with_pad(GstElement* bin, const char* element, const char* pad, Fn fn) {
  GstElement* e = gst_bin_get_by_name(GST_BIN(bin), element);
  if (!e) return;
  if (GstPad* p = gst_element_get_static_pad(e, pad)) {
    fn(p);
    gst_object_unref(p);
  }
  gst_object_unref(e);
}

}  // namespace

void FlowMeter::count_frames(GstElement* bin, const char* element, const char* pad) {
  with_pad(bin, element, pad, [this](GstPad* p) { count_frames(p); });
}

void FlowMeter::count_bytes(GstElement* bin, const char* element, const char* pad) {
  with_pad(bin, element, pad, [this](GstPad* p) { count_bytes(p); });
}

void FlowMeter::reset() {
  last_frames_ = frames_.load();
  last_bytes_ = bytes_.load();
  last_ = std::chrono::steady_clock::now();
}

void FlowMeter::take(double& fps, double& kbps) {
  auto now = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(now - last_).count();
  uint64_t frames = frames_.load(std::memory_order_relaxed);
  uint64_t bytes = bytes_.load(std::memory_order_relaxed);
  fps = secs > 0 ? (frames - last_frames_) / secs : 0;
  kbps = secs > 0 ? (bytes - last_bytes_) * 8 / 1000.0 / secs : 0;
  last_frames_ = frames;
  last_bytes_ = bytes;
  last_ = now;
}

}  // namespace streamline
