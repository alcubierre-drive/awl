#include "awl_alert.h"
#include "awl_log.h"
#include "awl_redraw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* awl.c: the notification goes through its spawn() */
void awl_alert_notify( const char* text );

static const char* names[AwlAlertLast] = {
    [AwlAlertLogind] = "logind",
    [AwlAlertLocker] = "locker",
};
static char texts[AwlAlertLast][128];
static unsigned int version;

void awl_alert_set( awl_alert_id_t id, const char* fmt, ... ) {
    char text[sizeof texts[0]];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( text, sizeof text, fmt, ap );
    va_end( ap );
    if (!strcmp( text, texts[id] )) return;
    awl_err_printf( "ALERT (%s): %s\n", names[id], text );
    /* one notification per alert, not per change of its text */
    if (!texts[id][0]) awl_alert_notify( text );
    memcpy( texts[id], text, sizeof text );
    version++;
    awl_redraw_request();
}

void awl_alert_clear( awl_alert_id_t id ) {
    if (!texts[id][0]) return;
    awl_log_printf( "alert (%s) cleared\n", names[id] );
    texts[id][0] = 0;
    version++;
    awl_redraw_request();
}

unsigned int awl_alert_version( void ) {
    return version;
}

int awl_alert_up( awl_alert_id_t id ) {
    return texts[id][0] != 0;
}

const char* awl_alert_text( void ) {
    for (int i = 0; i < AwlAlertLast; i++)
        if (texts[i][0]) return texts[i];
    return NULL;
}
