// streamline-signal: WebSocket signaling server.
// Rooms hold one host and any number of viewers; offer/answer/ice are relayed
// between them. No media passes through here. Protocol: src/common/json.h.
//
// usage: streamline-signal [port] [--local]   (--local: listen on 127.0.0.1 only)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include <libsoup/soup.h>

#include "common/json.h"

using streamline::json::Message;

namespace {

struct Client {
  SoupWebsocketConnection* conn = nullptr;
  std::string id;
  std::string room;  // empty until joined
  bool host = false;
};

struct Room {
  Client* host = nullptr;
  std::map<std::string, Client*> viewers;
};

std::map<SoupWebsocketConnection*, std::unique_ptr<Client>> g_clients;
std::map<std::string, Room> g_rooms;
unsigned g_next_id = 1;

void send(Client* c, const Message& m) {
  if (soup_websocket_connection_get_state(c->conn) == SOUP_WEBSOCKET_STATE_OPEN) {
    soup_websocket_connection_send_text(c->conn, m.to_string().c_str());
  }
}

void send_error(Client* c, const std::string& text) {
  send(c, Message().set("type", "error").set("message", text));
}

void join(Client* c, const Message& m) {
  std::string room = m.str("room");
  std::string role = m.str("role");
  if (!c->room.empty()) return send_error(c, "already joined");
  if (room.empty() || (role != "host" && role != "viewer")) return send_error(c, "bad join");

  Room& r = g_rooms[room];
  if (role == "host") {
    if (r.host) return send_error(c, "room already has a host");
    c->host = true;
    c->room = room;
    r.host = c;
    send(c, Message().set("type", "joined").set("id", c->id));
    for (auto& [id, v] : r.viewers) send(c, Message().set("type", "viewer-joined").set("id", id));
  } else {
    c->room = room;
    r.viewers[c->id] = c;
    send(c, Message().set("type", "joined").set("id", c->id));
    if (r.host) send(r.host, Message().set("type", "viewer-joined").set("id", c->id));
  }
  std::printf("%s joined %s as %s\n", c->id.c_str(), room.c_str(), role.c_str());
}

// offer / answer / ice: forward to "to" within the same room, stamping "from".
void relay(Client* c, const char* data, size_t len) {
  Message m(data, len);
  auto it = g_rooms.find(c->room);
  if (it == g_rooms.end()) return;
  Room& r = it->second;
  std::string to = m.str("to");
  Client* target = nullptr;
  if (r.host && r.host->id == to) {
    target = r.host;
  } else if (auto v = r.viewers.find(to); v != r.viewers.end()) {
    target = v->second;
  }
  if (!target || target == c) return;
  m.set("from", c->id);
  send(target, m);
}

void leave(Client* c) {
  auto it = g_rooms.find(c->room);
  if (it == g_rooms.end()) return;
  Room& r = it->second;
  if (c->host) {
    r.host = nullptr;
    for (auto& [id, v] : r.viewers) send(v, Message().set("type", "host-left"));
  } else {
    r.viewers.erase(c->id);
    if (r.host) send(r.host, Message().set("type", "viewer-left").set("id", c->id));
  }
  if (!r.host && r.viewers.empty()) g_rooms.erase(it);
  std::printf("%s left %s\n", c->id.c_str(), c->room.c_str());
}

void on_message(SoupWebsocketConnection* conn, gint type, GBytes* bytes, gpointer) {
  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
  Client* c = g_clients[conn].get();
  gsize len = 0;
  const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &len));
  Message m(data, len);
  if (!m.valid()) return send_error(c, "invalid json");

  std::string type_name = m.str("type");
  if (type_name == "join") {
    join(c, m);
  } else if (c->room.empty()) {
    send_error(c, "join first");
  } else if (type_name == "offer" || type_name == "answer" || type_name == "ice") {
    relay(c, data, len);
  }
}

void on_closed(SoupWebsocketConnection* conn, gpointer) {
  auto it = g_clients.find(conn);
  if (it == g_clients.end()) return;
  leave(it->second.get());
  std::printf("%s disconnected\n", it->second->id.c_str());
  g_clients.erase(it);
  g_object_unref(conn);
}

void on_websocket(SoupServer*, SoupServerMessage* msg, const char*, SoupWebsocketConnection* conn,
                  gpointer) {
  auto client = std::make_unique<Client>();
  client->conn = static_cast<SoupWebsocketConnection*>(g_object_ref(conn));
  client->id = "p" + std::to_string(g_next_id++);

  GInetSocketAddress* addr = G_INET_SOCKET_ADDRESS(soup_server_message_get_remote_address(msg));
  gchar* ip = g_inet_address_to_string(g_inet_socket_address_get_address(addr));
  std::printf("%s connected from %s\n", client->id.c_str(), ip);
  g_free(ip);

  soup_websocket_connection_set_keepalive_interval(conn, 15);
  g_signal_connect(conn, "message", G_CALLBACK(on_message), nullptr);
  g_signal_connect(conn, "closed", G_CALLBACK(on_closed), nullptr);
  g_clients[conn] = std::move(client);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  guint port = 8443;
  bool local = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--local") == 0) {
      local = true;
    } else {
      port = static_cast<guint>(std::atoi(argv[i]));
    }
  }

  SoupServer* server = soup_server_new("server-header", "streamline-signal", nullptr);
  soup_server_add_websocket_handler(server, "/ws", nullptr, nullptr, on_websocket, nullptr,
                                    nullptr);

  GError* error = nullptr;
  auto opts = static_cast<SoupServerListenOptions>(0);
  bool ok = local ? soup_server_listen_local(server, port, opts, &error)
                  : soup_server_listen_all(server, port, opts, &error);
  if (!ok) {
    std::fprintf(stderr, "listen failed: %s\n", error->message);
    g_error_free(error);
    return 1;
  }
  std::printf("streamline-signal listening on ws://%s:%u/ws\n", local ? "127.0.0.1" : "0.0.0.0",
              port);

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  return 0;
}
