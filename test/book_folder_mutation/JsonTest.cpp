#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "BookMutationJson.h"
using namespace bookmutation;
namespace {
PathEdit change(void*, PathField field, const char*, const char* value, char* out, size_t n) {
  if (field == PathField::Book && !strcmp(value, "/gone")) return PathEdit::Remove;
  if (!strcmp(value, "/old")) {
    snprintf(out, n, "/new");
    return PathEdit::Replace;
  }
  return PathEdit::Keep;
}
bool run(const std::string& text, std::string& result, JsonKind kind = JsonKind::Recent) {
  auto root = std::filesystem::temp_directory_path() / "crossink-mutation-json";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  mutation_test::reset(root.string());
  std::ofstream(root / "in") << text;
  FsFile in = Storage.open("/in"), out = Storage.open("/out", O_WRONLY | O_CREAT | O_TRUNC);
  JsonScratch scratch;
  bool ok = rewriteSharedJson(in, out, kind, change, nullptr, scratch);
  in.close();
  out.close();
  std::ifstream f(root / "out");
  result.assign(std::istreambuf_iterator<char>(f), {});
  f.close();
  std::filesystem::remove_all(root);
  return ok;
}
}  // namespace
TEST(MutationJson, RetainsUnknownLargeValuesAndChangesPathsRegardlessOfFieldOrder) {
  std::string out;
  std::string unknown(20000, 'z');
  ASSERT_TRUE(run("{\"future\":{\"long\":\"" + unknown +
                      "\"},\"books\":[{\"coverBmpPath\":\"/cover\",\"path\":\"/old\",\"extra\":[1,true,null]}]}",
                  out));
  EXPECT_NE(out.find(unknown), std::string::npos);
  EXPECT_NE(out.find("\"path\":\"/new\""), std::string::npos);
  EXPECT_NE(out.find("\"extra\":[1,true,null]"), std::string::npos);
}
TEST(MutationJson, RemovingMiddleRowPreservesValidSeparators) {
  std::string out;
  ASSERT_TRUE(run("{\"books\":[{\"path\":\"/keep\"},{\"path\":\"/gone\"},{\"path\":\"/old\"}]}", out));
  EXPECT_EQ(out, "{\"books\":[{\"path\":\"/keep\"},{\"path\":\"/new\"}]}");
}
TEST(MutationJson, RejectsTruncationAndOversizedRelevantPaths) {
  std::string out;
  EXPECT_FALSE(run("{\"books\":[{\"path\":\"/x\"}", out));
  EXPECT_FALSE(run("{\"books\":[{\"path\":\"/" + std::string(1024, 'x') + "\"}]}", out));
}
TEST(MutationJson, PreservesUnknownStateAndDecodesEscapedPaths) {
  std::string out;
  ASSERT_TRUE(run("{\"unknown\":{\"a\":8},\"openEpubPath\":\"\\u002fold\"}", out, JsonKind::State));
  EXPECT_EQ(out, "{\"unknown\":{\"a\":8},\"openEpubPath\":\"/new\"}");
}

TEST(MutationJson, RejectsTrailingCommaInRecentArray) {
  std::string out;
  EXPECT_FALSE(run("{\"books\":[{\"path\":\"/old\"}, ]}", out));
}

namespace bookmutation {
void serviceMutation() {}
}  // namespace bookmutation
