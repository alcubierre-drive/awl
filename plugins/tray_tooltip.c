/* The items' tooltips, see tray_tooltip.h. Main thread only. */

#include "tray_tooltip.h"
#include "../awl_host.h"
#include "tray_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** ms the pointer rests on an icon before its tooltip shows */
enum { TOOLTIP_DELAY = 500 };

static struct {
    awl_window_t* win;
    awl_timer_t* timer;
    /** what is shown, or waits for the timer; token 0: nothing */
    uint32_t token;
    char monitor[64];
    int center_x;
    char* text;
} T;

static void show( void* data ) {
    Monitor* m = awl_host->monitor_named( T.monitor );
    if (!T.token || !m || awl_tray_menu_window_is_open()) return;
    awl_host->window_show( T.win, m, T.center_x, T.text, 0 );
}

void awl_tray_tooltip_hide( void ) {
    T.token = 0;
    if (T.timer) awl_host->timer_set( T.timer, 0 );
    if (T.win) awl_host->window_hide( T.win );
}

void awl_tray_tooltip_show( uint32_t token, const char* monitor, int center_x, const char* text ) {
    char* t = strdup( text );
    if (!t || !(T.win || (T.win = awl_host->window_create())) ||
        !(T.timer || (T.timer = awl_host->timer_add( show, NULL )))) {
        free( t );
        return;
    }
    /* from one icon to the next: at once */
    const int shown = awl_host->window_monitor( T.win ) != NULL;
    T.token = token;
    snprintf( T.monitor, sizeof T.monitor, "%s", monitor );
    T.center_x = center_x;
    free( T.text );
    T.text = t;
    if (shown) show( NULL );
    else awl_host->timer_set( T.timer, TOOLTIP_DELAY );
}

void awl_tray_tooltip_drop( uint32_t token ) {
    if (token == T.token) awl_tray_tooltip_hide();
}

void awl_tray_tooltip_fini( void ) {
    awl_tray_tooltip_hide();
    awl_host->timer_remove( T.timer );
    if (T.win) awl_host->window_destroy( T.win );
    free( T.text );
    memset( &T, 0, sizeof T );
}
