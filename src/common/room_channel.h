#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/json.h"
#include "common/mqtt.h"

namespace streamline {

// Serverless signaling: a room is a topic on public MQTT brokers, and every
// message is end-to-end encrypted with a key derived from the room code.
//
// Room code: 16 random bytes, base64url (22 chars). It is the only shared secret;
// from it we derive (BLAKE2b, separate contexts) the topic name and the
// XChaCha20-Poly1305 key, so brokers never see the code, the key or the content.
// Several brokers are used at once; duplicates are dropped by message id.
//
// Every method and callback runs on the GlibThread that called open().
class RoomChannel {
 public:
  using MessageFn = std::function<void(const json::Message&)>;
  using LogFn = std::function<void(const std::string&)>;

  static std::string new_room_code();
  static bool valid_room_code(const std::string& code);
  static const std::vector<std::string>& default_brokers();

  ~RoomChannel() { close(); }

  // `on_ready` fires once, when the first broker is connected and subscribed.
  bool open(const std::string& code, const std::vector<std::string>& brokers,
            MessageFn on_message, LogFn log, std::function<void()> on_ready);
  // Adds v/id/from/ts, encrypts and publishes on every connected broker.
  void send(const json::Message& msg);
  // Says "bye" and disconnects.
  void close();

  const std::string& self_id() const { return self_id_; }

 private:
  std::string seal(const std::string& plaintext) const;
  bool open_sealed(const std::string& payload, std::string& plaintext) const;
  std::string envelope(const json::Message& msg) const;
  void on_publish(const std::string& topic, const std::string& payload);
  bool seen(const std::string& id);

  std::vector<std::unique_ptr<MqttClient>> clients_;
  std::string topic_;
  uint8_t key_[32] = {};
  std::string self_id_;
  MessageFn on_message_;
  LogFn log_;
  std::function<void()> on_ready_;
  bool ready_ = false;
  int failed_ = 0;
  std::unordered_set<std::string> seen_ids_;
  std::deque<std::string> seen_order_;
};

}  // namespace streamline
