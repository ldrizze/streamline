#include "common/signaling.h"

namespace streamline {

void SignalingClient::connect(const std::string& url, MessageFn on_message, StateFn on_state) {
  close();
  on_message_ = std::move(on_message);
  on_state_ = std::move(on_state);

  SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!msg) {
    on_state_(false, "URL do servidor inválida: " + url);
    return;
  }
  session_ = soup_session_new();
  cancel_ = g_cancellable_new();
  soup_session_websocket_connect_async(session_, msg, nullptr, nullptr, G_PRIORITY_DEFAULT,
                                       cancel_, &SignalingClient::on_connected, this);
  g_object_unref(msg);
}

void SignalingClient::on_connected(GObject* session, GAsyncResult* res, gpointer self) {
  GError* error = nullptr;
  SoupWebsocketConnection* conn =
      soup_session_websocket_connect_finish(SOUP_SESSION(session), res, &error);
  if (!conn) {
    // Cancelled means close() ran and `self` may be gone.
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      static_cast<SignalingClient*>(self)->on_state_(
          false, std::string("Falha ao conectar ao servidor: ") + error->message);
    }
    g_error_free(error);
    return;
  }
  auto* c = static_cast<SignalingClient*>(self);
  c->conn_ = conn;
  soup_websocket_connection_set_keepalive_interval(conn, 15);
  g_signal_connect(conn, "message", G_CALLBACK(&SignalingClient::on_ws_message), c);
  g_signal_connect(conn, "closed", G_CALLBACK(&SignalingClient::on_ws_closed), c);
  c->on_state_(true, {});
}

void SignalingClient::on_ws_message(SoupWebsocketConnection*, gint type, GBytes* data,
                                    gpointer self) {
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
  gsize len = 0;
  const char* text = static_cast<const char*>(g_bytes_get_data(data, &len));
  json::Message msg(text, len);
  if (msg.valid()) static_cast<SignalingClient*>(self)->on_message_(msg);
}

void SignalingClient::on_ws_closed(SoupWebsocketConnection*, gpointer self) {
  static_cast<SignalingClient*>(self)->on_state_(false, "Conexão com o servidor encerrada.");
}

void SignalingClient::send(const json::Message& msg) {
  if (conn_ && soup_websocket_connection_get_state(conn_) == SOUP_WEBSOCKET_STATE_OPEN) {
    soup_websocket_connection_send_text(conn_, msg.to_string().c_str());
  }
}

void SignalingClient::close() {
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
