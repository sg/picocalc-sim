#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "picocalc-sim/sim.h"
#include "terminal.h"

// The lines are a ring: the oldest kept at `head`, the screen the last TERM_ROWS of `count`. The
// cursor's column reaches TERM_COLS when a character has just filled the last one, so the line
// wraps only once another arrives, as on a VT100.

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static term_cell_t lines[TERM_HISTORY][TERM_COLS];
static unsigned head, count = TERM_ROWS, generation;
static unsigned cx, cy, saved_cx, saved_cy;
static uint8_t fg = TERM_DEFAULT, bg = TERM_DEFAULT, attr;
static bool cursor_on = true, ready;

enum { GROUND, ESCAPE, CSI, OSC, SKIP_UTF8 };
static int state = GROUND;
static unsigned params[16], nparams, utf8_left;
static bool private_mode, osc_esc;

static term_cell_t *row(unsigned y)
{
    return lines[(head + count - TERM_ROWS + y) % TERM_HISTORY];
}

static term_cell_t blank(void)
{
    return (term_cell_t){' ', fg, bg, 0};
}

static void clear_cells(term_cell_t *c, unsigned n)
{
    term_cell_t b = blank();
    for (unsigned i = 0; i < n; i++)
        c[i] = b;
}

// The screen moves down a line, its top one going into the history.
static void scroll(void)
{
    if (count < TERM_HISTORY)
        count++;
    else
        head = (head + 1) % TERM_HISTORY;
    clear_cells(row(TERM_ROWS - 1), TERM_COLS);
}

static void line_feed(void)
{
    if (cy + 1 < TERM_ROWS)
        cy++;
    else
        scroll();
}

static void put(char ch)
{
    if (cx >= TERM_COLS) {
        cx = 0;
        line_feed();
    }
    row(cy)[cx++] = (term_cell_t){ch, fg, bg, attr};
}

static unsigned param(unsigned i, unsigned dflt)
{
    return i < nparams && params[i] ? params[i] : dflt;
}

static unsigned clamp(unsigned v, unsigned max)
{
    return v > max ? max : v;
}

static void sgr(void)
{
    if (!nparams)
        nparams = 1, params[0] = 0;
    for (unsigned i = 0; i < nparams; i++) {
        unsigned p = params[i];
        if (p == 0)
            fg = bg = TERM_DEFAULT, attr = 0;
        else if (p == 1)
            attr |= TERM_BOLD;
        else if (p == 4)
            attr |= TERM_UNDERLINE;
        else if (p == 7)
            attr |= TERM_INVERSE;
        else if (p == 22)
            attr &= (uint8_t)~TERM_BOLD;
        else if (p == 24)
            attr &= (uint8_t)~TERM_UNDERLINE;
        else if (p == 27)
            attr &= (uint8_t)~TERM_INVERSE;
        else if (p >= 30 && p <= 37)
            fg = (uint8_t)(p - 30);
        else if (p == 39)
            fg = TERM_DEFAULT;
        else if (p >= 40 && p <= 47)
            bg = (uint8_t)(p - 40);
        else if (p == 49)
            bg = TERM_DEFAULT;
        else if (p >= 90 && p <= 97)
            fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107)
            bg = (uint8_t)(p - 100 + 8);
        // 256-colour and RGB forms name a colour these 16 cannot show; their arguments are skipped.
        else if ((p == 38 || p == 48) && i + 1 < nparams)
            i += params[i + 1] == 5 ? 2 : 4;
    }
}

// The reply to a cursor position report, sent once the lock is let go.
static char reply[32];

static void reset(void)
{
    head = 0;
    count = TERM_ROWS;
    fg = bg = TERM_DEFAULT;
    attr = 0;
    for (unsigned y = 0; y < TERM_ROWS; y++)
        clear_cells(row(y), TERM_COLS);
    cx = cy = saved_cx = saved_cy = 0;
    cursor_on = true;
}

static void csi(char final)
{
    unsigned n = param(0, 1);
    term_cell_t *r = row(cy);
    unsigned x = clamp(cx, TERM_COLS - 1);
    if (private_mode) {
        if ((final == 'h' || final == 'l') && param(0, 0) == 25)
            cursor_on = final == 'h';
        return;
    }
    switch (final) {
    case 'A': cy = cy >= n ? cy - n : 0; break;
    case 'B': cy = clamp(cy + n, TERM_ROWS - 1); break;
    case 'C': cx = clamp(x + n, TERM_COLS - 1); break;
    case 'D': cx = x >= n ? x - n : 0; break;
    case 'E': cy = clamp(cy + n, TERM_ROWS - 1), cx = 0; break;
    case 'F': cy = cy >= n ? cy - n : 0, cx = 0; break;
    case 'G': cx = clamp(n - 1, TERM_COLS - 1); break;
    case 'd': cy = clamp(n - 1, TERM_ROWS - 1); break;
    case 'H':
    case 'f':
        cy = clamp(param(0, 1) - 1, TERM_ROWS - 1);
        cx = clamp(param(1, 1) - 1, TERM_COLS - 1);
        break;
    case 'J': {
        unsigned how = param(0, 0);
        if (how == 0) {
            clear_cells(r + x, TERM_COLS - x);
            for (unsigned y = cy + 1; y < TERM_ROWS; y++)
                clear_cells(row(y), TERM_COLS);
        } else if (how == 1) {
            for (unsigned y = 0; y < cy; y++)
                clear_cells(row(y), TERM_COLS);
            clear_cells(r, x + 1);
        } else {
            for (unsigned y = 0; y < TERM_ROWS; y++)
                clear_cells(row(y), TERM_COLS);
        }
        break;
    }
    case 'K': {
        unsigned how = param(0, 0);
        if (how == 0)
            clear_cells(r + x, TERM_COLS - x);
        else if (how == 1)
            clear_cells(r, x + 1);
        else
            clear_cells(r, TERM_COLS);
        break;
    }
    case 'P':
        n = clamp(n, TERM_COLS - x);
        memmove(r + x, r + x + n, (TERM_COLS - x - n) * sizeof(*r));
        clear_cells(r + TERM_COLS - n, n);
        break;
    case '@':
        n = clamp(n, TERM_COLS - x);
        memmove(r + x + n, r + x, (TERM_COLS - x - n) * sizeof(*r));
        clear_cells(r + x, n);
        break;
    case 'X': clear_cells(r + x, clamp(n, TERM_COLS - x)); break;
    case 'L':
    case 'M': {
        n = clamp(n, TERM_ROWS - cy);
        term_cell_t tmp[TERM_ROWS][TERM_COLS];
        for (unsigned y = cy; y < TERM_ROWS; y++)
            memcpy(tmp[y], row(y), sizeof(tmp[y]));
        for (unsigned y = cy; y < TERM_ROWS; y++) {
            unsigned from = final == 'L' ? y - n : y + n;
            if ((final == 'L' && y < cy + n) || (final == 'M' && from >= TERM_ROWS))
                clear_cells(row(y), TERM_COLS);
            else
                memcpy(row(y), tmp[from], sizeof(tmp[from]));
        }
        break;
    }
    case 'm': sgr(); break;
    case 's': saved_cx = cx, saved_cy = cy; break;
    case 'u': cx = saved_cx, cy = saved_cy; break;
    case 'n':
        if (param(0, 0) == 6)
            snprintf(reply, sizeof(reply), "\x1b[%u;%uR", cy + 1, clamp(cx, TERM_COLS - 1) + 1);
        break;
    default: break;
    }
}

static void byte(uint8_t b)
{
    switch (state) {
    case ESCAPE:
        state = GROUND;
        if (b == '[') {
            state = CSI;
            nparams = 0;
            params[0] = 0;
            private_mode = false;
        } else if (b == ']') {
            state = OSC;
            osc_esc = false;
        } else if (b == '7') {
            saved_cx = cx, saved_cy = cy;
        } else if (b == '8') {
            cx = saved_cx, cy = saved_cy;
        } else if (b == 'c') {
            reset();
        } else if (b == 'M') {
            cy = cy ? cy - 1 : 0;
        }
        return;
    case CSI:
        if (b >= '0' && b <= '9') {
            if (!nparams)
                nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (!nparams)
                nparams = 1;
            if (nparams < 16)
                params[nparams++] = 0;
        } else if (b == '?' || b == '>' || b == '=') {
            private_mode = true;
        } else if (b >= 0x40 && b <= 0x7e) {
            state = GROUND;
            csi((char)b);
        } else if (b < 0x20 || b > 0x7e) {
            state = GROUND;
        }
        return;
    case OSC:
        // Titles and the like run to BEL or ESC backslash and are not shown.
        if (b == 7 || (osc_esc && b == '\\'))
            state = GROUND;
        osc_esc = b == 0x1b;
        return;
    case SKIP_UTF8:
        if ((b & 0xc0) == 0x80 && --utf8_left)
            return;
        state = GROUND;
        if ((b & 0xc0) == 0x80)
            return;
        break;
    default: break;
    }
    switch (b) {
    case 0x1b: state = ESCAPE; break;
    case '\r': cx = 0; break;
    case '\n':
    case 0x0b:
    case 0x0c: line_feed(); break;
    case '\b':
        cx = clamp(cx, TERM_COLS - 1);
        if (cx)
            cx--;
        break;
    case '\t': cx = clamp((cx / 8 + 1) * 8, TERM_COLS - 1); break;
    default:
        if (b >= 0x20 && b < 0x7f) {
            put((char)b);
        } else if (b >= 0xc0 && b < 0xf8) {
            // A character beyond ASCII shows as one '?', its continuation bytes skipped.
            put('?');
            state = SKIP_UTF8;
            utf8_left = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
        }
        break;
    }
}

// The cells start out blank rather than zero, which would be black NULs.
static void take_lock(void)
{
    pthread_mutex_lock(&lock);
    if (!ready) {
        reset();
        ready = true;
    }
}

void term_feed(const uint8_t *data, size_t len)
{
    take_lock();
    for (size_t i = 0; i < len; i++)
        byte(data[i]);
    generation++;
    char out[sizeof(reply)];
    snprintf(out, sizeof(out), "%s", reply);
    reply[0] = 0;
    pthread_mutex_unlock(&lock);
    if (out[0])
        picocalc_sim_usb_serial_send((const uint8_t *)out, strlen(out));
}

void term_clear(void)
{
    take_lock();
    reset();
    state = GROUND;
    generation++;
    pthread_mutex_unlock(&lock);
}

void term_lock(void)
{
    take_lock();
}

void term_unlock(void)
{
    pthread_mutex_unlock(&lock);
}

unsigned term_count(void)
{
    return count;
}

const term_cell_t *term_line(unsigned i)
{
    return lines[(head + i) % TERM_HISTORY];
}

void term_cursor(unsigned *line, unsigned *col, bool *visible)
{
    *line = count - TERM_ROWS + cy;
    *col = clamp(cx, TERM_COLS - 1);
    *visible = cursor_on;
}

unsigned term_generation(void)
{
    return generation;
}

size_t term_key_bytes(int key, bool ctrl, uint8_t *out, size_t cap)
{
    static const char *const seq[] = {"\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[H",
                                      "\x1b[F", "\x1b[2~", "\x1b[3~", "\x1b[5~", "\x1b[6~"};
    if (key >= TERM_KEY_UP && key <= TERM_KEY_PAGE_DOWN) {
        const char *s = seq[key - TERM_KEY_UP];
        size_t n = strlen(s) < cap ? strlen(s) : cap;
        memcpy(out, s, n);
        return n;
    }
    if (!cap || key < 0 || key > 0x7f)
        return 0;
    // Ctrl with a letter or one of @[\]^_ gives the control code below it.
    if (ctrl && key >= 'a' && key <= 'z')
        key -= 'a' - 'A';
    if (ctrl && key >= '@' && key <= '_')
        key &= 0x1f;
    out[0] = (uint8_t)key;
    return 1;
}
