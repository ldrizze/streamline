#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <libsoup/soup.h>

namespace streamline {

// Minimal MQTT 3.1.1 client over WebSocket (subprotocol "mqtt"): clean session,
// QoS 0 publish/subscribe, keepalive pings and an optional last-will message.
// Every method and callback runs on the GlibThread that called connect().
class MqttClient {
 public:
  struct Callbacks {
    std::function<void()> connected;  // CONNACK accepted
    std::function<void(const std::string& topic, const std::string& payload)> message;
    std::function<void(const std::string& error)> closed;  // not called after close()
  };

  ~MqttClient() { close(); }

  void connect(const std::string& url, const std::string& client_id, const std::string& will_topic,
               const std::string& will_payload, Callbacks callbacks);
  void subscribe(const std::string& topic);
  void publish(const std::string& topic, const std::string& payload);
  // Graceful DISCONNECT: the broker discards the last will.
  void close();

  bool ready() const { return ready_; }
  const std::string& url() const { return url_; }

 private:
  static void on_ws_connected(GObject* session, GAsyncResult* res, gpointer self);
  static void on_ws_message(SoupWebsocketConnection*, gint type, GBytes* data, gpointer self);
  static void on_ws_closed(SoupWebsocketConnection*, gpointer self);
  static gboolean on_ping(gpointer self);

  void send_packet(uint8_t header, const std::string& body);
  void handle_packet(uint8_t header, const std::string& body);
  void fail(const std::string& error);
  void teardown();

  std::string url_;
  std::string client_id_;
  std::string will_topic_;
  std::string will_payload_;
  Callbacks callbacks_;
  SoupSession* session_ = nullptr;
  SoupWebsocketConnection* conn_ = nullptr;
  GCancellable* cancel_ = nullptr;
  GSource* ping_ = nullptr;
  std::string inbuf_;  // partial MQTT packets across WebSocket frames
  uint16_t next_packet_id_ = 1;
  bool ready_ = false;
};

}  // namespace streamline
