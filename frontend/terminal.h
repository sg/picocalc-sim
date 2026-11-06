#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A VT100-style terminal for the window: an 80 by 24 screen at the foot of the lines scrolled off
// it. Fed from the emulation thread, read from the UI's.

#define TERM_COLS 80
#define TERM_ROWS 24
#define TERM_HISTORY 2000

// Colours are ANSI 0-15; TERM_DEFAULT is the terminal's own.
enum { TERM_DEFAULT = 0xff };
enum { TERM_BOLD = 1, TERM_INVERSE = 2, TERM_UNDERLINE = 4 };

typedef struct {
    char ch;
    uint8_t fg, bg, attr;
} term_cell_t;

void term_feed(const uint8_t *data, size_t len);
void term_clear(void);

// Between these, term_line gives line i counting from the oldest kept, of term_count.
void term_lock(void);
void term_unlock(void);
unsigned term_count(void);
const term_cell_t *term_line(unsigned i);
// The cursor's line, in the same numbering, and column.
void term_cursor(unsigned *line, unsigned *col, bool *visible);
// Changes whenever anything shown does.
unsigned term_generation(void);

// What a key sends, as a terminal with VT100 cursor keys would: Enter is CR, Backspace is DEL.
size_t term_key_bytes(int key, bool ctrl, uint8_t *out, size_t cap);
enum { TERM_KEY_UP = 256, TERM_KEY_DOWN, TERM_KEY_RIGHT, TERM_KEY_LEFT, TERM_KEY_HOME, TERM_KEY_END,
       TERM_KEY_INSERT, TERM_KEY_DELETE, TERM_KEY_PAGE_UP, TERM_KEY_PAGE_DOWN };

#ifdef __cplusplus
}
#endif
