/* The main thread's half of the items' menus (see tray_menu.h): the menu
 * open, in awl's popups, one per level: the menu, and each submenu open beside the row it belongs to. One
 * menu at a time. Hovering a submenu's row opens it after OPEN_DELAY;
 * hovering another row closes it after CLOSE_DELAY, unless the pointer gets
 * back into it first (on the way there, across the rows between). A new
 * layout that no longer has a submenu open waits LAYOUT_GRACE for one that
 * does (apps that rebuild their menus, network-manager-applet's rescans).
 * Off the menu, its submenus and its item for LEAVE_DELAY, it closes.
 * While open it has the keyboard: Up/Down choose a row of the deepest level,
 * Return activates it, Right/Left open and close submenus, Escape closes
 * one level.
 * Everything here runs on awl's main thread: what the tray's loop sends
 * comes through sni.c's mailbox, awl_window's callbacks are awl's. */

#include "tray_menu.h"
#include "tray_tooltip.h"
#include "../awl_log.h"
#include "../plugins.h" /* awl_config() */
#include "../icons/icon.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    /** levels: the menu and its submenus */
    MAX_LEVELS = 8,
    /** ms */
    OPEN_DELAY = 200,
    CLOSE_DELAY = 400,
    LAYOUT_GRACE = 600,
    LEAVE_DELAY = 300,
};

/** a row shown, one of the level's node's children */
typedef struct {
    int node;
    /** from the content's top, physical pixels; y of rows above top is
     * meaningless */
    int y, h;
} row_t;

/** the menu (level 0) or a submenu open */
typedef struct {
    awl_window_t* win;
    /** the node whose children are shown */
    int node;
    /** the first row shown, of more than fit; scrolling not yet in rows */
    int top;
    /** a submenu went left of its menu: its own go there too */
    int left;
    double acc;
    /** the row highlighted: the pointer's (hover()), or the keys' */
    int sel;

    /* measure()'s layout, physical pixels; x0 paint()'s */
    float scale;
    row_t* rows;
    int nrows, shown;
    int w, x0, pad, gap, check_w, icon_w, label_w, arrow_w;
} level_t;

static struct {
    /** the menu shown or being opened, 0: none */
    uint64_t serial;
    char monitor[64];
    int center_x;
    /** the request whose snapshots are taken */
    uint32_t gen;
    /** NULL while it is being opened */
    awl_tray_menu_snap_t* snap;
    /** the snapshot's icons decoded at icon_px, by node; tried: decoded or
     * failed */
    pixman_image_t** icons;
    char* tried;
    int icon_px;

    level_t lv[MAX_LEVELS];
    /** the levels shown, 0 .. depth - 1; 0 while it is being opened */
    int depth;
    /** what hovering asked for, when timer fires: row of level pend_level
     * opens its submenu, or with pend_row -1 the level's submenus close;
     * pend_level -1 nothing */
    awl_timer_t* timer;
    int pend_level, pend_row;
    /** the levels the pointer is on; on_icon: on the item's tray window */
    char inside[MAX_LEVELS];
    int on_icon;
    /** fires LEAVE_DELAY after the pointer got off all of them; leaving:
     * armed */
    awl_timer_t* leave_timer;
    int leaving;
    /** hiding a level that stays open (its row scrolled away) */
    int quiet;
    /** a layout that would close submenus open, waiting for a better one */
    awl_tray_menu_snap_t* held;
    awl_timer_t* held_timer;
} M = { .pend_level = -1 };

static int px( const level_t* L, float v ) {
    return (int)lroundf( v * L->scale );
}

/* ---- drawing ---- */

/** a check box or radio button of flags, s x s at x, y */
static void draw_check( pixman_image_t* pix, uint32_t flags, float x, float y, float s, uint32_t fg ) {
    const float t = fmaxf( 1, roundf( s / 9 ) );
    const int radio = !!(flags & AWL_TRAY_MENU_RADIO);
    const float r = radio ? s / 2 : s / 5;
    awl_fill_shape( pix, x, y, s, s, r, t, fg );
    const float in = 2 * t + 1;
    if (flags & AWL_TRAY_MENU_CHECKED)
        awl_fill_shape( pix, x + in, y + in, s - 2 * in, s - 2 * in, radio ? (s - 2 * in) / 2 : r / 2, 0, fg );
    else if (flags & AWL_TRAY_MENU_MIXED)
        awl_fill_shape( pix, x + in, y + s / 2 - t, s - 2 * in, 2 * t, 0, 0, fg );
}

/* ---- icons: decoded here, at the monitor's scale; read on the tray's loop ---- */

static void icons_clear( void ) {
    if (M.icons && M.snap)
        for (int i = 0; i < M.snap->nnodes; i++)
            if (M.icons[i]) pixman_image_unref( M.icons[i] );
    free( M.icons );
    free( M.tried );
    M.icons = NULL;
    M.tried = NULL;
}

/** node i's icon at M.icon_px, NULL if it has none */
static pixman_image_t* icon( int i ) {
    awl_tray_menu_node_t* n = &M.snap->nodes[i];
    if (!n->icon || M.icon_px <= 0) return NULL;
    if (!M.icons) {
        M.icons = calloc( (size_t)M.snap->nnodes, sizeof(*M.icons) );
        M.tried = calloc( (size_t)M.snap->nnodes, 1 );
    }
    if (!M.tried[i]) {
        M.tried[i] = 1;
        /* an SVG or XPM only from a file: icon-data is a PNG */
        char source[128];
        snprintf( source, sizeof source, "tray menu entry '%s' (its %s)", n->label,
                  n->flags & AWL_TRAY_MENU_FILE ? "icon-name's file" : "icon-data" );
        M.icons[i] = awl_icon_decode( n->icon, n->icon_len, !!(n->flags & AWL_TRAY_MENU_FILE), M.icon_px, M.icon_px,
                                      source );
        if (!M.icons[i]) awl_vrb_printf( "tray: menu entry '%s': can't decode its icon\n", n->label );
    }
    return M.icons[i];
}

/* ---- the painter, one per level (data) ---- */

static const char MORE[] = "›";

static int measure( void* data, struct fcft_font* font, float scale, int max_w, int max_h, int* w, int* h ) {
    level_t* L = data;
    if (!M.snap || L->node < 0 || L->node >= M.snap->nnodes) return 0;
    L->scale = scale;
    const awl_tray_menu_node_t* lv = &M.snap->nodes[L->node];
    const int fh = font->height;
    const int rh = fh + 2 * px( L, 3 ), sep_h = px( L, 9 );

    free( L->rows );
    L->nrows = lv->n;
    L->rows = calloc( (size_t)(L->nrows ? L->nrows : 1), sizeof(*L->rows) );
    int checks = 0, icons = 0, subs = 0, label_w = 0, shortcut_w = 0;
    for (int i = 0; i < lv->n; i++) {
        const awl_tray_menu_node_t* n = &M.snap->nodes[lv->first + i];
        int sep = !!(n->flags & AWL_TRAY_MENU_SEPARATOR);
        L->rows[i] = (row_t){ lv->first + i, 0, sep ? sep_h : rh };
        if (sep) continue;
        checks |= !!(n->flags & (AWL_TRAY_MENU_CHECK | AWL_TRAY_MENU_RADIO));
        icons |= !!n->icon;
        subs |= !!(n->flags & AWL_TRAY_MENU_SUBMENU);
        int lw = (int)awl_host->text_width( font, n->label );
        if (lw > label_w) label_w = lw;
        if (n->shortcut && (lw = (int)awl_host->text_width( font, n->shortcut )) > shortcut_w) shortcut_w = lw;
    }
    if (!L->nrows) return 0;

    L->pad = px( L, 6 );
    L->gap = px( L, 8 );
    L->check_w = checks ? (int)lroundf( fh * 0.7f ) + L->gap : 0;
    if (M.icon_px != fh) {
        icons_clear();
        M.icon_px = fh;
    }
    L->icon_w = icons ? M.icon_px + L->gap : 0;
    L->arrow_w = subs ? (int)awl_host->text_width( font, MORE ) + L->gap : 0;
    L->label_w = label_w;
    int W = 2 * L->pad + L->check_w + L->icon_w + label_w + (shortcut_w ? 2 * L->gap + shortcut_w : 0) +
            L->arrow_w;
    if (W < px( L, 120 )) W = px( L, 120 );
    if (W > max_w) W = max_w;
    L->w = W;

    /* as many rows as fit from top on, top as low as lets the last one in */
    int total = 0;
    for (int i = 0; i < L->nrows; i++) total += L->rows[i].h;
    if (total <= max_h) L->top = 0;
    else {
        int last_top = L->nrows - 1, hh = L->rows[last_top].h;
        while (last_top > 0 && hh + L->rows[last_top - 1].h <= max_h) hh += L->rows[--last_top].h;
        if (L->top > last_top) L->top = last_top;
        if (L->top < 0) L->top = 0;
    }
    int y = 0;
    L->shown = 0;
    for (int i = L->top; i < L->nrows && y + L->rows[i].h <= max_h; i++, L->shown++) {
        L->rows[i].y = y;
        y += L->rows[i].h;
    }
    if (!L->shown) return 0;
    *w = W;
    *h = y;
    return 1;
}

/** the level after L, if it is open: the submenu of one of L's rows */
static const level_t* child_of( const level_t* L ) {
    int k = (int)(L - M.lv);
    return k + 1 < M.depth ? &M.lv[k + 1] : NULL;
}

static void paint( void* data, pixman_image_t* pix, struct fcft_font* font, float scale, int x0, int y0,
                   int hover ) {
    level_t* L = data;
    const awl_colors_t* k = &awl_config()->colors;
    const uint32_t dim = k->menu_dim.v, highlight_fg = k->menu_highlight_fg.v;
    const int radius = MIN( awl_config()->popup.menu.radius.v, 4 );
    const level_t* sub = child_of( L );
    L->x0 = x0;
    for (int i = L->top; i < L->top + L->shown; i++) {
        const row_t* r = &L->rows[i];
        const awl_tray_menu_node_t* n = &M.snap->nodes[r->node];
        const uint32_t flags = n->flags;
        const int y = y0 + r->y;
        if (flags & AWL_TRAY_MENU_SEPARATOR) {
            float t = fmaxf( 1, (float)px( L, 1 ) );
            awl_fill_shape( pix, (float)(x0 + L->pad), (float)y + ((float)r->h - t) / 2, (float)(L->w - 2 * L->pad), t,
                        0, 0, dim );
            continue;
        }
        /* the row whose submenu is open stays highlighted */
        const int lit = i == L->sel || (sub && sub->node == r->node);
        /* rounded as the frame, at most 4 */
        if (lit)
            awl_fill_shape( pix, (float)x0, (float)y, (float)L->w, (float)r->h, (float)px( L, radius ),
                        0, k->menu_highlight.v );
        const uint32_t fg = (flags & AWL_TRAY_MENU_DISABLED) ? dim : lit ? highlight_fg : k->menu_fg.v;
        int x = x0 + L->pad;
        if (L->check_w && (flags & (AWL_TRAY_MENU_CHECK | AWL_TRAY_MENU_RADIO))) {
            float s = (float)(L->check_w - L->gap);
            draw_check( pix, flags, (float)x, (float)y + ((float)r->h - s) / 2, s, fg );
        }
        x += L->check_w;
        pixman_image_t* img = L->icon_w ? icon( r->node ) : NULL;
        if (img) {
            int iw = pixman_image_get_width( img ), ih = pixman_image_get_height( img );
            int ix = x + (M.icon_px - iw) / 2, iy = y + (r->h - ih) / 2;
            if (flags & AWL_TRAY_MENU_SYMBOLIC) {
                /* its shape in fg */
                const uint16_t a = (uint16_t)((fg & 0xff) * 0x101);
                const pixman_color_t col = { (uint16_t)((fg >> 24) * 0x101 * a / 0xffff),
                                             (uint16_t)(((fg >> 16) & 0xff) * 0x101 * a / 0xffff),
                                             (uint16_t)(((fg >> 8) & 0xff) * 0x101 * a / 0xffff), a };
                pixman_image_t* solid = pixman_image_create_solid_fill( &col );
                pixman_image_composite32( PIXMAN_OP_OVER, solid, img, pix, 0, 0, 0, 0, ix, iy, iw, ih );
                pixman_image_unref( solid );
            } else {
                pixman_image_composite32( PIXMAN_OP_OVER, img, NULL, pix, 0, 0, 0, 0, ix, iy, iw, ih );
            }
        }
        x += L->icon_w;
        int right = x0 + L->w - L->pad - L->arrow_w;
        if (n->shortcut) {
            int sw = (int)awl_host->text_width( font, n->shortcut );
            if (right - sw > x + L->gap) {
                awl_host->text_over( pix, font, right - sw, y, (unsigned)sw, (unsigned)r->h, n->shortcut, dim );
                right -= sw + 2 * L->gap;
            }
        }
        if (right > x) awl_host->text_over( pix, font, x, y, (unsigned)(right - x), (unsigned)r->h, n->label, fg );
        if (flags & AWL_TRAY_MENU_SUBMENU) {
            int aw = (int)awl_host->text_width( font, MORE );
            awl_host->text_over( pix, font, x0 + L->w - L->pad - aw, y, (unsigned)aw, (unsigned)r->h, MORE, fg );
        }
    }
}

/** the row at px, py, -1 none or one that can't be chosen */
static int hit( void* data, int x, int y ) {
    level_t* L = data;
    if (x < 0 || x >= L->w) return -1;
    for (int i = L->top; i < L->top + L->shown; i++) {
        const row_t* r = &L->rows[i];
        if (y >= r->y && y < r->y + r->h)
            return (M.snap->nodes[r->node].flags & (AWL_TRAY_MENU_SEPARATOR | AWL_TRAY_MENU_DISABLED)) ? -1 : i;
    }
    return -1;
}

/** a row, across the whole window (its frame too: where a submenu goes
 * beside it) */
static int box( void* data, int part, int* x, int* y, int* w, int* h ) {
    level_t* L = data;
    if (part < L->top || part >= L->top + L->shown) return 0;
    *x = -L->x0;
    *y = L->rows[part].y;
    *w = L->w + 2 * L->x0;
    *h = L->rows[part].h;
    return 1;
}

static const awl_window_painter_t painter = { measure, paint, hit, box };

/* ---- the levels ---- */

static void pend( int level, int row, int ms ) {
    M.pend_level = level;
    M.pend_row = row;
    awl_host->timer_set( M.timer, ms );
}

static void pend_cancel( void ) {
    M.pend_level = -1;
    awl_host->timer_set( M.timer, 0 );
}

static void level_free( level_t* L ) {
    free( L->rows );
    L->rows = NULL;
    L->nrows = L->shown = 0;
    L->node = -1;
    L->top = 0;
    L->left = 0;
    L->acc = 0;
    L->sel = -1;
}

static void leave_check( void );

/** hides levels k and up; with events, tells the menu the submenus closed */
static void close_from( int k, int events ) {
    if (k < 1) k = 1;
    if (M.depth <= k) return;
    /* what's pending for them, or closes them, too */
    if (M.pend_level >= k || (M.pend_level == k - 1 && M.pend_row < 0)) pend_cancel();
    while (M.depth > k) {
        level_t* L = &M.lv[--M.depth];
        /* first: hiding calls hidden() */
        int32_t id = M.snap && L->node >= 0 && L->node < M.snap->nnodes ? M.snap->nodes[L->node].id : 0;
        level_free( L );
        M.inside[M.depth] = 0;
        awl_host->window_hide( L->win );
        if (events && id) awl_tray_menu_event( M.serial, id, "closed" );
    }
    /* the row that had it open isn't highlighted any more */
    if (M.depth > 0) awl_host->window_redraw( M.lv[M.depth - 1].win );
    /* the pointer may have been on them */
    leave_check();
}

/** the row of level k showing node, -1 none */
static int row_of( int k, int node ) {
    const level_t* L = &M.lv[k];
    for (int i = 0; i < L->nrows; i++)
        if (L->rows[i].node == node) return i;
    return -1;
}

/** shows level k (>= 1) beside its row in level k - 1; hidden while the row
 * isn't (scrolled away), or while there are no entries yet (the menu's
 * filling it, AboutToShow) */
static void place_level( int k ) {
    level_t* L = &M.lv[k];
    Monitor* m = awl_host->window_monitor( M.lv[0].win );
    int x, y, w, h;
    int row = row_of( k - 1, L->node );
    if (!m || row < 0 || !awl_host->window_part_box( M.lv[k - 1].win, row, &x, &y, &w, &h )) {
        M.inside[k] = 0;
        M.quiet++;
        awl_host->window_hide( L->win );
        M.quiet--;
        return;
    }
    awl_host->window_redraw( L->win );
    awl_host->window_show_beside( L->win, m, x, y, w, h, M.lv[k - 1].left );
    int cx, cy, cw, ch;
    if (L->shown && awl_host->window_part_box( L->win, L->top, &cx, &cy, &cw, &ch )) L->left = cx < x;
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data );
static void scroll( awl_window_t* win, int row, int col, uint32_t orientation, double delta, int32_t value120,
                    void* data );
static void hover( awl_window_t* win, int row, int inside, void* data );
static void hidden( awl_window_t* win, void* data );
static int key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods, void* data );

/** level k's window, made on first use; NULL before awl is set up */
static awl_window_t* window( int k ) {
    level_t* L = &M.lv[k];
    if (L->win) return L->win;
    if (!(L->win = awl_host->window_create())) return NULL;
    void* data = (void*)(intptr_t)k;
    awl_host->window_set_painter( L->win, &painter, L );
    awl_host->window_set_click( L->win, click, data );
    awl_host->window_set_scroll( L->win, scroll, data );
    awl_host->window_set_hover( L->win, hover, data );
    awl_host->window_set_hidden( L->win, hidden, data );
    /* the menu has the keys, for all its levels */
    if (!k) awl_host->window_set_key( L->win, key, NULL );
    awl_host->window_set_menu_style( L->win );
    return L->win;
}

/** opens the submenu of level k's row beside it, closing what's open
 * beside k */
static void open_sub( int k, int row ) {
    const level_t* P = &M.lv[k];
    if (row < 0 || row >= P->nrows) return;
    int node = P->rows[row].node;
    if (M.depth > k + 1 && M.lv[k + 1].node == node) return;
    close_from( k + 1, 1 );
    if (k + 1 >= MAX_LEVELS || !window( k + 1 )) return;
    level_t* L = &M.lv[k + 1];
    level_free( L );
    L->node = node;
    M.depth = k + 2;
    /* lazy menus (Qt's) fill it now: a new layout comes */
    awl_tray_menu_event( M.serial, M.snap->nodes[node].id, "opened" );
    /* setting it again forgets the hovered row of the one shown before */
    awl_host->window_set_painter( L->win, &painter, L );
    awl_host->window_redraw( P->win );
    place_level( k + 1 );
}

static void pend_fire( void* data ) {
    int k = M.pend_level, row = M.pend_row;
    M.pend_level = -1;
    if (!M.snap || k < 0 || k >= M.depth) return;
    if (row >= 0) {
        open_sub( k, row );
        return;
    }
    /* not while the pointer's on them */
    for (int j = k + 1; j < M.depth; j++)
        if (M.inside[j]) return;
    close_from( k + 1, 1 );
}

/* ---- the menu open ---- */

static void reset( void ) {
    pend_cancel();
    awl_host->timer_set( M.leave_timer, 0 );
    M.leaving = 0;
    M.on_icon = 0;
    awl_host->timer_set( M.held_timer, 0 );
    awl_tray_menu_snap_free( M.held );
    M.held = NULL;
    icons_clear();
    awl_tray_menu_snap_free( M.snap );
    M.snap = NULL;
    for (int k = 0; k < MAX_LEVELS; k++) level_free( &M.lv[k] );
    memset( M.inside, 0, sizeof M.inside );
    M.serial = 0;
}

/** hides it; with events, tells the menu it closed (its submenus first) if
 * it was open */
static void close_menu( int events ) {
    uint64_t serial = M.serial;
    int open = M.snap != NULL;
    if (open) close_from( 1, events );
    M.depth = 0;
    /* first: hiding calls hidden() */
    reset();
    if (M.lv[0].win) awl_host->window_hide( M.lv[0].win );
    if (events && open) awl_tray_menu_event( serial, 0, "closed" );
}

/** awl hides its popups by itself too (a press elsewhere, the monitor
 * going away): the whole menu closes. Levels close_from() and close_menu()
 * hide are no longer open by then. */
static void hidden( awl_window_t* win, void* data ) {
    int k = (int)(intptr_t)data;
    if (M.snap && k < M.depth && !M.quiet) close_menu( 1 );
}

/** whether the pointer is off the menu, its submenus and its tray */
static int pointer_off( void ) {
    if (M.on_icon) return 0;
    for (int k = 0; k < M.depth; k++)
        if (M.inside[k]) return 0;
    return 1;
}

/** arms the leave timer once the pointer got off, disarms it when it's back */
static void leave_check( void ) {
    if (!M.serial || !M.leave_timer) return;
    const int off = pointer_off();
    if (off == M.leaving) return;
    M.leaving = off;
    awl_host->timer_set( M.leave_timer, off ? LEAVE_DELAY : 0 );
}

static void leave_fire( void* data ) {
    M.leaving = 0;
    if (M.serial && pointer_off()) {
        awl_vrb_printf( "tray: menu %llu: pointer left, closing\n", (unsigned long long)M.serial );
        close_menu( 1 );
    }
}

static void hover( awl_window_t* win, int row, int inside, void* data ) {
    int k = (int)(intptr_t)data;
    if (!M.snap || k >= M.depth || row >= M.lv[k].nrows) return;
    M.inside[k] = (char)inside;
    if (M.lv[k].sel != row) {
        M.lv[k].sel = row;
        awl_host->window_redraw( win );
    }
    leave_check();
    /* into a submenu in time: what hovering its menus asked for doesn't
     * happen */
    if (inside && M.pend_level >= 0 && M.pend_level < k) pend_cancel();
    const level_t* L = &M.lv[k];
    const int node = row >= 0 ? L->rows[row].node : -1;
    const int open = M.depth > k + 1;
    if (node >= 0 && (M.snap->nodes[node].flags & AWL_TRAY_MENU_SUBMENU)) {
        /* back on the row whose submenu is open: it stays */
        if (open && M.lv[k + 1].node == node) pend_cancel();
        /* with another one open, as long as for closing it */
        else pend( k, row, open ? CLOSE_DELAY : OPEN_DELAY );
        return;
    }
    if (row < 0) {
        /* off the rows (into the submenu, off the menu): what's open stays,
         * what hovering would have opened doesn't */
        if (M.pend_level == k && M.pend_row >= 0) pend_cancel();
        return;
    }
    if (open) pend( k, -1, CLOSE_DELAY );
    else pend_cancel();
}

static void click( awl_window_t* win, int row, int col, uint32_t button, void* data ) {
    int k = (int)(intptr_t)data;
    if (!M.snap || k >= M.depth || row < 0 || row >= M.lv[k].nrows || (button != BTN_LEFT && button != BTN_RIGHT))
        return;
    const awl_tray_menu_node_t* n = &M.snap->nodes[M.lv[k].rows[row].node];
    if (n->flags & (AWL_TRAY_MENU_SEPARATOR | AWL_TRAY_MENU_DISABLED)) return;
    if (n->flags & AWL_TRAY_MENU_SUBMENU) {
        pend_cancel();
        open_sub( k, row );
        return;
    }
    awl_tray_menu_event( M.serial, n->id, "clicked" );
    close_menu( 1 );
}

/** whether level L's row i can be chosen */
static int choosable( const level_t* L, int i ) {
    return i >= 0 && i < L->nrows &&
           !(M.snap->nodes[L->rows[i].node].flags & (AWL_TRAY_MENU_SEPARATOR | AWL_TRAY_MENU_DISABLED));
}

/** moves level k's highlight to the next row that can be chosen in
 * direction dir (1 down, -1 up), round the end; scrolls it into view */
static void move_sel( int k, int dir ) {
    level_t* L = &M.lv[k];
    int i = L->sel;
    for (int n = 0; n < L->nrows; n++) {
        i = i < 0 ? (dir > 0 ? 0 : L->nrows - 1) : (i + dir + L->nrows) % L->nrows;
        if (choosable( L, i )) break;
    }
    if (!choosable( L, i )) return;
    L->sel = i;
    if (i < L->top) L->top = i;
    else if (L->shown && i >= L->top + L->shown) L->top = i - L->shown + 1;
    awl_host->window_redraw( L->win );
    if (k + 1 < M.depth) place_level( k + 1 );
}

/** opens the submenu of level k's highlighted row with its first row
 * highlighted; returns whether it is one */
static int open_sel( int k ) {
    level_t* L = &M.lv[k];
    if (!choosable( L, L->sel ) || !(M.snap->nodes[L->rows[L->sel].node].flags & AWL_TRAY_MENU_SUBMENU))
        return 0;
    pend_cancel();
    open_sub( k, L->sel );
    if (M.depth > k + 1) move_sel( k + 1, 1 );
    return 1;
}

static int key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods, void* data ) {
    if (!M.snap || !M.depth) return 0;
    const int k = M.depth - 1;
    level_t* L = &M.lv[k];
    switch (sym) {
    case XKB_KEY_Escape:
        if (k) close_from( k, 1 );
        else close_menu( 1 );
        return 1;
    case XKB_KEY_Down:
    case XKB_KEY_Tab:
        move_sel( k, 1 );
        return 1;
    case XKB_KEY_Up:
    case XKB_KEY_ISO_Left_Tab:
        move_sel( k, -1 );
        return 1;
    case XKB_KEY_Right:
        open_sel( k );
        return 1;
    case XKB_KEY_Left:
        if (k) close_from( k, 1 );
        return 1;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        if (choosable( L, L->sel ) && !open_sel( k )) {
            awl_tray_menu_event( M.serial, M.snap->nodes[L->rows[L->sel].node].id, "clicked" );
            close_menu( 1 );
        }
        return 1;
    }
    return 0;
}

static void scroll( awl_window_t* win, int row, int col, uint32_t orientation, double delta, int32_t value120,
                    void* data ) {
    int k = (int)(intptr_t)data;
    if (!M.snap || k >= M.depth || orientation != 0) return;
    level_t* L = &M.lv[k];
    L->acc += value120 ? value120 / 120.0 : delta / 10.0;
    int steps = (int)L->acc;
    if (!steps) return;
    L->acc -= steps;
    L->top += steps;
    if (L->top < 0) L->top = 0;
    awl_host->window_redraw( L->win );
    /* its submenu moves with its row */
    if (k + 1 < M.depth) place_level( k + 1 );
}

/** snap's node for level k (>= 1), whose parent level is snap's node
 * parent: the same id, else the same label (apps that rebuild their menus
 * give the entries new ids); -1 if snap has no such submenu */
static int match( const awl_tray_menu_snap_t* snap, int parent, int k ) {
    const awl_tray_menu_node_t* old = &M.snap->nodes[M.lv[k].node];
    const awl_tray_menu_node_t* p = &snap->nodes[parent];
    for (int pass = 0; pass < 2; pass++)
        for (int i = p->first; i < p->first + p->n; i++) {
            const awl_tray_menu_node_t* n = &snap->nodes[i];
            if ((n->flags & AWL_TRAY_MENU_SUBMENU) &&
                (pass ? !strcmp( n->label, old->label ) : n->id == old->id))
                return i;
        }
    return -1;
}

/** how many of the levels open snap has too, their nodes into nodes */
static int matched( const awl_tray_menu_snap_t* snap, int* nodes ) {
    if (!snap->nodes[0].n) return 0;
    nodes[0] = 0;
    int k = 1;
    for (; k < M.depth && (nodes[k] = match( snap, nodes[k - 1], k )) >= 0; k++)
        ;
    return k;
}

/** makes snap the layout shown, closing what it doesn't have */
static void apply( awl_tray_menu_snap_t* snap ) {
    int nodes[MAX_LEVELS];
    int keep = matched( snap, nodes );
    awl_host->timer_set( M.held_timer, 0 );
    if (M.held != snap) awl_tray_menu_snap_free( M.held );
    M.held = NULL;
    if (!keep) {
        awl_tray_menu_snap_free( snap );
        close_menu( 1 );
        return;
    }
    /* the ones it doesn't have are gone, their ids too: no events */
    close_from( keep, 0 );
    /* a submenu about to open: the same one in the new layout, if it has it */
    const int pk = M.pend_level;
    int32_t pid = 0;
    char* plabel = NULL;
    if (pk >= 0 && M.pend_row >= 0) {
        const awl_tray_menu_node_t* n = &M.snap->nodes[M.lv[pk].rows[M.pend_row].node];
        pid = n->id;
        plabel = strdup( n->label );
    }
    icons_clear();
    awl_tray_menu_snap_free( M.snap );
    M.snap = snap;
    for (int k = 0; k < M.depth; k++) {
        M.lv[k].node = nodes[k];
        awl_host->window_redraw( M.lv[k].win );
        if (k) place_level( k );
    }
    if (plabel) {
        int row = -1;
        const level_t* L = &M.lv[pk];
        for (int pass = 0; pass < 2 && row < 0; pass++)
            for (int i = 0; i < L->nrows && row < 0; i++) {
                const awl_tray_menu_node_t* n = &snap->nodes[L->rows[i].node];
                if ((n->flags & AWL_TRAY_MENU_SUBMENU) && (pass ? !strcmp( n->label, plabel ) : n->id == pid))
                    row = i;
            }
        if (row >= 0) M.pend_row = row;
        else pend_cancel();
        free( plabel );
    }
}

static void held_fire( void* data ) {
    if (M.held) apply( M.held );
}

void awl_tray_menu_window_toggle( uint64_t serial, const char* monitor, int center_x ) {
    static uint32_t gen;
    awl_vrb_printf( "tray: menu %llu: toggle on %s at %d (open: %llu)\n", (unsigned long long)serial, monitor,
                    center_x, (unsigned long long)M.serial );
    awl_tray_tooltip_hide();
    if (M.serial == serial && !strcmp( M.monitor, monitor )) {
        close_menu( 1 );
        return;
    }
    close_menu( 1 );
    if (!window( 0 )) return;
    if (!M.timer) M.timer = awl_host->timer_add( pend_fire, NULL );
    if (!M.held_timer) M.held_timer = awl_host->timer_add( held_fire, NULL );
    if (!M.leave_timer) M.leave_timer = awl_host->timer_add( leave_fire, NULL );
    M.serial = serial;
    /* opened by a click on it */
    M.on_icon = 1;
    snprintf( M.monitor, sizeof M.monitor, "%s", monitor );
    M.center_x = center_x;
    M.gen = ++gen;
    awl_tray_menu_request( serial, M.gen );
    leave_check();
}

void awl_tray_menu_window_pointer( uint64_t serial, int inside ) {
    if (serial != M.serial) return;
    M.on_icon = inside;
    leave_check();
}

void awl_tray_menu_window_snap( awl_tray_menu_snap_t* snap ) {
    awl_vrb_printf( "tray: menu %llu: layout %u of request %u, %d entries (open: %llu, request %u)\n",
                    (unsigned long long)snap->serial, snap->revision, snap->gen, snap->nnodes,
                    (unsigned long long)M.serial, M.gen );
    if (!M.serial || snap->serial != M.serial || snap->gen != M.gen) {
        awl_tray_menu_snap_free( snap );
        return;
    }
    if (M.snap) {
        /* a new layout of the menu shown: its submenus open, if it still has
         * them, else the newest layout once they've had time to come back */
        int nodes[MAX_LEVELS];
        if (matched( snap, nodes ) < M.depth) {
            awl_vrb_printf( "tray: menu %llu: layout %u lacks a submenu open, held\n",
                            (unsigned long long)M.serial, snap->revision );
            if (!M.held) awl_host->timer_set( M.held_timer, LAYOUT_GRACE );
            awl_tray_menu_snap_free( M.held );
            M.held = snap;
            return;
        }
        apply( snap );
        return;
    }
    M.snap = snap;
    Monitor* m = awl_host->monitor_named( M.monitor );
    if (!m || !snap->nodes[0].n) {
        if (m) awl_log_printf( "tray: menu has no entries\n" );
        reset();
        return;
    }
    level_free( &M.lv[0] );
    M.lv[0].node = 0;
    M.depth = 1;
    awl_host->window_set_painter( M.lv[0].win, &painter, &M.lv[0] );
    awl_host->window_show( M.lv[0].win, m, M.center_x );
    awl_tray_menu_event( M.serial, 0, "opened" );
}

void awl_tray_menu_window_gone( uint64_t serial ) {
    awl_vrb_printf( "tray: menu %llu: gone\n", (unsigned long long)serial );
    if (serial == M.serial) close_menu( 0 );
}

int awl_tray_menu_window_is_open( void ) {
    return M.serial != 0;
}

int awl_tray_menu_window_shows( uint64_t serial ) {
    return serial && M.serial == serial;
}

void awl_tray_menu_window_fini( void ) {
    close_menu( 1 );
    awl_host->timer_remove( M.timer );
    awl_host->timer_remove( M.held_timer );
    awl_host->timer_remove( M.leave_timer );
    M.timer = M.held_timer = M.leave_timer = NULL;
    for (int k = 0; k < MAX_LEVELS; k++) {
        if (M.lv[k].win) awl_host->window_destroy( M.lv[k].win );
        M.lv[k].win = NULL;
    }
}
