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

// ------------------------------------------------------------ UTF-8 translation

static const char* xl(const char* src) {
    static char buf[256];
    riftTranslateUTF8(buf, src, sizeof(buf));
    return buf;
}

#define BLOCK  "\xDB"

TEST(TranslateUTF8, AsciiPassesThrough) {
    EXPECT_STREQ("Hello, world! 123", xl("Hello, world! 123"));
    EXPECT_STREQ("", xl(""));
}

TEST(TranslateUTF8, NordicCharactersMapToCP437) {
    // ae, a-ring, a-umlaut, o-umlaut all exist in the font
    EXPECT_STREQ("\x91\x86\x84\x94", xl("æåäö"));
    EXPECT_STREQ("\x92\x8F\x8E\x99", xl("ÆÅÄÖ"));
}

TEST(TranslateUTF8, SlashedOUsesTheSynthesisedGlyphs) {
    // absent from CP437; the display driver draws these from the base letter
    EXPECT_STREQ("\x01\x02", xl("øØ"));
}

// The case that made ordinary traffic look far worse than its emoji count: a
// variation selector is invisible in Unicode but used to become a second block.
TEST(TranslateUTF8, VariationSelectorDoesNotBecomeAGlyph) {
    EXPECT_STREQ("\x03", xl("\xE2\x9D\xA4\xEF\xB8\x8F"));   // U+2764 U+FE0F
}

TEST(TranslateUTF8, SkinToneModifierIsDropped) {
    // thumbs up carries its own equivalent; the modifier must add nothing to it
    EXPECT_STREQ("+1", xl("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"));

    // and on an emoji with no equivalent, the pair is still a single block rather
    // than two - U+1F64F folded hands, deliberately not in the table
    EXPECT_STREQ(BLOCK, xl("\xF0\x9F\x99\x8F\xF0\x9F\x8F\xBD"));
}

// A ZWJ family is five code points and used to draw five squares in a row.
TEST(TranslateUTF8, ZwjSequenceCollapsesToOneBlock) {
    const char* family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    EXPECT_STREQ(BLOCK, xl(family));
}

TEST(TranslateUTF8, ConsecutiveUnmappableCollapse) {
    // deliberate: three unrelated unmappable emoji also become one block, since
    // three blocks say nothing more than one
    EXPECT_STREQ(BLOCK, xl("\xF0\x9F\x94\xB4\xF0\x9F\x94\xB5\xF0\x9F\x9F\xA2"));
}

TEST(TranslateUTF8, BlocksSeparatedByTextDoNotCollapse) {
    EXPECT_STREQ(BLOCK "a" BLOCK, xl("\xF0\x9F\x94\xB4" "a" "\xF0\x9F\x94\xB5"));
}

TEST(TranslateUTF8, MappedEmojiBecomeReadableEquivalents) {
    EXPECT_STREQ(":)", xl("\xF0\x9F\x99\x82"));            // U+1F642 slightly smiling
    EXPECT_STREQ(":D", xl("\xF0\x9F\x98\x82"));            // U+1F602 tears of joy
    EXPECT_STREQ(";)", xl("\xF0\x9F\x98\x89"));            // U+1F609 winking
    EXPECT_STREQ(":(", xl("\xF0\x9F\x98\xAD"));            // U+1F62D loudly crying
    EXPECT_STREQ("\x0E", xl("\xF0\x9F\x8E\xB5"));          // U+1F3B5 musical note
    EXPECT_STREQ("+1", xl("\xF0\x9F\x91\x8D"));            // U+1F44D thumbs up
    EXPECT_STREQ("\xFB", xl("\xE2\x9C\x85"));              // U+2705 check mark
    EXPECT_STREQ("\x0F", xl("\xE2\x98\x80"));              // U+2600 sun
    EXPECT_STREQ("\x1A", xl("\xE2\x9E\xA1"));              // U+27A1 right arrow
}

// Adafruit_GFX::write() swallows 0x0A as a newline and 0x0D as a carriage return
// before drawChar ever sees them, so a glyph mapped to either renders as nothing.
// 0x0D is CP437's eighth note and that is exactly the mistake this caught.
// Every replacement string in the table has to survive this.
TEST(TranslateUTF8, NoReplacementUsesAByteTheFontNeverDraws) {
    // walk every code point the table could plausibly cover, plus the Nordic set
    for (uint32_t cp = 0x20; cp < 0x2C00; cp++) {
        const char* out = riftEmojiToText(cp);
        if (out == NULL) continue;
        for (const char* p = out; *p; p++) {
            EXPECT_FALSE(riftNoGlyphAt(*p)) << "code point " << cp << " maps to an undrawable byte";
        }
    }
    for (uint32_t cp = 0x1F300; cp < 0x1FA00; cp++) {
        const char* out = riftEmojiToText(cp);
        if (out == NULL) continue;
        for (const char* p = out; *p; p++) {
            EXPECT_FALSE(riftNoGlyphAt(*p)) << "code point " << cp << " maps to an undrawable byte";
        }
    }
    // and the Nordic mappings, for the same reason
    static const uint32_t NORDIC[] = { 0xE6, 0xC6, 0xE5, 0xC5, 0xE4, 0xC4, 0xF6, 0xD6, 0xF8, 0xD8 };
    for (size_t i = 0; i < sizeof(NORDIC) / sizeof(NORDIC[0]); i++) {
        EXPECT_FALSE(riftNoGlyphAt(riftNordicToCP437(NORDIC[i])));
    }
    // the fallback block itself must be drawable
    EXPECT_FALSE(riftNoGlyphAt((char) RIFT_GLYPH_BLOCK));
}

// The reason the table is conservative: a sobbing face must not resolve to a
// smile. Anything uncertain stays a block, which is honest about what is missing.
TEST(TranslateUTF8, SadFacesNeverBecomeSmiles) {
    EXPECT_STREQ(":(", xl("\xF0\x9F\x98\xA2"));            // U+1F622 crying
    EXPECT_STRNE(":)", xl("\xF0\x9F\x98\xA2"));
    EXPECT_STREQ(":(", xl("\xF0\x9F\x99\x81"));            // U+1F641 frowning
}

TEST(TranslateUTF8, MixedRealMessage) {
    // what an ordinary incoming message looks like
    // ae -> 0x91, o-slash -> the synthesised 0x01, heart+VS16 -> one 0x03, U+1F642 -> ":)"
    EXPECT_STREQ("H\x91r er n\x01kkelen \x03 ok:)",
                 xl("Hær er nøkkelen \xE2\x9D\xA4\xEF\xB8\x8F ok\xF0\x9F\x99\x82"));
}

TEST(TranslateUTF8, MalformedSequencesBecomeOneBlockAndDoNotStall) {
    const char stray_continuation[] = { 'a', (char) 0x80, 'b', 0 };
    EXPECT_STREQ("a" BLOCK "b", xl(stray_continuation));

    const char truncated[] = { 'a', (char) 0xF0, (char) 0x9F, 0 };
    EXPECT_STREQ("a" BLOCK, xl(truncated));

    const char bad_lead[] = { 'a', (char) 0xFF, 'b', 0 };
    EXPECT_STREQ("a" BLOCK "b", xl(bad_lead));
}

TEST(TranslateUTF8, ControlCharactersAreDroppedNotBlocked) {
    const char with_ctrl[] = { 'a', 0x07, 'b', 0 };
    EXPECT_STREQ("ab", xl(with_ctrl));
}

TEST(TranslateUTF8, RespectsDestSize) {
    char buf[4];
    riftTranslateUTF8(buf, "abcdefgh", sizeof(buf));
    EXPECT_STREQ("abc", buf);

    // a two-character replacement must not be written half-way
    char tight[3];
    riftTranslateUTF8(tight, "a\xF0\x9F\x99\x82", sizeof(tight));
    EXPECT_STREQ("a", tight);

    // never writes past a one-byte buffer
    char one[1];
    riftTranslateUTF8(one, "abc", sizeof(one));
    EXPECT_STREQ("", one);
}

TEST(TranslateUTF8, DegenerateInputs) {
    char buf[8] = "keep";
    riftTranslateUTF8(buf, "x", 0);
    EXPECT_STREQ("keep", buf);      // dest_size 0 must not be written at all
    riftTranslateUTF8(buf, NULL, sizeof(buf));
    EXPECT_STREQ("", buf);
    riftTranslateUTF8(NULL, "x", 8);   // must not crash
}

TEST(Utf8Decode, ReportsLengthAndRejectsMalformed) {
    uint32_t cp = 0;
    EXPECT_EQ(1, riftUtf8Decode("A", &cp));               EXPECT_EQ(0x41u, cp);
    EXPECT_EQ(2, riftUtf8Decode("\xC3\xA6", &cp));        EXPECT_EQ(0xE6u, cp);
    EXPECT_EQ(3, riftUtf8Decode("\xE2\x9D\xA4", &cp));    EXPECT_EQ(0x2764u, cp);
    EXPECT_EQ(4, riftUtf8Decode("\xF0\x9F\x99\x82", &cp)); EXPECT_EQ(0x1F642u, cp);

    EXPECT_EQ(0, riftUtf8Decode("\x80", &cp));            // stray continuation
    EXPECT_EQ(0, riftUtf8Decode("\xFF", &cp));            // invalid lead
    EXPECT_EQ(0, riftUtf8Decode("\xF0\x9F", &cp));        // truncated at terminator
    EXPECT_EQ(0, riftUtf8Decode("\xC3\x41", &cp));        // bad continuation
}

// ------------------------------------------------------------ Nordic variants

TEST(NordicVariants, BaseVowelsOfferTheirForms) {
    const char* v[RIFT_NORDIC_MAX_VARIANTS] = { NULL, NULL, NULL };

    EXPECT_EQ(3, riftNordicVariants('a', v));
    EXPECT_STREQ("\xC3\xA6", v[0]);   // ae
    EXPECT_STREQ("\xC3\xA5", v[1]);   // a-ring
    EXPECT_STREQ("\xC3\xA4", v[2]);   // a-umlaut

    EXPECT_EQ(2, riftNordicVariants('o', v));
    EXPECT_STREQ("\xC3\xB8", v[0]);   // o-slash
    EXPECT_STREQ("\xC3\xB6", v[1]);   // o-umlaut
}

TEST(NordicVariants, CaseIsCarriedThrough) {
    const char* v[RIFT_NORDIC_MAX_VARIANTS] = { NULL, NULL, NULL };
    EXPECT_EQ(3, riftNordicVariants('A', v));
    EXPECT_STREQ("\xC3\x86", v[0]);   // AE
    EXPECT_EQ(2, riftNordicVariants('O', v));
    EXPECT_STREQ("\xC3\x98", v[0]);   // O-slash
}

TEST(NordicVariants, OtherKeysOfferNothing) {
    const char* v[RIFT_NORDIC_MAX_VARIANTS] = { NULL, NULL, NULL };
    EXPECT_EQ(0, riftNordicVariants('e', v));
    EXPECT_EQ(0, riftNordicVariants('z', v));
    EXPECT_EQ(0, riftNordicVariants(' ', v));
    EXPECT_EQ(0, riftNordicVariants(0, v));
    EXPECT_EQ(0, riftNordicVariants('a', NULL));
}

// Every variant has to be UTF-8, because the compose buffer is what goes on the
// air. A CP437 code here would send a control byte other clients cannot decode -
// o-slash is 0x01 on the display and 0xC3 0xB8 on the wire, and confusing the two
// is the sharpest trap in this feature.
TEST(NordicVariants, EveryVariantIsValidTwoByteUtf8) {
    static const char BASES[] = { 'a', 'A', 'o', 'O' };
    for (size_t b = 0; b < sizeof(BASES); b++) {
        const char* v[RIFT_NORDIC_MAX_VARIANTS] = { NULL, NULL, NULL };
        int n = riftNordicVariants(BASES[b], v);
        for (int i = 0; i < n; i++) {
            ASSERT_NE(nullptr, v[i]);
            EXPECT_EQ(2u, strlen(v[i])) << "variant " << i << " of " << BASES[b];
            uint32_t cp = 0;
            EXPECT_EQ(2, riftUtf8Decode(v[i], &cp));
            // and it must be one the panel can actually draw back
            EXPECT_NE(0, riftNordicToCP437(cp)) << "variant " << i << " has no glyph";
        }
    }
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

// ------------------------------------------------------------ DM capability

TEST(CanDirectMessage, ChatAndRoomsReceive) {
    EXPECT_TRUE(riftCanDirectMessage(RIFT_ADV_CHAT));
    EXPECT_TRUE(riftCanDirectMessage(RIFT_ADV_ROOM));   // a room server stores messages
}

TEST(CanDirectMessage, RepeatersAndSensorsDoNot) {
    EXPECT_FALSE(riftCanDirectMessage(RIFT_ADV_REPEATER));
    EXPECT_FALSE(riftCanDirectMessage(RIFT_ADV_SENSOR));
    EXPECT_FALSE(riftCanDirectMessage(RIFT_ADV_NONE));
    EXPECT_FALSE(riftCanDirectMessage(200));            // unknown type
}

// The whole reason this is one function: NODES allowed chat only while the COMMS
// picker allowed chat and rooms, so a room was offered on one screen and refused
// on the other.
TEST(CanDirectMessage, OneAnswerForEveryType) {
    for (int t = 0; t < 256; t++) {
        bool a = riftCanDirectMessage((uint8_t) t);
        bool b = riftCanDirectMessage((uint8_t) t);
        EXPECT_EQ(a, b);
        if (!a) EXPECT_STRNE("", riftAdvertTypeName((uint8_t) t));   // always explainable
    }
}

// ------------------------------------------------------------ message log flush

TEST(ShouldFlush, WaitsForTheBurstToSettle) {
    EXPECT_FALSE(riftShouldFlush(true, 1000, 1000, 20000, 0, 0));
    EXPECT_FALSE(riftShouldFlush(true, 20999, 1000, 20000, 0, 0));
    EXPECT_TRUE (riftShouldFlush(true, 21000, 1000, 20000, 0, 0));
}

TEST(ShouldFlush, CleanLogIsNeverWritten) {
    EXPECT_FALSE(riftShouldFlush(false, 999999, 0, 20000, 0, 0));
}

// The bug: a failed save left the log dirty with an old timestamp, so the
// condition stayed true and the save was retried every loop iteration - at
// ~553ms each, on the same loop as the radio.
TEST(ShouldFlush, AFailedSaveDoesNotRetryImmediately) {
    uint32_t now = 100000;
    uint32_t retry = now + riftSaveBackoffMillis(1);
    EXPECT_FALSE(riftShouldFlush(true, now + 1, 0, 20000, 1, retry));
    EXPECT_FALSE(riftShouldFlush(true, retry - 1, 0, 20000, 1, retry));
    EXPECT_TRUE (riftShouldFlush(true, retry, 0, 20000, 1, retry));
}

TEST(ShouldFlush, BackoffGrowsAndThenHolds) {
    EXPECT_EQ(5000u,  riftSaveBackoffMillis(1));
    EXPECT_EQ(15000u, riftSaveBackoffMillis(2));
    EXPECT_EQ(60000u, riftSaveBackoffMillis(3));
    EXPECT_EQ(60000u, riftSaveBackoffMillis(50));
    EXPECT_EQ(60000u, riftSaveBackoffMillis(255));
}

TEST(ShouldFlush, SurvivesTheMillisWrap) {
    // Retry deadline computed just before the wrap, now just after it. dirty_at
    // has to be before the wrap too, or the debounce is what blocks rather than
    // the backoff - which is what the first draft of this test actually measured.
    uint32_t dirty_at = 0xFFFF0000u;
    uint32_t retry = 0xFFFFFF00u + 5000u;      // wraps to 0x00001288
    EXPECT_FALSE(riftShouldFlush(true, 0xFFFFFF00u, dirty_at, 20000, 1, retry));
    EXPECT_TRUE (riftShouldFlush(true, retry, dirty_at, 20000, 1, retry));
    EXPECT_TRUE (riftShouldFlush(true, retry + 1000, dirty_at, 20000, 1, retry));

    // and the debounce itself has to survive the wrap independently: 10s after
    // dirty_at is not yet due, and a now that has wrapped past it is
    EXPECT_FALSE(riftShouldFlush(true, dirty_at + 10000u, dirty_at, 20000, 0, 0));
    EXPECT_TRUE (riftShouldFlush(true, retry, dirty_at, 20000, 0, 0));
}

// ---------------------------------------------------------- millis deadlines

TEST(Due, OrdinaryCase) {
    EXPECT_FALSE(riftDue(1000, 2000));
    EXPECT_TRUE (riftDue(2000, 2000));    // at the deadline counts as due
    EXPECT_TRUE (riftDue(2001, 2000));
}

TEST(Due, SurvivesTheWrap) {
    // the case the old millis() > deadline comparison got wrong: a deadline set
    // shortly before the wrap lands on a small number while now is still large
    const uint32_t before = 0xFFFFFF00u;
    const uint32_t deadline = before + 0x200u;    // wraps to 0x00000100
    EXPECT_FALSE(riftDue(before, deadline));      // not yet - the old form said yes
    EXPECT_FALSE(riftDue(0xFFFFFFFFu, deadline));
    EXPECT_FALSE(riftDue(0x000000FFu, deadline)); // one tick short, after the wrap
    EXPECT_TRUE (riftDue(0x00000100u, deadline)); // and now it is due
    EXPECT_TRUE (riftDue(0x00000101u, deadline));
}

TEST(Due, HalfTheRangeIsTheLimit) {
    // Valid while the interval is under 2^31. Documented rather than guarded,
    // because every interval in this firmware is seconds - but this is the reason
    // "_next_refresh = 0" had to become "= millis()": under a signed difference a
    // fixed small constant is not "long past" for the whole cycle.
    EXPECT_TRUE (riftDue(0x7FFFFFFFu, 0));   // still reads as past
    EXPECT_FALSE(riftDue(0x80000000u, 0));   // and here it stops
}

// ------------------------------------------------------------- UTF-8 decoding
//
// The decoder used to accept three classes of sequence that are arithmetically
// decodable but are not valid UTF-8. Only the overlong forms had a consequence:
// they decode to a code point in the ASCII range, which riftTranslateUTF8 then
// drops as a control character, so a sender could put bytes on the wire that this
// display silently swallowed while other clients rendered them.
//
// The sequences are byte arrays rather than string escapes. Written as escapes in
// this file they were transformed on the way in - "0xC0" became the character
// U+00C0 and reached the decoder as 0xC3 0x80 - so the first version of these
// tests failed while testing something other than what it claimed to.

static const unsigned char U8_OVERLONG_NUL2[]  = { 0xC0, 0x80, 0 };
static const unsigned char U8_OVERLONG_7F[]    = { 0xC1, 0xBF, 0 };
static const unsigned char U8_OVERLONG_NUL3[]  = { 0xE0, 0x80, 0x80, 0 };
static const unsigned char U8_OVERLONG_7FF[]   = { 0xE0, 0x9F, 0xBF, 0 };
static const unsigned char U8_OVERLONG_NUL4[]  = { 0xF0, 0x80, 0x80, 0x80, 0 };
static const unsigned char U8_OVERLONG_FFFF[]  = { 0xF0, 0x8F, 0xBF, 0xBF, 0 };
static const unsigned char U8_SURROGATE_LO[]   = { 0xED, 0xA0, 0x80, 0 };   // U+D800
static const unsigned char U8_SURROGATE_HI[]   = { 0xED, 0xBF, 0xBF, 0 };   // U+DFFF
static const unsigned char U8_ABOVE_MAX[]      = { 0xF4, 0x90, 0x80, 0x80, 0 }; // U+110000
static const unsigned char U8_LEAD_F5[]        = { 0xF5, 0x80, 0x80, 0x80, 0 };
static const unsigned char U8_FIVE_BYTE[]      = { 0xF8, 0x80, 0x80, 0x80, 0 };

static const unsigned char U8_MIN_2[]  = { 0xC2, 0x80, 0 };              // U+0080
static const unsigned char U8_MIN_3[]  = { 0xE0, 0xA0, 0x80, 0 };        // U+0800
static const unsigned char U8_MIN_4[]  = { 0xF0, 0x90, 0x80, 0x80, 0 };  // U+10000
static const unsigned char U8_MAX_4[]  = { 0xF4, 0x8F, 0xBF, 0xBF, 0 };  // U+10FFFF
static const unsigned char U8_AE[]     = { 0xC3, 0xA6, 0 };
static const unsigned char U8_OE[]     = { 0xC3, 0xB8, 0 };
static const unsigned char U8_AA[]     = { 0xC3, 0xA5, 0 };

#define RIFT_DECODE(bytes, cp)  riftUtf8Decode((const char*) (bytes), (cp))

TEST(Utf8Decode, RejectsOverlongForms) {
    uint32_t cp = 0xFFFF;
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_NUL2, &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_7F,   &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_NUL3, &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_7FF,  &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_NUL4, &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_OVERLONG_FFFF, &cp));
}

TEST(Utf8Decode, RejectsSurrogatesAndOutOfRange) {
    uint32_t cp = 0xFFFF;
    EXPECT_EQ(0, RIFT_DECODE(U8_SURROGATE_LO, &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_SURROGATE_HI, &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_ABOVE_MAX,    &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_LEAD_F5,      &cp));
    EXPECT_EQ(0, RIFT_DECODE(U8_FIVE_BYTE,    &cp));
}

TEST(Utf8Decode, StillAcceptsTheBoundaries) {
    uint32_t cp = 0;
    // the shortest legal form at each length, and the last legal code point - the
    // values the overlong rule has to let through
    EXPECT_EQ(2, RIFT_DECODE(U8_MIN_2, &cp)); EXPECT_EQ(0x80u,     cp);
    EXPECT_EQ(3, RIFT_DECODE(U8_MIN_3, &cp)); EXPECT_EQ(0x800u,    cp);
    EXPECT_EQ(4, RIFT_DECODE(U8_MIN_4, &cp)); EXPECT_EQ(0x10000u,  cp);
    EXPECT_EQ(4, RIFT_DECODE(U8_MAX_4, &cp)); EXPECT_EQ(0x10FFFFu, cp);
    // and the three letters this firmware exists to carry
    EXPECT_EQ(2, RIFT_DECODE(U8_AE, &cp)); EXPECT_EQ(0xE6u, cp);
    EXPECT_EQ(2, RIFT_DECODE(U8_OE, &cp)); EXPECT_EQ(0xF8u, cp);
    EXPECT_EQ(2, RIFT_DECODE(U8_AA, &cp)); EXPECT_EQ(0xE5u, cp);
}

TEST(Utf8Decode, OverlongNullShowsAsABlockRatherThanVanishing) {
    // the point of the change. Before it, C0 80 decoded to U+0000 and was dropped
    // as a control character, so the message read "AB" here and something else
    // as a control character, so the message read "AB" here and something else
    // malformed path, and one block says a character was present.
    const unsigned char src[] = { 'A', 0xC0, 0x80, 'B', 0 };
    char out[32];
    riftTranslateUTF8(out, (const char*) src, sizeof(out));
    ASSERT_EQ(3u, strlen(out));
    EXPECT_EQ('A', out[0]);
    EXPECT_EQ((char) RIFT_GLYPH_BLOCK, out[1]);
    EXPECT_EQ('B', out[2]);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
