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

// ------------------------------------------------------------ screen transitions

// Arguments are (same_screen, from_overlay, to_overlay).

TEST(ScreenTransition, RealNavigationFiresBoth) {
    EXPECT_EQ(RIFT_XN_LEAVE | RIFT_XN_ENTER, riftScreenTransition(false, false, false));
}

// The bug that panicked the device: an incoming message raised the preview while
// RADAR was scanning, RADAR was told it had lost focus, and the BT teardown ran
// from inside the LoRa receive path.
TEST(ScreenTransition, RaisingAPopupLeavesNothing) {
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(false, false, true));
}

// The other one, same cause: SYSTEM was told it had been left and wiped the
// one-time channel key while the user was still reading it off the screen.
TEST(ScreenTransition, PopupOverAModalScreenStillLeavesNothing) {
    // the rule does not depend on what is underneath - a popup is never navigation
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(false, false, true));
}

// Not reachable as the UI is wired today: overlays never become the current
// screen, so nothing transitions out of one. Asserted anyway, because the fourth
// row of a four-row table should not be left to guesswork.
TEST(ScreenTransition, DismissingAPopupOnlyEnters) {
    // the overlay was never really "on" anything, so there is no screen to leave
    EXPECT_EQ(RIFT_XN_ENTER, riftScreenTransition(false, true, false));
}

TEST(ScreenTransition, ReselectingTheSameScreenIsNotATransition) {
    // a nav-bar tap on the screen already showing must not wipe its state
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(true, false, false));
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(true, true, true));
}

TEST(ScreenTransition, SameScreenWinsOverEverythingElse) {
    // ordering matters: the same-screen check has to come first, or re-selecting
    // a screen would be treated as a dismissal and fire onEnter again
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(true, true, false));
    EXPECT_EQ(RIFT_XN_NONE, riftScreenTransition(true, false, true));
}

// ---------------------------------------------------------------- mesh activity

TEST(MeshActivity, NothingHeardIsNotJustVeryOld) {
    // NEVER and a long-stale QUIET mean different things in the field: one points
    // at frequency/SF/antenna, the other at a quiet network. The elapsed value is
    // meaningless when nothing has been heard, so it must not decide the state.
    EXPECT_EQ(RIFT_MESH_NEVER, riftMeshActivity(false, 0));
    EXPECT_EQ(RIFT_MESH_NEVER, riftMeshActivity(false, 500));
    EXPECT_EQ(RIFT_MESH_NEVER, riftMeshActivity(false, 6UL * 3600 * 1000));
}

TEST(MeshActivity, FreshTrafficIsActive) {
    EXPECT_EQ(RIFT_MESH_ACTIVE, riftMeshActivity(true, 0));
    EXPECT_EQ(RIFT_MESH_ACTIVE, riftMeshActivity(true, 59999));
}

TEST(MeshActivity, BoundariesBelongToTheQuieterState) {
    // exactly at a threshold counts as the older band - the comparison is <, so
    // a state cannot appear to hold one millisecond longer than its label claims
    EXPECT_EQ(RIFT_MESH_IDLE,  riftMeshActivity(true, RIFT_MESH_ACTIVE_MILLIS));
    EXPECT_EQ(RIFT_MESH_IDLE,  riftMeshActivity(true, RIFT_MESH_IDLE_MILLIS - 1));
    EXPECT_EQ(RIFT_MESH_QUIET, riftMeshActivity(true, RIFT_MESH_IDLE_MILLIS));
}

TEST(MeshActivity, StaysQuietNoMatterHowOld) {
    EXPECT_EQ(RIFT_MESH_QUIET, riftMeshActivity(true, 0xFFFFFFFFUL));
}

// ---------------------------------------------------------------- age formatting

static const char* age_of(uint32_t millis_since) {
    static char buf[RIFT_AGE_BUF_LEN];
    riftFormatAge(millis_since, buf, sizeof(buf));
    return buf;
}

TEST(FormatAge, PicksOneUnit) {
    EXPECT_STREQ("0s",  age_of(0));
    EXPECT_STREQ("9s",  age_of(9500));
    EXPECT_STREQ("59s", age_of(59999));
    EXPECT_STREQ("1m",  age_of(60000));
    EXPECT_STREQ("59m", age_of(3599999));
    EXPECT_STREQ("1h",  age_of(3600000));
    EXPECT_STREQ("23h", age_of(86399999));
    EXPECT_STREQ("1d",  age_of(86400000));
}

TEST(FormatAge, TruncatesRatherThanRounding) {
    // 119 seconds is not "2m" - the number must never run ahead of the elapsed time
    EXPECT_STREQ("1m", age_of(119999));
    EXPECT_STREQ("1h", age_of(7199999));
}

TEST(FormatAge, NeverExceedsTheFieldWidth) {
    // the widest output has to fit RIFT_AGE_BUF_LEN, including the terminator;
    // 0xFFFFFFFF ms is 49.7 days, so walk past it in seconds to reach the cap
    EXPECT_LE(strlen(age_of(0xFFFFFFFFUL)), (size_t) RIFT_AGE_BUF_LEN - 1);
    for (uint32_t ms = 0; ms < 0xFFFFFFFFUL - 5000000UL; ms += 5000000UL) {
        EXPECT_LE(strlen(age_of(ms)), (size_t) RIFT_AGE_BUF_LEN - 1);
    }
}

TEST(FormatAge, DegenerateBufferIsNotWritten) {
    char buf[RIFT_AGE_BUF_LEN] = "keep";
    riftFormatAge(1000, buf, 0);
    EXPECT_STREQ("keep", buf);
    riftFormatAge(1000, NULL, sizeof(buf));   // must not crash
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
