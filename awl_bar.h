#pragma once

/**
 * The bars, part of awl: each monitor's `awl_bar_t` (``Monitor.bar``) and
 * the scene buffer that shows it (awl_bar.c). awl lays the widgets out,
 * draws them into a ring of two buffers, hands the scene only the pixels
 * that changed, and routes the pointer to them: presses, the wheel, hovering
 * (`widget_t.callback_hover`, with its delays) and `widget_t.callback_view`.
 * The widgets themselves come from the plugin library
 * (awl_plugin_host.h). Main thread only.
 */

#include "awl_server.h"

/** the hover's timers, before the first monitor */
void barinit(void);
void barfini(void);
/** sets up m's bar with the library's widgets; it is drawn with the others */
void baraddmon(Monitor *m);
/** m is going: forgets the hover's widgets in its bar, releases its fonts
 * and tears its bar down (closemon() then leaves the bar alone);
 * cleanupmon() destroys the scene nodes and buffers */
void barremovemon(Monitor *m);
/** before the plugin library goes (`awl_plugins_reload()`): ends the hover
 * (callback_leave, callback_view) while its widgets are still there, and
 * clears every bar's widgets */
void barwidgetsclear(void);
/** after it: every bar gets the library's widgets again */
void barwidgetsattach(void);
/** after plugins_config.c changed: the bars' fonts (and with them their
 * height), the popups' and the desktop file list's, the window list's blur */
void barconfigure(void);

/** m's bar width, and its font and height if m's scale changed (or it has
 * none yet) */
void updatebar(Monitor *m);
/** redraws every bar now; barsdirty() is what to call on a change */
void drawbars(void);
/** Something the bars show changed: they are redrawn once the event loop
 * has handled what it has now, however many changes came in. */
void barsdirty(void);
/** shows or hides m's bar */
void togglebar_mon(Monitor *m);
/** a new, cleared Buffer of width x height pixels for the scene: the bars'
 * and the alerts' */
Buffer *buffercreate(int width, int height);

/** whether the pointer is on selmon's bar */
int pointeronbar(void);
/** c is the client under the pointer, which may cover the bar. Exclusive
 * focus (e.g. a launcher) deliberately doesn't reset the hover
 * state. */
void updatehover(Client *c);
/** The wheel turned by delta with the pointer on selmon's bar: the widget
 * there gets it (its callback_scroll, in steps); returns whether one was
 * there. */
int baraxis(double delta);
/** A press with the pointer on selmon's bar: closes what the hover of the
 * widget there opened, or keeps it from opening. Returns 1 if the widget's
 * callback_click took the press; else ``*click`` becomes ClkBar. */
int barbutton(uint32_t button, unsigned int *click);

/* m's awl_bar_t (awl_bar_type.h) */
/** makes bar m's (m->bar, unused), without font or widgets */
void awl_bar_setup(awl_bar_t *bar, Monitor *m);
/** Makes the registry's font (awl_font_get(), or awl_font_get_exact() if
 * !fallback) bar's, giving back its old one. Returns it; NULL (bar keeps
 * its font) if none loads. */
struct fcft_font *awl_bar_set_font(awl_bar_t *bar, const char *name, float size, float scale,
        int fallback);
/** makes bar->pix an image of bits, see awl_draw_image() */
void awl_bar_prepare_drawing(awl_bar_t *bar, unsigned int w, unsigned int h, uint32_t *bits, int stride);
/** lets go of what `awl_bar_prepare_drawing()` pointed at */
void awl_bar_finish_drawing(awl_bar_t *bar);
/** gives back bar's font and image, frees its widgets and empties it: its
 * monitor has no bar anymore (bar->m is NULL) */
void awl_bar_teardown(awl_bar_t *bar);
/** Frees every widget's userdata and empties the widget lists, so nothing in
 * the bar points into ``libawlplugins.so`` anymore. */
void awl_bar_widgets_clear(awl_bar_t *bar);
