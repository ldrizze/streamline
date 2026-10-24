#pragma once

#include <functional>
#include <string>

#include <libsoup/soup.h>

#include "common/json.h"

namespace streamline {

// WebSocket client for streamline-signal. Every method and callback runs on the
// GlibThread that called connect().
class SignalingClient {
 public:
  using MessageFn = std::function<void(const json::Message&)>;
  using StateFn = std::function<void(bool connected, const std::string& error)>;

  ~SignalingClient() { close(); }

  void connect(const std::string& url, MessageFn on_message, StateFn on_state);
  void send(const json::Message& msg);
  void close();

 private:
  static void on_connected(GObject* session, GAsyncResult* res, gpointer self);
  static void on_ws_message(SoupWebsocketConnection*, gint type, GBytes* data, gpointer self);
  static void on_ws_closed(SoupWebsocketConnection*, gpointer self);

  SoupSession* session_ = nullptr;
  SoupWebsocketConnection* conn_ = nullptr;
  GCancellable* cancel_ = nullptr;
  MessageFn on_message_;
  StateFn on_state_;
};

}  // namespace streamline
