#pragma once

#include <string>
#include <vector>

namespace streamline {

// Points GStreamer at the bundled plugins when running from a packaged
// folder (bin/ next to lib/gstreamer-1.0). Must run before gst_init().
// Returns true if the bundled layout was found.
bool setup_gst_environment();

// Elements the app needs at runtime; returns the ones that are missing.
std::vector<std::string> missing_elements();

const std::vector<std::string>& required_elements();

}  // namespace streamline
