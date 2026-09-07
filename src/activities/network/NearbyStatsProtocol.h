#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "activities/reader/ReadingLanguageStats.h"

namespace nearby_stats {
constexpr size_t kSummaryBytes = 223, kHeaderBytes = 14;
enum class PacketType : uint8_t { HELLO = 1, STATS = 2, ACK = 3, NAME = 4, INVALID_STATS = 255 };
struct Event {
  PacketType type = PacketType::HELLO;
  std::array<uint8_t, 6> sourceMac{}, deviceMac{};
  std::array<uint8_t, kSummaryBytes> stats{};
  std::array<char, 21> deviceName{};
  uint8_t statsSize = 0, capability = 0;
};
bool decode(const uint8_t* source, const uint8_t* bytes, size_t size, Event&);
size_t encode(PacketType, const uint8_t* device, uint8_t capability, const uint8_t* payload, uint8_t size,
              uint8_t* out);
bool publishSummary(const char* path, const uint8_t* bytes, uint8_t size);

// Application ACK means publishSummary succeeded; send callbacks only queue radio
// packets and must never mark a local summary as remotely imported.
class Session {
 public:
  enum class State : uint8_t { Discovering, Syncing, Synced, VersionMismatch, StorageError, Timeout, Overflow };
  struct Callbacks {
    void* context;
    bool (*send)(void*, PacketType);
    bool (*publish)(void*, const Event&);
  };
  void start(uint32_t now);
  void receive(const Event&, uint32_t now, const Callbacks&);
  void tick(uint32_t now, const Callbacks&);
  void overflow() {
    state = State::Overflow;
    acked = false;
  }
  bool terminal() const { return state >= State::Synced; }
  State state = State::Discovering;
  std::array<uint8_t, 6> sourceMac{}, deviceMac{};
  bool peerSeen = false, sent = false, acked = false, saved = false, incompatible = false;

 private:
  uint8_t nonzeroCapability = 0;
  uint32_t started = 0, lastSend = 0;
  void sendStats(uint32_t now, const Callbacks&);
};
}  // namespace nearby_stats
