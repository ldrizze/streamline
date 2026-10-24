#include "common/mqtt.h"

namespace streamline {

namespace {

constexpr uint16_t kKeepAliveSec = 30;

enum : uint8_t {
  kConnect = 0x10,
  kConnAck = 0x20,
  kPublish = 0x30,
  kSubscribe = 0x82,  // fixed flags 0010
  kSubAck = 0x90,
  kPingReq = 0xC0,
  kPingResp = 0xD0,
  kDisconnect = 0xE0,
};

std::string u16(uint16_t v) { return {char(v >> 8), char(v & 0xFF)}; }

// Length-prefixed UTF-8 string / binary data.
std::string field(const std::string& s) { return u16(static_cast<uint16_t>(s.size())) + s; }

std::string remaining_length(size_t n) {
  std::string out;
  do {
    uint8_t b = n % 128;
    n /= 128;
    if (n > 0) b |= 0x80;
    out += char(b);
  } while (n > 0);
  return out;
}

}  // namespace

void MqttClient::connect(const std::string& url, const std::string& client_id,
                         const std::string& will_topic, const std::string& will_payload,
                         Callbacks callbacks) {
  close();
  url_ = url;
  client_id_ = client_id;
  will_topic_ = will_topic;
  will_payload_ = will_payload;
  callbacks_ = std::move(callbacks);

  SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!msg) {
    fail("URL inválida");
    return;
  }
  session_ = soup_session_new();
  cancel_ = g_cancellable_new();
  const char* protocols[] = {"mqtt", nullptr};
  soup_session_websocket_connect_async(session_, msg, nullptr, const_cast<char**>(protocols),
                                       G_PRIORITY_DEFAULT, cancel_, &MqttClient::on_ws_connected,
                                       this);
  g_object_unref(msg);
}

void MqttClient::on_ws_connected(GObject* session, GAsyncResult* res, gpointer self) {
  GError* error = nullptr;
  SoupWebsocketConnection* conn =
      soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
  if (!conn) {
    // Cancelled means close() ran and `self` may be gone.
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      static_cast<MqttClient*>(self)->fail(error->message);
    }
    g_error_free(error);
    return;
  }
  auto* c = static_cast<MqttClient*>(self);
  c->conn_ = conn;
  g_signal_connect(conn, "message", G_CALLBACK(&MqttClient::on_ws_message), c);
  g_signal_connect(conn, "closed", G_CALLBACK(&MqttClient::on_ws_closed), c);

  bool will = !c->will_topic_.empty();
  std::string body = field("MQTT") + char(4) /* 3.1.1 */ +
                     char(0x02 /* clean session */ | (will ? 0x04 : 0)) + u16(kKeepAliveSec) +
                     field(c->client_id_);
  if (will) body += field(c->will_topic_) + field(c->will_payload_);
  c->send_packet(kConnect, body);
}

void MqttClient::on_ws_message(SoupWebsocketConnection*, gint, GBytes* data, gpointer self) {
  auto* c = static_cast<MqttClient*>(self);
  gsize len = 0;
  const char* bytes = static_cast<const char*>(g_bytes_get_data(data, &len));
  c->inbuf_.append(bytes, len);

  // A frame may carry several packets, or part of one.
  for (;;) {
    if (c->inbuf_.size() < 2) return;
    size_t length = 0, pos = 1;
    int shift = 0;
    for (;;) {
      if (pos >= c->inbuf_.size()) return;  // length bytes incomplete
      uint8_t b = static_cast<uint8_t>(c->inbuf_[pos++]);
      length |= size_t(b & 0x7F) << shift;
      if (!(b & 0x80)) break;
      shift += 7;
      if (shift > 21) {
        c->fail("pacote MQTT inválido");
        return;
      }
    }
    if (c->inbuf_.size() < pos + length) return;
    uint8_t header = static_cast<uint8_t>(c->inbuf_[0]);
    std::string body = c->inbuf_.substr(pos, length);
    c->inbuf_.erase(0, pos + length);
    c->handle_packet(header, body);
    if (!c->conn_) return;  // handler failed/closed
  }
}

void MqttClient::handle_packet(uint8_t header, const std::string& body) {
  switch (header & 0xF0) {
    case kConnAck: {
      uint8_t rc = body.size() >= 2 ? static_cast<uint8_t>(body[1]) : 0xFF;
      if (rc != 0) {
        fail("broker recusou a conexão (código " + std::to_string(rc) + ")");
        return;
      }
      ready_ = true;
      ping_ = g_timeout_source_new_seconds(kKeepAliveSec / 2);
      g_source_set_callback(ping_, &MqttClient::on_ping, this, nullptr);
      g_source_attach(ping_, g_main_context_get_thread_default());
      if (callbacks_.connected) callbacks_.connected();
      break;
    }
    case kPublish: {
      if (body.size() < 2) return;
      size_t topic_len = (uint8_t(body[0]) << 8) | uint8_t(body[1]);
      size_t pos = 2 + topic_len;
      if ((header >> 1) & 0x03) pos += 2;  // QoS > 0 carries a packet id
      if (pos > body.size()) return;
      if (callbacks_.message) callbacks_.message(body.substr(2, topic_len), body.substr(pos));
      break;
    }
    default:  // SUBACK, PINGRESP: nothing to do
      break;
  }
}

void MqttClient::subscribe(const std::string& topic) {
  if (!ready_) return;
  uint16_t id = next_packet_id_++;
  if (next_packet_id_ == 0) next_packet_id_ = 1;
  send_packet(kSubscribe, u16(id) + field(topic) + char(0) /* QoS 0 */);
}

void MqttClient::publish(const std::string& topic, const std::string& payload) {
  if (ready_) send_packet(kPublish, field(topic) + payload);
}

gboolean MqttClient::on_ping(gpointer self) {
  static_cast<MqttClient*>(self)->send_packet(kPingReq, {});
  return G_SOURCE_CONTINUE;
}

void MqttClient::send_packet(uint8_t header, const std::string& body) {
  if (!conn_ || soup_websocket_connection_get_state(conn_) != SOUP_WEBSOCKET_STATE_OPEN) return;
  std::string packet = char(header) + remaining_length(body.size()) + body;
  soup_websocket_connection_send_binary(conn_, packet.data(), packet.size());
}

void MqttClient::on_ws_closed(SoupWebsocketConnection*, gpointer self) {
  static_cast<MqttClient*>(self)->fail("conexão encerrada pelo broker");
}

void MqttClient::fail(const std::string& error) {
  teardown();
  if (callbacks_.closed) callbacks_.closed(error);
}

void MqttClient::close() {
  if (ready_) send_packet(kDisconnect, {});
  teardown();
}

void MqttClient::teardown() {
  ready_ = false;
  inbuf_.clear();
  if (ping_) {
    g_source_destroy(ping_);
    g_source_unref(ping_);
    ping_ = nullptr;
  }
  if (cancel_) {
    g_cancellable_cancel(cancel_);
    g_clear_object(&cancel_);
  }
  if (conn_) {
    g_signal_handlers_disconnect_by_data(conn_, this);
    if (soup_websocket_connection_get_state(conn_) == SOUP_WEBSOCKET_STATE_OPEN) {
      soup_websocket_connection_close(conn_, SOUP_WEBSOCKET_CLOSE_NORMAL, nullptr);
    }
    g_clear_object(&conn_);
  }
  g_clear_object(&session_);
}

}  // namespace streamline
