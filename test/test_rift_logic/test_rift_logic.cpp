#include <gtest/gtest.h>
#include <cmath>
#include <vector>

// Included by relative path on purpose: this keeps the native test environments
// exactly as upstream has them. The header is standalone - stdint and string.h
// only - so there is nothing to link.
#include "../../examples/companion_radio/ui-rift/RiftLogic.h"
#include "../../examples/companion_radio/CompanionCmdLimits.h"

// ---------------------------------------------------------------- hash resolution

static const uint8_t ALPHA[] = { 0xA1, 0xB2, 0xC3, 0xD4 };
static const uint8_t BRAVO[] = { 0xA1, 0xB2, 0x99, 0x00 };  // shares one byte with ALPHA
static const uint8_t CHARLIE[] = { 0x5F, 0x00, 0x00, 0x00 };

// Channels now carry a fingerprint of their key, so a test channel needs one. Distinct
// per slot, and non-zero, because zero means "not recorded" and matches on slot alone.
static RiftConvKey ch(uint8_t slot) { return riftConvChannel(slot, 0xC0DE0000u + slot); }

// A 7-byte key, so the dedup prefix has something to compare. Distinct nodes differ
// in the last byte, which is past every hash length used here.
static void key7(uint8_t* out, uint8_t lead, uint8_t tail) {
    for (int i = 0; i < 7; i++) out[i] = lead;
    out[6] = tail;
}

TEST(ResolveHash, NoMatchIsUnknownRatherThanAGuess) {
    uint8_t a[7], b[7], want = 0x99;
    key7(a, 0x11, 1); key7(b, 0x22, 2);
    RiftHashResolve r;
    riftResolveBegin(&r);
    riftResolveStep(&r, &want, 1, a);
    riftResolveStep(&r, &want, 1, b);
    EXPECT_EQ(RIFT_RESOLVE_NONE, riftResolveResult(&r));
}

TEST(ResolveHash, OneMatchIsUnique) {
    uint8_t a[7], b[7], want = 0x22;
    key7(a, 0x11, 1); key7(b, 0x22, 2);
    RiftHashResolve r;
    riftResolveBegin(&r);
    EXPECT_FALSE(riftResolveStep(&r, &want, 1, a));
    EXPECT_TRUE(riftResolveStep(&r, &want, 1, b));   // true: copy this one's name
    EXPECT_EQ(RIFT_RESOLVE_UNIQUE, riftResolveResult(&r));
}

TEST(ResolveHash, TwoDistinctNodesAreAmbiguous) {
    uint8_t a[7], b[7], want = 0x22;
    key7(a, 0x22, 1); key7(b, 0x22, 2);             // same lead byte, different node
    RiftHashResolve r;
    riftResolveBegin(&r);
    riftResolveStep(&r, &want, 1, a);
    riftResolveStep(&r, &want, 1, b);
    EXPECT_EQ(RIFT_RESOLVE_AMBIGUOUS, riftResolveResult(&r));
}

// The reviewer's scenario, and the reason the whole function moved. ALPHA is in the
// recent advert cache, BRAVO only in the contact table, and they share a short hash.
// Resolving against one set said UNIQUE; the honest answer is AMBIGUOUS.
TEST(ResolveHash, ACollisionAcrossTheTwoIdentitySetsIsAmbiguous) {
    uint8_t alpha[7], bravo[7], want = 0x5A;
    key7(alpha, 0x5A, 0xA1);
    key7(bravo, 0x5A, 0xB2);

    RiftHashResolve r;
    riftResolveBegin(&r);
    riftResolveStep(&r, &want, 1, bravo);            // walked from the contact table
    EXPECT_EQ(RIFT_RESOLVE_UNIQUE, riftResolveResult(&r));   // as far as it knows so far
    riftResolveStep(&r, &want, 1, alpha);            // then the advert cache
    EXPECT_EQ(RIFT_RESOLVE_AMBIGUOUS, riftResolveResult(&r));
}

// The dedup, which is load-bearing rather than tidy: a node is normally in both sets,
// so without this every known node would resolve as ambiguous with itself.
TEST(ResolveHash, TheSameNodeInBothSetsIsStillOneNode) {
    uint8_t k[7], want = 0x33;
    key7(k, 0x33, 7);
    RiftHashResolve r;
    riftResolveBegin(&r);
    EXPECT_TRUE(riftResolveStep(&r, &want, 1, k));    // contact table
    EXPECT_FALSE(riftResolveStep(&r, &want, 1, k));   // advert cache, same node
    EXPECT_EQ(RIFT_RESOLVE_UNIQUE, riftResolveResult(&r));
}

TEST(ResolveHash, ALongerHashSeparatesWhatAShortOneCannot) {
    uint8_t a[7], b[7];
    key7(a, 0x44, 1); key7(b, 0x44, 2);
    a[1] = 0x01; b[1] = 0x02;                         // differ at byte 1
    uint8_t want[2] = { 0x44, 0x01 };

    RiftHashResolve one, two;
    riftResolveBegin(&one);
    riftResolveStep(&one, want, 1, a);
    riftResolveStep(&one, want, 1, b);
    EXPECT_EQ(RIFT_RESOLVE_AMBIGUOUS, riftResolveResult(&one));

    riftResolveBegin(&two);
    riftResolveStep(&two, want, 2, a);
    riftResolveStep(&two, want, 2, b);
    EXPECT_EQ(RIFT_RESOLVE_UNIQUE, riftResolveResult(&two));
}

// Once ambiguous, a further match cannot make the answer less uncertain.
TEST(ResolveHash, AmbiguousIsSticky) {
    uint8_t a[7], b[7], want = 0x55;
    key7(a, 0x55, 1); key7(b, 0x55, 2);
    RiftHashResolve r;
    riftResolveBegin(&r);
    riftResolveStep(&r, &want, 1, a);
    riftResolveStep(&r, &want, 1, b);
    ASSERT_EQ(RIFT_RESOLVE_AMBIGUOUS, riftResolveResult(&r));
    EXPECT_FALSE(riftResolveStep(&r, &want, 1, a));
    EXPECT_EQ(RIFT_RESOLVE_AMBIGUOUS, riftResolveResult(&r));
}

TEST(ResolveHash, DegenerateInputs) {
    uint8_t k[7], want = 0x11;
    key7(k, 0x11, 1);
    RiftHashResolve r;
    riftResolveBegin(&r);
    EXPECT_FALSE(riftResolveStep(&r, NULL, 1, k));
    EXPECT_FALSE(riftResolveStep(&r, &want, 0, k));
    EXPECT_FALSE(riftResolveStep(&r, &want, 1, NULL));
    EXPECT_FALSE(riftResolveStep(NULL, &want, 1, k));
    EXPECT_EQ(RIFT_RESOLVE_NONE, riftResolveResult(&r));
    EXPECT_EQ(RIFT_RESOLVE_NONE, riftResolveResult(NULL));
}

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

// ------------------------------------------------------------ channel colours
//
// The contrast is computed here rather than taken from the design note, because
// the design note had it wrong. It picked colours in 24-bit and checked them
// there; quantising to RGB565 raises the luminance, and every candidate fell
// below the 4.5:1 text threshold against white once the panel had rounded it -
// one of them to 4.38. This asserts on the value the panel is actually given,
// which is the only number that describes what a user sees.

static void rgb565to888(uint16_t v, int* r, int* g, int* b) {
    int r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);      // replicate the high bits, as the panel does
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static double srgbChannel(int c) {
    double x = c / 255.0;
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

static double relativeLuminance(uint16_t v) {
    int r, g, b;
    rgb565to888(v, &r, &g, &b);
    return 0.2126 * srgbChannel(r) + 0.7152 * srgbChannel(g) + 0.0722 * srgbChannel(b);
}

// The three shapes the message log actually stores. The first version of these
// tests asserted a bare name for incoming entries, which is not what
// UITask::newMsg writes - it decorates with the hop count - so they passed while
// the feature was broken on the device. These strings are the ones the device
// printed when it was asked.

TEST(OriginHops, ReadsWhatOriginNameStrips) {
    int h = -1; bool d = true;
    ASSERT_TRUE(riftOriginHops("(0) #test:", &h, &d));  EXPECT_EQ(0, h);  EXPECT_FALSE(d);
    ASSERT_TRUE(riftOriginHops("(6) Public:", &h, &d)); EXPECT_EQ(6, h);  EXPECT_FALSE(d);
    ASSERT_TRUE(riftOriginHops("(12) mesh:", &h, &d));  EXPECT_EQ(12, h); EXPECT_FALSE(d);
    // "(D)" is a direct path, not a count
    ASSERT_TRUE(riftOriginHops("(D) Bob:", &h, &d));    EXPECT_TRUE(d);
}

TEST(OriginHops, RefusesWhatCarriesNoCount) {
    int h = 99; bool d = false;
    // an outgoing entry: this node sent it, so there is no hop count to report
    EXPECT_FALSE(riftOriginHops("to #test:", &h, &d));
    EXPECT_FALSE(riftOriginHops("#test", &h, &d));
    EXPECT_FALSE(riftOriginHops("(x) name:", &h, &d));
    EXPECT_FALSE(riftOriginHops("(1", &h, &d));          // unclosed
    EXPECT_FALSE(riftOriginHops("(1)name:", &h, &d));     // no space after
    EXPECT_FALSE(riftOriginHops("(123) name:", &h, &d));  // never three digits
    EXPECT_FALSE(riftOriginHops("() name:", &h, &d));
    EXPECT_FALSE(riftOriginHops("", &h, &d));
    EXPECT_FALSE(riftOriginHops(NULL, &h, &d));
    EXPECT_EQ(99, h) << "a refusal must not write to the output";
}

TEST(OriginName, IncomingCarriesTheHopMarker) {
    char out[64];
    ASSERT_TRUE(riftOriginName("(0) #test:", out, sizeof(out)));
    EXPECT_STREQ("#test", out);
    ASSERT_TRUE(riftOriginName("(2) #oslo:", out, sizeof(out)));
    EXPECT_STREQ("#oslo", out);
    // "(D)" is what newMsg writes when the path is direct rather than a count
    ASSERT_TRUE(riftOriginName("(D) Bob:", out, sizeof(out)));
    EXPECT_STREQ("Bob", out);
    // two-digit counts exist at the 2-byte hash setting
    ASSERT_TRUE(riftOriginName("(12) lillemesh:", out, sizeof(out)));
    EXPECT_STREQ("lillemesh", out);
}

TEST(OriginName, OutgoingCarriesTheToMarker) {
    char out[64];
    ASSERT_TRUE(riftOriginName("to #test:", out, sizeof(out)));
    EXPECT_STREQ("#test", out);
    ASSERT_TRUE(riftOriginName("to Bob:", out, sizeof(out)));
    EXPECT_STREQ("Bob", out);
}

TEST(OriginName, BareNamesStillPassThrough) {
    char out[64];
    ASSERT_TRUE(riftOriginName("#test", out, sizeof(out)));
    EXPECT_STREQ("#test", out);
    ASSERT_TRUE(riftOriginName("Public", out, sizeof(out)));
    EXPECT_STREQ("Public", out);
}

TEST(OriginName, TrailingWhitespaceAndColonsAreStripped) {
    char out[64];
    ASSERT_TRUE(riftOriginName("(0) #test: ", out, sizeof(out)));
    EXPECT_STREQ("#test", out);
    ASSERT_TRUE(riftOriginName("#test:", out, sizeof(out)));
    EXPECT_STREQ("#test", out);
}

TEST(OriginName, MalformedDecorationDoesNotEatTheName) {
    char out[64];
    // an unclosed bracket is not a hop marker, so the name is left alone rather
    // than the rest of the string being swallowed
    ASSERT_TRUE(riftOriginName("(unclosed #test", out, sizeof(out)));
    EXPECT_STREQ("(unclosed #test", out);
    // and a bracket not followed by a space is not the marker either
    ASSERT_TRUE(riftOriginName("(x)name", out, sizeof(out)));
    EXPECT_STREQ("(x)name", out);
}

TEST(OriginName, IncomingAndOutgoingResolveToTheSameChannel) {
    // the property the colours depend on: one channel, one colour, whichever
    // direction the message went
    char a[64], b[64];
    ASSERT_TRUE(riftOriginName("(0) #test:", a, sizeof(a)));
    ASSERT_TRUE(riftOriginName("to #test:", b, sizeof(b)));
    EXPECT_STREQ(a, b);
}

TEST(OriginName, NothingToNameIsRefused) {
    char out[64];
    EXPECT_FALSE(riftOriginName("", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName("to :", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName("(0) :", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName(":", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName("::::", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName(NULL, out, sizeof(out)));
    EXPECT_FALSE(riftOriginName("general", out, 0));
}

TEST(OriginName, RefusesRatherThanTruncating) {
    // A truncated name would match the wrong channel, which is worse than matching
    // none: the caller uses this to pick an identity colour.
    char out[8];
    EXPECT_FALSE(riftOriginName("to a-very-long-channel:", out, sizeof(out)));
    EXPECT_FALSE(riftOriginName("exactly8", out, sizeof(out)));
    ASSERT_TRUE (riftOriginName("seven77", out, sizeof(out)));
    EXPECT_STREQ("seven77", out);
}

TEST(OriginName, DoesNotConflateSimilarNames) {
    char a[64], b[64];
    ASSERT_TRUE(riftOriginName("to general:", a, sizeof(a)));
    ASSERT_TRUE(riftOriginName("to general-2:", b, sizeof(b)));
    EXPECT_STRNE(a, b);
}

TEST(ChannelColour, EveryColourClears45OnBothFields) {
    // A channel colour cannot swap between night and day mode without ceasing to
    // be an identity, so one value has to be legible on both #000000 and #FFFFFF.
    for (int slot = 1; slot <= 4; slot++) {
        uint16_t c = riftChannelColour(slot);
        ASSERT_NE(RIFT_CHAN_COL_NONE, c) << "slot " << slot << " has no colour";
        double L = relativeLuminance(c);
        EXPECT_GE((L + 0.05) / 0.05, 4.5) << "slot " << slot << " against black";
        EXPECT_GE(1.05 / (L + 0.05), 4.5) << "slot " << slot << " against white";
    }
}

TEST(ChannelColour, TheBandIsWhereTheAnalysisSaysItIs) {
    // L 0.175 to 0.1833 is the whole window in which both fields clear 4.5:1.
    // Asserted so that a future edit to the table cannot drift out of it while
    // still happening to pass the ratios above by rounding.
    for (int slot = 1; slot <= 4; slot++) {
        double L = relativeLuminance(riftChannelColour(slot));
        EXPECT_GE(L, 0.1750) << "slot " << slot << " too dark for white";
        EXPECT_LE(L, 0.1834) << "slot " << slot << " too light for black";
    }
}

TEST(ChannelColour, PublicAndUnassignedSlotsGetNone) {
    // slot 0 is the public channel: the default every node shares, so marking it
    // would read as one of the user's own
    EXPECT_EQ(RIFT_CHAN_COL_NONE, riftChannelColour(0));
    // beyond four, no colour rather than a repeat - two channels that look
    // identical can be mistaken for each other, two with no marker cannot
    EXPECT_EQ(RIFT_CHAN_COL_NONE, riftChannelColour(5));
    EXPECT_EQ(RIFT_CHAN_COL_NONE, riftChannelColour(39));
    EXPECT_EQ(RIFT_CHAN_COL_NONE, riftChannelColour(-1));
}

TEST(ChannelColour, AllFourAreDistinctAndNotTheAccent) {
    uint16_t seen[4];
    for (int slot = 1; slot <= 4; slot++) {
        uint16_t c = riftChannelColour(slot);
        for (int j = 0; j < slot - 1; j++) {
            EXPECT_NE(seen[j], c) << "slots " << (j + 1) << " and " << slot << " share a colour";
        }
        seen[slot - 1] = c;
        EXPECT_NE(0xFA00, c) << "slot " << slot << " is the accent";
    }
}

// ------------------------------------------------------------------ civil time
//
// Reference values were computed with calendar.timegm rather than by hand, because
// the whole point of the conversion is that nobody should be doing this arithmetic
// mentally - including while writing its test.

TEST(CivilTime, KnownEpochs) {
    uint32_t e = 0;
    // 1970 and 2000 are outside the supported range by design - see the refusal
    // test - so the earliest value here is the low bound itself
    ASSERT_TRUE(riftEpochFromCivil(2020, 1, 1, 0, 0, &e));  EXPECT_EQ(1577836800u, e);
    ASSERT_TRUE(riftEpochFromCivil(2020, 2, 29, 12, 0, &e)); EXPECT_EQ(1582977600u, e);
    ASSERT_TRUE(riftEpochFromCivil(2026, 8, 20, 14, 32, &e)); EXPECT_EQ(1787236320u, e);
    ASSERT_TRUE(riftEpochFromCivil(2024, 3, 1, 0, 0, &e));  EXPECT_EQ(1709251200u, e);
    ASSERT_TRUE(riftEpochFromCivil(2099, 12, 31, 23, 59, &e)); EXPECT_EQ(4102444740u, e);
}

TEST(CivilTime, RoundTripsBothWays) {
    const uint32_t cases[] = { 1577836800u, 1582977600u, 1787236320u, 4102444740u };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int y, mo, d, h, mi;
        riftCivilFromEpoch(cases[i], &y, &mo, &d, &h, &mi);
        uint32_t back = 0;
        ASSERT_TRUE(riftEpochFromCivil(y, mo, d, h, mi, &back)) << "case " << i;
        EXPECT_EQ(cases[i], back) << "case " << i;
    }
}

TEST(CivilTime, LeapYears) {
    EXPECT_EQ(29, riftDaysInMonth(2024, 2));   // divisible by 4
    EXPECT_EQ(28, riftDaysInMonth(2100, 2));   // divisible by 100, not 400
    EXPECT_EQ(29, riftDaysInMonth(2000, 2));   // divisible by 400
    EXPECT_EQ(28, riftDaysInMonth(2023, 2));
    uint32_t e = 0;
    EXPECT_TRUE (riftEpochFromCivil(2024, 2, 29, 0, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2023, 2, 29, 0, 0, &e));
}

TEST(CivilTime, ImpossibleDatesAreRefusedNotNormalised) {
    // a typo turned into a different date is worse than a refusal, because the user
    // cannot see it happen
    uint32_t e = 0xDEADBEEF, before = e;
    EXPECT_FALSE(riftEpochFromCivil(2026, 13, 1, 0, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2026, 0, 1, 0, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2026, 4, 31, 0, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2026, 1, 0, 0, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2026, 1, 1, 24, 0, &e));
    EXPECT_FALSE(riftEpochFromCivil(2026, 1, 1, 0, 60, &e));
    EXPECT_FALSE(riftEpochFromCivil(2019, 1, 1, 0, 0, &e));   // below the unset threshold
    EXPECT_FALSE(riftEpochFromCivil(2100, 1, 1, 0, 0, &e));
    EXPECT_EQ(before, e) << "a refused conversion must not write to the output";
}

TEST(CivilTime, ParserIsStrict) {
    uint32_t e = 0;
    ASSERT_TRUE(riftParseCivil("2026-08-20 14:32", &e));
    EXPECT_EQ(1787236320u, e);

    EXPECT_FALSE(riftParseCivil("2026-8-20 14:32", &e));    // one-digit month
    EXPECT_FALSE(riftParseCivil("2026-08-20 14:32 ", &e));  // trailing space
    EXPECT_FALSE(riftParseCivil("2026-08-20T14:32", &e));   // wrong separator
    EXPECT_FALSE(riftParseCivil("2026-08-20 14.32", &e));
    EXPECT_FALSE(riftParseCivil("2026-08-20", &e));         // no time
    EXPECT_FALSE(riftParseCivil("", &e));
    EXPECT_FALSE(riftParseCivil("abcd-ef-gh ij:kl", &e));
    EXPECT_FALSE(riftParseCivil(NULL, &e));
    EXPECT_FALSE(riftParseCivil("2026-02-30 00:00", &e));   // validation still applies
}

// ------------------------------------------------------------- packet decoding

TEST(PacketNames, HeaderSplitsIntoRouteAndType) {
    // MeshCore packs the route in bits 0-1 and the payload type in bits 2-5. An
    // ADVERT (0x04) sent as flood (0x01) is therefore 0x11.
    EXPECT_EQ(0x04, riftHeaderPayloadType(0x11));
    EXPECT_EQ(0x01, riftHeaderRouteType(0x11));
    // and a direct TXT_MSG (0x02, route 0x02) is 0x0A
    EXPECT_EQ(0x02, riftHeaderPayloadType(0x0A));
    EXPECT_EQ(0x02, riftHeaderRouteType(0x0A));
    // bits above 5 belong to neither and must not leak into either
    EXPECT_EQ(0x04, riftHeaderPayloadType(0xD1));
    EXPECT_EQ(0x01, riftHeaderRouteType(0xD1));
}

TEST(PacketNames, EveryDefinedTypeHasAName) {
    // The table is the kind of thing an off-by-one hides in, and a reading of the
    // screen would not catch a shifted row - every value would still show *a* name.
    EXPECT_STREQ("REQ",     riftPayloadTypeName(0x00));
    EXPECT_STREQ("RESP",    riftPayloadTypeName(0x01));
    EXPECT_STREQ("TXT",     riftPayloadTypeName(0x02));
    EXPECT_STREQ("ACK",     riftPayloadTypeName(0x03));
    EXPECT_STREQ("ADVERT",  riftPayloadTypeName(0x04));
    EXPECT_STREQ("GRP TXT", riftPayloadTypeName(0x05));
    EXPECT_STREQ("GRP DAT", riftPayloadTypeName(0x06));
    EXPECT_STREQ("ANONREQ", riftPayloadTypeName(0x07));
    EXPECT_STREQ("PATH",    riftPayloadTypeName(0x08));
    EXPECT_STREQ("TRACE",   riftPayloadTypeName(0x09));
    EXPECT_STREQ("MULTI",   riftPayloadTypeName(0x0A));
    EXPECT_STREQ("CONTROL", riftPayloadTypeName(0x0B));
    EXPECT_STREQ("RAW",     riftPayloadTypeName(0x0F));
    // the gaps are reserved and must read as unknown rather than as a neighbour
    EXPECT_STREQ("?", riftPayloadTypeName(0x0C));
    EXPECT_STREQ("?", riftPayloadTypeName(0x0E));
}

TEST(PacketNames, RouteNamesFitTheColumn) {
    EXPECT_STREQ("TF", riftRouteTypeName(0x00));
    EXPECT_STREQ("F",  riftRouteTypeName(0x01));
    EXPECT_STREQ("D",  riftRouteTypeName(0x02));
    EXPECT_STREQ("TD", riftRouteTypeName(0x03));
    // masked, so a whole header can be passed by mistake without producing rubbish
    EXPECT_STREQ("F",  riftRouteTypeName(0x11));
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

// ---------------------------------------------------------------- hop buckets

TEST(HopBucket, FixedRangesDirectOneTwoThreeFiveSixPlus) {
    EXPECT_EQ(RIFT_HOPB_DIRECT, riftHopBucket(0));
    EXPECT_EQ(RIFT_HOPB_1_2,    riftHopBucket(1));
    EXPECT_EQ(RIFT_HOPB_1_2,    riftHopBucket(2));
    EXPECT_EQ(RIFT_HOPB_3_5,    riftHopBucket(3));
    EXPECT_EQ(RIFT_HOPB_3_5,    riftHopBucket(5));
    EXPECT_EQ(RIFT_HOPB_6PLUS,  riftHopBucket(6));
    EXPECT_EQ(RIFT_HOPB_6PLUS,  riftHopBucket(7));
    EXPECT_EQ(RIFT_HOPB_6PLUS,  riftHopBucket(63));
}

// The four columns this replaces failed by putting almost everything in one. The
// fix is not to move the boundaries to suit the current mesh - a column whose
// meaning shifts is unreadable - so the mapping must not depend on the population.
// Measured live: 13 of 16 nodes beyond 2 hops, max 7.
TEST(HopBucket, DoesNotAdaptToTheNetwork) {
    // the same input gives the same bucket regardless of anything else
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(RIFT_HOPB_3_5,   riftHopBucket(4));
        EXPECT_EQ(RIFT_HOPB_6PLUS, riftHopBucket(9));
    }
    // and the boundaries are where the labels say they are
    EXPECT_STREQ("1-2", riftHopBucketLabel(riftHopBucket(2)));
    EXPECT_STREQ("3-5", riftHopBucketLabel(riftHopBucket(3)));
    EXPECT_STREQ("3-5", riftHopBucketLabel(riftHopBucket(5)));
    EXPECT_STREQ("6+",  riftHopBucketLabel(riftHopBucket(6)));
}

// A stored contact with no learned route carries path_len 0xFF - "flood, route
// unknown". riftHopCount masks bits 0-5, so decoding it yields 63 and a node with
// no route at all gets filed at the far edge of the mesh. That was built and
// reverted once; ask riftHopsUnknown() before decoding.
TEST(HopBucket, UnknownIsABucketAndNotSixtyThree) {
    EXPECT_TRUE(riftHopsUnknown(RIFT_PATH_UNKNOWN));
    EXPECT_FALSE(riftHopsUnknown(0));
    EXPECT_FALSE(riftHopsUnknown(7));

    EXPECT_EQ(RIFT_HOPB_UNKNOWN, riftHopBucket(-1));
    EXPECT_STREQ("?", riftHopBucketLabel(RIFT_HOPB_UNKNOWN));

    // and it must not be confused with the far end of the mesh
    EXPECT_NE(riftHopBucket(-1), riftHopBucket(63));
}

TEST(HopBucket, EveryBucketHasALabelAndNoneRepeat) {
    for (int b = 0; b < RIFT_HOPB_COUNT; b++) {
        const char* l = riftHopBucketLabel(b);
        ASSERT_NE(nullptr, l);
        EXPECT_GT(strlen(l), 0u);
        for (int o = 0; o < b; o++) {
            EXPECT_STRNE(l, riftHopBucketLabel(o)) << "buckets " << b << " and " << o;
        }
    }
}

// ---------------------------------------------------------------- conversations

static const uint8_t PEER_A[6] = { 1,2,3,4,5,6 };
static const uint8_t PEER_B[6] = { 1,2,3,4,5,7 };   // differs in the last byte only

TEST(ConvKey, ChannelsMatchByIndex) {
    EXPECT_TRUE(riftConvSame(ch(0), ch(0)));
    EXPECT_FALSE(riftConvSame(ch(0), ch(1)));
}

TEST(ConvKey, DmsMatchOnTheWholePrefix) {
    EXPECT_TRUE(riftConvSame(riftConvDM(PEER_A), riftConvDM(PEER_A)));
    EXPECT_FALSE(riftConvSame(riftConvDM(PEER_A), riftConvDM(PEER_B)));
}

TEST(ConvKey, KindsNeverMatchEachOther) {
    EXPECT_FALSE(riftConvSame(ch(0), riftConvDM(PEER_A)));
    EXPECT_FALSE(riftConvSame(ch(2), riftConvUnknown()));
}

// Entries restored from a pre-v2 log all carry unknown. Treating those as one
// conversation would collect an entire history into a single fake bucket - and make
// the eviction below judge it the largest and empty it first.
TEST(ConvKey, TwoUnknownsAreNotTheSameConversation) {
    EXPECT_FALSE(riftConvSame(riftConvUnknown(), riftConvUnknown()));
}

TEST(ConvKey, NullPeerIsUnknownRatherThanAZeroDm) {
    RiftConvKey k = riftConvDM(NULL);
    EXPECT_EQ(RIFT_CONV_UNKNOWN, k.kind);
}

TEST(LargestConv, PicksTheBusiestConversation) {
    RiftConvKey keys[6] = {
        ch(0), ch(0), ch(0),
        riftConvDM(PEER_A), ch(1), riftConvDM(PEER_A)
    };
    RiftConvKey out = riftConvUnknown();
    EXPECT_EQ(3, riftLargestConv(keys, 6, &out));
    EXPECT_TRUE(riftConvSame(out, ch(0)));
}

// The case the whole change exists for: one loud channel and one quiet DM. The DM
// must not be what gets dropped.
TEST(LargestConv, ALoudChannelIsChosenOverAQuietDm) {
    RiftConvKey keys[8];
    for (int i = 0; i < 7; i++) keys[i] = ch(0);
    keys[7] = riftConvDM(PEER_A);
    RiftConvKey out = riftConvUnknown();
    EXPECT_EQ(7, riftLargestConv(keys, 8, &out));
    EXPECT_TRUE(riftConvSame(out, ch(0)));
    EXPECT_FALSE(riftConvSame(out, riftConvDM(PEER_A)));
}

TEST(LargestConv, AllUnknownFallsBackRatherThanPickingOne) {
    RiftConvKey keys[3] = { riftConvUnknown(), riftConvUnknown(), riftConvUnknown() };
    RiftConvKey out = ch(9);
    EXPECT_EQ(0, riftLargestConv(keys, 3, &out));
    EXPECT_EQ(RIFT_CONV_UNKNOWN, out.kind);
}

TEST(LargestConv, DegenerateInputs) {
    RiftConvKey out = riftConvUnknown();
    RiftConvKey one = ch(0);
    EXPECT_EQ(0, riftLargestConv(NULL, 3, &out));
    EXPECT_EQ(0, riftLargestConv(&one, 0, &out));
    EXPECT_EQ(0, riftLargestConv(&one, 1, NULL));
}

TEST(EvictIndex, DropsTheOldestOfTheBusiestConversation) {
    // three of channel 0 at the front, then a DM, then more channel 0. The oldest
    // channel-0 entry is index 0 here, which happens to also be the oldest overall.
    RiftConvKey keys[6] = {
        ch(0), ch(0), riftConvDM(PEER_A),
        ch(0), ch(1), riftConvDM(PEER_A)
    };
    EXPECT_EQ(0, riftEvictIndex(keys, 6));
}

// The case the whole change exists for. The DM is the oldest entry in the log, so
// age alone would drop it and the conversation would open empty - while the channel
// it lost to has five more of its own to spare.
TEST(EvictIndex, AQuietDmSurvivesALoudChannel) {
    RiftConvKey keys[6] = {
        riftConvDM(PEER_A),  ch(0), ch(0),
        ch(0),  ch(0), ch(0)
    };
    EXPECT_EQ(1, riftEvictIndex(keys, 6));   // oldest channel entry, not the DM
}

TEST(EvictIndex, AllUnknownFallsBackToOldest) {
    RiftConvKey keys[4] = { riftConvUnknown(), riftConvUnknown(),
                            riftConvUnknown(), riftConvUnknown() };
    EXPECT_EQ(0, riftEvictIndex(keys, 4));
}

// No conversation holds more than one, so there is nothing dominant to take from.
// Age is blunt; picking whichever key the scan reached first would be arbitrary.
TEST(EvictIndex, NoDominantConversationFallsBackToOldest) {
    RiftConvKey keys[4] = { riftConvDM(PEER_A), ch(1),
                            ch(2), riftConvDM(PEER_B) };
    EXPECT_EQ(0, riftEvictIndex(keys, 4));
}

// A version 1 history still in the log must not become one giant bucket that the
// rule then empties ahead of live traffic.
TEST(EvictIndex, MigratedEntriesAreNotOneBucket) {
    RiftConvKey keys[6] = {
        riftConvUnknown(), riftConvUnknown(), riftConvUnknown(),
        riftConvUnknown(), ch(0), ch(0)
    };
    // channel 0 holds two, the unknowns hold one each, so the channel is the
    // largest and its oldest goes - the migrated entries are left alone
    EXPECT_EQ(4, riftEvictIndex(keys, 6));
}

TEST(EvictIndex, DegenerateInputs) {
    RiftConvKey one = ch(0);
    EXPECT_EQ(0, riftEvictIndex(NULL, 4));
    EXPECT_EQ(0, riftEvictIndex(&one, 0));
    EXPECT_EQ(0, riftEvictIndex(&one, 1));
}

// ------------------------------------------------------------------- unread table

TEST(Unread, MarkAndCount) {
    RiftUnread u;
    EXPECT_EQ(0, u.count(ch(0)));
    u.mark(ch(0));
    u.mark(ch(0));
    EXPECT_EQ(2, u.count(ch(0)));
    EXPECT_EQ(0, u.count(ch(1)));
}

TEST(Unread, UnknownIsNotAttributable) {
    RiftUnread u;
    u.mark(riftConvUnknown());
    EXPECT_EQ(0, u.n);
}

TEST(Unread, ClearRemovesOnlyThatConversation) {
    RiftUnread u;
    u.mark(ch(0));
    u.mark(ch(1));
    u.clear(ch(0));
    EXPECT_EQ(0, u.count(ch(0)));
    EXPECT_EQ(1, u.count(ch(1)));
    EXPECT_EQ(1, u.n);
}

TEST(Unread, ClearingSomethingAbsentIsHarmless) {
    RiftUnread u;
    u.mark(ch(3));
    u.clear(ch(9));
    EXPECT_EQ(1, u.n);
    EXPECT_EQ(1, u.count(ch(3)));
}

// The policy the comment describes, which the code did not implement: a further
// message moves a conversation to the newest position. Without it a conversation
// could take a hundred messages and still be the first one evicted.
TEST(Unread, AFurtherMessageMakesAConversationTheNewest) {
    RiftUnread u;
    u.mark(ch(0));
    u.mark(ch(1));
    u.mark(ch(0));            // channel 0 is now the most recent
    ASSERT_EQ(2, u.n);
    EXPECT_TRUE(riftConvSame(u.keys[u.n - 1], ch(0)));
    EXPECT_EQ(2, u.count(ch(0)));
    EXPECT_EQ(1, u.count(ch(1)));
}

TEST(Unread, AFullTableDropsTheLeastRecentlyActive) {
    RiftUnread u;
    for (int i = 0; i < RIFT_UNREAD_MAX; i++) u.mark(ch((uint8_t) i));

    // channel 0 is the oldest by mark order; touching it again must save it
    u.mark(ch(0));
    // channel 1 is now the oldest, so the overflow should take that one
    u.mark(riftConvDM((const uint8_t*) "ABCDEF"));

    EXPECT_EQ(RIFT_UNREAD_MAX, u.n);
    EXPECT_EQ(0, u.count(ch(1)));            // evicted
    EXPECT_EQ(2, u.count(ch(0)));            // saved by being touched
    EXPECT_EQ(1, u.count(riftConvDM((const uint8_t*) "ABCDEF")));
}

TEST(Unread, CountSaturatesRatherThanWrapping) {
    RiftUnread u;
    for (int i = 0; i < 300; i++) u.mark(ch(0));
    EXPECT_EQ(255, u.count(ch(0)));
    EXPECT_EQ(1, u.n);
}

// --------------------------------------------------------------- settings migration

// The reported bug, as a test. A real v0.7 flags byte: day mode on, always-on on,
// radar set to BLE, and bit 4 clear because sound did not exist yet.
TEST(Settings, V1UpgradeKeepsEverythingAndTurnsSoundOn) {
    const uint8_t v07_flags = 1 | 2 | (RIFT_SETTINGS_SRC_BLE << 2);   // 0x0B
    RiftSettings s;
    ASSERT_TRUE(riftDecodeSettings(1, v07_flags, &s));

    EXPECT_TRUE(s.day_mode);                                 // preserved
    EXPECT_TRUE(s.always_on);                                // preserved
    EXPECT_EQ(RIFT_SETTINGS_SRC_BLE, s.radar_src);           // preserved
    EXPECT_TRUE(s.sound_on);                                 // the new default, not the old zero
    EXPECT_TRUE(s.migrated);                                 // and rewritten as v2
}

// A version 1 file with bit 4 *set* still takes the default. It cannot be a user
// choice: nothing that wrote version 1 before v0.8 knew about the bit.
TEST(Settings, V1IgnoresItsOwnSoundBitEitherWay) {
    RiftSettings a, b;
    ASSERT_TRUE(riftDecodeSettings(1, 0, &a));
    ASSERT_TRUE(riftDecodeSettings(1, 16, &b));
    EXPECT_TRUE(a.sound_on);
    EXPECT_TRUE(b.sound_on);
}

TEST(Settings, V2HonoursTheStoredSoundChoice) {
    RiftSettings on, off;
    ASSERT_TRUE(riftDecodeSettings(RIFT_SETTINGS_VERSION, 16, &on));
    ASSERT_TRUE(riftDecodeSettings(RIFT_SETTINGS_VERSION, 0, &off));
    EXPECT_TRUE(on.sound_on);
    EXPECT_FALSE(off.sound_on);
    EXPECT_FALSE(on.migrated);      // already current, so no rewrite
    EXPECT_FALSE(off.migrated);
}

TEST(Settings, AnUnknownVersionIsNotGuessedAt) {
    RiftSettings s;
    s.sound_on = false;
    s.day_mode = false;
    EXPECT_FALSE(riftDecodeSettings(0, 0xFF, &s));
    EXPECT_FALSE(riftDecodeSettings(3, 0xFF, &s));
    EXPECT_FALSE(riftDecodeSettings(200, 0xFF, &s));
    EXPECT_FALSE(s.sound_on);       // untouched, so the caller keeps its defaults
    EXPECT_FALSE(s.day_mode);
    EXPECT_FALSE(riftDecodeSettings(RIFT_SETTINGS_VERSION, 0, NULL));
}

// Radar source 3 is never written. Reading it back as BOTH rather than trusting it
// is the same rule the old inline code had, kept because a corrupt byte should land
// on a working state.
TEST(Settings, RadarSourceThreeIsTreatedAsBoth) {
    RiftSettings s;
    ASSERT_TRUE(riftDecodeSettings(RIFT_SETTINGS_VERSION, (3 << 2), &s));
    EXPECT_EQ(RIFT_SETTINGS_SRC_BOTH, s.radar_src);
}

// The reader and the writer must agree about every bit position. A disagreement here
// would move all four settings at once, which is why they share one pair of functions.
TEST(Settings, EncodeAndDecodeRoundTrip) {
    for (int day = 0; day < 2; day++)
    for (int on = 0; on < 2; on++)
    for (int snd = 0; snd < 2; snd++)
    for (uint8_t src = 0; src <= 2; src++) {
        RiftSettings w;
        w.day_mode = day != 0;
        w.always_on = on != 0;
        w.sound_on = snd != 0;
        w.radar_src = src;
        w.migrated = false;

        RiftSettings r;
        ASSERT_TRUE(riftDecodeSettings(RIFT_SETTINGS_VERSION, riftEncodeSettings(w), &r));
        EXPECT_EQ(w.day_mode, r.day_mode);
        EXPECT_EQ(w.always_on, r.always_on);
        EXPECT_EQ(w.sound_on, r.sound_on);
        EXPECT_EQ(w.radar_src, r.radar_src);
    }
}

// ------------------------------------------- unread as the single source of truth

// The reviewer's first scenario. A message arrives in the conversation already on
// screen: it is marked, the frame showing it clears it, and nothing is left lit.
TEST(UnreadModel, AMessageInTheOpenConversationLeavesNothingLit) {
    RiftUnread u;
    u.mark(ch(0));
    EXPECT_TRUE(u.any());
    u.clear(ch(0));           // what COMMS does on the next render
    EXPECT_FALSE(u.any());
    EXPECT_EQ(0, u.total());
}

// The reviewer's second scenario, and the behaviour change. Dismissing the popup used
// to zero a global counter, marking every conversation read including ones whose
// messages had scrolled past the six rows it shows.
TEST(UnreadModel, DismissingThePreviewDoesNotMarkConversationsRead) {
    RiftUnread u;
    u.mark(ch(0));
    u.mark(riftConvDM((const uint8_t*) "PEER01"));
    ASSERT_EQ(2, u.total());

    u.onPreviewDismissed();

    EXPECT_TRUE(u.any());                  // the dot stays on
    EXPECT_EQ(2, u.total());
    EXPECT_EQ(1, u.count(ch(0)));
    EXPECT_EQ(1, u.count(riftConvDM((const uint8_t*) "PEER01")));
}

// Opening each one in turn is what clears it, which is the defined action that
// replaced dismissal as the way the dot goes out.
TEST(UnreadModel, OpeningEachConversationIsWhatClearsTheDot) {
    RiftUnread u;
    u.mark(ch(0));
    u.mark(riftConvDM((const uint8_t*) "PEER01"));

    u.clear(ch(0));
    EXPECT_TRUE(u.any());                  // the DM is still unread
    u.clear(riftConvDM((const uint8_t*) "PEER01"));
    EXPECT_FALSE(u.any());
}

// A message with no conversation identity cannot be given a row or opened, so it is
// counted rather than dropped - a model that silently loses a notification is worse
// than one that admits it cannot place it.
TEST(UnreadModel, AnUnattributableMessageStillLightsTheDot) {
    RiftUnread u;
    u.mark(riftConvUnknown());
    EXPECT_TRUE(u.any());
    EXPECT_EQ(1, u.total());
    EXPECT_EQ(0, u.n);                     // and occupies no conversation row

    u.onPreviewDismissed();                // the only acknowledgement available
    EXPECT_FALSE(u.any());
}

TEST(UnreadModel, TotalCountsEveryConversationAndSaturates) {
    RiftUnread u;
    u.mark(ch(0));
    u.mark(ch(0));
    u.mark(ch(1));
    u.mark(riftConvUnknown());
    EXPECT_EQ(4, u.total());

    for (int i = 0; i < 300; i++) u.mark(ch(2));
    EXPECT_EQ(255 + 4, u.total());         // per-conversation saturates, total does not wrap
}

// -------------------------------------------------------------- channel identity

// The reported case. Slot 2 holds #privateA, its history is stored, the channel is
// deleted, and a different channel is created in the same slot. The old history must
// not appear under the new one.
TEST(ChannelIdentity, AReusedSlotIsNotTheSameConversation) {
    RiftConvKey a = riftConvChannel(2, riftChannelFingerprint((const uint8_t*) "keyAAAAAAAAAAAAA", 16));
    RiftConvKey b = riftConvChannel(2, riftChannelFingerprint((const uint8_t*) "keyBBBBBBBBBBBBB", 16));
    EXPECT_EQ(2, a.channel_idx);
    EXPECT_EQ(2, b.channel_idx);
    EXPECT_FALSE(riftConvSame(a, b));
}

TEST(ChannelIdentity, TheSameChannelInTheSameSlotStillMatches) {
    RiftConvKey a = riftConvChannel(2, riftChannelFingerprint((const uint8_t*) "keyAAAAAAAAAAAAA", 16));
    RiftConvKey b = riftConvChannel(2, riftChannelFingerprint((const uint8_t*) "keyAAAAAAAAAAAAA", 16));
    EXPECT_TRUE(riftConvSame(a, b));
}

// The same key moved to a different slot is a different conversation. Debatable, and
// this is the deliberate choice: the strip, the colours and the unread dots are all
// keyed on the slot, so treating a move as continuity would put history under a row
// that no longer exists.
TEST(ChannelIdentity, TheSameKeyInADifferentSlotDoesNotMatch) {
    uint32_t fp = riftChannelFingerprint((const uint8_t*) "keyAAAAAAAAAAAAA", 16);
    EXPECT_FALSE(riftConvSame(riftConvChannel(2, fp), riftConvChannel(3, fp)));
}

// An entry restored from a log written before the fingerprint existed cannot prove
// which channel it belonged to, so it matches on the slot alone - as good as it was
// when written, and no better.
TEST(ChannelIdentity, AMissingFingerprintIsNotAWildcard) {
    // This test asserted the opposite until the policy changed, and the policy was
    // wrong: a slot is not an identity. Delete the channel in slot 2, create another,
    // and the old one's history would have been handed to the new one - one person's
    // private conversation displayed under a different channel's name.
    //
    // Records without a fingerprint are loaded as unknown conversations now, so
    // nothing should reach riftConvSame with zero. If anything does, it must fail to
    // match rather than match every channel in that slot.
    RiftConvKey legacy  = riftConvChannel(2, 0);
    RiftConvKey current = riftConvChannel(2, 0xABCDEF01u);
    EXPECT_FALSE(riftConvSame(legacy, current));
    EXPECT_FALSE(riftConvSame(current, legacy));          // and symmetrically
    EXPECT_FALSE(riftConvSame(legacy, riftConvChannel(3, 0)));

    // two legacy records in the same slot are still the same conversation as each
    // other - that much the slot does say
    EXPECT_TRUE(riftConvSame(legacy, riftConvChannel(2, 0)));
}

TEST(ChannelFingerprint, DiffersOnASingleBitOfKey) {
    uint8_t k1[16], k2[16];
    memset(k1, 0, sizeof(k1));
    memset(k2, 0, sizeof(k2));
    k2[15] = 1;
    EXPECT_NE(riftChannelFingerprint(k1, 16), riftChannelFingerprint(k2, 16));
}

TEST(ChannelFingerprint, IsStableAndNeverZero) {
    const uint8_t k[16] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 };
    uint32_t a = riftChannelFingerprint(k, 16);
    EXPECT_EQ(a, riftChannelFingerprint(k, 16));
    EXPECT_NE(0u, a);
    EXPECT_EQ(0u, riftChannelFingerprint(NULL, 16));     // degenerate, and 0 says so
    EXPECT_EQ(0u, riftChannelFingerprint(k, 0));
}

// 128-bit and 256-bit keys are hashed over their real length, so the same 16 bytes
// followed by zeroes is not the same channel as those 16 bytes alone.
TEST(ChannelFingerprint, LengthIsPartOfTheInput) {
    uint8_t k[32];
    memset(k, 0, sizeof(k));
    for (int i = 0; i < 16; i++) k[i] = (uint8_t) (i + 1);
    EXPECT_NE(riftChannelFingerprint(k, 16), riftChannelFingerprint(k, 32));
}

// ---- Repeater status decoding ---------------------------------------------
//
// The reply is a foreign struct copied out of another device's memory, and it
// has grown across firmware versions. These cover the short forms an older
// repeater sends, because that is where a struct copy would have read past the
// end and shown invented numbers.

namespace {

// Builds a reply the way a repeater does: four-byte tag, then the struct.
// `stats_len` truncates it to what an older firmware would have sent.
std::vector<uint8_t> makeStatsReply(int stats_len) {
  std::vector<uint8_t> r;
  for (int i = 0; i < 4; i++) r.push_back(0xAA);   // tag
  auto u16 = [&](uint16_t v) { r.push_back(v & 0xFF); r.push_back(v >> 8); };
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; i++) r.push_back((v >> (8 * i)) & 0xFF);
  };
  u16(4100);          // batt_milli_volts
  u16(3);             // curr_tx_queue_len
  u16((uint16_t)(int16_t)-118);  // noise_floor
  u16((uint16_t)(int16_t)-92);   // last_rssi
  u32(120000);        // n_packets_recv
  u32(80000);         // n_packets_sent
  u32(3600);          // total_air_time_secs
  u32(864000);        // total_up_time_secs  (10 days)
  u32(11);            // n_sent_flood
  u32(22);            // n_sent_direct
  u32(33);            // n_recv_flood
  u32(44);            // n_recv_direct
  u16(7);             // err_events
  u16((uint16_t)(int16_t)-26); // last_snr x4  (-6.5 dB)
  u16(55);            // n_direct_dups
  u16(66);            // n_flood_dups
  u32(1800);          // total_rx_air_time_secs
  u32(9);             // n_recv_errors
  r.resize(4 + stats_len);
  return r;
}

}  // namespace

TEST(RepeaterStats, DecodesEveryFieldOfAFullReply) {
  auto r = makeStatsReply(RIFT_STATS_FULL);
  RiftRepeaterStats s;
  ASSERT_TRUE(riftDecodeRepeaterStats(r.data(), (int) r.size(), &s));

  EXPECT_EQ(4100, s.batt_milli_volts);
  EXPECT_EQ(3, s.tx_queue_len);
  EXPECT_EQ(-118, s.noise_floor);
  EXPECT_EQ(-92, s.last_rssi);
  EXPECT_EQ(120000u, s.packets_recv);
  EXPECT_EQ(80000u, s.packets_sent);
  EXPECT_EQ(3600u, s.air_time_secs);
  EXPECT_EQ(864000u, s.up_time_secs);
  EXPECT_EQ(11u, s.sent_flood);
  EXPECT_EQ(22u, s.sent_direct);
  EXPECT_EQ(33u, s.recv_flood);
  EXPECT_EQ(44u, s.recv_direct);
  EXPECT_EQ(7, s.err_events);
  EXPECT_EQ(-26, s.last_snr_x4);
  EXPECT_EQ(55, s.direct_dups);
  EXPECT_EQ(66, s.flood_dups);
  EXPECT_EQ(1800u, s.rx_air_time_secs);
  EXPECT_EQ(9u, s.recv_errors);
  EXPECT_TRUE(s.have_dups);
  EXPECT_TRUE(s.have_rx_air);
}

TEST(RepeaterStats, AShortReplyLeavesTheFieldsItDidNotCarryFlaggedAbsent) {
  auto r = makeStatsReply(RIFT_STATS_MIN);
  RiftRepeaterStats s;
  ASSERT_TRUE(riftDecodeRepeaterStats(r.data(), (int) r.size(), &s));

  // present in every version
  EXPECT_EQ(864000u, s.up_time_secs);
  EXPECT_EQ(-26, s.last_snr_x4);

  // A struct copy would have shown whatever followed the reply here.
  EXPECT_FALSE(s.have_dups);
  EXPECT_FALSE(s.have_rx_air);
  EXPECT_EQ(0, s.direct_dups);
  EXPECT_EQ(0u, s.rx_air_time_secs);
}

TEST(RepeaterStats, TheMiddleTierIsReportedWithoutTheLastOne) {
  auto r = makeStatsReply(RIFT_STATS_DUPS);
  RiftRepeaterStats s;
  ASSERT_TRUE(riftDecodeRepeaterStats(r.data(), (int) r.size(), &s));
  EXPECT_TRUE(s.have_dups);
  EXPECT_EQ(55, s.direct_dups);
  EXPECT_EQ(66, s.flood_dups);
  EXPECT_FALSE(s.have_rx_air);
  EXPECT_EQ(0u, s.recv_errors);
}

TEST(RepeaterStats, ARelpyShorterThanTheCommonFieldsIsRefused) {
  RiftRepeaterStats s;
  for (int stats_len = 0; stats_len < RIFT_STATS_MIN; stats_len++) {
    auto r = makeStatsReply(stats_len);
    EXPECT_FALSE(riftDecodeRepeaterStats(r.data(), (int) r.size(), &s))
        << "accepted a " << stats_len << "-byte struct";
  }
}

TEST(RepeaterStats, NullArgumentsAreRefused) {
  auto r = makeStatsReply(RIFT_STATS_FULL);
  RiftRepeaterStats s;
  EXPECT_FALSE(riftDecodeRepeaterStats(NULL, (int) r.size(), &s));
  EXPECT_FALSE(riftDecodeRepeaterStats(r.data(), (int) r.size(), NULL));
}

TEST(RepeaterStats, ALongerReplyFromNewerFirmwareStillDecodes) {
  // Forward compatibility: the struct grows again and we must read what we know
  // rather than refuse the whole reply.
  auto r = makeStatsReply(RIFT_STATS_FULL);
  for (int i = 0; i < 16; i++) r.push_back(0x5A);
  RiftRepeaterStats s;
  ASSERT_TRUE(riftDecodeRepeaterStats(r.data(), (int) r.size(), &s));
  EXPECT_EQ(864000u, s.up_time_secs);
  EXPECT_TRUE(s.have_rx_air);
  EXPECT_EQ(9u, s.recv_errors);
}

TEST(FormatDuration, PicksTheLargestUnitPresent) {
  char b[16];
  riftFormatDuration(45, b, sizeof(b));        EXPECT_STREQ("45s", b);
  riftFormatDuration(90, b, sizeof(b));        EXPECT_STREQ("1m30s", b);
  riftFormatDuration(3600, b, sizeof(b));      EXPECT_STREQ("1h00m", b);
  riftFormatDuration(3600 + 1830, b, sizeof(b)); EXPECT_STREQ("1h30m", b);
  riftFormatDuration(864000, b, sizeof(b));    EXPECT_STREQ("10d00h", b);
  riftFormatDuration(0, b, sizeof(b));         EXPECT_STREQ("0s", b);
}

TEST(FormatDuration, RefusesABufferItCannotFillSafely) {
  char b[4] = { 'x', 'x', 'x', 'x' };
  riftFormatDuration(864000, b, 4);
  EXPECT_STREQ("", b);   // empties rather than truncating to a wrong number
}

TEST(FormatDuration, ASevenCharacterResultFitsTheStatedMinimum) {
  // The guard above claims eight bytes is enough. 999 days is the widest thing
  // a uint32 of seconds can produce in the day branch.
  char b[8];
  riftFormatDuration(4294967295u, b, sizeof(b));
  EXPECT_LT(strlen(b), sizeof(b));
}

TEST(DutyCycle, IsTenthsOfAPercentOfUptime) {
  EXPECT_EQ(0, riftDutyTenths(0, 3600));
  EXPECT_EQ(100, riftDutyTenths(360, 3600));      // 10.0%
  EXPECT_EQ(1000, riftDutyTenths(3600, 3600));    // 100.0%
  EXPECT_EQ(4, riftDutyTenths(1, 240));           // 0.4%
}

TEST(DutyCycle, RejectsRatherThanClampsWhatCannotBeTrue) {
  EXPECT_EQ(-1, riftDutyTenths(3600, 0));         // just booted, not idle
  EXPECT_EQ(-1, riftDutyTenths(3601, 3600));      // airtime over uptime
}

TEST(DutyCycle, DoesNotOverflowOnALongUptime) {
  // air_secs * 1000 exceeds 32 bits above about 50 days of airtime, which a
  // repeater left up for a year reaches.
  EXPECT_EQ(500, riftDutyTenths(15768000u, 31536000u));   // half of a year
}

// ---- CLI secret and destructive-command policy ----------------------------
//
// The reason these are pure functions rather than checks inside the screen: the
// rule has to hold for the command menu and for free text alike, and the first
// version of it only held for the menu.

TEST(CliSecrets, RecognisesEverySecretUpstreamCanEchoBack) {
  EXPECT_TRUE(riftCliIsSecret("password hunter2"));
  EXPECT_TRUE(riftCliIsSecret("set guest.password abc"));
  EXPECT_TRUE(riftCliIsSecret("get guest.password"));
  EXPECT_TRUE(riftCliIsSecret("set prv.key deadbeef"));
  EXPECT_TRUE(riftCliIsSecret("set bridge.secret s3cret"));
  EXPECT_TRUE(riftCliIsSecret("get bridge.secret"));
}

TEST(CliSecrets, LeavesOrdinaryCommandsAlone) {
  EXPECT_FALSE(riftCliIsSecret("advert"));
  EXPECT_FALSE(riftCliIsSecret("neighbors"));
  EXPECT_FALSE(riftCliIsSecret("get freq"));
  EXPECT_FALSE(riftCliIsSecret("clock sync"));
  EXPECT_FALSE(riftCliIsSecret(""));
  EXPECT_FALSE(riftCliIsSecret(NULL));
}

TEST(CliSecrets, CoversAKeyNobodyHasAddedYet) {
  // The suffix rule is the point: a new secret config key upstream is redacted
  // without anyone remembering to edit the list.
  EXPECT_TRUE(riftCliIsSecret("set uplink.password x"));
  EXPECT_TRUE(riftCliIsSecret("set mqtt.secret x"));
  EXPECT_TRUE(riftCliIsSecret("set signing.key x"));
}

TEST(CliRedaction, KeepsTheCommandAndDropsTheValue) {
  char out[64];
  riftRedactCliCommand("password hunter2", out, sizeof(out));
  EXPECT_STREQ("password [redacted]", out);

  riftRedactCliCommand("set bridge.secret s3cret", out, sizeof(out));
  EXPECT_STREQ("set bridge.secret [redacted]", out);
}

TEST(CliRedaction, ReplacesEverythingAfterTheKeyWithOnePlaceholder) {
  // A password with spaces in it must not leak its tail, and must not produce a
  // row of placeholders either.
  char out[64];
  riftRedactCliCommand("password one two three", out, sizeof(out));
  EXPECT_STREQ("password [redacted]", out);
}

TEST(CliRedaction, PassesThroughWhatCarriesNoSecret) {
  char out[64];
  riftRedactCliCommand("get freq", out, sizeof(out));
  EXPECT_STREQ("get freq", out);
  riftRedactCliCommand("advert", out, sizeof(out));
  EXPECT_STREQ("advert", out);
}

TEST(CliRedaction, NeverWritesPastTheBuffer) {
  for (int sz = 1; sz <= 24; sz++) {
    std::vector<char> buf((size_t) sz + 8, '\x7F');
    riftRedactCliCommand("password hunter2", buf.data(), sz);
    EXPECT_LT(strlen(buf.data()), (size_t) sz) << "size " << sz;
    for (int i = 0; i < 8; i++) {
      EXPECT_EQ('\x7F', buf[(size_t) sz + i]) << "overran at size " << sz;
    }
  }
}

TEST(CliRedaction, HandlesNullAndZeroSizeWithoutWriting) {
  char out[8] = { 'k', 0 };
  riftRedactCliCommand(NULL, out, sizeof(out));
  EXPECT_STREQ("", out);
  riftRedactCliCommand("password x", out, 0);   // must not touch it
}

TEST(CliSecrets, RecognisesUpstreamsPasswordEcho) {
  // CommonCLI confirms a change by replying with the new password in clear.
  EXPECT_TRUE(riftCliReplyEchoesSecret("password now: hunter2"));
  EXPECT_FALSE(riftCliReplyEchoesSecret("OK - Advert sent"));
  EXPECT_FALSE(riftCliReplyEchoesSecret(""));
  EXPECT_FALSE(riftCliReplyEchoesSecret(NULL));
}

TEST(DestructiveCli, CatchesTheOnesTypedAsWellAsThePickedOnes) {
  EXPECT_TRUE(riftCliIsDestructive("reboot"));
  EXPECT_TRUE(riftCliIsDestructive("clear stats"));
  EXPECT_TRUE(riftCliIsDestructive("poweroff"));
  EXPECT_TRUE(riftCliIsDestructive("start ota"));
  EXPECT_TRUE(riftCliIsDestructive("clock sync"));
  EXPECT_TRUE(riftCliIsDestructive("erase"));
  EXPECT_TRUE(riftCliIsDestructive("password x"));
  EXPECT_TRUE(riftCliIsDestructive("  reboot"));      // leading spaces
}

TEST(DestructiveCli, LeavesReadOnlyCommandsUnconfirmed) {
  EXPECT_FALSE(riftCliIsDestructive("advert"));
  EXPECT_FALSE(riftCliIsDestructive("neighbors"));
  EXPECT_FALSE(riftCliIsDestructive("ver"));
  EXPECT_FALSE(riftCliIsDestructive("clock"));        // reading is not syncing
  EXPECT_FALSE(riftCliIsDestructive("get freq"));
  EXPECT_FALSE(riftCliIsDestructive(NULL));
}

TEST(ClockPlausible, RefusesAnUnsetClockAndAcceptsARealOne) {
  EXPECT_FALSE(riftClockPlausible(0));
  EXPECT_FALSE(riftClockPlausible(1500000000u));               // July 2017
  EXPECT_TRUE(riftClockPlausible(1787000000u));                // 2026
  EXPECT_FALSE(riftClockPlausible(4200000000u));               // past 2100
}

// ---- name colours ---------------------------------------------------------

TEST(NameColour, IsStableForTheSameName) {
  // The whole point: a person keeps their colour across reboots and devices.
  EXPECT_EQ(riftNameColour("ALPHA"), riftNameColour("ALPHA"));
  EXPECT_EQ(riftNameColour("Bravo-2"), riftNameColour("Bravo-2"));
}

TEST(NameColour, OnlyEverReturnsAVerifiedColour) {
  // Never an unmeasured value: these four are the ones swept for 4.5:1 after
  // RGB565 quantisation against both black and white.
  const char* names[] = { "ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO",
                          "FOXTROT", "a", "zz", "Andre", "OSLO-1" };
  for (const char* n : names) {
    uint16_t c = riftNameColour(n);
    bool known = false;
    for (int i = 0; i < RIFT_NAME_COLOURS; i++) {
      if (c == riftNameColourAt(i)) known = true;
    }
    EXPECT_TRUE(known) << n << " got an unverified colour";
  }
}

TEST(NameColour, DistinguishesNamesThatDifferInOneCharacter) {
  // Not a guarantee - four colours cannot separate every pair - but a hash that
  // ignored the tail would put a whole squad on one colour.
  EXPECT_NE(riftNameColour("NODE-1"), riftNameColour("NODE-3"));
}

TEST(NameColour, UsesTheWholePalette) {
  bool seen[RIFT_NAME_COLOURS] = { false };
  char buf[12];
  for (int i = 0; i < 200; i++) {
    snprintf(buf, sizeof(buf), "node-%d", i);
    uint16_t c = riftNameColour(buf);
    for (int k = 0; k < RIFT_NAME_COLOURS; k++) if (c == riftNameColourAt(k)) seen[k] = true;
  }
  for (int k = 0; k < RIFT_NAME_COLOURS; k++) EXPECT_TRUE(seen[k]) << "colour " << k << " never used";
}

TEST(NameColour, HasNoColourForAnEmptyOrMissingName) {
  EXPECT_EQ(RIFT_CHAN_COL_NONE, riftNameColour(NULL));
  EXPECT_EQ(RIFT_CHAN_COL_NONE, riftNameColour(""));
}

TEST(NameColour, EveryPaletteEntryIsDistinct) {
  // A duplicate would silently halve the palette, which is the complaint this
  // widening exists to answer.
  for (int i = 0; i < RIFT_NAME_COLOURS; i++) {
    for (int j = i + 1; j < RIFT_NAME_COLOURS; j++) {
      EXPECT_NE(riftNameColourAt(i), riftNameColourAt(j)) << i << " and " << j;
    }
  }
}

TEST(NameColour, TheFirstFourAreStillTheChannelColours) {
  // Widening must not repaint the channels: those are on screen in the tab strip
  // and in every outgoing row.
  for (int i = 0; i < 4; i++) EXPECT_EQ(riftChannelColour(i + 1), riftNameColourAt(i));
}

// ---- channel sender prefix ------------------------------------------------

TEST(ChannelSender, TakesTheNameAndPointsAtTheBody) {
  char who[40];
  const char* msg = "ALPHA: heading north";
  int skip = riftChannelSender(msg, who, sizeof(who));
  EXPECT_STREQ("ALPHA", who);
  EXPECT_STREQ("heading north", msg + skip);
}

TEST(ChannelSender, KeepsAColonInsideTheBody) {
  // Only the first colon-space is the separator; the rest is what was said.
  char who[40];
  const char* msg = "BRAVO: eta 14:30, bring water: and rope";
  int skip = riftChannelSender(msg, who, sizeof(who));
  EXPECT_STREQ("BRAVO", who);
  EXPECT_STREQ("eta 14:30, bring water: and rope", msg + skip);
}

TEST(ChannelSender, LeavesAMessageWithNoPrefixAlone) {
  // A node that does not add one must not lose its first word.
  char who[40];
  EXPECT_EQ(0, riftChannelSender("no prefix here", who, sizeof(who)));
  EXPECT_STREQ("", who);
  EXPECT_EQ(0, riftChannelSender("", who, sizeof(who)));
  EXPECT_EQ(0, riftChannelSender(NULL, who, sizeof(who)));
}

TEST(ChannelSender, RefusesSomethingTooLongToBeAName) {
  // Without an upper bound, a colon halfway through a sentence reads as a very
  // long sender and eats the start of the message.
  char who[64];
  EXPECT_EQ(0, riftChannelSender(
      "this is a long sentence that happens to contain: a colon", who, sizeof(who)));
}

TEST(ChannelSender, RefusesAnEmptyNameAndAControlCharacter) {
  char who[40];
  EXPECT_EQ(0, riftChannelSender(": no name", who, sizeof(who)));
  // 10 rather than an escape: an escape in this file has twice now been turned
  // into the character it names on the way in, and silently changed the test.
  const char with_nl[] = { 'A', (char) 10, 'B', ':', ' ', 'x', 0 };
  EXPECT_EQ(0, riftChannelSender(with_nl, who, sizeof(who)));
}

TEST(ChannelSender, TruncatesIntoASmallBufferWithoutOverrunning) {
  char who[4] = { 'x', 'x', 'x', 'x' };
  int skip = riftChannelSender("CHARLIE: hi", who, sizeof(who));
  EXPECT_EQ(9, skip);              // the caller still skips the whole prefix
  EXPECT_STREQ("CHA", who);
  EXPECT_LT(strlen(who), sizeof(who));
}

TEST(ChannelSender, KeepsNordicCharactersInAName) {
  // Names are UTF-8 here; the separator is ASCII, so a multi-byte name survives.
  char who[40];
  const char msg[] = { 'A', (char) 0xC3, (char) 0x85, 'S', ':', ' ', 'h', 'i', 0 };
  int skip = riftChannelSender(msg, who, sizeof(who));
  EXPECT_EQ(6, skip);
  EXPECT_STREQ("hi", msg + skip);
}

// ---- companion command minimum lengths ------------------------------------
//
// The table exists because a per-branch guard has to be remembered, and several
// were not. These are the values, stated once where they can be checked, rather
// than a lint over the parser that could not see cmd_frame[i] at all.

TEST(CmdMinLen, CoversTheHandlersThatReadWithoutGuarding) {
  EXPECT_EQ(9,  companionCmdMinLen(21));   // SET_TUNING_PARAMS: two 32-bit values
  EXPECT_EQ(11, companionCmdMinLen(11));   // SET_RADIO_PARAMS: freq, bw, sf, cr
  EXPECT_EQ(7,  companionCmdMinLen(19));   // REBOOT: memcmp "reboot"
  EXPECT_EQ(6,  companionCmdMinLen(51));   // FACTORY_RESET: memcmp "reset"
  EXPECT_EQ(3,  companionCmdMinLen(61));   // SET_PATH_HASH_MODE
}

TEST(CmdMinLen, DefaultsToNoRestriction) {
  // A minimum larger than a working companion sends is worse than the bug it
  // would fix, so anything not read and justified stays at 1.
  EXPECT_EQ(1, companionCmdMinLen(0));
  EXPECT_EQ(1, companionCmdMinLen(2));     // SEND_TXT_MSG guards in its branch
  EXPECT_EQ(1, companionCmdMinLen(200));   // not a command at all
  EXPECT_EQ(1, companionCmdMinLen(255));
}

TEST(CmdMinLen, EveryEntryIsAtLeastOne) {
  // Zero would mean an empty frame reaches a handler; the dispatcher only calls
  // this when len >= 1, and a zero here would silently undo that.
  for (int c = 0; c < 256; c++) EXPECT_GE(companionCmdMinLen((uint8_t) c), 1);
}

// ---- channel sender, nth delimiter ----------------------------------------

TEST(ChannelSenderNth, WalksTheCandidates) {
  // "Ops: North" is a legal node name, so this message is genuinely ambiguous
  // and only a caller that knows the contacts can settle it.
  const char* msg = "Ops: North: hello";
  char who[40];
  EXPECT_EQ(5,  riftChannelSenderNth(msg, 0, who, sizeof(who)));
  EXPECT_STREQ("Ops", who);
  EXPECT_EQ(12, riftChannelSenderNth(msg, 1, who, sizeof(who)));
  EXPECT_STREQ("Ops: North", who);
  EXPECT_EQ(0,  riftChannelSenderNth(msg, 2, who, sizeof(who)));
}

TEST(ChannelSenderNth, MatchesTheFirstDelimiterAtZero) {
  char a[40], b[40];
  const char* msg = "ALPHA: heading north";
  EXPECT_EQ(riftChannelSender(msg, a, sizeof(a)), riftChannelSenderNth(msg, 0, b, sizeof(b)));
  EXPECT_STREQ(a, b);
}

TEST(ChannelSenderNth, StillRefusesWhatIsNotAName) {
  char who[64];
  // Second candidate is past the name-length bound, so it is not offered.
  EXPECT_EQ(0, riftChannelSenderNth(
      "A: this is a long stretch of words that later contains: a colon", 1, who, sizeof(who)));
  EXPECT_EQ(0, riftChannelSenderNth("no delimiter", 0, who, sizeof(who)));
  EXPECT_EQ(0, riftChannelSenderNth(NULL, 0, who, sizeof(who)));
}

// ---- tropo detector -------------------------------------------------------

namespace {
uint8_t pathByte(int hops, int hash_size_bits = 1) {
  return (uint8_t) ((hash_size_bits << 6) | (hops & 63));
}
}  // namespace

TEST(Tropo, IgnoresTheNoPathSentinel) {
  // 0xFF means "no path recorded" and its low six bits are 63. Compared naively
  // against a hop threshold it fires on every such packet, forever - which is
  // the whole reason this function exists.
  RiftTropo t; riftTropoReset(&t);
  for (int i = 0; i < 100; i++) {
    EXPECT_FALSE(riftTropoStep(&t, 1000u + i, 0xFF));
  }
  EXPECT_FALSE(t.active);
  EXPECT_EQ(0, t.deep_count);
}

TEST(Tropo, IgnoresShallowPaths) {
  RiftTropo t; riftTropoReset(&t);
  for (int i = 0; i < 50; i++) EXPECT_FALSE(riftTropoStep(&t, 1000u + i, pathByte(3)));
  EXPECT_FALSE(t.active);
}

TEST(Tropo, OpensOnceAfterEnoughDeepPackets) {
  RiftTropo t; riftTropoReset(&t);
  uint32_t now = 100000;
  bool opened = false;
  for (int i = 0; i < RIFT_TROPO_NEEDED; i++) {
    opened = riftTropoStep(&t, now + i * 1000, pathByte(RIFT_TROPO_HOPS));
  }
  EXPECT_TRUE(opened);          // the transition, on the packet that reached it
  EXPECT_TRUE(t.active);

  // and not again while it stays open
  for (int i = 0; i < 20; i++) {
    EXPECT_FALSE(riftTropoStep(&t, now + 20000 + i * 1000, pathByte(30)));
  }
  EXPECT_TRUE(t.active);
  EXPECT_EQ(30, t.peak_hops);
}

TEST(Tropo, DoesNotOpenOnDeepPacketsSpreadTooThin) {
  // One deep packet per window is a long path, not an opening.
  RiftTropo t; riftTropoReset(&t);
  uint32_t now = 100000;
  for (int i = 0; i < 10; i++) {
    now += RIFT_TROPO_WINDOW_MS + 1000;
    EXPECT_FALSE(riftTropoStep(&t, now, pathByte(40)));
  }
  EXPECT_FALSE(t.active);
}

TEST(Tropo, ClosesAfterTheHoldAndOnlyOnce) {
  RiftTropo t; riftTropoReset(&t);
  uint32_t now = 100000;
  for (int i = 0; i < RIFT_TROPO_NEEDED; i++) riftTropoStep(&t, now + i, pathByte(25));
  ASSERT_TRUE(t.active);

  EXPECT_FALSE(riftTropoTick(&t, now + RIFT_TROPO_HOLD_MS / 2));   // still open
  EXPECT_TRUE(t.active);

  EXPECT_TRUE(riftTropoTick(&t, now + RIFT_TROPO_HOLD_MS + 1000)); // the transition
  EXPECT_FALSE(t.active);
  EXPECT_FALSE(riftTropoTick(&t, now + RIFT_TROPO_HOLD_MS + 9000)); // not again
}

TEST(Tropo, SurvivesTheMillisWrap) {
  // The same wrap-safe comparison the rest of the firmware uses; a window that
  // straddles the rollover must not look like one that never ends.
  RiftTropo t; riftTropoReset(&t);
  uint32_t now = 0xFFFFF000u;
  for (int i = 0; i < RIFT_TROPO_NEEDED; i++) riftTropoStep(&t, now + i * 100, pathByte(25));
  EXPECT_TRUE(t.active);
  EXPECT_TRUE(riftTropoTick(&t, now + RIFT_TROPO_HOLD_MS + 100000));   // wrapped
  EXPECT_FALSE(t.active);
}

TEST(Tropo, HandlesNullWithoutCrashing) {
  EXPECT_FALSE(riftTropoStep(NULL, 1, 0x20));
  EXPECT_FALSE(riftTropoTick(NULL, 1));
  riftTropoReset(NULL);
}

TEST(Tropo, UsableHopsMasksTheHashSizeBits) {
  // Bits 6-7 carry the hash size, not the count - reading the whole byte was the
  // bug that once showed a two-hop route as "(66)".
  uint8_t hops = 0;
  EXPECT_TRUE(riftTropoUsableHops(pathByte(2, 1), &hops));
  EXPECT_EQ(2, hops);
  EXPECT_TRUE(riftTropoUsableHops(pathByte(2, 3), &hops));
  EXPECT_EQ(2, hops);
  EXPECT_FALSE(riftTropoUsableHops(0xFF, &hops));
}

// ---- flood scope names ----------------------------------------------------

TEST(ScopeName, AcceptsAnOrdinaryRegionName) {
  EXPECT_TRUE(riftScopeNameValid("#oslo"));
  EXPECT_TRUE(riftScopeNameValid("North West"));
  EXPECT_TRUE(riftScopeNameValid("a"));
}

TEST(ScopeName, RefusesWhatWouldMeanNoScope) {
  // Empty means "use the node default" everywhere this is stored, so an empty or
  // blank name must never become a scope in its own right - the two send
  // differently and nothing on screen would distinguish them.
  EXPECT_FALSE(riftScopeNameValid(""));
  EXPECT_FALSE(riftScopeNameValid("   "));
  EXPECT_FALSE(riftScopeNameValid(NULL));
}

TEST(ScopeName, RefusesControlCharactersAndOverlongNames) {
  const char ctrl[] = { 'a', (char) 9, 'b', 0 };
  EXPECT_FALSE(riftScopeNameValid(ctrl));

  char longname[RIFT_SCOPE_NAME_MAX + 8];
  memset(longname, 'x', sizeof(longname) - 1);
  longname[sizeof(longname) - 1] = 0;
  EXPECT_FALSE(riftScopeNameValid(longname));

  // The longest that still fits with its terminator is accepted.
  char fits[RIFT_SCOPE_NAME_MAX];
  memset(fits, 'x', sizeof(fits) - 1);
  fits[sizeof(fits) - 1] = 0;
  EXPECT_TRUE(riftScopeNameValid(fits));
}

// ---- drag to steps --------------------------------------------------------

TEST(DragSteps, CarriesTheRemainderSoASlowDragStillMoves) {
  // A finger moving a pixel per report would never reach a step if the remainder
  // were dropped, which is what "touch does nothing" would look like.
  int r = 0, total = 0;
  for (int i = 0; i < 16; i++) total += riftDragSteps(&r, 1, 16);
  EXPECT_EQ(-1, total);
}

TEST(DragSteps, HandlesAFastDragInOneReport) {
  int r = 0;
  EXPECT_EQ(-3, riftDragSteps(&r, 48, 16));
  EXPECT_EQ(0, r);
}

TEST(DragSteps, DraggingUpGoesForward) {
  int r = 0;
  EXPECT_EQ(2, riftDragSteps(&r, -32, 16));
}

TEST(DragSteps, KeepsTheSignOfAPartialMoveInBothDirections) {
  // Truncation toward zero matters: a -15 remainder followed by -1 must step,
  // and must not be rounded away by an integer division that floors.
  int r = 0;
  EXPECT_EQ(0, riftDragSteps(&r, -15, 16));
  EXPECT_EQ(1, riftDragSteps(&r, -1, 16));

  r = 0;
  EXPECT_EQ(0, riftDragSteps(&r, 15, 16));
  EXPECT_EQ(-1, riftDragSteps(&r, 1, 16));
}

TEST(DragSteps, RefusesNullAndANonsensePitch) {
  int r = 0;
  EXPECT_EQ(0, riftDragSteps(NULL, 100, 16));
  EXPECT_EQ(0, riftDragSteps(&r, 100, 0));
  EXPECT_EQ(0, riftDragSteps(&r, 100, -4));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
