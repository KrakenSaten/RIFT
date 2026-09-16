#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <memory>

// Included by relative path on purpose, the same way test_rift_logic does it: this
// keeps the native test environments exactly as upstream has them. The header is
// standalone - RiftLogic.h and the Arduino mock's millis(), nothing to link.
#include "../../examples/companion_radio/ui-rift/RiftRxLog.h"

// ------------------------------------------------- which row an annotation lands on
//
// The air log used to annotate whatever row was newest, on the reasoning that the
// Dispatcher logs a frame and hands it up inside the same call. That holds only when
// nothing is held in between. With a receive delay configured (rxdelay), a flood
// packet is logged and queued for up to 32 seconds, other packets are logged while it
// waits, and the decode then wrote the held packet's sender and text onto somebody
// else's signal measurements and timestamp.
//
// A plain int stands in for a Packet here: the log only ever compares the address,
// which is what makes the association safe - packets come from a fixed pool and are
// never reallocated.

static std::unique_ptr<RiftRxLog> freshLog() {
  // On the heap: the ring is about twelve kilobytes, which is more than a test
  // frame wants to carry.
  return std::unique_ptr<RiftRxLog>(new RiftRxLog());
}

TEST(AirLogRow, AnnotationFollowsTheHeldPacketNotTheNewestRow) {
    auto log = freshLog();
    int pktA = 0, pktB = 0;

    log->add(5.0f, -90.0f, 0x12, 0, 40);   // A is heard, and held for its score delay
    log->bindPacket(&pktA);
    log->add(4.0f, -95.0f, 0x12, 0, 60);   // B is heard and decoded while A waits
    log->bindPacket(&pktB);
    log->beginDecode(&pktB);
    log->annotateLast(RIFT_AIR_K_ADVERT, "bravo", NULL);
    log->endDecode();

    log->beginDecode(&pktA);               // A's delay expires
    log->annotateLast(RIFT_AIR_K_DM, "alpha", "hei");
    log->endDecode();

    EXPECT_STREQ("bravo", log->peek(0)->who);    // B's row is untouched
    EXPECT_STREQ("",      log->peek(0)->text);
    EXPECT_STREQ("alpha", log->peek(1)->who);    // and A's text is on A's row
    EXPECT_STREQ("hei",   log->peek(1)->text);
    EXPECT_EQ(40, (int) log->peek(1)->len);      // with A's length beside it
    EXPECT_EQ(60, (int) log->peek(0)->len);
}

TEST(AirLogRow, AnnotationIsNotDroppedWhenATransmitLandsInBetween) {
    // The worse half of the same fault: annotateLast() refuses to write to a
    // transmit row, so when the newest row was a transmit the held packet's
    // identity was discarded rather than misfiled.
    auto log = freshLog();
    int pktA = 0;

    log->add(5.0f, -90.0f, 0x12, 0, 40);
    log->bindPacket(&pktA);
    log->addTx(true, 120, 0x12, 0, 30);

    log->beginDecode(&pktA);
    log->annotateLast(RIFT_AIR_K_DM, "alpha", "hei");
    log->endDecode();

    EXPECT_EQ(RIFT_AIR_TX, (int) log->peek(0)->dir);
    EXPECT_STREQ("", log->peek(0)->who);
    EXPECT_STREQ("alpha", log->peek(1)->who);
    EXPECT_EQ(RIFT_AIR_K_DM, (int) log->peek(1)->kind);
}

TEST(AirLogRow, ScopeGoesToTheSameRowAsTheAnnotation) {
    // riftNoteScope() runs in the same decode handlers and had the same fault.
    auto log = freshLog();
    int pktA = 0;

    log->add(5.0f, -90.0f, 0x12, 0, 40);
    log->bindPacket(&pktA);
    log->add(4.0f, -95.0f, 0x12, 0, 60);

    log->beginDecode(&pktA);
    log->setLastScope("#nord");
    log->endDecode();

    EXPECT_STREQ("#nord", log->peek(1)->scope);
    EXPECT_STREQ("",      log->peek(0)->scope);
}

TEST(AirLogRow, WithNothingHeldTheNewestRowIsStillTheRightAnswer) {
    // The default configuration: rx_delay_base is 0, so the Dispatcher decodes
    // inside the same call that logged, and nothing is ever bound. This is the
    // behaviour that must not change.
    auto log = freshLog();
    log->add(5.0f, -90.0f, 0x12, 0, 40);
    log->annotateLast(RIFT_AIR_K_ADVERT, "alpha", NULL);
    EXPECT_STREQ("alpha", log->peek(0)->who);

    // and explicitly decoding a packet nobody bound - a loopback from
    // importContact(), say - falls back to the same answer
    auto log2 = freshLog();
    int stranger = 0;
    log2->add(5.0f, -90.0f, 0x12, 0, 40);
    log2->beginDecode(&stranger);
    log2->annotateLast(RIFT_AIR_K_ADVERT, "alpha", NULL);
    log2->endDecode();
    EXPECT_STREQ("alpha", log2->peek(0)->who);
}

TEST(AirLogRow, ARowOverwrittenWhileItsPacketWaitedIsNotAnnotated) {
    // Ring-slot reuse. A row number names a row for as long as that row exists and
    // never afterwards, so a packet held long enough for its row to be overwritten
    // annotates nothing rather than stamping its identity onto the row that took
    // its slot.
    auto log = freshLog();
    int pktA = 0;

    log->add(1.0f, -90.0f, 0x12, 0, 40);
    log->bindPacket(&pktA);
    for (int i = 0; i < RIFT_RX_LOG_LINES; i++) {
        log->add(1.0f, -90.0f, 0x12, 0, 50);
    }

    log->beginDecode(&pktA);
    log->annotateLast(RIFT_AIR_K_DM, "alpha", "hei");
    log->endDecode();

    for (int i = 0; i < log->count; i++) {
        EXPECT_STREQ("", log->peek(i)->who) << "row " << i << " was claimed by a packet that aged out";
        EXPECT_STREQ("", log->peek(i)->text);
    }
}

TEST(AirLogRow, EachHeldPacketGetsItsOwnRow) {
    // Several packets in flight at once, released out of order.
    auto log = freshLog();
    int a = 0, b = 0, c = 0;

    log->add(1.0f, -90.0f, 0x12, 0, 41); log->bindPacket(&a);
    log->add(1.0f, -90.0f, 0x12, 0, 42); log->bindPacket(&b);
    log->add(1.0f, -90.0f, 0x12, 0, 43); log->bindPacket(&c);

    log->beginDecode(&b); log->annotateLast(RIFT_AIR_K_DM, "bee",   NULL); log->endDecode();
    log->beginDecode(&c); log->annotateLast(RIFT_AIR_K_DM, "cee",   NULL); log->endDecode();
    log->beginDecode(&a); log->annotateLast(RIFT_AIR_K_DM, "ay",    NULL); log->endDecode();

    EXPECT_STREQ("cee", log->peek(0)->who);   EXPECT_EQ(43, (int) log->peek(0)->len);
    EXPECT_STREQ("bee", log->peek(1)->who);   EXPECT_EQ(42, (int) log->peek(1)->len);
    EXPECT_STREQ("ay",  log->peek(2)->who);   EXPECT_EQ(41, (int) log->peek(2)->len);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
