#include <gtest/gtest.h>

// Included by relative path on purpose: this keeps the native test environments
// exactly as upstream has them. The header is standalone - stdint and string.h
// only - so there is nothing to link.
#include "../../examples/companion_radio/ui-rift/RiftLogic.h"

// ---------------------------------------------------------------- hash resolution

static const uint8_t ALPHA[] = { 0xA1, 0xB2, 0xC3, 0xD4 };
static const uint8_t BRAVO[] = { 0xA1, 0xB2, 0x99, 0x00 };  // shares one byte with ALPHA
static const uint8_t CHARLIE[] = { 0x5F, 0x00, 0x00, 0x00 };

TEST(ResolveHash, NoMatchIsNotAGuess) {
    const uint8_t* nodes[] = { ALPHA, BRAVO, CHARLIE };
    const uint8_t wanted[] = { 0xEE };
    EXPECT_EQ(RIFT_HASH_NONE, riftResolveHash(wanted, 1, nodes, 3));
}

TEST(ResolveHash, SingleMatchResolves) {
    const uint8_t* nodes[] = { ALPHA, BRAVO, CHARLIE };
    const uint8_t wanted[] = { 0x5F };
    EXPECT_EQ(2, riftResolveHash(wanted, 1, nodes, 3));
}

// The case that shipped naming the wrong repeater: at one byte, ALPHA and BRAVO
// are indistinguishable, and taking the first match asserted it was ALPHA.
TEST(ResolveHash, OneByteCollisionIsAmbiguousNotTheFirstMatch) {
    const uint8_t* nodes[] = { ALPHA, BRAVO, CHARLIE };
    const uint8_t wanted[] = { 0xA1 };
    EXPECT_EQ(RIFT_HASH_AMBIGUOUS, riftResolveHash(wanted, 1, nodes, 3));
}

TEST(ResolveHash, LongerHashSeparatesWhatOneByteCouldNot) {
    const uint8_t* nodes[] = { ALPHA, BRAVO, CHARLIE };
    const uint8_t wanted[] = { 0xA1, 0xB2, 0xC3 };
    EXPECT_EQ(0, riftResolveHash(wanted, 3, nodes, 3));
}

TEST(ResolveHash, TwoBytesStillCollideHere) {
    // ALPHA and BRAVO agree on the first two bytes, so two is not enough either
    const uint8_t* nodes[] = { ALPHA, BRAVO, CHARLIE };
    const uint8_t wanted[] = { 0xA1, 0xB2 };
    EXPECT_EQ(RIFT_HASH_AMBIGUOUS, riftResolveHash(wanted, 2, nodes, 3));
}

TEST(ResolveHash, SkipsEmptySlots) {
    // nodes not drawn this frame are passed as NULL and must not match
    const uint8_t* nodes[] = { NULL, BRAVO, NULL };
    const uint8_t wanted[] = { 0xA1 };
    EXPECT_EQ(1, riftResolveHash(wanted, 1, nodes, 3));
}

TEST(ResolveHash, DegenerateInputsDoNotMatchAnything) {
    const uint8_t* nodes[] = { ALPHA };
    const uint8_t wanted[] = { 0xA1 };
    EXPECT_EQ(RIFT_HASH_NONE, riftResolveHash(NULL, 1, nodes, 1));
    EXPECT_EQ(RIFT_HASH_NONE, riftResolveHash(wanted, 0, nodes, 1));
    EXPECT_EQ(RIFT_HASH_NONE, riftResolveHash(wanted, 1, NULL, 1));
    EXPECT_EQ(RIFT_HASH_NONE, riftResolveHash(wanted, 1, nodes, 0));
}

// ---------------------------------------------------------------- channel capacity

TEST(ChannelCapacity, LongerNameLeavesLessRoom) {
    // MAX_TEXT_LEN is 160 on this firmware
    EXPECT_EQ(160 - 3,  riftChannelCapacity(160, "a"));
    EXPECT_EQ(160 - 12, riftChannelCapacity(160, "0123456789"));
    EXPECT_EQ(160 - 22, riftChannelCapacity(160, "01234567890123456789"));
    EXPECT_EQ(160 - 33, riftChannelCapacity(160, "0123456789012345678901234567890"));
}

TEST(ChannelCapacity, CountsTheColonAndSpace) {
    // the prefix is "<name>: " - two characters beyond the name itself
    EXPECT_EQ(100 - strlen("NODE") - 2, (size_t) riftChannelCapacity(100, "NODE"));
}

TEST(ChannelCapacity, NeverGoesNegative) {
    // a name longer than the whole message budget must clamp, not wrap
    EXPECT_EQ(0, riftChannelCapacity(10, "a-very-long-node-name-indeed"));
    EXPECT_EQ(0, riftChannelCapacity(0, "x"));
}

TEST(ChannelCapacity, MissingNameCostsOnlyTheSeparator) {
    EXPECT_EQ(158, riftChannelCapacity(160, ""));
    EXPECT_EQ(158, riftChannelCapacity(160, NULL));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
