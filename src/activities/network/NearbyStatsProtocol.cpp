#include "NearbyStatsProtocol.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
namespace nearby_stats {
bool decode(const uint8_t* source, const uint8_t* bytes, size_t size, Event& event) {
  if (!source || !bytes || size < kHeaderBytes || memcmp(bytes, "CISS", 4) || bytes[4] != 1) return false;
  const auto type = static_cast<PacketType>(bytes[5]);
  if (type != PacketType::HELLO && type != PacketType::STATS && type != PacketType::ACK && type != PacketType::NAME)
    return false;
  event = {};
  event.type = type;
  event.statsSize = bytes[6];
  event.capability = bytes[7];
  memcpy(event.sourceMac.data(), source, 6);
  memcpy(event.deviceMac.data(), bytes + 8, 6);
  if (event.deviceMac == std::array<uint8_t, 6>{} || event.sourceMac == std::array<uint8_t, 6>{}) return false;
  if (type == PacketType::STATS) {
    if (size != kHeaderBytes + event.statsSize || event.statsSize > kSummaryBytes ||
        !validateGlobalReadingStatsSummary(bytes + kHeaderBytes, event.statsSize)) {
      event.type = PacketType::INVALID_STATS;
      event.statsSize = 0;
    } else
      memcpy(event.stats.data(), bytes + kHeaderBytes, event.statsSize);
  } else if (type == PacketType::NAME) {
    if (event.statsSize < 1 || event.statsSize > 20 || size != kHeaderBytes + event.statsSize) return false;
    memcpy(event.deviceName.data(), bytes + kHeaderBytes, event.statsSize);
  } else if (event.statsSize || size != kHeaderBytes)
    return false;
  return true;
}
size_t encode(PacketType type, const uint8_t* device, uint8_t capability, const uint8_t* payload, uint8_t size,
              uint8_t* out) {
  if (!device || !out || (size && !payload)) return 0;
  if (type == PacketType::STATS && !validateGlobalReadingStatsSummary(payload, size)) return 0;
  if (type == PacketType::NAME && (!size || size > 20)) return 0;
  if (type != PacketType::STATS && type != PacketType::NAME && size) return 0;
  memcpy(out, "CISS", 4);
  out[4] = 1;
  out[5] = static_cast<uint8_t>(type);
  out[6] = size;
  out[7] = capability;
  memcpy(out + 8, device, 6);
  if (size) memcpy(out + kHeaderBytes, payload, size);
  return kHeaderBytes + size;
}
bool publishSummary(const char* path, const uint8_t* bytes, uint8_t size) {
  if (!validateGlobalReadingStatsSummary(bytes, size)) return false;
  char tmp[96], backup[96];
  const int a = snprintf(tmp, sizeof(tmp), "%s.part", path), b = snprintf(backup, sizeof(backup), "%s.bak", path);
  if (a < 0 || b < 0 || static_cast<size_t>(a) >= sizeof(tmp) || static_cast<size_t>(b) >= sizeof(backup)) return false;
  if (Storage.exists(tmp) && !Storage.remove(tmp)) return false;
  HalFile file;
  if (!Storage.openFileForWrite("NSTATS", tmp, file)) return false;
  bool ok = file.write(bytes, size) == size;
  if (ok) {
    file.flush();
    ok = file.sync();
  }
  if (!file.close()) ok = false;
  if (!ok) {
    LOG_ERR("NSTATS", "Summary write/sync/close failed");
    Storage.remove(tmp);
    return false;
  }
  const bool hadOld = Storage.exists(path);
  if (hadOld && ((Storage.exists(backup) && !Storage.remove(backup)) || !Storage.rename(path, backup))) {
    Storage.remove(tmp);
    return false;
  }
  if (!Storage.rename(tmp, path)) {
    LOG_ERR("NSTATS", "Summary publication failed");
    if (hadOld && !Storage.rename(backup, path)) LOG_ERR("NSTATS", "Summary backup retained");
    Storage.remove(tmp);
    return false;
  }
  if (hadOld && !Storage.remove(backup)) LOG_ERR("NSTATS", "Summary published; cleanup failed");
  return true;
}
void Session::start(uint32_t now) {
  *this = {};
  started = now;
  lastSend = now;
}
void Session::sendStats(uint32_t now, const Callbacks& callbacks) {
  lastSend = now;
  if (peerSeen && !incompatible) {
    callbacks.send(callbacks.context, PacketType::NAME);
    // A subsequent radio queue failure must not erase a previous queued send.
    sent = callbacks.send(callbacks.context, PacketType::STATS) || sent;
  }
}
void Session::receive(const Event& event, uint32_t now, const Callbacks& callbacks) {
  if (terminal()) {
    // Our final ACK may have been lost after we reached SYNCED. A peer retry must
    // still receive a durable-import ACK, without restarting our exchange.
    if (state == State::Synced && event.type == PacketType::STATS && event.deviceMac == deviceMac &&
        event.sourceMac == sourceMac && (!event.capability || event.capability == nonzeroCapability) &&
        callbacks.publish(callbacks.context, event))
      callbacks.send(callbacks.context, PacketType::ACK);
    return;
  }
  if (event.type == PacketType::ACK && (!peerSeen || !sent)) return;
  if (peerSeen && (event.deviceMac != deviceMac || event.sourceMac != sourceMac)) return;
  if (!peerSeen) {
    peerSeen = true;
    deviceMac = event.deviceMac;
    sourceMac = event.sourceMac;
    state = State::Syncing;
  }
  if (event.capability) {
    if (nonzeroCapability && nonzeroCapability != event.capability) {
      acked = false;
      state = State::VersionMismatch;
      return;
    }
    nonzeroCapability = event.capability;
  }
  const uint8_t maximum = nonzeroCapability ? nonzeroCapability : 3;
  incompatible = maximum < 4;
  if (event.type == PacketType::INVALID_STATS || (event.type == PacketType::STATS && event.stats[0] > maximum)) {
    acked = false;
    state = State::VersionMismatch;
    return;
  }
  if (event.type == PacketType::ACK) {
    if (!incompatible) acked = true;
  }
  if (event.type == PacketType::HELLO) {
    callbacks.send(callbacks.context, PacketType::NAME);
    sendStats(now, callbacks);
  }
  if (event.type == PacketType::STATS) {
    // Duplicate STATS retries are idempotent summary replacement. ACK only follows
    // successful publication; no packet delivery assumption enters this branch.
    if (!callbacks.publish(callbacks.context, event)) {
      acked = false;
      state = State::StorageError;
      return;
    }
    saved = true;
    callbacks.send(callbacks.context, PacketType::ACK);
    if (incompatible) {
      state = State::VersionMismatch;
      return;
    }
    if (!sent) sendStats(now, callbacks);
  }
  if (saved && acked && !incompatible) state = State::Synced;
}
void Session::tick(uint32_t now, const Callbacks& callbacks) {
  if (terminal()) return;
  if (now - started > 12000) {
    acked = false;
    state = State::Timeout;
    return;
  }
  if (peerSeen && !acked && now - lastSend >= 750) sendStats(now, callbacks);
}
}  // namespace nearby_stats
