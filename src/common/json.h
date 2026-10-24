#pragma once

// Tiny json-glib wrapper for the flat messages of the signaling protocol (sent
// encrypted through RoomChannel, which adds "v", "id", "from" and "ts"):
//   {"type": "join"}                                         viewer -> room, repeated
//   {"type": "offer" | "answer", "to": "<id>", "sdp": "..."}
//   {"type": "ice", "to": "<id>", "candidate": "...", "mline": 0}
//   {"type": "bye"}                                          leaving (also the MQTT will)

#include <cstdint>
#include <string>

#include <json-glib/json-glib.h>

namespace streamline::json {

class Message {
 public:
  Message() : object_(json_object_new()) {}
  Message(const char* data, size_t len) {
    JsonParser* parser = json_parser_new();
    if (json_parser_load_from_data(parser, data, static_cast<gssize>(len), nullptr)) {
      JsonNode* root = json_parser_get_root(parser);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) object_ = json_object_ref(json_node_get_object(root));
    }
    g_object_unref(parser);
  }
  ~Message() {
    if (object_) json_object_unref(object_);
  }
  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;

  bool valid() const { return object_ != nullptr; }

  std::string str(const char* key) const {
    JsonNode* n = value(key);
    return n && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
  }
  int64_t num(const char* key, int64_t fallback = 0) const {
    JsonNode* n = value(key);
    return n && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : fallback;
  }

  Message& set(const char* key, const std::string& v) {
    json_object_set_string_member(object_, key, v.c_str());
    return *this;
  }
  Message& set(const char* key, int64_t v) {
    json_object_set_int_member(object_, key, v);
    return *this;
  }

  std::string to_string() const {
    JsonNode* node = json_node_init_object(json_node_alloc(), object_);
    gchar* s = json_to_string(node, FALSE);
    std::string out(s);
    g_free(s);
    json_node_unref(node);
    return out;
  }

 private:
  JsonNode* value(const char* key) const {
    if (!object_ || !json_object_has_member(object_, key)) return nullptr;
    JsonNode* n = json_object_get_member(object_, key);
    return JSON_NODE_HOLDS_VALUE(n) ? n : nullptr;
  }

  JsonObject* object_ = nullptr;
};

}  // namespace streamline::json
