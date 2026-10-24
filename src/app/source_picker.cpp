#include "app/source_picker.h"

#include <wx/listctrl.h>
#include <wx/srchctrl.h>

using streamline::CaptureTarget;

namespace {

enum Column { kType, kName, kProcess, kPid, kSize };

wxString size_text(int w, int h) { return wxString::Format("%d x %d", w, h); }

}  // namespace

SourcePickerDialog::SourcePickerDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, wxString::FromUTF8("Escolher o que transmitir"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* top = new wxBoxSizer(wxHORIZONTAL);
  filter_ = new wxSearchCtrl(this, wxID_ANY);
  filter_->SetDescriptiveText(wxString::FromUTF8("Filtrar por título ou processo"));
  auto* refresh = new wxButton(this, wxID_ANY, wxString::FromUTF8("Atualizar"));
  top->Add(filter_, 1, wxRIGHT, FromDIP(8));
  top->Add(refresh, 0);

  list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                         wxLC_REPORT | wxLC_SINGLE_SEL);
  list_->AppendColumn("Tipo", wxLIST_FORMAT_LEFT, FromDIP(70));
  list_->AppendColumn("Nome", wxLIST_FORMAT_LEFT, FromDIP(330));
  list_->AppendColumn("Processo", wxLIST_FORMAT_LEFT, FromDIP(130));
  list_->AppendColumn("PID", wxLIST_FORMAT_RIGHT, FromDIP(60));
  list_->AppendColumn("Tamanho", wxLIST_FORMAT_LEFT, FromDIP(100));

  hint_ = new wxStaticText(this, wxID_ANY, "");
  exclude_discord_ = new wxCheckBox(this, wxID_ANY,
                                    wxString::FromUTF8("Não transmitir o áudio do Discord"));
  exclude_discord_->SetValue(true);

  auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
  ok_ = static_cast<wxButton*>(FindWindow(wxID_OK));
  ok_->SetLabel(wxString::FromUTF8("Selecionar"));
  FindWindow(wxID_CANCEL)->SetLabel("Cancelar");  // no wx translation catalog is bundled

  sizer->Add(top, 0, wxEXPAND | wxALL, FromDIP(12));
  sizer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
  sizer->Add(hint_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
  sizer->Add(exclude_discord_, 0, wxEXPAND | wxALL, FromDIP(12));
  sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
  SetSizer(sizer);
  SetSize(FromDIP(wxSize(760, 520)));
  CentreOnParent();

  refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Reload(); });
  filter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Populate(); });
  list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { OnSelectionChanged(); });
  list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { OnSelectionChanged(); });
  list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, &SourcePickerDialog::OnItemActivated, this);
  exclude_discord_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { OnSelectionChanged(); });
  ok_->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
    if (Accept()) e.Skip();
  });

  Reload();
}

void SourcePickerDialog::Reload() {
  monitors_ = streamline::list_monitors();
  windows_ = streamline::list_windows();
  Populate();
}

void SourcePickerDialog::Populate() {
  wxString filter = filter_->GetValue().Lower();
  auto matches = [&](const std::wstring& a, const std::wstring& b) {
    return filter.empty() || wxString(a).Lower().Contains(filter) ||
           wxString(b).Lower().Contains(filter);
  };

  list_->Freeze();
  list_->DeleteAllItems();
  rows_.clear();

  auto add_row = [&](const wxString& type, const wxString& name, const wxString& process,
                     const wxString& pid, const wxString& size, CaptureTarget target) {
    long i = list_->InsertItem(list_->GetItemCount(), type);
    list_->SetItem(i, kName, name);
    list_->SetItem(i, kProcess, process);
    list_->SetItem(i, kPid, pid);
    list_->SetItem(i, kSize, size);
    rows_.push_back(std::move(target));
    return i;
  };

  for (const auto& m : monitors_) {
    if (!matches(m.name, m.device)) continue;
    wxString name = wxString(m.name) + (m.primary ? wxString::FromUTF8(" (principal)") : "");
    add_row("Tela", name, "", "",
            size_text(m.rect.right - m.rect.left, m.rect.bottom - m.rect.top),
            {CaptureTarget::Kind::Monitor, m.handle, nullptr, 0, m.name});
  }
  for (const auto& w : windows_) {
    if (!matches(w.title, w.process)) continue;
    long i = add_row("Janela", w.title, w.process, wxString::Format("%lu", w.pid),
                     w.minimized ? wxString::FromUTF8("minimizada") : size_text(w.width, w.height),
                     {CaptureTarget::Kind::Window, nullptr, w.hwnd, w.pid, w.title});
    if (w.minimized) list_->SetItemTextColour(i, wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  }
  list_->Thaw();
  OnSelectionChanged();
}

void SourcePickerDialog::OnSelectionChanged() {
  long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  ok_->Enable(i >= 0);
  bool monitor = i >= 0 && rows_[i].kind == CaptureTarget::Kind::Monitor;
  exclude_discord_->Enable(monitor);  // a window's audio never includes Discord anyway
  if (i < 0) {
    hint_->SetLabel("");
  } else if (monitor) {
    hint_->SetLabel(wxString::FromUTF8(exclude_discord_->GetValue()
                                           ? "Áudio: todo o som do sistema, exceto o Discord."
                                           : "Áudio: todo o som do sistema."));
  } else {
    hint_->SetLabel(wxString::FromUTF8("Áudio: somente deste aplicativo (PID ") +
                    wxString::Format("%lu", rows_[i].pid) + ").");
  }
  hint_->GetParent()->Layout();
}

void SourcePickerDialog::OnItemActivated(wxListEvent&) {
  if (Accept()) EndModal(wxID_OK);
}

bool SourcePickerDialog::Accept() {
  long i = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
  if (i < 0) return false;
  const CaptureTarget& t = rows_[i];
  if (t.kind == CaptureTarget::Kind::Window) {
    if (!IsWindow(t.hwnd)) {
      wxMessageBox(wxString::FromUTF8("Essa janela foi fechada. A lista será atualizada."),
                   "streamline", wxICON_WARNING, this);
      Reload();
      return false;
    }
    if (IsIconic(t.hwnd)) {
      wxMessageBox(wxString::FromUTF8("A janela está minimizada e não pode ser capturada. "
                                      "Restaure-a e tente novamente."),
                   "streamline", wxICON_INFORMATION, this);
      return false;
    }
  }
  selection_ = t;
  selection_->exclude_discord = exclude_discord_->GetValue();
  return true;
}
