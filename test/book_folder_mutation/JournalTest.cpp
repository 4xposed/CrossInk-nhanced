#include <gtest/gtest.h>

#include "BookMutationJournal.h"
using namespace bookmutation;
TEST(MutationJournal, EndianCrcAndHeaderRoundtrip) {
  EXPECT_EQ(crc32("123456789", 9), 0xcbf43926U);
  Header in;
  in.roots = 1;
  in.books = 1;
  in.files = 3;
  in.id = 0x0102030405060708ULL;
  uint8_t bytes[HeaderBytes];
  encodeHeader(in, bytes);
  Header out;
  ASSERT_TRUE(decodeHeader(bytes, out));
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.files, 3);
}
TEST(MutationJournal, ReferencesCannotPrecedePublication) {
  Header h;
  h.roots = 1;
  h.books = 1;
  h.files = 1;
  h.id = 7;
  Replay r;
  uint8_t p[PhaseBytes];
  encodePhase(h, 1, Phase::Prepared, 0xffff, p);
  ASSERT_TRUE(acceptAppend(h, p, sizeof(p), r));
  encodePhase(h, 2, Phase::Moved, 0xffff, p);
  ASSERT_TRUE(acceptAppend(h, p, sizeof(p), r));
  encodePhase(h, 3, Phase::RecentPublished, 0xffff, p);
  EXPECT_FALSE(acceptAppend(h, p, sizeof(p), r));
}
