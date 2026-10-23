// SPDX-License-Identifier: MIT
//
// Bounded text output for the dashboard's pages, JSON, and CSV. Private to
// this component.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace fy::dashboard {

// Appends formatted text, tracking overflow instead of truncating silently.
struct Writer {
  char *out;
  size_t cap;
  size_t len = 0;
  bool overflow = false;

  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (overflow) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out + len, cap - len, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= cap - len) {
      overflow = true;
      return;
    }
    len += static_cast<size_t>(n);
  }

  void add_char(char c) {
    if (overflow) return;
    if (len + 1 >= cap) {
      overflow = true;
      return;
    }
    out[len++] = c;
    out[len] = '\0';
  }

  // Text with the HTML-special characters escaped.
  void add_html(const char *text) {
    for (const char *p = text; p && *p && !overflow; p++) {
      switch (*p) {
        case '<': add("&lt;"); break;
        case '>': add("&gt;"); break;
        case '&': add("&amp;"); break;
        case '"': add("&quot;"); break;
        case '\'': add("&#39;"); break;
        default: add_char(*p); break;
      }
    }
  }

  // A quoted JSON string of at most `max` bytes of `text`. Bytes outside
  // printable ASCII are written as \u00XX (read as Latin-1), so the output is
  // valid JSON whatever a device advertised.
  void add_json_string(const char *text, size_t max = static_cast<size_t>(-1)) {
    add_char('"');
    for (size_t i = 0; text && i < max && text[i] && !overflow; i++) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c == '"' || c == '\\') {
        add_char('\\');
        add_char(static_cast<char>(c));
      } else if (c < 0x20 || c >= 0x7f) {
        add("\\u%04x", c);
      } else {
        add_char(static_cast<char>(c));
      }
    }
    add_char('"');
  }

  // A CSV field, quoted when needed. A leading =, +, -, or @ is prefixed
  // with an apostrophe so spreadsheets do not run advertised names as
  // formulas. Bytes outside printable ASCII become '?'.
  void add_csv(const char *text, size_t max = static_cast<size_t>(-1)) {
    if (!text) return;
    bool quote = false;
    for (size_t i = 0; i < max && text[i]; i++) {
      if (text[i] == ',' || text[i] == '"' || text[i] == '\n' || text[i] == '\r') quote = true;
    }
    if (quote) add_char('"');
    if (text[0] == '=' || text[0] == '+' || text[0] == '-' || text[0] == '@') add_char('\'');
    for (size_t i = 0; i < max && text[i] && !overflow; i++) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      if (c == '"') {
        add("\"\"");
      } else if (c < 0x20 || c >= 0x7f) {
        add_char('?');
      } else {
        add_char(static_cast<char>(c));
      }
    }
    if (quote) add_char('"');
  }

  // Returns the length written, or 0 (and an empty string) on overflow.
  size_t finish() {
    if (overflow) {
      if (cap) out[0] = '\0';
      return 0;
    }
    return len;
  }
};

}  // namespace fy::dashboard
