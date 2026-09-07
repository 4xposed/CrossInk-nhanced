#pragma once
#include <cstddef>
#include <cstdint>

namespace bookmutation {
constexpr size_t HeaderBytes = 48, PhaseBytes = 24, OutcomeBytes = 64;
constexpr uint32_t MaxPayloadBytes = 2334216;
enum class Operation : uint8_t { Move = 1, Delete = 2 };
enum class Phase : uint8_t {
  Prepared = 1,
  Moved,
  FilePublished,
  RecentPublished,
  StatePublished,
  ReferencesDone,
  SourceRemoved,
  TokenRemoved,
  Done,
  DeleteStarted,
  ContentSucceeded,
  ContentFailed,
  DeleteMetadataDone,
  Aborted
};
struct Header {
  Operation operation = Operation::Move;
  uint16_t roots = 0, books = 0, files = 0;
  uint64_t id = 0;
  uint32_t payloadBytes = 0, payloadCrc = 0;
};
struct Outcome {
  uint64_t absent = 0, recentBytes = 0, stateBytes = 0;
  uint32_t recentCrc = 0, stateCrc = 0;
  bool partial = false;
};
struct Replay {
  uint32_t sequence = 0;
  uint16_t published = 0, removed = 0;
  uint64_t cleaned = 0;
  bool prepared = false, moved = false, recent = false, state = false;
  bool references = false, tokenRemoved = false, done = false, aborted = false;
  bool deleteStarted = false, contentReturned = false, hasOutcome = false;
  Outcome outcome;
};
uint16_t u16(const uint8_t*);
uint32_t u32(const uint8_t*);
uint64_t u64(const uint8_t*);
void put16(uint8_t*, uint16_t);
void put32(uint8_t*, uint32_t);
void put64(uint8_t*, uint64_t);
uint32_t crc32(const void*, size_t, uint32_t previous = 0);
void encodeHeader(const Header&, uint8_t*);
bool decodeHeader(const uint8_t*, Header&);
void encodePhase(const Header&, uint32_t sequence, Phase, uint16_t index, uint8_t*);
void encodeOutcome(const Header&, uint32_t sequence, const Outcome&, uint8_t*);
// CRC/framing is checked before this strict operation-specific state machine.
bool acceptAppend(const Header&, const uint8_t*, size_t, Replay&);
}  // namespace bookmutation
