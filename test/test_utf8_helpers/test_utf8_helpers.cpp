#include <gtest/gtest.h>

#include <helpers/UTF8Helpers.h>

TEST(UTF8Helpers, KeepsCompleteNameWithinLimit) {
  const char* name = "Example RPT 🔋🇵🇱";

  EXPECT_EQ(24u, mesh::validUtf8PrefixLength(name, 24));
}

TEST(UTF8Helpers, StopsBeforeCodePointCrossingLimit) {
  const char* name = "Example RPT 🔋🇵🇱";

  EXPECT_EQ(20u, mesh::validUtf8PrefixLength(name, 23));
}

TEST(UTF8Helpers, RejectsMalformedAndTruncatedSequences) {
  const char overlong[] = {'A', static_cast<char>(0xC0), static_cast<char>(0xAF), 0};
  const char surrogate[] = {'A', static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80), 0};
  const char out_of_range[] = {'A', static_cast<char>(0xF4), static_cast<char>(0x90), static_cast<char>(0x80), static_cast<char>(0x80), 0};
  const char truncated[] = {'A', static_cast<char>(0xF0), static_cast<char>(0x9F), 0};

  EXPECT_EQ(1u, mesh::validUtf8PrefixLength(overlong, sizeof(overlong)));
  EXPECT_EQ(1u, mesh::validUtf8PrefixLength(surrogate, sizeof(surrogate)));
  EXPECT_EQ(1u, mesh::validUtf8PrefixLength(out_of_range, sizeof(out_of_range)));
  EXPECT_EQ(1u, mesh::validUtf8PrefixLength(truncated, sizeof(truncated)));
}

TEST(UTF8Helpers, RejectsUnexpectedContinuationByte) {
  const char invalid[] = {'A', static_cast<char>(0x80), 'B', 0};

  EXPECT_EQ(1u, mesh::validUtf8PrefixLength(invalid, sizeof(invalid)));
}

// ------------------------------------------------- companion frame boundary cases
//
// MyMesh forwards received text into a companion frame and truncates it to fit
// MAX_FRAME_SIZE. That cut used to be byte-wise, with upstream's own "TODO: UTF-8 ??"
// beside it. These are the boundaries the reviewer asked about: a limit landing inside
// a character must yield the characters before it and nothing else.

TEST(UTF8Frame, ANordicLetterIsNeverSplit) {
  // "hallo " is 6 bytes, then ae oe aa at two bytes each: 6,8,10,12
  const char* text = "hallo \xC3\xA6\xC3\xB8\xC3\xA5";
  ASSERT_EQ(12u, mesh::validUtf8PrefixLength(text, 12));   // all of it

  EXPECT_EQ(6u,  mesh::validUtf8PrefixLength(text, 7));    // mid ae   -> drop it
  EXPECT_EQ(8u,  mesh::validUtf8PrefixLength(text, 8));    // after ae -> keep it
  EXPECT_EQ(8u,  mesh::validUtf8PrefixLength(text, 9));    // mid oe   -> drop it
  EXPECT_EQ(10u, mesh::validUtf8PrefixLength(text, 10));
  EXPECT_EQ(10u, mesh::validUtf8PrefixLength(text, 11));   // mid aa   -> drop it
}

TEST(UTF8Frame, AFourByteEmojiIsNeverSplit) {
  const char* text = "ok \xF0\x9F\x94\x8B";                 // "ok " then a 4-byte emoji
  ASSERT_EQ(7u, mesh::validUtf8PrefixLength(text, 7));

  EXPECT_EQ(3u, mesh::validUtf8PrefixLength(text, 4));      // one byte in
  EXPECT_EQ(3u, mesh::validUtf8PrefixLength(text, 5));      // two bytes in
  EXPECT_EQ(3u, mesh::validUtf8PrefixLength(text, 6));      // three bytes in
}

// The real shape of the bug: 179 bytes of text - which MAX_PACKET_PAYLOAD permits even
// though MAX_TEXT_LEN does not - cut to fit a frame, with a two-byte character
// straddling the limit.
TEST(UTF8Frame, ALongRemoteMessageCutToFitKeepsWholeCharacters) {
  char text[200];
  int i = 0;
  while (i < 159) text[i++] = 'x';
  text[i++] = (char) 0xC3;                                  // ae, straddling 160
  text[i++] = (char) 0xA6;
  text[i] = 0;

  EXPECT_EQ(159u, mesh::validUtf8PrefixLength(text, 160));   // not 160
  EXPECT_EQ(161u, mesh::validUtf8PrefixLength(text, 161));   // room for both bytes
}

TEST(UTF8Frame, ALimitOfZeroYieldsNothing) {
  EXPECT_EQ(0u, mesh::validUtf8PrefixLength("\xC3\xA6", 0));
  EXPECT_EQ(0u, mesh::validUtf8PrefixLength("\xC3\xA6", 1));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
