#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>

// Included by relative path on purpose, the same way test_rift_logic does it: this
// keeps the native test environments exactly as upstream has them. RiftRepeater.h
// is standalone - RiftLogic.h, the Arduino mock's millis(), and one inline helper
// from helpers/UTF8Helpers.h.
#include "../../examples/companion_radio/ui-rift/RiftRepeater.h"

// ------------------------------------------------------- withholding a CLI secret
//
// The repeater panel replaced a secret reply with a placeholder, but the air log had
// already stored the raw text on the way past: `get guest.password` showed
// "(secret reply not shown)" on the panel and the password itself on AIR. The
// classification is one question now, asked before anything keeps the text.

static void key6(uint8_t* out, uint8_t fill) {
    memset(out, fill, RIFT_REP_KEY_LEN);
}

TEST(CliSecret, ReplyIsWithheldWhileTheReadIsOutstanding) {
    RiftRepeaterSession rep;
    uint8_t key[RIFT_REP_KEY_LEN]; key6(key, 0xAB);

    g_mock_millis = 1000;
    rep.setTarget(key);
    rep.noteCliSent("get guest.password");

    EXPECT_TRUE(rep.replyIsSecret(key, "secret123"));
}

TEST(CliSecret, TheWindowExpires) {
    RiftRepeaterSession rep;
    uint8_t key[RIFT_REP_KEY_LEN]; key6(key, 0xAB);

    g_mock_millis = 1000;
    rep.setTarget(key);
    rep.noteCliSent("get guest.password");
    g_mock_millis = 1000 + 30000;   // the window is 30s, and riftDue() is >=

    EXPECT_FALSE(rep.replyIsSecret(key, "ordinary reply"));
}

TEST(CliSecret, AnEchoedPasswordIsSecretWhicheverNodeSentIt) {
    // The window belongs to the node we asked. This test does not open one at all:
    // upstream echoes a new password back unasked, and that text must be withheld
    // whether or not it came from the panel's current target.
    RiftRepeaterSession rep;
    uint8_t stranger[RIFT_REP_KEY_LEN]; key6(stranger, 0x11);

    EXPECT_TRUE(rep.replyIsSecret(stranger, "password now: hunter2"));
}

TEST(CliSecret, AnOrdinaryReplyIsShown) {
    RiftRepeaterSession rep;
    uint8_t key[RIFT_REP_KEY_LEN]; key6(key, 0xAB);

    g_mock_millis = 1000;
    rep.setTarget(key);
    rep.noteCliSent("neighbors");

    EXPECT_FALSE(rep.replyIsSecret(key, "3 neighbours"));
    EXPECT_FALSE(rep.replyIsSecret(key, NULL));
}

TEST(CliSecret, ThePanelAndTheQuestionAgree) {
    // The panel's own redaction now comes from the same call, so the air log cannot
    // be told one thing while the panel shows another.
    RiftRepeaterSession rep;
    uint8_t key[RIFT_REP_KEY_LEN]; key6(key, 0xAB);

    g_mock_millis = 1000;
    rep.setTarget(key);
    rep.noteCliSent("get guest.password");

    const bool withheld = rep.replyIsSecret(key, "secret123");
    rep.onCliReply(key, "secret123");

    EXPECT_TRUE(withheld);
    ASSERT_GT(rep.cliCount(), 0);
    EXPECT_STREQ(RIFT_CLI_SECRET_SHOWN, rep.cliLine(0));

    // and the secret is in no buffer the panel keeps
    for (int i = 0; i < rep.cliCount(); i++) {
        EXPECT_EQ(nullptr, strstr(rep.cliLine(i), "secret123")) << "line " << i;
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
