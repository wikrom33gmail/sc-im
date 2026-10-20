/**
 * \file whichkey.c
 * \brief Which-key style popup: shows available keys while waiting for more input
 *
 * When the user types a key that is a prefix of multi-key commands (e.g. 'g' in
 * normal mode) and pauses, sc-im waits for more input before deciding what to
 * execute. While it waits, this module draws a popup near the cursor listing
 * every continuation of the typed prefix with its description — like neovim's
 * which-key plugin. The popup is drawn on a pad composited over stdscr, so the
 * grid underneath (main_win) is never modified and can be repainted exactly
 * when the popup disappears.
 */

#include <string.h>
#include "sc.h"
#include "macros.h"
#include "conf.h"
#include "tui.h"
#include "color.h"
#include "buffer.h"
#include "whichkey.h"

extern WINDOW * main_win;
extern int curwinrow, curwincol;

struct wk_entry {
    const wchar_t *keys;   /* leading sequence to match against the typed prefix */
    const char *suffix;    /* open-ended continuation shown after keys ("" if none) */
    const char *desc;      /* description shown in popup */
};

/* multi-key sequences of NORMAL_MODE, with descriptions from src/doc */
static const struct wk_entry normal_keys[] = {
    { L"gf", "", "Open file or URL under cursor" },
    { L"gM", "", "Middle column on screen" },
    { L"gg", "", "First cell of sheet" },
    { L"go", "...", "Go to cell (e.g. goAB24)" },
    { L"gl", "", "Last occupied cell position" },
    { L"gt", "", "Next sheet in file" },
    { L"gT", "", "Previous sheet in file" },
    { L"g0", "", "Leftmost column on screen" },
    { L"g$", "", "Rightmost column on screen" },
    { L" ",  "<SPACE>...", "Go to cell (e.g. ' AB24')" },
    { L"Pv", "", "Paste values only (below/left)" },
    { L"Pf", "", "Paste formatting only (below/left)" },
    { L"Pt", "", "Paste transposed (below/left)" },
    { L"Pc", "", "Paste with adjusted refs (below/left)" },
    { L"Tv", "", "Paste values only (above/right)" },
    { L"Tf", "", "Paste formatting only (above/right)" },
    { L"Tt", "", "Paste transposed (above/right)" },
    { L"Tc", "", "Paste with adjusted refs (above/right)" },
    { L"aa", "", "Autofit column width" },
    { L"Zr", "", "Hide current row" },
    { L"Zc", "", "Hide current column" },
    { L"Sr", "", "Show hidden rows in range" },
    { L"Sc", "", "Show hidden columns in range" },
    { L"yy", "", "Yank cell to yankbuffer" },
    { L"yr", "", "Yank current row" },
    { L"yc", "", "Yank current column" },
    { L"'",  "{a-z}", "Go to mark (a-z)" },
    { L"m",  "{a-z}", "Set mark on cell or range" },
    { L"c",  "{a-z}", "Copy marked cell/range to cursor" },
    { L"R",  "{a-z}{a-z}", "Select range defined by two marks" },
    { L"sh", "", "Shift cell/range left" },
    { L"se", "", "Shift cell/range right" },
    { L"sk", "", "Shift cell/range down" },
    { L"sl", "", "Shift cell/range up" },
    { L"ir", "", "Insert row before cursor" },
    { L"ic", "", "Insert column before cursor" },
    { L"or", "", "Open row after cursor (insert)" },
    { L"oc", "", "Open column after cursor (insert)" },
    { L"dd", "", "Delete cell/range to yankbuffer" },
    { L"dr", "", "Delete current row" },
    { L"dc", "", "Delete current column" },
    { L"rl", "", "Lock cell or range" },
    { L"ru", "", "Unlock cell or range" },
    { L"rv", "", "Valueize (remove expressions)" },
    { L"f<", "", "Decrease column width" },
    { L"f>", "", "Increase column width" },
    { L"f-", "", "Decrease decimal precision" },
    { L"f+", "", "Increase decimal precision" },
    { L"fk", "", "Increase row height" },
    { L"fl", "", "Decrease row height" },
    { L"fr", "", "Freeze current row" },
    { L"fc", "", "Freeze current column" },
    { L"fa", "", "Freeze area (row + column)" },
    { L"zh", "", "Scroll left one column" },
    { L"ze", "", "Scroll right one column" },
    { L"zH", "", "Scroll left half page" },
    { L"zL", "", "Scroll right half page" },
    { L"zm", "", "Center cell horizontally" },
    { L"zz", "", "Center cell vertically (also z.)" },
    { L"zt", "", "Cell at top of screen" },
    { L"zb", "", "Cell at bottom of screen" },
};

/* multi-key sequences of VISUAL_MODE */
static const struct wk_entry visual_keys[] = {
    { L"'",  "{a-z}", "Go to mark (a-z)" },
    { L"dd", "", "Delete selection to yankbuffer" },
    { L"sh", "", "Shift selection left" },
    { L"se", "", "Shift selection right" },
    { L"sk", "", "Shift selection down" },
    { L"sl", "", "Shift selection up" },
    { L"Zr", "", "Hide selected rows" },
    { L"Zc", "", "Hide selected columns" },
    { L"Sr", "", "Show hidden rows in range" },
    { L"Sc", "", "Show hidden columns in range" },
    { L"rl", "", "Lock selection" },
    { L"ru", "", "Unlock selection" },
    { L"rv", "", "Valueize selection (remove expressions)" },
    { L"m",  "{a-z}", "Set mark on selected range" },
};

static int wk_visible = 0;
static wchar_t last_pref[64] = L"";    // prefix the visible popup was drawn for

/**
 * \brief Check if an entry continues the typed prefix
 *
 * An entry matches while the command is still incomplete: the typed prefix
 * must agree with the leading key sequence and be shorter than it (plus one
 * open-ended continuation when a suffix is present).
 */
static int wk_match(const struct wk_entry * e, const wchar_t * pref) {
    size_t plen = wcslen(pref), klen = wcslen(e->keys);
    if (plen >= klen + (e->suffix ? 1 : 0)) return 0; // complete -> would have executed
    return wcsncmp(e->keys, pref, plen < klen ? plen : klen) == 0;
}

/**
 * \brief Remove the popup from screen and repaint the grid underneath
 */
void wk_hide(void) {
    if (!wk_visible || !main_win) return;

    /* Repaint the grid that was under the popup from main_win's intact buffer */
    touchwin(main_win);
    wrefresh(main_win);

    wk_visible = 0;
}

/**
 * \brief Draw the popup listing continuations of the typed prefix
 *
 * Called from the input loop while sc-im waits for more keys. The popup is
 * composited over stdscr with a pad, so main_win's grid content stays intact.
 */
void wk_show(struct block * pending) {
    if (get_conf_int("nocurses") || !main_win) return;
    if (! get_conf_int("whichkey")) { wk_hide(); return; }
    if (curmode != NORMAL_MODE && curmode != VISUAL_MODE) { wk_hide(); return; }
    /* typed prefix, skipping leading multiplier digits */
    wchar_t pref[64]; int n = 0;
    for (int k = 0; k < get_bufsize(pending) && n < 63; k++) {
        wint_t v = get_bufval(pending, k);
        if (v >= L'0' && v <= L'9') continue; // multiplier digits
        pref[n++] = (wchar_t)v;
    }
    pref[n] = L'\0';

    /* nothing typed yet -> no popup */
    if (!n) { wk_hide(); return; }

    /* prefix unchanged since last draw -> popup already on screen */
    if (wk_visible && wcscmp(pref, last_pref) == 0) return;

    const struct wk_entry * tab; size_t nent;
    if (curmode == NORMAL_MODE) {
        tab = normal_keys; nent = sizeof(normal_keys) / sizeof(*normal_keys);
    } else {
        tab = visual_keys; nent = sizeof(visual_keys) / sizeof(*visual_keys);
    }

    /* collect matching entries and compute column widths */
    int rows[128], nrows = 0, keyw = 0, descw = 0;
    for (size_t i = 0; i < nent && nrows < 128; i++) {
        if (!wk_match(&tab[i], pref)) continue;
        rows[nrows++] = (int)i;
        int kw = (int)wcslen(tab[i].keys) + (int)strlen(tab[i].suffix);
        if (kw > keyw) keyw = kw;
        int dw = (int)strlen(tab[i].desc);
        if (dw > descw) descw = dw;
    }

    /* no continuation for this prefix -> hide popup */
    if (!nrows) { wk_hide(); wcscpy(last_pref, pref); return; }

    /* size the box and clamp it to the area occupied by main_win */
    int h = nrows + 2; // top and bottom border
    int w = keyw + descw + 6; // borders + padding
    int grid_top = get_conf_int("input_bar_bottom") ? 0 : RESROW;
    int grid_bot = get_conf_int("input_bar_bottom") ? LINES - RESROW : LINES;
    if (h > grid_bot - grid_top) { h = grid_bot - grid_top; nrows = h - 2; }
    if (w > COLS) { w = COLS; descw = w - keyw - 6; if (descw < 4) descw = 4; }

    /* position near the cursor, like neovim's which-key */
    int top = grid_top + curwinrow + 2;
    if (top + h > grid_bot) top = grid_top + curwinrow - h - 1;
    if (top < grid_top) top = grid_top;

    int left = curwincol + 1;
    if (left + w > COLS) left = COLS - w;
    if (left < 0) left = 0;

    /* draw into a pad and composite it over the grid */
    static WINDOW * wk_pad = NULL;
    if (!wk_pad || getmaxy(wk_pad) != h || getmaxx(wk_pad) != w) {
        if (wk_pad) delwin(wk_pad);
        wk_pad = newpad(h, w);
    }
    werase(wk_pad);

    #ifdef USECOLORS
    ui_set_ucolor(wk_pad, &ucolors[NORMAL], DEFAULT_COLOR);
    #endif

    /* border */
    for (int x = 1; x < w - 1; x++) {
        mvwaddch(wk_pad, 0, x, '-');
        mvwaddch(wk_pad, h - 1, x, '-');
    }
    for (int y = 1; y < h - 1; y++) {
        mvwaddch(wk_pad, y, 0, '|');
        mvwaddch(wk_pad, y, w - 1, '|');
    }
    mvwaddch(wk_pad, 0, 0, '+');
    mvwaddch(wk_pad, 0, w - 1, '+');
    mvwaddch(wk_pad, h - 1, 0, '+');
    mvwaddch(wk_pad, h - 1, w - 1, '+');

    /* entries: key column then description */
    for (int r = 0; r < nrows; r++) {
        const struct wk_entry * e = &tab[rows[r]];

        #ifdef USECOLORS
        ui_set_ucolor(wk_pad, &ucolors[HELP_HIGHLIGHT], DEFAULT_COLOR);
        #endif
        mvwprintw(wk_pad, r + 1, 2, "%ls", e->keys);
        if (e->suffix) {
            int kw = (int)wcslen(e->keys);
            mvwaddstr(wk_pad, r + 1, 2 + kw, e->suffix);
        }

        #ifdef USECOLORS
        ui_set_ucolor(wk_pad, &ucolors[HELP_TEXT], DEFAULT_COLOR);
        #endif
        char dl[80]; // description truncated to fit the box
        snprintf(dl, sizeof dl, "%.*s", descw, e->desc);
        mvwaddstr(wk_pad, r + 1, keyw + 4, dl);
    }

    prefresh(wk_pad, 0, 0, top, left, top + h - 1, left + w - 1);

    wcscpy(last_pref, pref);
    wk_visible = 1;
}
