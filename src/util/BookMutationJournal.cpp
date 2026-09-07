#include "BookMutationJournal.h"

#include <cstring>
namespace bookmutation {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
void put16(uint8_t* p, uint16_t n) {
  p[0] = n;
  p[1] = n >> 8;
}
void put32(uint8_t* p, uint32_t n) {
  put16(p, n);
  put16(p + 2, n >> 16);
}
void put64(uint8_t* p, uint64_t n) {
  put32(p, n);
  put32(p + 4, n >> 32);
}
uint32_t crc32(const void* data, size_t n, uint32_t previous) {
  uint32_t crc = ~previous;
  const auto* p = static_cast<const uint8_t*>(data);
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
void encodeHeader(const Header& h, uint8_t* p) {
  memset(p, 0, HeaderBytes);
  memcpy(p, "CMJ1", 4);
  put16(p + 4, 1);
  put16(p + 6, HeaderBytes);
  p[8] = uint8_t(h.operation);
  put16(p + 10, h.roots);
  put16(p + 12, h.books);
  put16(p + 14, h.files);
  put64(p + 16, h.id);
  put32(p + 24, h.payloadBytes);
  put32(p + 28, h.payloadCrc);
  put32(p + 44, crc32(p, 44));
}
bool decodeHeader(const uint8_t* p, Header& h) {
  if (memcmp(p, "CMJ1", 4) || u16(p + 4) != 1 || u16(p + 6) != HeaderBytes || p[9] || (p[8] != 1 && p[8] != 2) ||
      u32(p + 44) != crc32(p, 44))
    return false;
  for (size_t i = 32; i < 44; ++i)
    if (p[i]) return false;
  Header value;
  value.operation = Operation(p[8]);
  value.roots = u16(p + 10);
  value.books = u16(p + 12);
  value.files = u16(p + 14);
  value.id = u64(p + 16);
  value.payloadBytes = u32(p + 24);
  value.payloadCrc = u32(p + 28);
  if (!value.id || !value.roots || value.roots > 64 || value.books > 64 || value.files > 1024 ||
      value.files > value.books * 16 || value.payloadBytes > MaxPayloadBytes ||
      (value.operation == Operation::Move && (value.roots != 1 || !value.books)) ||
      (value.operation == Operation::Delete && value.files))
    return false;
  h = value;
  return true;
}
void encodePhase(const Header& h, uint32_t sequence, Phase phase, uint16_t index, uint8_t* p) {
  memset(p, 0, PhaseBytes);
  memcpy(p, "CMP1", 4);
  put32(p + 4, sequence);
  p[8] = uint8_t(phase);
  put16(p + 10, index);
  put64(p + 12, h.id);
  put32(p + 20, crc32(p, 20));
}
void encodeOutcome(const Header& h, uint32_t sequence, const Outcome& o, uint8_t* p) {
  memset(p, 0, OutcomeBytes);
  memcpy(p, "CMO1", 4);
  put32(p + 4, sequence);
  put64(p + 8, h.id);
  put64(p + 16, o.absent);
  put64(p + 24, o.recentBytes);
  put32(p + 32, o.recentCrc);
  put64(p + 36, o.stateBytes);
  put32(p + 44, o.stateCrc);
  put32(p + 48, o.partial ? 1 : 0);
  put32(p + 60, crc32(p, 60));
}
bool acceptAppend(const Header& h, const uint8_t* p, size_t size, Replay& state) {
  if ((size != PhaseBytes && size != OutcomeBytes) || u32(p + size - 4) != crc32(p, size - 4) ||
      state.sequence == UINT32_MAX || u32(p + 4) != state.sequence + 1 || state.done)
    return false;
  Replay r = state;
  const bool move = h.operation == Operation::Move;
  if (size == OutcomeBytes) {
    if (memcmp(p, "CMO1", 4) || move || !r.deleteStarted || r.hasOutcome || u64(p + 8) != h.id || u32(p + 48) > 1)
      return false;
    for (size_t i = 52; i < 60; ++i)
      if (p[i]) return false;
    r.outcome.absent = u64(p + 16);
    if ((h.books < 64 && (r.outcome.absent >> h.books)) || (!r.contentReturned && !(u32(p + 48) & 1))) return false;
    r.outcome.recentBytes = u64(p + 24);
    r.outcome.recentCrc = u32(p + 32);
    r.outcome.stateBytes = u64(p + 36);
    r.outcome.stateCrc = u32(p + 44);
    r.outcome.partial = u32(p + 48) != 0;
    r.hasOutcome = true;
  } else {
    if (memcmp(p, "CMP1", 4) || p[9] || u64(p + 12) != h.id) return false;
    const auto phase = Phase(p[8]);
    const uint16_t index = u16(p + 10);
    const bool indexed =
        phase == Phase::FilePublished || phase == Phase::SourceRemoved || phase == Phase::DeleteMetadataDone;
    if (!indexed && index != UINT16_MAX) return false;
    switch (phase) {
      case Phase::Prepared:
        if (r.sequence || r.prepared) return false;
        r.prepared = true;
        break;
      case Phase::Moved:
        if (!move || !r.prepared || r.moved) return false;
        r.moved = true;
        break;
      case Phase::FilePublished:
        if (!move || !r.moved || index != r.published || index >= h.files || r.recent) return false;
        ++r.published;
        break;
      case Phase::RecentPublished:
        if (r.recent ||
            (move ? (!r.moved || r.published != h.files) : (!r.hasOutcome || r.cleaned != r.outcome.absent)))
          return false;
        r.recent = true;
        break;
      case Phase::StatePublished:
        if (!r.recent || r.state) return false;
        r.state = true;
        break;
      case Phase::ReferencesDone:
        if (!r.state || r.references) return false;
        r.references = true;
        break;
      case Phase::SourceRemoved:
        if (!move || !r.references || index != r.removed || index >= h.files || r.tokenRemoved) return false;
        ++r.removed;
        break;
      case Phase::TokenRemoved:
        if (!move || !r.references || r.removed != h.files || r.tokenRemoved) return false;
        r.tokenRemoved = true;
        break;
      case Phase::Aborted:
        if (r.moved || r.deleteStarted || r.references) return false;
        r.aborted = r.done = true;
        break;
      case Phase::Done:
        if (!r.references || (move && !r.tokenRemoved)) return false;
        r.done = true;
        break;
      case Phase::DeleteStarted:
        if (move || !r.prepared || r.deleteStarted) return false;
        r.deleteStarted = true;
        break;
      case Phase::ContentSucceeded:
      case Phase::ContentFailed:
        if (move || !r.deleteStarted || r.contentReturned || r.hasOutcome) return false;
        r.contentReturned = true;
        break;
      case Phase::DeleteMetadataDone:
        if (move || !r.hasOutcome || r.recent || index >= h.books || !(r.outcome.absent & (uint64_t(1) << index)) ||
            (r.cleaned & (uint64_t(1) << index)))
          return false;
        r.cleaned |= uint64_t(1) << index;
        break;
      default:
        return false;
    }
  }
  ++r.sequence;
  state = r;
  return true;
}
}  // namespace bookmutation
