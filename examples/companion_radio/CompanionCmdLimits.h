#pragma once

#include <stdint.h>

// Minimum frame length per companion command, checked once before dispatch.
//
// handleCmdFrame() is a sixty-branch if/else chain over a frame whose length the
// host chose, and every branch has had to remember its own guard. Several did
// not: CMD_SET_TUNING_PARAMS read eight bytes and persisted them, so a one-byte
// frame wrote zero to the airtime factor and the receive delay and saved it.
// CMD_SET_RADIO_PARAMS read ten. CMD_REBOOT and CMD_FACTORY_RESET ran memcmp
// against a magic string without first proving the string was there.
//
// tools/audit-cmd-guards.py did not catch any of these, and the reason is worth
// recording rather than quietly fixing: it only recognises literal indices like
// cmd_frame[4], and these handlers use cmd_frame[i]. That limitation was written
// in the script's own docstring, and its "0 unguarded" was still read - by me -
// as though it meant the parser was safe. A heuristic lint is not a proof.
//
// This table is the beginning of the answer, not the whole of it. It carries the
// commands whose handlers have been read and whose minimum can be stated from
// what they unconditionally consume. Everything else returns 1, which is no
// restriction at all: a minimum that is too large refuses frames a working
// companion sends, and that failure is worse than the one being fixed.
//
// Kept as a pure function of the command byte so it can be tested natively,
// which is the part that makes extending it safe.

#ifndef PUB_KEY_SIZE
  #define PUB_KEY_SIZE 32
#endif

static inline uint8_t companionCmdMinLen(uint8_t cmd) {
  switch (cmd) {
    // 4-byte receive delay and 4-byte airtime factor, both stored and saved.
    case 21: return 9;    // CMD_SET_TUNING_PARAMS
    // freq 4, bandwidth 4, spreading factor 1, coding rate 1.
    case 11: return 11;   // CMD_SET_RADIO_PARAMS
    // memcmp(&cmd_frame[1], "reboot", 6)
    case 19: return 7;    // CMD_REBOOT
    // memcmp(&cmd_frame[1], "reset", 5)
    case 51: return 6;    // CMD_FACTORY_RESET
    // Reads cmd_frame[1] in its own condition, before the len test beside it -
    // C++ evaluates left to right, so the guard was behind the read it guarded.
    case 61: return 3;    // CMD_SET_PATH_HASH_MODE
    default: return 1;    // command byte only; the branch guards the rest
  }
}
