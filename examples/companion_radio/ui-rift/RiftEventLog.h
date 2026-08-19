#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// A ring of recent events, readable on SYSTEM -> View log.
//
// Every hardware problem in this firmware was found by putting a value on screen
// after reasoning had failed - the I2C timing, the touch byte layout, the trackball
// pins, the boot time, the missing notification. What the SYSTEM readouts could not
// do was say what happened *earlier*: they show the current value of everything and
// the history of nothing, so a fault that has passed leaves no trace.
//
// RAM only, and deliberately. Persisting it would put a second writer on the
// filesystem that holds the node identity, for data whose whole value is that it
// describes the session you are still in.
//
// A header rather than a block inside UITask.cpp because MyMesh writes to it too -
// received adverts are the most frequent evidence that the radio is hearing
// anything, and they arrive in the mesh callbacks, not in the UI. Every call site
// outside ui-rift is guarded on RIFT_VERSION, which is defined only for this build.

#define RIFT_LOG_LINES  128
// 64, not 48: a line now wraps to a second row on screen, so there is room for
// about 88 characters of it, and 48 was cutting the message out of every rx line.
#define RIFT_LOG_TEXT   64

struct RiftEventLog {
  struct Line {
    uint32_t at_ms;
    char text[RIFT_LOG_TEXT];
  };
  Line lines[RIFT_LOG_LINES];
  int head = RIFT_LOG_LINES - 1;   // index of the newest
  int count = 0;
  uint16_t dropped = 0;            // lines lost to the ring, so the screen can say so

  void add(const char* text) {
    head = (head + 1) % RIFT_LOG_LINES;
    if (count < RIFT_LOG_LINES) count++;
    else if (dropped < 0xFFFF) dropped++;
    Line* l = &lines[head];
    l->at_ms = (uint32_t) millis();
    size_t n = strnlen(text, RIFT_LOG_TEXT - 1);
    memcpy(l->text, text, n);
    l->text[n] = 0;
  }

  // back == 0 is the newest
  const Line* peek(int back) const {
    if (back < 0 || back >= count) return NULL;
    return &lines[(head - back + RIFT_LOG_LINES * 2) % RIFT_LOG_LINES];
  }
};

// One instance across both translation units. A function-local static in an inline
// function is guaranteed to be a single object, which a file-scope definition in a
// header would not be.
inline RiftEventLog& riftLog() {
  static RiftEventLog log;
  return log;
}

// Callers pass a format so the line is built once, here, rather than each site
// carrying its own buffer. Truncation is silent: a truncated log line is still
// worth having and there is nothing useful to do about it at the call site.
inline void riftLogf(const char* fmt, ...) {
  char buf[RIFT_LOG_TEXT];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  riftLog().add(buf);
}
