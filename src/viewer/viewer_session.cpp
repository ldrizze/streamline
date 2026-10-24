#include "viewer/viewer_session.h"

#include <gst/sdp/sdp.h>
#include <gst/video/videooverlay.h>
#include <gst/webrtc/webrtc.h>

namespace streamline {

namespace {

constexpr const char* kStunServer = "stun://stun.l.google.com:19302";
constexpr guint kJoinRetrySec = 3;
constexpr size_t kMaxPendingIce = 64;

std::string video_branch(const std::string& encoding) {
  if (encoding == "AV1") {
    return std::string("queue ! rtpav1depay ! av1parse ! ") +
           (has_element("d3d11av1dec") ? "d3d11av1dec" : "dav1ddec") +
           " ! d3d11videosink name=vsink sync=false";
  }
  if (encoding == "VP8") {
    return "queue ! rtpvp8depay ! vp8dec ! d3d11videosink name=vsink sync=false";
  }
  return {};
}

constexpr const char* kAudioBranch =
    "queue ! rtpopusdepay ! opusdec ! audioconvert ! audioresample "
    "! level interval=200000000 ! wasapi2sink sync=false";

}  // namespace

bool ViewerSession::start(const std::string& room, const std::vector<std::string>& brokers,
                          HWND video_window, Callbacks callbacks, std::string& error) {
  stop();
  if (!RoomChannel::valid_room_code(room)) {
    error = "Código de convite inválido.";
    return false;
  }
  callbacks_ = std::move(callbacks);
  video_window_ = video_window;

  pipeline_ = gst_pipeline_new("viewer");
  webrtc_ = gst_element_factory_make("webrtcbin", "webrtc");
  g_object_set(webrtc_, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "stun-server",
               kStunServer, "latency", 100, nullptr);
  gst_bin_add(GST_BIN(pipeline_), webrtc_);
  g_signal_connect(webrtc_, "pad-added", G_CALLBACK(on_pad_added), this);
  g_signal_connect(webrtc_, "on-ice-candidate", G_CALLBACK(on_ice_candidate), this);
  meter_.reset();
  audio_db_ = -100;

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    error = "Falha ao iniciar o WebRTC.";
    gst_object_unref(pipeline_);
    pipeline_ = webrtc_ = nullptr;
    return false;
  }

  thread_.start();
  thread_.invoke([this, room, brokers] {
    channel_.open(
        room, brokers, [this](const json::Message& m) { on_signal(m); },
        [this](const std::string& s) { log(s); },
        [this] {
          log("Procurando o transmissor na sala...");
          send_join();
          // The host may not be there yet (or missed it): ask again until an offer arrives.
          join_timer_ = g_timeout_source_new_seconds(kJoinRetrySec);
          g_source_set_callback(
              join_timer_,
              [](gpointer self) -> gboolean {
                static_cast<ViewerSession*>(self)->send_join();
                return G_SOURCE_CONTINUE;
              },
              this, nullptr);
          g_source_attach(join_timer_, g_main_context_get_thread_default());
        });
  });
  return true;
}

void ViewerSession::send_join() { channel_.send(json::Message().set("type", "join")); }

void ViewerSession::stop_join_timer() {
  if (!join_timer_) return;
  g_source_destroy(join_timer_);
  g_source_unref(join_timer_);
  join_timer_ = nullptr;
}

void ViewerSession::stop() {
  // Tear the pipeline down on the session thread so no callback races the shutdown.
  thread_.stop([this] {
    stop_join_timer();
    pending_ice_.clear();
    channel_.close();
    if (pipeline_) {
      g_signal_handlers_disconnect_by_data(webrtc_, this);
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
      pipeline_ = webrtc_ = nullptr;
    }
  });
  if (pipeline_) {  // thread never started
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = webrtc_ = nullptr;
  }
  host_id_.clear();
}

ViewerSession::Stats ViewerSession::stats() {
  Stats s;
  meter_.take(s.fps, s.video_kbps);
  s.audio_db = audio_db_;
  return s;
}

void ViewerSession::on_signal(const json::Message& m) {
  std::string type = m.str("type");
  std::string from = m.str("from");
  if (type == "offer") {
    if (!host_id_.empty() && from != host_id_) return;  // one host per room
    stop_join_timer();
    log("Transmissor encontrado. Negociando...");
    on_offer(from, m.str("sdp"));
    // Candidates that overtook the offer (different brokers, no ordering between them).
    for (const auto& p : pending_ice_) {
      if (p.from == host_id_) add_remote_ice(p.candidate, p.mline);
    }
    pending_ice_.clear();
  } else if (type == "ice" && host_id_.empty()) {
    if (pending_ice_.size() < kMaxPendingIce) {
      pending_ice_.push_back({from, m.str("candidate"), static_cast<guint>(m.num("mline"))});
    }
  } else if (from != host_id_) {
    return;  // ignore other viewers
  } else if (type == "ice") {
    add_remote_ice(m.str("candidate"), static_cast<guint>(m.num("mline")));
  } else if (type == "bye") {
    log("O transmissor encerrou.");
  }
}

void ViewerSession::add_remote_ice(const std::string& candidate, guint mline) {
  if (!candidate.empty()) {
    g_signal_emit_by_name(webrtc_, "add-ice-candidate", mline, candidate.c_str());
  }
}

void ViewerSession::on_offer(const std::string& from, const std::string& sdp) {
  host_id_ = from;
  GstSDPMessage* msg = nullptr;
  if (gst_sdp_message_new_from_text(sdp.c_str(), &msg) != GST_SDP_OK) {
    log("Oferta SDP inválida.");
    return;
  }
  auto* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, msg);
  GstPromise* promise = gst_promise_new_with_change_func(on_remote_set, this, nullptr);
  g_signal_emit_by_name(webrtc_, "set-remote-description", offer, promise);
  gst_webrtc_session_description_free(offer);
}

void ViewerSession::on_remote_set(GstPromise* promise, gpointer self) {
  gst_promise_unref(promise);
  auto* s = static_cast<ViewerSession*>(self);
  GstPromise* answer = gst_promise_new_with_change_func(on_answer_created, s, nullptr);
  g_signal_emit_by_name(s->webrtc_, "create-answer", nullptr, answer);
}

void ViewerSession::on_answer_created(GstPromise* promise, gpointer self) {
  auto* s = static_cast<ViewerSession*>(self);
  const GstStructure* reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription* answer = nullptr;
  if (reply) gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
  gst_promise_unref(promise);
  if (!answer) {
    s->thread_.invoke([s] { s->log("Falha ao criar a resposta WebRTC."); });
    return;
  }
  g_signal_emit_by_name(s->webrtc_, "set-local-description", answer, nullptr);
  gchar* text = gst_sdp_message_as_text(answer->sdp);
  std::string sdp = text;
  g_free(text);
  gst_webrtc_session_description_free(answer);
  s->thread_.invoke([s, sdp] {
    s->channel_.send(
        json::Message().set("type", "answer").set("to", s->host_id_).set("sdp", sdp));
    s->log("Negociação concluída. Conectando...");
  });
}

void ViewerSession::on_ice_candidate(GstElement*, guint mline, gchar* candidate, gpointer self) {
  auto* s = static_cast<ViewerSession*>(self);
  s->thread_.invoke([s, cand = std::string(candidate), mline] {
    s->channel_.send(json::Message()
                           .set("type", "ice")
                           .set("to", s->host_id_)
                           .set("candidate", cand)
                           .set("mline", static_cast<int64_t>(mline)));
  });
}

void ViewerSession::on_pad_added(GstElement*, GstPad* pad, gpointer self) {
  auto* s = static_cast<ViewerSession*>(self);
  if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;

  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const GstStructure* st = gst_caps_get_structure(caps, 0);
  std::string media = gst_structure_get_string(st, "media") ? gst_structure_get_string(st, "media") : "";
  std::string encoding =
      gst_structure_get_string(st, "encoding-name") ? gst_structure_get_string(st, "encoding-name") : "";
  gst_caps_unref(caps);

  std::string desc = media == "video" ? video_branch(encoding) : kAudioBranch;
  if (desc.empty()) {
    s->thread_.invoke([s, encoding] { s->log("Codec de vídeo não suportado: " + encoding); });
    return;
  }

  GError* err = nullptr;
  GstElement* branch = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
  if (!branch) {
    std::string msg = err ? err->message : "?";
    g_clear_error(&err);
    s->thread_.invoke([s, msg] { s->log("Erro ao montar a reprodução: " + msg); });
    return;
  }
  gst_bin_add(GST_BIN(s->pipeline_), branch);

  if (media == "video") {
    GstElement* sink = gst_bin_get_by_name(GST_BIN(branch), "vsink");
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),
                                        reinterpret_cast<guintptr>(s->video_window_));
    GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
    s->meter_.count_frames(sink_pad);
    gst_object_unref(sink_pad);
    gst_object_unref(sink);
    s->meter_.count_bytes(pad);
  }

  gst_element_sync_state_with_parent(branch);
  GstPad* branch_sink = gst_element_get_static_pad(branch, "sink");
  gst_pad_link(pad, branch_sink);
  gst_object_unref(branch_sink);

  s->thread_.invoke([s, media, encoding] {
    s->log(std::string("Recebendo ") + (media == "video" ? "vídeo " : "áudio ") + encoding);
  });
}

}  // namespace streamline
