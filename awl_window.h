#pragma once

/**
 * Popups that awl draws itself, part of awl: a box of text, or what a
 * painter (awl_window_draw.h) draws, like the plugins' calendar, next to
 * the bar, styled by ``cfg->popup`` and ``cfg->colors``' popup_* (border,
 * corner radius, padding; blurred behind like the launcher), in its
 * ``font`` (NULL: the bar's), its ``margin`` away from the bar and the
 * monitor's sides. Not a client: no Wayland
 * surface; no keyboard focus, but with a key callback it takes the keys
 * while shown (awl_window_set_key()). Without a click or scroll callback it takes no
 * input, clicks, scrolling and the pointer go through to what is below; with
 * either, awl_cursor.c routes all of the pointer to it (awl_window_takes_pointer()):
 * it gets the presses and the scrolling, a callback missing for one of them
 * just drops those, and with a click callback it highlights the row under
 * the pointer. Main thread only.
 *
 * A window is a scene tree in the parent awl gives it, shown on one monitor
 * at a time. What the plugins reach through awl_host_t (the setters,
 * awl_window_redraw(), awl_window_hide(), awl_window_monitor()) takes NULL
 * for win and does nothing.
 */

#include "awl_core.h"
#include "awl_window_draw.h" /* awl_window_style_t, the drawing */

typedef struct awl_window awl_window_t;
/** a press on win: row and col its cell (awl_window_cell_at(), -1 off the
 * text), button a BTN_* */
typedef void (*awl_window_click_t)( awl_window_t* win, int row, int col, uint32_t button, void* data );
/** a scroll on win: row and col as for a click; orientation a
 * WL_POINTER_AXIS_* (0 vertical, 1 horizontal); delta the motion along it as
 * wlr_pointer_axis_event has it, positive down or right; value120 a wheel's
 * in 120ths of a notch (WLR_POINTER_AXIS_DISCRETE_STEP), 0 for continuous
 * scrolling (a touchpad), which only has delta */
typedef void (*awl_window_scroll_t)( awl_window_t* win, int row, int col, uint32_t orientation,
                                     double delta, int32_t value120, void* data );
/** win was hidden: by awl_window_hide(), whoever called it, its monitor
 * going away (awl_window_removemon()) included; not when it is destroyed,
 * nor while its output is off (it comes back with it) */
typedef void (*awl_window_hidden_t)( awl_window_t* win, void* data );

/** the pointer moved onto row (part) of win, -1: none (something that
 * can't be chosen, or off win); inside: whether it is on win at all */
typedef void (*awl_window_hover_t)( awl_window_t* win, int row, int inside, void* data );

/** a key pressed (or repeated) while win has the keyboard: sym an
 * XKB_KEY_*, utf8 what it types ("" for nothing), mods WLR_MODIFIER_*'s;
 * returns whether it took the key, else awl's key bindings get it */
typedef int (*awl_window_key_t)( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods,
                                 void* data );

/** A hidden, empty window in parent, an empty scene tree already placed in
 * the stacking order. */
awl_window_t* awl_window_create( struct wlr_scene_tree* parent, const awl_window_style_t* style );
void awl_window_destroy( awl_window_t* win );

/** moves win's scene tree into parent, at the top of its stacking order */
void awl_window_reparent( awl_window_t* win, struct wlr_scene_tree* parent );

/** plugins_config.c was reloaded */
void awl_window_set_style( awl_window_t* win, const awl_window_style_t* style );

/** Replaces the text: rows separated by '\n', columns by '\t', plain text
 * (no markup). Columns are as wide as their widest cell. More rows than fit
 * the monitor's window area end in a row "…" instead. */
void awl_window_set_text( awl_window_t* win, const char* text );

/** Makes win draw itself with painter (copied; with data) instead of its
 * text, NULL for the text again (awl_window_draw.h). Parts stand in for
 * cells: clicks and scrolls get the part under the pointer as their row,
 * column 0 (-1, -1 off them), and with a click callback the painter gets
 * the hovered one. */
void awl_window_set_painter( awl_window_t* win, const awl_window_painter_t* painter, void* data );
/** what a painter draws changed: measures and paints it again if shown */
void awl_window_redraw( awl_window_t* win );

/** Shows the window on m, or moves it there: on the bar's side of m's window
 * area (the top if top, else the bottom), centered on center_x (logical
 * pixels from m's left edge) but kept within m. It draws in m->popupfont,
 * else in the bar's font; with neither it stays hidden. */
void awl_window_show( awl_window_t* win, Monitor* m, int center_x, int top );
/** Shows the window on m, or moves it there, beside anchor (layout
 * coordinates; a row of another window, of the menu a submenu belongs to):
 * right of it (left, if left), else on the other side, where it fits on m,
 * its content's top at anchor's (awl_window_place_beside()). */
void awl_window_show_beside( awl_window_t* win, Monitor* m, const struct wlr_box* anchor, int left );
/** Shows the window on m, or moves it there, at a spot of m's window area,
 * an AWL_WINDOW_AT_* (awl.h), the margin away from its edges, moved by
 * dx, dy logical pixels (awl_window_place_at()); the strips
 * (AWL_WINDOW_AT_BAR, AWL_WINDOW_AT_STRIP_TOP, _BOTTOM) instead cover m's
 * bar, or the bar's height along its top or bottom edge, flush (dx, dy
 * ignored). */
void awl_window_show_at( awl_window_t* win, Monitor* m, int at, int dx, int dy );
void awl_window_hide( awl_window_t* win );
/** the monitor it is shown on, NULL while hidden */
Monitor* awl_window_monitor( const awl_window_t* win );
/** its last placement, layout coordinates and logical size; returns
 * whether it is visible (shown, drawn and its monitor enabled) */
int awl_window_box( const awl_window_t* win, struct wlr_box* box );
/** the room it has on m at a spot (`awl_window_show_at()`): m's window
 * area less its margins, layout coordinates */
struct wlr_box awl_window_room( const awl_window_t* win, Monitor* m );

/** m was arranged, or its font or scale changed: places and redraws the
 * window if it is shown there (cheap if nothing changed), hides it if m is
 * disabled */
void awl_window_update( awl_window_t* win, Monitor* m );
/** m is going away: hides the window if it is shown there */
void awl_window_removemon( awl_window_t* win, Monitor* m );

/** Gives win the pointer, presses going to click (with data); NULL lets it
 * all through again. */
void awl_window_set_click( awl_window_t* win, awl_window_click_t click, void* data );
/** While shown, win takes the keyboard: every key pressed goes to key
 * (with data) before awl's bindings, none to the clients (awl_popup.h
 * holds the grab); NULL: no more. */
void awl_window_set_key( awl_window_t* win, awl_window_key_t key, void* data );
/** whether win has a key callback and is shown */
int awl_window_takes_keys( const awl_window_t* win );
/** a key for win: calls its key callback; returns whether it took it */
int awl_window_key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods );
/** Gives win the pointer, scrolling going to scroll (with data); NULL lets
 * it through again unless there is a click callback. */
void awl_window_set_scroll( awl_window_t* win, awl_window_scroll_t scroll, void* data );
/** Calls hidden (with data) whenever win is hidden from now on, NULL: no
 * more. */
void awl_window_set_hidden( awl_window_t* win, awl_window_hidden_t hidden, void* data );
/** Calls hover (with data) whenever the highlighted row changes, or the
 * pointer comes onto win or leaves it, which needs a click callback; NULL:
 * no more. */
void awl_window_set_hover( awl_window_t* win, awl_window_hover_t hover, void* data );
/** whether win is visible at lx, ly (layout coordinates) or in the margin
 * around it (awl_window_style_t's, the gap to the bar the pointer crosses
 * to get to it), whether or not it takes the pointer */
int awl_window_contains( const awl_window_t* win, double lx, double ly );
/** whether win takes the pointer (has a click or scroll callback) and is
 * visible at lx, ly (layout coordinates) */
int awl_window_takes_pointer( const awl_window_t* win, double lx, double ly );
/** The pointer is at lx, ly over win: highlights the row there. Returns
 * it, -1 off the text. */
int awl_window_pointer_motion( awl_window_t* win, double lx, double ly );
/** the pointer left win: no row highlighted */
void awl_window_pointer_leave( awl_window_t* win );
/** A press at lx, ly: calls the click callback; the cell into *row, *col. */
void awl_window_pointer_button( awl_window_t* win, double lx, double ly, uint32_t button, int* row, int* col );
/** A scroll at lx, ly: calls the scroll callback, if any; the cell into
 * *row, *col. */
void awl_window_pointer_axis( awl_window_t* win, double lx, double ly, uint32_t orientation,
                              double delta, int32_t value120, int* row, int* col );
/** the highlighted row, -1 none */
int awl_window_hover_row( const awl_window_t* win );
/** where a painted window's part is, layout coordinates and logical size;
 * 0 if it is not shown */
int awl_window_part_box( const awl_window_t* win, int part, struct wlr_box* box );
/** the center of cell row, col in layout coordinates (of part row, col 0,
 * of a painted window), for tests; 0 if it is not shown */
int awl_window_cell_center( const awl_window_t* win, int row, int col, double* lx, double* ly );
