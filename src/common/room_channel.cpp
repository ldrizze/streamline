#include "common/room_channel.h"

#include <chrono>
#include <cstdlib>

#include <sodium.h>

namespace streamline {

namespace {

constexpr size_t kRoomBytes = 16;
constexpr int64_t kMaxAgeMs = 120'000;  // replay window (and tolerated clock skew)
constexpr size_t kSeenIds = 1024;
constexpr char kTopicContext[] = "streamline/topic/v1";
constexpr char kKeyContext[] = "streamline/key/v1";
constexpr int kBase64 = sodium_base64_VARIANT_URLSAFE_NO_PADDING;

bool decode_room(const std::string& code, uint8_t (&secret)[kRoomBytes]) {
  size_t len = 0;
  return sodium_init() >= 0 &&
         sodium_base642bin(secret, kRoomBytes, code.data(), code.size(), nullptr, &len, nullptr,
                           kBase64) == 0 &&
         len == kRoomBytes;
}

std::string random_hex(size_t bytes) {
  std::string raw(bytes, '\0');
  randombytes_buf(raw.data(), bytes);
  std::string hex(bytes * 2 + 1, '\0');
  sodium_bin2hex(hex.data(), hex.size(), reinterpret_cast<const uint8_t*>(raw.data()), bytes);
  hex.pop_back();
  return hex;
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string broker_host(const std::string& url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  return url.substr(start, url.find_first_of(":/", start) - start);
}

}  // namespace

std::string RoomChannel::new_room_code() {
  if (sodium_init() < 0) std::abort();
  uint8_t secret[kRoomBytes];
  randombytes_buf(secret, sizeof secret);
  char out[sodium_base64_ENCODED_LEN(kRoomBytes, kBase64)];
  sodium_bin2base64(out, sizeof out, secret, sizeof secret, kBase64);
  sodium_memzero(secret, sizeof secret);
  return out;
}

bool RoomChannel::valid_room_code(const std::string& code) {
  uint8_t secret[kRoomBytes];
  bool ok = decode_room(code, secret);
  sodium_memzero(secret, sizeof secret);
  return ok;
}

const std::vector<std::string>& RoomChannel::default_brokers() {
  // Public test brokers with MQTT over secure WebSocket. No guarantees, hence several.
  static const std::vector<std::string> brokers = {
      "wss://broker.emqx.io:8084/mqtt",
      "wss://broker.hivemq.com:8884/mqtt",
      "wss://test.mosquitto.org:8081/mqtt",
  };
  return brokers;
}

bool RoomChannel::open(const std::string& code, const std::vector<std::string>& brokers,
                       MessageFn on_message, LogFn log, std::function<void()> on_ready) {
  close();
  uint8_t secret[kRoomBytes];
  if (!decode_room(code, secret)) return false;

  uint8_t topic_hash[16];
  crypto_generichash(topic_hash, sizeof topic_hash, reinterpret_cast<const uint8_t*>(kTopicContext),
                     sizeof kTopicContext - 1, secret, sizeof secret);
  crypto_generichash(key_, sizeof key_, reinterpret_cast<const uint8_t*>(kKeyContext),
                     sizeof kKeyContext - 1, secret, sizeof secret);
  sodium_memzero(secret, sizeof secret);
  char hex[sizeof topic_hash * 2 + 1];
  sodium_bin2hex(hex, sizeof hex, topic_hash, sizeof topic_hash);
  topic_ = std::string("streamline/v1/") + hex;

  self_id_ = random_hex(8);
  on_message_ = std::move(on_message);
  log_ = std::move(log);
  on_ready_ = std::move(on_ready);
  ready_ = false;
  failed_ = 0;

  // Last will: if we vanish without a clean disconnect, the broker tells the room.
  json::Message bye;
  bye.set("type", "bye");
  std::string will = seal(envelope(bye));

  for (const auto& url : brokers) {
    auto client = std::make_unique<MqttClient>();
    MqttClient* c = client.get();
    std::string host = broker_host(url);
    c->connect(url, "sl" + random_hex(8), topic_, will,
               {[this, c, host] {
                  c->subscribe(topic_);
                  if (log_) log_("Sinalização: conectado a " + host);
                  if (!ready_) {
                    ready_ = true;
                    if (on_ready_) on_ready_();
                  }
                },
                [this](const std::string& topic, const std::string& payload) {
                  on_publish(topic, payload);
                },
                [this, host, total = brokers.size()](const std::string& error) {
                  if (log_) log_("Sinalização: " + host + " indisponível (" + error + ")");
                  if (++failed_ == static_cast<int>(total) && !ready_ && log_) {
                    log_("Nenhum broker MQTT disponível. Verifique a conexão com a internet.");
                  }
                }});
    clients_.push_back(std::move(client));
  }
  return true;
}

void RoomChannel::close() {
  if (clients_.empty()) return;
  json::Message bye;
  bye.set("type", "bye");
  send(bye);
  clients_.clear();  // ~MqttClient sends DISCONNECT, so the will isn't published
  sodium_memzero(key_, sizeof key_);
  seen_ids_.clear();
  seen_order_.clear();
  ready_ = false;
}

std::string RoomChannel::envelope(const json::Message& msg) const {
  std::string body = msg.to_string();
  json::Message m(body.data(), body.size());
  m.set("v", int64_t{1}).set("id", random_hex(8)).set("from", self_id_).set("ts", now_ms());
  return m.to_string();
}

void RoomChannel::send(const json::Message& msg) {
  std::string payload = seal(envelope(msg));
  for (auto& c : clients_) {
    if (c->ready()) c->publish(topic_, payload);
  }
}

std::string RoomChannel::seal(const std::string& plaintext) const {
  // nonce || ciphertext+tag; the topic is bound as associated data.
  std::string out(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plaintext.size() +
                      crypto_aead_xchacha20poly1305_ietf_ABYTES,
                  '\0');
  auto* nonce = reinterpret_cast<uint8_t*>(out.data());
  randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  unsigned long long len = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(
      nonce + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &len,
      reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(),
      reinterpret_cast<const uint8_t*>(topic_.data()), topic_.size(), nullptr, nonce, key_);
  return out;
}

bool RoomChannel::open_sealed(const std::string& payload, std::string& plaintext) const {
  constexpr size_t kNonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  constexpr size_t kTag = crypto_aead_xchacha20poly1305_ietf_ABYTES;
  if (payload.size() < kNonce + kTag) return false;
  plaintext.resize(payload.size() - kNonce - kTag);
  unsigned long long len = 0;
  const auto* in = reinterpret_cast<const uint8_t*>(payload.data());
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          reinterpret_cast<uint8_t*>(plaintext.data()), &len, nullptr, in + kNonce,
          payload.size() - kNonce, reinterpret_cast<const uint8_t*>(topic_.data()), topic_.size(),
          in, key_) != 0) {
    return false;
  }
  plaintext.resize(len);
  return true;
}

bool RoomChannel::seen(const std::string& id) {
  if (!seen_ids_.insert(id).second) return true;
  seen_order_.push_back(id);
  if (seen_order_.size() > kSeenIds) {
    seen_ids_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  return false;
}

void RoomChannel::on_publish(const std::string& topic, const std::string& payload) {
  std::string plaintext;
  if (topic != topic_ || !open_sealed(payload, plaintext)) return;  // not ours / forged
  json::Message m(plaintext.data(), plaintext.size());
  std::string id = m.str("id");
  std::string from = m.str("from");
  std::string to = m.str("to");
  if (!m.valid() || m.num("v") != 1 || id.empty() || from.empty() || from == self_id_) return;
  if (!to.empty() && to != self_id_) return;
  // A last-will "bye" may be delivered long after it was sealed; anything else must be fresh.
  if (m.str("type") != "bye" && std::llabs(now_ms() - m.num("ts")) > kMaxAgeMs) return;
  if (seen(id)) return;  // same message via another broker, or a replay
  if (on_message_) on_message_(m);
}

}  // namespace streamline
