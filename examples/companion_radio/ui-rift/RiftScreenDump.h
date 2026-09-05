#pragma once

// Screen dump over the USB companion link, for design work.
//
// Every screenshot in design/ was a photograph, and every design round has had to
// say that the photographs were stale. This is the device's own frame: the host
// sends one companion frame and the firmware answers with the composed canvas,
// 320*240 RGB565, after drawing the screen it was asked for.
//
// Wire format, host to device: a normal companion frame whose first byte is
// CMD_RIFT_SCREEN_DUMP and whose optional second byte is a nav index (0..4) to
// switch to first, or 0x10..0x12 for a SYSTEM sub-screen (air log, event log,
// diagnostics), or absent/0xFF for whatever is showing now, overlays included.
//
// Device to host, written raw on the serial port outside the companion framing
// because a frame holds 184 bytes and a screen is 153600:
//
//   "RIFTSCRN"  8 bytes, the marker to search for
//   width       uint16 little-endian
//   height      uint16 little-endian
//   bpp         uint8, 16
//   reserved    3 bytes, zero
//   pixels      width*height*2 bytes, RGB565 little-endian, row-major from top-left
//
// tools/rift-screenshot.py is the host side. The dump runs on the main loop and
// blocks it for the transfer - a fraction of a second at USB speed - which is
// acceptable for a bench command and would not be for anything on the packet path.
//
// 0xF0: well clear of MeshCore's command numbers, which reach the sixties.
#define CMD_RIFT_SCREEN_DUMP  0xF0

// Called from MyMesh's command parser; the UI does the work after its next render.
void riftRequestScreenDump(int nav);
