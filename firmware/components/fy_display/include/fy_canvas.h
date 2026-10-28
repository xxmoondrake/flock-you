// SPDX-License-Identifier: MIT
//
// 128x64 monochrome frame buffer with a 5x7 text font. Pure C++ with no
// ESP-IDF dependencies so rendering can be tested on a host computer.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fy::display {

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr size_t kFrameBytes = kWidth * kHeight / 8;

// Text uses a 6x8 cell (5x7 glyph plus spacing): 21 columns by 8 rows.
constexpr int kCharWidth = 6;
constexpr int kLineHeight = 8;
constexpr int kColumns = kWidth / kCharWidth;

class Canvas {
 public:
  Canvas() { clear(); }

  void clear();
  void set_pixel(int x, int y, bool on);
  bool pixel(int x, int y) const;
  void fill_rect(int x, int y, int w, int h, bool on);

  // Draws printable ASCII; other bytes render as '?'. Each font pixel is a
  // `scale` x `scale` block. Pixels outside the canvas are clipped. Returns
  // the x position after the last glyph.
  int draw_text(int x, int y, const char *text, bool on = true, int scale = 1);

  // Replaces the canvas with a frame in the same order, optionally inverted.
  void load_frame(const uint8_t *frame, bool inverted = false);

  // SSD1306 page order: 8 pages of 128 column bytes, bit 0 at the top.
  const uint8_t *frame() const { return buf_; }

 private:
  uint8_t buf_[kFrameBytes];
};

}  // namespace fy::display
