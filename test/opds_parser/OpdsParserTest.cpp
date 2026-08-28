#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include "OpdsParser.h"

TEST(OpdsParser, XtcOnlyAcquisitionProducesXtcBook) {
  constexpr char feed[] = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>Catalog</title>
  <entry>
    <title>XTC chapter</title>
    <id>urn:crossink:book:42</id>
    <link rel="http://opds-spec.org/acquisition"
          type="application/vnd.xteink.xtc"
          href="/opds/crossink/book/42/xtc" />
  </entry>
</feed>)xml";

  std::array<OpdsEntry, 1> entries{};
  OpdsParser parser(entries.data(), entries.size());

  ASSERT_TRUE(parser.parse(feed, sizeof(feed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 1U);

  const OpdsEntry* entry = parser.getEntry(0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->type, OpdsEntryType::BOOK);
  EXPECT_EQ(entry->format, OpdsAcquisitionFormat::XTC);
  EXPECT_EQ(entry->title, "XTC chapter");
  EXPECT_EQ(entry->href, "/opds/crossink/book/42/xtc");
}

TEST(OpdsParser, EpubOnlyAcquisitionProducesEpubBook) {
  constexpr char feed[] = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title>Catalog</title>
  <entry>
    <title>EPUB book</title>
    <id>urn:crossink:book:7</id>
    <link rel="http://opds-spec.org/acquisition"
          type="application/epub+zip"
          href="/books/7.epub" />
  </entry>
</feed>)xml";

  std::array<OpdsEntry, 1> entries{};
  OpdsParser parser(entries.data(), entries.size());

  ASSERT_TRUE(parser.parse(feed, sizeof(feed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 1U);

  const OpdsEntry* entry = parser.getEntry(0);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->type, OpdsEntryType::BOOK);
  EXPECT_EQ(entry->format, OpdsAcquisitionFormat::EPUB);
  EXPECT_EQ(entry->title, "EPUB book");
  EXPECT_EQ(entry->href, "/books/7.epub");
}
