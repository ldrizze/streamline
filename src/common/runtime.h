#pragma once

#include <string>
#include <vector>

namespace streamline {

// Points GStreamer (and GIO, for TLS) at the bundled modules when running from a
// packaged folder (bin/ next to lib/gstreamer-1.0). Must run before gst_init().
// Returns true if the bundled layout was found.
bool setup_gst_environment();

// Elements the app needs at runtime; returns the ones that are missing.
std::vector<std::string> missing_elements();
const std::vector<std::string>& required_elements();

// Hardware-dependent elements with software fallbacks (AMF AV1 encode, D3D11 AV1 decode).
std::vector<std::string> missing_optional_elements();
const std::vector<std::string>& optional_elements();

// Packaged build only: loads every bundled plugin DLL, returns "file: error" for
// those that fail (e.g. a missing dependency DLL). `checked` gets the plugin count.
std::vector<std::string> failed_plugins(int& checked);

}  // namespace streamline
