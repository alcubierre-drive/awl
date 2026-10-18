/* The items' tooltips, see tooltip.h. */

#include "tooltip.h"
#include "../awl_host.h"
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** ms the pointer rests on an icon before its tooltip shows */
enum { TOOLTIP_DELAY = 500 };

typedef struct {
    uint32_t token;
    char* monitor;
    int center_x;
    /** NULL: leave */
    char* text;
} hover_t;

/* ---- awl's main thread ---- */

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

static void main_hover( void* data ) {
    hover_t* h = data;
    if (!h->text) {
        if (h->token == T.token) awl_tray_tooltip_hide();
    } else if ((T.win || (T.win = awl_host->window_create_kept())) &&
               (T.timer || (T.timer = awl_host->timer_add( show, NULL )))) {
        /* from one icon to the next: at once */
        const int shown = awl_host->window_monitor( T.win ) != NULL;
        T.token = h->token;
        snprintf( T.monitor, sizeof T.monitor, "%s", h->monitor );
        T.center_x = h->center_x;
        free( T.text );
        T.text = h->text;
        h->text = NULL;
        if (shown) show( NULL );
        else awl_host->timer_set( T.timer, TOOLTIP_DELAY );
    }
    free( h->monitor );
    free( h->text );
    free( h );
}

/* ---- the tray's GTK thread ---- */

static void post( uint32_t token, const char* monitor, int center_x, const char* text ) {
    hover_t* h = calloc( 1, sizeof(*h) );
    *h = (hover_t){ token, strdup( monitor ? monitor : "" ), center_x, text ? strdup( text ) : NULL };
    awl_host->post( main_hover, h );
}

uint32_t awl_tray_tooltip_enter( const char* monitor, int center_x, const char* text ) {
    static uint32_t next;
    if (!text || !*text) return 0;
    if (!++next) next = 1;
    post( next, monitor, center_x, text );
    return next;
}

void awl_tray_tooltip_leave( uint32_t token ) {
    if (token) post( token, NULL, 0, NULL );
}
