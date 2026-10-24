#include "host/host_session.h"

#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>

#include "host/host_pipeline.h"

namespace streamline {

namespace {

constexpr const char* kStunServer = "stun://stun.l.google.com:19302";
constexpr int kVideoPt = 96;
constexpr int kAudioPt = 97;

// Heap context handed to GStreamer callbacks; freed with the handler/promise.
struct PeerCtx {
  HostSession* session;
  std::string id;
};

PeerCtx* new_ctx(HostSession* s, const std::string& id) { return new PeerCtx{s, id}; }
void free_ctx(gpointer p, GClosure* = nullptr) { delete static_cast<PeerCtx*>(p); }

GstElement* make(const char* factory, const std::string& name) {
  return gst_element_factory_make(factory, name.c_str());
}

}  // namespace

void HostSession::start(HostPipeline& pipeline, const std::string& room,
                        const std::vector<std::string>& brokers, Callbacks callbacks) {
  stop();
  pipeline_ = &pipeline;
  callbacks_ = std::move(callbacks);
  thread_.start();
  thread_.invoke([this, room, brokers] {
    channel_.open(
        room, brokers, [this](const json::Message& m) { on_signal(m); },
        [this](const std::string& s) { log(s); },
        [this] { log("Sala aberta. Envie o código de convite aos espectadores."); });
  });
}

void HostSession::stop() {
  // Elements belong to the HostPipeline's bin; it tears them down when it stops.
  thread_.stop([this] {
    channel_.close();
    for (auto& [id, peer] : peers_) disconnect_handlers(peer.webrtc);
    peers_.clear();
  });
  pipeline_ = nullptr;
}

void HostSession::on_signal(const json::Message& m) {
  std::string type = m.str("type");
  std::string from = m.str("from");
  if (type == "join") {
    add_peer(from);  // viewers repeat "join" until they get an offer; duplicates are ignored
  } else if (type == "bye") {
    remove_peer(from);
  } else if (type == "answer") {
    on_answer(from, m.str("sdp"));
  } else if (type == "ice") {
    on_remote_ice(from, m.str("candidate"), static_cast<int>(m.num("mline")));
  }
}

void HostSession::add_peer(const std::string& id) {
  if (!pipeline_ || !pipeline_->pipeline() || peers_.count(id)) return;
  GstBin* bin = GST_BIN(pipeline_->pipeline());

  Peer p;
  p.id = id;
  p.webrtc = make("webrtcbin", "webrtc_" + id);
  g_object_set(p.webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "stun-server",
               kStunServer, nullptr);

  bool av1 = pipeline_->video_is_av1();
  // Leaky queues: a slow viewer drops its own packets instead of stalling the tee.
  for (int i : {0, 2}) {
    p.elements[i] = make("queue", (i == 0 ? "vq_" : "aq_") + id);
    g_object_set(p.elements[i], "leaky", 2, "max-size-buffers", 0, "max-size-bytes", 0,
                 "max-size-time", G_GUINT64_CONSTANT(500000000), nullptr);
  }
  p.elements[1] = make(av1 ? "rtpav1pay" : "rtpvp8pay", "vpay_" + id);
  p.elements[3] = make("rtpopuspay", "apay_" + id);
  g_object_set(p.elements[1], "pt", kVideoPt, nullptr);
  g_object_set(p.elements[3], "pt", kAudioPt, nullptr);

  gst_bin_add_many(bin, p.webrtc, p.elements[0], p.elements[1], p.elements[2], p.elements[3],
                   nullptr);
  gst_element_link(p.elements[0], p.elements[1]);
  gst_element_link(p.elements[2], p.elements[3]);

  g_signal_connect_data(p.webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed),
                        new_ctx(this, id), free_ctx, GConnectFlags(0));
  g_signal_connect_data(p.webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate),
                        new_ctx(this, id), free_ctx, GConnectFlags(0));
  g_signal_connect_data(p.webrtc, "notify::ice-connection-state", G_CALLBACK(on_ice_state),
                        new_ctx(this, id), free_ctx, GConnectFlags(0));

  // One sendonly transceiver per media, with codec preferences so the offer
  // doesn't depend on caps having flowed yet.
  GstPadTemplate* templ =
      gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(p.webrtc), "sink_%u");
  const char* media_caps[2] = {
      av1 ? "application/x-rtp,media=video,encoding-name=AV1,clock-rate=90000,payload=96"
          : "application/x-rtp,media=video,encoding-name=VP8,clock-rate=90000,payload=96",
      "application/x-rtp,media=audio,encoding-name=OPUS,clock-rate=48000,payload=97"};
  for (int m = 0; m < 2; ++m) {
    GstCaps* caps = gst_caps_from_string(media_caps[m]);
    GstPad* sink = gst_element_request_pad(p.webrtc, templ, nullptr, caps);
    GstWebRTCRTPTransceiver* trans = nullptr;
    g_object_get(sink, "transceiver", &trans, nullptr);
    g_object_set(trans, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                 "codec-preferences", caps, nullptr);
    gst_object_unref(trans);
    GstPad* pay_src = gst_element_get_static_pad(p.elements[m * 2 + 1], "src");
    gst_pad_link(pay_src, sink);
    gst_object_unref(pay_src);
    gst_object_unref(sink);
    gst_caps_unref(caps);
  }

  gst_element_sync_state_with_parent(p.webrtc);
  for (GstElement* e : p.elements) gst_element_sync_state_with_parent(e);

  // Finally attach to the running tees.
  const char* tees[2] = {"video_tee", "audio_tee"};
  for (int m = 0; m < 2; ++m) {
    GstElement* tee = gst_bin_get_by_name(bin, tees[m]);
    p.tee_pads[m] = gst_element_request_pad_simple(tee, "src_%u");
    GstPad* q_sink = gst_element_get_static_pad(p.elements[m * 2], "sink");
    gst_pad_link(p.tee_pads[m], q_sink);
    if (m == 0) {
      // New viewer needs a keyframe to start decoding.
      gst_pad_push_event(q_sink, gst_video_event_new_upstream_force_key_unit(
                                     GST_CLOCK_TIME_NONE, TRUE, 0));
    }
    gst_object_unref(q_sink);
    gst_object_unref(tee);
  }

  peers_[id] = p;
  log("Espectador " + id + " entrou. Negociando WebRTC...");
  if (callbacks_.viewers) callbacks_.viewers(static_cast<int>(peers_.size()));
}

void HostSession::disconnect_handlers(GstElement* webrtc) {
  for (gpointer fn : {reinterpret_cast<gpointer>(on_negotiation_needed),
                      reinterpret_cast<gpointer>(on_ice_candidate),
                      reinterpret_cast<gpointer>(on_ice_state)}) {
    g_signal_handlers_disconnect_matched(webrtc, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, fn, nullptr);
  }
}

void HostSession::remove_peer(const std::string& id) {
  auto it = peers_.find(id);
  if (it == peers_.end() || !pipeline_ || !pipeline_->pipeline()) return;
  Peer p = it->second;
  peers_.erase(it);

  GstBin* bin = GST_BIN(pipeline_->pipeline());
  const char* tees[2] = {"video_tee", "audio_tee"};
  for (int m = 0; m < 2; ++m) {
    // tee (allow-not-linked) tolerates a pad being unlinked and released while running.
    GstElement* tee = gst_bin_get_by_name(bin, tees[m]);
    if (GstPad* peer = gst_pad_get_peer(p.tee_pads[m])) {
      gst_pad_unlink(p.tee_pads[m], peer);
      gst_object_unref(peer);
    }
    gst_element_release_request_pad(tee, p.tee_pads[m]);
    gst_object_unref(p.tee_pads[m]);
    gst_object_unref(tee);
  }
  disconnect_handlers(p.webrtc);
  for (GstElement* e : {p.elements[0], p.elements[1], p.elements[2], p.elements[3], p.webrtc}) {
    gst_element_set_state(e, GST_STATE_NULL);
    gst_bin_remove(bin, e);
  }
  log("Espectador " + id + " saiu.");
  if (callbacks_.viewers) callbacks_.viewers(static_cast<int>(peers_.size()));
}

void HostSession::on_negotiation_needed(GstElement* webrtc, gpointer ctx) {
  auto* c = static_cast<PeerCtx*>(ctx);
  GstPromise* promise =
      gst_promise_new_with_change_func(on_offer_created, new_ctx(c->session, c->id),
                                       [](gpointer p) { free_ctx(p); });
  g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void HostSession::on_offer_created(GstPromise* promise, gpointer ctx) {
  auto* c = static_cast<PeerCtx*>(ctx);
  const GstStructure* reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription* offer = nullptr;
  if (reply) gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
  gst_promise_unref(promise);
  if (!offer) return;

  HostSession* s = c->session;
  std::string id = c->id;
  gchar* text = gst_sdp_message_as_text(offer->sdp);
  std::string sdp = text;
  g_free(text);
  s->thread_.invoke([s, id, sdp, offer] {
    auto it = s->peers_.find(id);
    if (it != s->peers_.end()) {
      g_signal_emit_by_name(it->second.webrtc, "set-local-description", offer, nullptr);
      s->channel_.send(json::Message().set("type", "offer").set("to", id).set("sdp", sdp));
    }
    gst_webrtc_session_description_free(offer);
  });
}

void HostSession::on_answer(const std::string& id, const std::string& sdp) {
  auto it = peers_.find(id);
  if (it == peers_.end()) return;
  GstSDPMessage* msg = nullptr;
  gst_sdp_message_new_from_text(sdp.c_str(), &msg);
  auto* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, msg);
  g_signal_emit_by_name(it->second.webrtc, "set-remote-description", answer, nullptr);
  gst_webrtc_session_description_free(answer);
}

void HostSession::on_remote_ice(const std::string& id, const std::string& candidate, int mline) {
  auto it = peers_.find(id);
  if (it == peers_.end() || candidate.empty()) return;
  g_signal_emit_by_name(it->second.webrtc, "add-ice-candidate", mline, candidate.c_str());
}

void HostSession::on_ice_candidate(GstElement*, guint mline, gchar* candidate, gpointer ctx) {
  auto* c = static_cast<PeerCtx*>(ctx);
  HostSession* s = c->session;
  s->thread_.invoke([s, id = c->id, cand = std::string(candidate), mline] {
    s->channel_.send(json::Message()
                           .set("type", "ice")
                           .set("to", id)
                           .set("candidate", cand)
                           .set("mline", static_cast<int64_t>(mline)));
  });
}

void HostSession::on_ice_state(GstElement* webrtc, GParamSpec*, gpointer ctx) {
  auto* c = static_cast<PeerCtx*>(ctx);
  GstWebRTCICEConnectionState state;
  g_object_get(webrtc, "ice-connection-state", &state, nullptr);
  const char* text = state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ? "conectado (P2P)"
                     : state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED  ? "falhou (NAT?)"
                     : state == GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED ? "desconectado"
                                                                             : nullptr;
  if (!text) return;
  HostSession* s = c->session;
  bool connected = state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED;
  s->thread_.invoke([s, id = c->id, connected, msg = "Espectador " + c->id + ": " + text] {
    s->log(msg);
    // The keyframe requested on join went out before media could flow; ask again.
    auto it = s->peers_.find(id);
    if (connected && it != s->peers_.end()) request_keyframe(it->second.elements[0]);
  });
}

void HostSession::request_keyframe(GstElement* video_queue) {
  GstPad* sink = gst_element_get_static_pad(video_queue, "sink");
  gst_pad_push_event(sink,
                     gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
  gst_object_unref(sink);
}

}  // namespace streamline
