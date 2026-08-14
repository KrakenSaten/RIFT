#include <gtest/gtest.h>
#include "Packet.h"

using namespace mesh;

// path_len packs two things into one byte: bits 6-7 hold the hash size minus
// one, bits 0-5 the number of hops. Reading it as a plain hop count is correct
// only while the hash size is 1, which is why the bug it caused stayed hidden
// until the path_hash_mode setting was exposed in the UI.

TEST(PathLen, SingleByteHashesReadAsPlainCount) {
    // hash size 1 leaves the upper bits clear, so the byte *is* the hop count
    for (uint8_t hops = 0; hops <= 63; hops++) {
        EXPECT_EQ(hops, Packet::pathHashCount(hops));
        EXPECT_EQ(1, Packet::pathHashSize(hops));
    }
}

TEST(PathLen, DecodesEveryHashSizeAndHopCount) {
    for (uint8_t size = 1; size <= 4; size++) {
        for (uint8_t hops = 0; hops <= 63; hops++) {
            uint8_t encoded = (uint8_t) (((size - 1) << 6) | hops);
            EXPECT_EQ(hops, Packet::pathHashCount(encoded))
                << "size=" << (int) size << " hops=" << (int) hops;
            EXPECT_EQ(size, Packet::pathHashSize(encoded))
                << "size=" << (int) size << " hops=" << (int) hops;
        }
    }
}

TEST(PathLen, TheCaseThatShippedWrong) {
    // two hops at the 2-byte setting. Read raw, this displayed as 66 hops.
    uint8_t encoded = (uint8_t) ((1 << 6) | 2);
    EXPECT_EQ(66, encoded);
    EXPECT_EQ(2, Packet::pathHashCount(encoded));
    EXPECT_EQ(2, Packet::pathHashSize(encoded));
}

TEST(PathLen, RoundTripsThroughTheSetter) {
    for (uint8_t size = 1; size <= 4; size++) {
        for (uint8_t hops = 0; hops <= 63; hops += 7) {
            Packet p;
            p.setPathHashSizeAndCount(size, hops);
            EXPECT_EQ(hops, p.getPathHashCount());
            EXPECT_EQ(size, p.getPathHashSize());
            EXPECT_EQ(hops * size, p.getPathByteLen());
        }
    }
}

TEST(PathLen, InstanceAccessorsAgreeWithStatics) {
    for (int raw = 0; raw <= 255; raw++) {
        Packet p;
        p.path_len = (uint8_t) raw;
        EXPECT_EQ(Packet::pathHashCount((uint8_t) raw), p.getPathHashCount());
        EXPECT_EQ(Packet::pathHashSize((uint8_t) raw), p.getPathHashSize());
    }
}

// 0xFF is not a path length. companion_radio uses it as a sentinel on the UI
// side meaning "direct, not flooded", and it must never be decoded as 63 hops of
// 4-byte hashes - which is what the arithmetic alone would say.
TEST(PathLen, DirectSentinelIsNotAPath) {
    EXPECT_FALSE(Packet::isValidPathLen(0xFF));
}

// The googletest package here is built without gtest_main, so every test file
// provides its own entry point - the same four lines as the other suites.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
