#include <gtest/gtest.h>
#include <base64.hpp>

// The channel-key decoder wrote into uint8_t psk[32] and checked the length
// afterwards. decode_base64() is never told how big the output is, so it cannot
// bound its own writes - checking the result is checking after the damage. The
// key entry field is char buf[68], and 67 base64 characters decode to 50 bytes.
//
// The fix establishes the size first with decode_base64_length() and rejects
// anything that is not exactly a 128- or 256-bit key. These tests pin the
// property that made the overflow possible: no input the field can hold, once
// accepted, decodes to more than 32 bytes.

// what MyMesh::addGroupChannelFromBase64() accepts
static bool accepted(unsigned int decoded_len) {
    return decoded_len == 16 || decoded_len == 32;
}

static unsigned int decoded_size(const std::string& in) {
    return decode_base64_length((const unsigned char*) in.c_str(),
                                (unsigned int) in.size());
}

TEST(Base64Key, A128BitKeyDecodesTo16) {
    // 22 base64 characters plus padding is 16 bytes
    std::string key(22, 'A');
    EXPECT_EQ(16u, decoded_size(key));
    EXPECT_TRUE(accepted(decoded_size(key)));
}

TEST(Base64Key, A256BitKeyDecodesTo32) {
    std::string key(43, 'A');
    EXPECT_EQ(32u, decoded_size(key));
    EXPECT_TRUE(accepted(decoded_size(key)));
}

// The case that overflowed: the entry field holds 67 characters, and the old
// code decoded all of them into a 32-byte buffer before looking at the length.
TEST(Base64Key, TheInputThatOverflowed) {
    std::string key(67, 'A');
    EXPECT_EQ(50u, decoded_size(key));
    EXPECT_FALSE(accepted(decoded_size(key)));
}

// The property that matters, over every length the field can physically hold.
TEST(Base64Key, NothingAcceptedExceedsTheBuffer) {
    for (unsigned int len = 0; len <= 67; len++) {
        std::string key(len, 'A');
        unsigned int n = decoded_size(key);
        if (accepted(n)) {
            EXPECT_LE(n, 32u) << "accepted a " << len << "-character key decoding to " << n;
        }
    }
}

TEST(Base64Key, OnlyTwoLengthsAreEverAccepted) {
    int accepted_count = 0;
    for (unsigned int len = 0; len <= 67; len++) {
        std::string key(len, 'A');
        if (accepted(decoded_size(key))) accepted_count++;
    }
    // 22 chars -> 16 bytes and 43 chars -> 32 bytes, and nothing else
    EXPECT_EQ(2, accepted_count);
}

TEST(Base64Key, EmptyInputIsRejected) {
    EXPECT_EQ(0u, decoded_size(""));
    EXPECT_FALSE(accepted(decoded_size("")));
}

TEST(Base64Key, DecodingStopsAtTheFirstInvalidCharacter) {
    // padding and anything else non-alphabet end the run, so a key with junk in
    // the middle cannot silently decode to a valid length from its tail
    EXPECT_EQ(16u, decoded_size(std::string(22, 'A') + "=="));
    EXPECT_EQ(0u,  decoded_size("!!!!!!!!"));
    EXPECT_LT(decoded_size(std::string(10, 'A') + "!" + std::string(40, 'A')), 16u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
