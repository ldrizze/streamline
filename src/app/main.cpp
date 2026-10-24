// wx first: it pulls in winsock2.h, which must precede windows.h.
#include <wx/clipbrd.h>
#include <wx/display.h>
#include <wx/wx.h>

#include <windows.h>

#include <cstdlib>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "app/source_picker.h"
#include "capture/sources.h"
#include "common/room_channel.h"
#include "common/runtime.h"
#include "host/host_pipeline.h"
#include "host/host_session.h"
#include "viewer/viewer_session.h"

namespace {

using streamline::CaptureTarget;
using streamline::HostPipeline;
using streamline::HostSession;
using streamline::RoomChannel;
using streamline::ViewerSession;

// GUI-subsystem exe: write to the inherited stdout (pipe/file) or the parent console.
void console_print(const std::string& text) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out == nullptr || out == INVALID_HANDLE_VALUE) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                      nullptr);
  }
  DWORD written = 0;
  WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::string environment_report(bool bundled, bool& ok) {
  std::string r = "streamline " STREAMLINE_VERSION "\n";
  r += std::string(gst_version_string()) + (bundled ? " (bundled)\n" : " (system)\n");
  auto missing = streamline::missing_elements();
  ok = missing.empty();
  r += "elements: " + std::to_string(streamline::required_elements().size() - missing.size()) + "/" +
       std::to_string(streamline::required_elements().size()) + " OK\n";
  for (const auto& m : missing) r += "  MISSING: " + m + "\n";
  for (const auto& m : streamline::missing_optional_elements()) {
    r += "  optional (GPU) not available: " + m + " (software fallback)\n";
  }
  return r;
}

std::string sources_report() {
  using streamline::to_utf8;
  std::string r = "monitors:\n";
  for (const auto& m : streamline::list_monitors()) {
    r += "  " + to_utf8(m.name) + " [" + to_utf8(m.device) + "] " +
         std::to_string(m.rect.right - m.rect.left) + "x" +
         std::to_string(m.rect.bottom - m.rect.top) + (m.primary ? " primary" : "") + "\n";
  }
  r += "windows:\n";
  for (const auto& w : streamline::list_windows()) {
    r += "  pid=" + std::to_string(w.pid) + " " + to_utf8(w.process) + " " +
         (w.minimized ? std::string("minimized")
                      : std::to_string(w.width) + "x" + std::to_string(w.height)) +
         " \"" + to_utf8(w.title) + "\"\n";
  }
  DWORD discord = streamline::find_discord_pid();
  r += discord ? "discord: pid=" + std::to_string(discord) + " (excluded from screen audio)\n"
               : std::string("discord: not running\n");
  return r;
}

// "monitor" or "<pid>" (first window of that process).
std::optional<CaptureTarget> test_target(const std::string& spec) {
  if (spec == "monitor") {
    auto monitors = streamline::list_monitors();
    if (monitors.empty()) return std::nullopt;
    return CaptureTarget{CaptureTarget::Kind::Monitor, monitors[0].handle, nullptr, 0,
                         monitors[0].name};
  }
  DWORD pid = std::strtoul(spec.c_str(), nullptr, 10);
  for (const auto& w : streamline::list_windows()) {
    if (w.pid == pid && !w.minimized) {
      return CaptureTarget{CaptureTarget::Kind::Window, nullptr, w.hwnd, w.pid, w.title};
    }
  }
  return std::nullopt;
}

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

// Transmission presets. The canvas is the largest monitor scaled to fit the chosen size.
struct Resolution {
  int width, height;
};
constexpr Resolution kResolutions[] = {{1920, 1080}, {1366, 768}, {1280, 720}};
constexpr int kFrameRates[] = {60, 30};

// 8 Mbps at 1080p60, scaled by pixel count; 30 fps gets 60% (screen content changes
// less per frame, so halving the frames doesn't halve the bits).
int video_kbps_for(const Resolution& r, int fps) {
  double kbps = 8000.0 * r.width * r.height / (1920.0 * 1080.0);
  if (fps < 60) kbps *= 0.6;
  return static_cast<int>(kbps / 100) * 100;
}

struct Options {
  std::vector<std::string> brokers = RoomChannel::default_brokers();
  std::string room;  // fixed invite code for --test-host
  std::optional<CaptureTarget> test_host;
  std::optional<CaptureTarget> test_switch;  // switch to this halfway through --test-host
  std::string test_watch;  // invite code to watch
  int test_seconds = 6;
  int resolution = 0;  // index into kResolutions
  int frame_rate = 0;  // index into kFrameRates
  std::string test_view;  // view to enter once the test starts
};

class MainFrame : public wxFrame {
 public:
  MainFrame(const std::string& report, std::vector<std::string> brokers)
      : wxFrame(nullptr, wxID_ANY, "streamline"), brokers_(std::move(brokers)), timer_(this) {
    auto* panel = new wxPanel(this);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Invite code: the only thing viewers need (it is also the room's encryption secret).
    auto* invite_row = new wxBoxSizer(wxHORIZONTAL);
    room_ = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    copy_ = new wxButton(panel, wxID_ANY, "Copiar");
    copy_->Disable();
    invite_row->Add(new wxStaticText(panel, wxID_ANY, "Convite"), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    invite_row->Add(room_, 1, wxRIGHT, FromDIP(8));
    invite_row->Add(copy_, 0);

    // Fixed for the whole transmission: the encoder size can't change mid-stream.
    auto* quality_row = new wxBoxSizer(wxHORIZONTAL);
    resolution_ = new wxChoice(panel, wxID_ANY);
    for (const auto& r : kResolutions) resolution_->Append(std::format("{}x{}", r.width, r.height));
    resolution_->SetSelection(0);
    frame_rate_ = new wxChoice(panel, wxID_ANY);
    for (int fps : kFrameRates) frame_rate_->Append(std::format("{} fps", fps));
    frame_rate_->SetSelection(0);
    quality_row->Add(new wxStaticText(panel, wxID_ANY, wxString::FromUTF8("Resolução")), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    quality_row->Add(resolution_, 0, wxRIGHT, FromDIP(16));
    quality_row->Add(new wxStaticText(panel, wxID_ANY, "Quadros"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    quality_row->Add(frame_rate_, 0);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    stream_ = new wxButton(panel, wxID_ANY, wxString::FromUTF8("Transmitir"));
    watch_ = new wxButton(panel, wxID_ANY, wxString::FromUTF8("Assistir"));
    switch_ = new wxButton(panel, wxID_ANY, wxString::FromUTF8("Trocar fonte"));
    switch_->Disable();
    stop_ = new wxButton(panel, wxID_ANY, wxString::FromUTF8("Parar"));
    stop_->Disable();
    buttons->Add(stream_, 1, wxRIGHT, FromDIP(8));
    buttons->Add(switch_, 1, wxRIGHT, FromDIP(8));
    buttons->Add(watch_, 1, wxRIGHT, FromDIP(8));
    buttons->Add(stop_, 0);

    // d3d11videosink renders into this window's HWND (host preview or received video).
    preview_ = new wxPanel(panel, wxID_ANY);
    preview_->SetBackgroundColour(*wxBLACK);

    status_ = new wxStaticText(panel, wxID_ANY, wxString::FromUTF8("Parado"));
    log_ = new wxTextCtrl(panel, wxID_ANY, wxString::FromUTF8(report), wxDefaultPosition,
                          FromDIP(wxSize(-1, 110)), wxTE_MULTILINE | wxTE_READONLY);

    sizer->Add(invite_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    sizer->Add(quality_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    preview_item_ = sizer->Add(preview_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->Add(status_, 0, wxEXPAND | wxALL, FromDIP(12));
    sizer->Add(log_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    panel->SetSizer(sizer);
    panel_ = panel;
    sizer_ = sizer;
    SetClientSize(FromDIP(wxSize(960, 760)));
    SetIcons(wxIconBundle("appicon", nullptr));

    stream_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PickSource(); });
    switch_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PickNewSource(); });
    watch_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AskRoomAndWatch(); });
    stop_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Stop(); });
    copy_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CopyInvite(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { OnTick(); });

    // Video views. Clicks reach preview_ because the sink's child window is disabled
    // (see PassClicksThrough); keys work wherever focus is in the frame.
    preview_->SetToolTip(wxString::FromUTF8(
        "Duplo clique: tela cheia  ·  Botão direito: modos de visualização"));
    preview_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) {
      SetView(view_ == View::Fullscreen ? View::Normal : View::Fullscreen);
    });
    preview_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
      if (view_ != View::Pip) return e.Skip();
      // Borderless PiP window: dragging the video moves it.
      ReleaseCapture();
      SendMessage(GetHWND(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
    });
    preview_->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { ShowViewMenu(); });
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
      if (e.GetActive() && view_ == View::Pip) ApplyTopmost();
      e.Skip();
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
      switch (e.GetKeyCode()) {
        case WXK_F11:
          SetView(view_ == View::Fullscreen ? View::Normal : View::Fullscreen);
          return;
        case WXK_ESCAPE:
          if (view_ != View::Normal) return SetView(View::Normal);
          break;
      }
      e.Skip();
    });

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
      StopAll();
      e.Skip();
    });
  }

  bool StartHosting(const CaptureTarget& target, std::string room = {}) {
    HostPipeline::Config config;
    config.target = target;
    const Resolution& res = kResolutions[resolution_->GetSelection()];
    config.max_width = res.width;
    config.max_height = res.height;
    config.fps = kFrameRates[frame_rate_->GetSelection()];
    config.video_kbps = video_kbps_for(res, config.fps);
    std::string error;
    if (!host_.start(config, preview_->GetHWND(), error)) {
      Log("Erro: " + error);
      return false;
    }
    if (room.empty()) room = RoomChannel::new_room_code();
    if (!RoomChannel::valid_room_code(room)) {
      host_.stop();
      Log("Erro: código de convite inválido.");
      return false;
    }
    room_->SetValue(room);
    Log("Transmitindo: " + host_.description());
    session_.start(host_, room, brokers_,
                   {[this](const std::string& s) { CallAfter([this, s] { Log(s); }); },
                    [this](int n) { CallAfter([this, n] { viewers_ = n; }); }});
    EnterMode(Mode::Hosting);
    return true;
  }

  bool StartWatching(const std::string& room) {
    std::string error;
    if (!viewer_.start(room, brokers_, preview_->GetHWND(),
                       {[this](const std::string& s) { CallAfter([this, s] { Log(s); }); }},
                       error)) {
      Log("Erro: " + error);
      return false;
    }
    room_->SetValue(room);
    EnterMode(Mode::Watching);
    return true;
  }

  // Room, invite code and viewer connections stay as they are; only the capture changes.
  bool SwitchSource(const CaptureTarget& target) {
    std::string error;
    if (!host_.switch_source(target, error)) {
      Log("Erro ao trocar a fonte: " + error);
      return false;
    }
    Log("Fonte trocada: " + host_.description());
    return true;
  }

  // Test mode: run for `seconds`, print the log and stats, exit 0 if video frames flowed.
  void SetTest(int seconds) { test_seconds_ = seconds; }
  // Hosting test: switch to `target` halfway through.
  void SetTestSwitch(const CaptureTarget& target) { test_switch_ = target; }
  void SetQuality(int resolution, int frame_rate) {
    resolution_->SetSelection(resolution);
    frame_rate_->SetSelection(frame_rate);
  }
  void FailTest() { Finish(false); }

  enum class View { Normal, Player, Fullscreen, Pip };

  // Normal: every control. Player: only the video, in the same window. Fullscreen: player
  // on the whole monitor. Pip: player in a small borderless always-on-top window.
  void SetView(View view) {
    if (view == view_ || (mode_ == Mode::Idle && view != View::Normal)) return;
    View view_before = view_;
    // Back to a normal window first, then apply the new view.
    if (view_ == View::Fullscreen) ShowFullScreen(false);
    if (view_ == View::Pip) {
      SetWindowStyleFlag(wxDEFAULT_FRAME_STYLE);
      SetSize(normal_rect_);
      if (was_maximized_) Maximize(true);
    }
    view_ = view;

    bool controls = view == View::Normal;
    for (size_t i = 0; i < sizer_->GetItemCount(); ++i) {
      if (sizer_->GetItem(i) != preview_item_) sizer_->Show(i, controls);
    }
    preview_item_->SetBorder(controls ? FromDIP(12) : 0);
    panel_->SetBackgroundColour(controls ? wxNullColour : *wxBLACK);
    panel_->Layout();
    panel_->Refresh();

    if (view == View::Fullscreen) ShowFullScreen(true, wxFULLSCREEN_ALL);
    if (view == View::Pip) {
      was_maximized_ = IsMaximized();
      if (was_maximized_) Maximize(false);
      normal_rect_ = GetRect();
      SetWindowStyleFlag(wxSTAY_ON_TOP | wxRESIZE_BORDER);
      // 16:9 in the bottom-right corner of the monitor the window is on.
      wxRect area = wxDisplay(this).GetClientArea();
      wxSize size = FromDIP(wxSize(480, 270));
      int margin = FromDIP(16);
      SetSize(area.GetRight() - size.x - margin, area.GetBottom() - size.y - margin, size.x,
              size.y);
    }
    // Switching between captioned and popup styles leaves the window hidden on MSW, and
    // wxSTAY_ON_TOP set after creation doesn't reach WS_EX_TOPMOST.
    if (view == View::Pip || view_before == View::Pip) ApplyTopmost();
  }

  // Test hook: enter a view by name (player, fullscreen, pip) one second into the test.
  bool SetTestView(const std::string& name) {
    static const std::pair<const char*, View> names[] = {
        {"player", View::Player}, {"fullscreen", View::Fullscreen}, {"pip", View::Pip}};
    for (const auto& [n, v] : names) {
      if (name == n) {
        test_view_ = v;
        return true;
      }
    }
    return false;
  }

 private:
  enum class Mode { Idle, Hosting, Watching };

  // Windows may ignore HWND_TOPMOST while the app isn't in the foreground; it is applied
  // again on every activation.
  void ApplyTopmost() {
    SetWindowPos(GetHWND(), view_ == View::Pip ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }

  void ShowViewMenu() {
    wxMenu menu;
    struct Item {
      View view;
      const char* label;
    };
    const Item items[] = {{View::Normal, "Mostrar controles\tEsc"},
                          {View::Player, "Somente o vídeo"},
                          {View::Fullscreen, "Tela cheia\tF11"},
                          {View::Pip, "Picture-in-picture"}};
    for (int i = 0; i < int(std::size(items)); ++i) {
      menu.AppendRadioItem(i + 1, wxString::FromUTF8(items[i].label))
          ->Check(items[i].view == view_);
    }
    int chosen = GetPopupMenuSelectionFromUser(menu);
    if (chosen != wxID_NONE) SetView(items[chosen - 1].view);
  }

  // d3d11videosink renders into a child window of preview_ that it creates (and may
  // recreate) on its own thread, and that child swallows the mouse. A disabled child
  // window's mouse input goes to its parent, so disable whatever it created.
  void PassClicksThrough() {
    EnumChildWindows(
        preview_->GetHWND(),
        [](HWND child, LPARAM) -> BOOL {
          if (IsWindowEnabled(child)) EnableWindow(child, FALSE);
          return TRUE;
        },
        0);
  }

  void PickSource() {
    SourcePickerDialog dlg(this);
    if (dlg.ShowModal() == wxID_OK) StartHosting(dlg.selection());
  }

  void PickNewSource() {
    SourcePickerDialog dlg(this);
    if (dlg.ShowModal() == wxID_OK && mode_ == Mode::Hosting) SwitchSource(dlg.selection());
  }

  void AskRoomAndWatch() {
    wxTextEntryDialog dlg(this, wxString::FromUTF8("Cole o código de convite:"), "Assistir");
    if (dlg.ShowModal() != wxID_OK) return;
    std::string room(dlg.GetValue().Trim().Trim(false).utf8_str());  // case-sensitive
    if (room.empty()) return;
    if (!RoomChannel::valid_room_code(room)) {
      wxMessageBox(wxString::FromUTF8("Código de convite inválido."), "streamline",
                   wxICON_WARNING, this);
      return;
    }
    StartWatching(room);
  }

  void CopyInvite() {
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(room_->GetValue()));
      wxTheClipboard->Close();
      Log("Código de convite copiado.");
    }
  }

  void EnterMode(Mode mode) {
    mode_ = mode;
    ticks_ = 0;
    viewers_ = 0;
    bool idle = mode == Mode::Idle;
    resolution_->Enable(idle);
    frame_rate_->Enable(idle);
    stream_->Enable(idle);
    watch_->Enable(idle);
    copy_->Enable(mode == Mode::Hosting);
    switch_->Enable(mode == Mode::Hosting);
    stop_->Enable(!idle);
    if (idle) {
      SetView(View::Normal);  // the controls are needed to start again
      timer_.Stop();
      status_->SetLabel(wxString::FromUTF8("Parado"));
      room_->Clear();
    } else {
      timer_.Start(250);
    }
  }

  void StopAll() {
    session_.stop();  // before the pipeline: its webrtcbins live in it
    host_.stop();
    viewer_.stop();
  }

  void Stop() {
    StopAll();
    preview_->Refresh();
    EnterMode(Mode::Idle);
    Log("Parado.");
  }

  void OnTick() {
    std::string error = mode_ == Mode::Hosting ? host_.poll() : viewer_.poll();
    if (!error.empty()) {
      Log("Erro: " + error);
      if (test_seconds_) Finish(false);
      Stop();
      return;
    }
    PassClicksThrough();
    if (++ticks_ % 4 != 0) return;  // stats once per second

    std::string line;
    double fps = 0;
    if (mode_ == Mode::Hosting) {
      auto s = host_.stats();
      fps = s.fps;
      line = std::format("{}  |  {:.0f} fps  |  {:.0f} kbps  |  áudio {:.0f} dB  |  {} espectador(es)",
                         host_.description(), s.fps, s.video_kbps, s.audio_db, viewers_);
    } else {
      auto s = viewer_.stats();
      fps = s.fps;
      line = std::format("Assistindo  |  {:.0f} fps  |  {:.0f} kbps recebidos  |  áudio {:.0f} dB",
                         s.fps, s.video_kbps, s.audio_db);
    }
    status_->SetLabel(wxString::FromUTF8(line));
    if (test_seconds_) {
      test_log_ += line + "\n";
      if (test_view_ && ticks_ == 4) SetView(*std::exchange(test_view_, std::nullopt));
      // Watching: success means frames arrived over WebRTC at some point.
      if (fps > 0) frames_seen_ = true;
      if (test_switch_ && ticks_ / 4 == test_seconds_ / 2) {
        if (!SwitchSource(*test_switch_)) Finish(false);
        test_switch_.reset();
      }
      if (ticks_ / 4 >= test_seconds_) Finish(frames_seen_);
    }
  }

  void Finish(bool ok) {
    console_print(std::string(log_->GetValue().utf8_str()) + "\n" + test_log_);
    StopAll();
    std::exit(ok ? 0 : 1);
  }

  void Log(const std::string& text) { log_->AppendText(wxString::FromUTF8("\n" + text)); }

  std::vector<std::string> brokers_;
  HostPipeline host_;
  HostSession session_;
  ViewerSession viewer_;
  wxTimer timer_;
  Mode mode_ = Mode::Idle;
  wxTextCtrl* room_ = nullptr;
  wxButton* copy_ = nullptr;
  wxChoice* resolution_ = nullptr;
  wxChoice* frame_rate_ = nullptr;
  wxButton* stream_ = nullptr;
  wxButton* switch_ = nullptr;
  wxButton* watch_ = nullptr;
  wxButton* stop_ = nullptr;
  wxPanel* preview_ = nullptr;
  wxPanel* panel_ = nullptr;
  wxBoxSizer* sizer_ = nullptr;
  wxSizerItem* preview_item_ = nullptr;
  View view_ = View::Normal;
  wxRect normal_rect_;  // window before PiP
  bool was_maximized_ = false;
  wxStaticText* status_ = nullptr;
  wxTextCtrl* log_ = nullptr;
  int ticks_ = 0;
  int viewers_ = 0;
  int test_seconds_ = 0;
  bool frames_seen_ = false;
  std::optional<CaptureTarget> test_switch_;
  std::optional<View> test_view_;
  std::string test_log_;
};

class App : public wxApp {
 public:
  bool OnInit() override {
    bool bundled = streamline::setup_gst_environment();
    gst_init(nullptr, nullptr);

    bool ok = false;
    std::string report = environment_report(bundled, ok);
    Options opt;

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i].utf8_string();
      auto value = [&](const char* prefix) -> std::optional<std::string> {
        return arg.starts_with(prefix) ? std::optional(arg.substr(std::strlen(prefix)))
                                       : std::nullopt;
      };
      if (arg == "--check") {
        console_print(report);
        std::exit(ok ? 0 : 1);  // wx would turn a false OnInit into -1
      } else if (arg == "--check-package") {
        // Hardware-independent (CI): every bundled plugin DLL must load.
        int checked = 0;
        auto failed = streamline::failed_plugins(checked);
        std::string r = "plugins: " + std::to_string(checked - int(failed.size())) + "/" +
                        std::to_string(checked) + " loaded\n";
        for (const auto& f : failed) r += "  FAILED: " + f + "\n";
        console_print(r);
        std::exit(failed.empty() && checked > 0 ? 0 : 1);
      } else if (arg == "--list-sources") {
        console_print(sources_report());
        std::exit(0);
      } else if (auto v = value("--brokers=")) {
        opt.brokers = split_commas(*v);
      } else if (auto v = value("--room=")) {
        opt.room = *v;
      } else if (auto v = value("--test-seconds=")) {
        opt.test_seconds = std::atoi(v->c_str());
      } else if (auto v = value("--size=")) {  // e.g. 1280x720, one of kResolutions
        for (int r = 0; r < int(std::size(kResolutions)); ++r) {
          if (*v == std::format("{}x{}", kResolutions[r].width, kResolutions[r].height)) {
            opt.resolution = r;
          }
        }
      } else if (auto v = value("--fps=")) {
        for (int f = 0; f < int(std::size(kFrameRates)); ++f) {
          if (std::atoi(v->c_str()) == kFrameRates[f]) opt.frame_rate = f;
        }
      } else if (auto v = value("--test-view=")) {
        opt.test_view = *v;
      } else if (auto v = value("--test-watch=")) {
        opt.test_watch = *v;
      } else if (auto v = value("--test-switch=")) {
        opt.test_switch = test_target(*v);
        if (!opt.test_switch) {
          console_print("test switch target not found\n");
          std::exit(2);
        }
      } else if (auto v = value("--test-host=")) {
        opt.test_host = test_target(*v);
        if (!opt.test_host) {
          console_print("test target not found\n");
          std::exit(2);
        }
      }
    }

    auto* frame = new MainFrame(report, opt.brokers);
    frame->SetQuality(opt.resolution, opt.frame_rate);
    if (!opt.test_view.empty() && !frame->SetTestView(opt.test_view)) {
      console_print("unknown view\n");
      std::exit(2);
    }
    frame->Show();
    if (opt.test_host) {
      frame->SetTest(opt.test_seconds);
      if (opt.test_switch) frame->SetTestSwitch(*opt.test_switch);
      if (!frame->StartHosting(*opt.test_host, opt.room)) frame->FailTest();
    } else if (!opt.test_watch.empty()) {
      frame->SetTest(opt.test_seconds);
      if (!frame->StartWatching(opt.test_watch)) frame->FailTest();
    }
    return true;
  }
};

}  // namespace

wxIMPLEMENT_APP(App);
