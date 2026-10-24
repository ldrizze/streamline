#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <gst/gst.h>

namespace streamline {

bool has_element(const char* name);

// Pops all pending bus messages. Returns the first error (empty if none) and
// updates `audio_db` from "level" element messages.
std::string drain_bus(GstElement* pipeline, double& audio_db);

// Counts frames / bytes flowing through pads and turns them into rates.
class FlowMeter {
 public:
  void count_frames(GstPad* pad);
  void count_bytes(GstPad* pad);
  // Same, looking the element up by name in `bin`.
  void count_frames(GstElement* bin, const char* element, const char* pad);
  void count_bytes(GstElement* bin, const char* element, const char* pad);

  void reset();
  // Rates since the previous call (or reset).
  void take(double& fps, double& kbps);

 private:
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> bytes_{0};
  uint64_t last_frames_ = 0;
  uint64_t last_bytes_ = 0;
  std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace streamline
