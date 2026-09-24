#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <filesystem>
#include <vector>

#include "activities/network/NearbyStatsProtocol.h"
#include "test/UniqueTempDirectory.h"
using namespace nearby_stats;
namespace {
struct Packet {
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 6> source;
};
struct Peer {
  Session session;
  std::array<uint8_t, 6> mac;
  std::vector<uint8_t> summary;
  std::deque<Packet>* wire;
  std::vector<Packet> sent;
  bool legacy = false, legacySaved = false, legacyAcked = false, drop = false;
  int ackBeforePublish = 0;
  std::string path;
  Peer(uint8_t id, std::deque<Packet>& q, bool old = false, uint8_t version = 3)
      : mac{id, id, id, id, id, id}, wire(&q), legacy(old) {
    path = "/.crosspoint/peer" + std::to_string(id) + ".bin";
    summary.resize(old ? (version == 1 ? 13 : version == 2 ? 17 : 159) : 223);
    summary[0] = old ? version : 4;
    summary[5] = 60;
    if (!old) {
      memcpy(summary.data() + 159, "und", 4);
      memcpy(summary.data() + 167, "mul", 4);
      summary[163] = 60;
    }
    session.start(0);
  }
  bool send(PacketType type) {
    const uint8_t* payload = nullptr;
    uint8_t size = 0;
    if (type == PacketType::STATS) {
      payload = summary.data();
      size = summary.size();
    }
    if (type == PacketType::NAME) {
      payload = reinterpret_cast<const uint8_t*>("Reader");
      size = 6;
    }
    Packet p;
    p.source = mac;
    p.bytes.resize(237);
    p.bytes.resize(encode(type, mac.data(), legacy ? 0 : 4, payload, size, p.bytes.data()));
    if (type == PacketType::ACK && !Storage.exists(path.c_str())) ++ackBeforePublish;
    sent.push_back(p);
    if (!drop) wire->push_back(p);
    return true;
  }
  Session::Callbacks callbacks() {
    return {this, [](void* p, PacketType t) { return static_cast<Peer*>(p)->send(t); },
            [](void* p, const Event& e) {
              auto& peer = *static_cast<Peer*>(p);
              return publishSummary(peer.path.c_str(), e.stats.data(), e.statsSize);
            }};
  }
  // Exact old v1-v3 validation expression, frozen from pre-v4 activity.
  static bool oldValid(const uint8_t* p, uint8_t n) {
    return (n == 13 && p[0] == 1) || (n == 17 && p[0] == 2) || (n == 159 && p[0] == 3);
  }
  void receive(const Packet& p) {
    Event event;
    if (!decode(p.source.data(), p.bytes.data(), p.bytes.size(), event)) return;
    if (legacy) {
      if (event.type == PacketType::HELLO) send(PacketType::STATS);
      if (event.type == PacketType::STATS && oldValid(event.stats.data(), event.statsSize)) {
        legacySaved = publishSummary(path.c_str(), event.stats.data(), event.statsSize);
        if (legacySaved) send(PacketType::ACK);
      }
      if (event.type == PacketType::ACK) legacyAcked = true;
    } else
      session.receive(event, 10, callbacks());
  }
};
void pump(Peer& a, Peer& b, std::deque<Packet>& q) {
  int budget = 100;
  while (!q.empty() && --budget) {
    auto p = q.front();
    q.pop_front();
    if (p.source == a.mac)
      b.receive(p);
    else
      a.receive(p);
  }
  EXPECT_GT(budget, 0);
}
class NearbyProtocolTest : public ::testing::Test {
  void SetUp() override {
    storage_test::root = uniqueTempDirectory("crossink-nearby-stats-fixture").string();
    std::filesystem::create_directories(storage_test::root + "/.crosspoint");
    storage_test::failWrite = false;
    storage_test::failSync = false;
    storage_test::failRename = false;
    storage_test::failWriteCall = storage_test::failSyncCall = storage_test::failCloseCall =
        storage_test::failRenameCall = storage_test::failRenameFromCall = 0;
  }
  void TearDown() override {
    EXPECT_EQ(storage_test::openFiles, 0);
    std::filesystem::remove_all(storage_test::root);
  }
};
}  // namespace
TEST_F(NearbyProtocolTest, UpdatedPeersExchange223ByteSummariesAndDurableAcks) {
  std::deque<Packet> q;
  Peer a(1, q), b(2, q);
  a.send(PacketType::HELLO);
  b.send(PacketType::HELLO);
  pump(a, b, q);
  EXPECT_EQ(a.session.state, Session::State::Synced);
  EXPECT_EQ(b.session.state, Session::State::Synced);
  EXPECT_EQ(a.ackBeforePublish + b.ackBeforePublish, 0);
  for (auto* p : {&a, &b}) {
    EXPECT_EQ(std::filesystem::file_size(storage_test::mapped(p->path.c_str())), 223u);
    for (const auto& packet : p->sent) {
      EXPECT_EQ(packet.bytes[7], 4);
      if (packet.bytes[5] == 2) {
        EXPECT_EQ(packet.bytes.size(), 237u);
        EXPECT_FALSE(Peer::oldValid(packet.bytes.data() + 14, 223));
      }
    }
  }
}
TEST_F(NearbyProtocolTest, UpdatedImportsEachLegacyVersionButNeverSendsV4OrClaimsSuccess) {
  for (uint8_t v : {1, 2, 3}) {
    std::deque<Packet> q;
    Peer a(1, q), b(2, q, true, v);
    a.send(PacketType::HELLO);
    b.send(PacketType::HELLO);
    pump(a, b, q);
    EXPECT_EQ(a.session.state, Session::State::VersionMismatch);
    EXPECT_TRUE(a.session.saved);
    EXPECT_FALSE(a.session.sent);
    EXPECT_FALSE(b.legacySaved);
    EXPECT_TRUE(b.legacyAcked);
    for (const auto& packet : a.sent) EXPECT_NE(packet.bytes[5], 2);
    EXPECT_EQ(a.ackBeforePublish, 0);
  }
}
TEST_F(NearbyProtocolTest, ExactLegacyValidatorRejectsCapturedV4WithoutAck) {
  std::deque<Packet> q;
  Peer updated(1, q), old(2, q, true);
  updated.send(PacketType::STATS);
  old.receive(q.front());
  EXPECT_FALSE(old.legacySaved);
  EXPECT_TRUE(old.sent.empty());
}
TEST_F(NearbyProtocolTest, AckBeforeSendAndSpoofedIdentityCannotAcknowledge) {
  std::deque<Packet> q;
  Peer a(1, q), b(2, q);
  b.send(PacketType::ACK);
  a.receive(q.back());
  EXPECT_FALSE(a.session.acked);
  EXPECT_FALSE(a.session.peerSeen);
  b.send(PacketType::HELLO);
  a.receive(q.back());
  ASSERT_TRUE(a.session.sent);
  b.send(PacketType::ACK);
  auto forged = q.back();
  forged.source.fill(3);
  a.receive(forged);
  EXPECT_FALSE(a.session.acked);
  forged = q.back();
  forged.bytes[8] = 3;
  a.receive(forged);
  EXPECT_FALSE(a.session.acked);
}
TEST_F(NearbyProtocolTest, InconsistentCapabilitiesAndMalformedEnvelopeNeverComplete) {
  std::deque<Packet> q;
  Peer a(1, q), b(2, q);
  b.send(PacketType::HELLO);
  a.receive(q.back());
  auto p = q.front();
  p.bytes[7] = 3;
  a.receive(p);
  EXPECT_EQ(a.session.state, Session::State::VersionMismatch);
  EXPECT_FALSE(a.session.acked);
  for (auto corruption : {0, 1, 2}) {
    Event e;
    p = q.front();
    if (corruption == 0) p.bytes.resize(7);
    if (corruption == 1) p.bytes[4] = 2;
    if (corruption == 2) p.bytes[6] = 1;
    EXPECT_FALSE(decode(p.source.data(), p.bytes.data(), p.bytes.size(), e));
  }
}
TEST_F(NearbyProtocolTest, StorageStageFailuresNeverAckOrComplete) {
  for (int failure = 0; failure < 4; ++failure) {
    std::deque<Packet> q;
    Peer a(1, q), b(2, q);
    b.send(PacketType::STATS);
    if (failure == 0) storage_test::failWriteCall = storage_test::writeCalls + 1;
    if (failure == 1) storage_test::failSyncCall = storage_test::syncCalls + 1;
    if (failure == 2) storage_test::failCloseCall = storage_test::closeCalls + 1;
    if (failure == 3) storage_test::failRenameCall = storage_test::renameCalls + 1;
    a.receive(q.front());
    EXPECT_EQ(a.session.state, Session::State::StorageError) << failure;
    EXPECT_FALSE(a.session.saved);
    EXPECT_FALSE(a.session.acked);
    for (const auto& packet : a.sent) EXPECT_NE(packet.bytes[5], 3);
  }
}
TEST_F(NearbyProtocolTest, LostSummaryTimesOutAndOverflowCannotBecomeSynced) {
  std::deque<Packet> q;
  Peer a(1, q), old(2, q, true);
  old.send(PacketType::HELLO);
  a.receive(q.front());
  a.session.tick(12001, a.callbacks());
  EXPECT_EQ(a.session.state, Session::State::Timeout);
  EXPECT_FALSE(a.session.acked);
  a.session.start(0);
  a.session.overflow();
  Peer b(2, q);
  b.send(PacketType::STATS);
  a.receive(q.back());
  EXPECT_EQ(a.session.state, Session::State::Overflow);
  EXPECT_FALSE(a.session.saved);
}
TEST_F(NearbyProtocolTest, RetransmissionAfterPacketLossCompletes) {
  std::deque<Packet> q;
  Peer a(1, q), b(2, q);
  a.drop = true;
  b.send(PacketType::HELLO);
  a.receive(q.front());
  q.clear();
  a.drop = false;
  a.session.tick(761, a.callbacks());
  pump(a, b, q);
  EXPECT_EQ(a.session.state, Session::State::Synced);
  EXPECT_EQ(b.session.state, Session::State::Synced);
}
TEST_F(NearbyProtocolTest, SyncedPeerStillAcknowledgesLostFinalAckRetry) {
  std::deque<Packet> q;
  Peer a(1, q), b(2, q);
  a.send(PacketType::HELLO);
  b.send(PacketType::HELLO);
  pump(a, b, q);
  ASSERT_EQ(a.session.state, Session::State::Synced);
  // The other peer retries its supported STATS after a lost final ACK.
  b.session.state = Session::State::Syncing;
  b.session.acked = false;
  b.send(PacketType::STATS);
  pump(a, b, q);
  EXPECT_EQ(b.session.state, Session::State::Synced);
  EXPECT_EQ(a.ackBeforePublish, 0);
}
