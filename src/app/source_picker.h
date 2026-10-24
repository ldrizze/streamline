#pragma once

#include <optional>
#include <vector>

#include <wx/wx.h>

#include "capture/sources.h"

class wxListCtrl;
class wxListEvent;
class wxSearchCtrl;

// Lets the user pick a monitor or a window to stream.
class SourcePickerDialog : public wxDialog {
 public:
  explicit SourcePickerDialog(wxWindow* parent);

  // Valid after ShowModal() returned wxID_OK.
  const streamline::CaptureTarget& selection() const { return *selection_; }

 private:
  void Reload();
  void Populate();
  void OnItemActivated(wxListEvent& e);
  void OnSelectionChanged();
  bool Accept();

  wxListCtrl* list_ = nullptr;
  wxSearchCtrl* filter_ = nullptr;
  wxButton* ok_ = nullptr;
  wxStaticText* hint_ = nullptr;

  std::vector<streamline::MonitorSource> monitors_;
  std::vector<streamline::WindowSource> windows_;
  std::vector<streamline::CaptureTarget> rows_;  // parallel to list items
  std::optional<streamline::CaptureTarget> selection_;
};
