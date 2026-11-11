/* The items' tooltips, see tray_tooltip.h. Main thread only. */

#include "tray_tooltip.h"
#include "../awl_host.h"
#include "tray_menu.h"
#include "../plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** ms the pointer rests on an icon before its tooltip shows */
enum { TOOLTIP_DELAY = 200 };

struct awl_tray_tooltip {
    /** the root, for the menus' window: no tooltip over an open menu */
    awl_plugin_data_t* p;
    awl_window_t* win;
    awl_timer_t* timer;
    /** what is shown, or waits for the timer; token 0: nothing */
    uint32_t token;
    char monitor[64];
    int center_x;
    char* text;
};

static void show( void* data ) {
    awl_tray_tooltip_t* t = data;
    Monitor* m = awl_host->monitor_named( awl_host->srv, t->monitor );
    if (!t->token || !m || awl_tray_menu_window_is_open( t->p->tray_menu )) return;
    awl_host->window_set_text( awl_host->srv, t->win, t->text );
    awl_host->window_show( awl_host->srv, t->win, m, t->center_x );
}

awl_tray_tooltip_t* awl_tray_tooltip_new( awl_plugin_data_t* p ) {
    awl_tray_tooltip_t* t = calloc( 1, sizeof *t );
    if (t) t->p = p;
    return t;
}

void awl_tray_tooltip_hide( awl_tray_tooltip_t* t ) {
    if (!t) return;
    t->token = 0;
    if (t->timer) awl_host->timer_set( t->timer, 0 );
    if (t->win) awl_host->window_hide( awl_host->srv, t->win );
}

void awl_tray_tooltip_show( awl_tray_tooltip_t* t, uint32_t token, const char* monitor, int center_x,
                            const char* text ) {
    if (!t) return;
    char* copy = strdup( text );
    if (!copy || !(t->win || (t->win = awl_host->window_create( awl_host->srv ))) ||
        !(t->timer || (t->timer = awl_host->timer_add( awl_host->srv, show, t )))) {
        free( copy );
        return;
    }
    /* from one icon to the next: at once */
    const int shown = awl_host->window_monitor( t->win ) != NULL;
    t->token = token;
    snprintf( t->monitor, sizeof t->monitor, "%s", monitor );
    t->center_x = center_x;
    free( t->text );
    t->text = copy;
    if (shown) show( t );
    else awl_host->timer_set( t->timer, TOOLTIP_DELAY );
}

void awl_tray_tooltip_drop( awl_tray_tooltip_t* t, uint32_t token ) {
    if (t && token == t->token) awl_tray_tooltip_hide( t );
}

void awl_tray_tooltip_fini( awl_tray_tooltip_t* t ) {
    if (!t) return;
    awl_tray_tooltip_hide( t );
    awl_host->timer_remove( t->timer );
    if (t->win) awl_host->window_destroy( awl_host->srv, t->win );
    free( t->text );
    free( t );
}
