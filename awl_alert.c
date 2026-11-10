#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_config.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_output.h"
#include "awl_redraw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void notify( awl_server_t* srv, const char* text );

static const char* const names[AwlAlertLast] = {
    [AwlAlertLogind] = "logind",
    [AwlAlertLocker] = "locker",
};
/** The alerts (:c:member:`awl_server_t.alerts`), from awl_alert_init() to
 * awl_alert_fini() */
struct awl_alerts {
    /** each alert's text, empty while it's down */
    char texts[AwlAlertLast][128];
    /** counts the changes, for :c:member:`Monitor.alert_version` */
    unsigned int version;
    /** the topmost tree, :c:func:`awl_alert_init`'s */
    struct wlr_scene_tree* tree;
};

void awl_alert_set( awl_server_t* srv, awl_alert_id_t id, const char* fmt, ... ) {
    char text[sizeof srv->alerts->texts[0]];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( text, sizeof text, fmt, ap );
    va_end( ap );
    if (!strcmp( text, srv->alerts->texts[id] )) return;
    awl_err_printf( "ALERT (%s): %s\n", names[id], text );
    /* one notification per alert, not per change of its text */
    if (!srv->alerts->texts[id][0]) notify( srv, text );
    memcpy( srv->alerts->texts[id], text, sizeof text );
    srv->alerts->version++;
    awl_redraw_request();
}

void awl_alert_clear( awl_server_t* srv, awl_alert_id_t id ) {
    if (!srv->alerts->texts[id][0]) return;
    awl_log_printf( "alert (%s) cleared\n", names[id] );
    srv->alerts->texts[id][0] = 0;
    srv->alerts->version++;
    awl_redraw_request();
}

int awl_alert_up( awl_server_t* srv, awl_alert_id_t id ) {
    return srv->alerts->texts[id][0] != 0;
}

const char* awl_alert_text( awl_server_t* srv ) {
    for (int i = 0; i < AwlAlertLast; i++)
        if (srv->alerts->texts[i][0]) return srv->alerts->texts[i];
    return NULL;
}

/** a critical notification of text, awl's own (no bus needed) */
static void notify( awl_server_t* srv, const char* text ) {
    awl_notif_send_t n = AWL_NOTIF_SEND_INIT;
    n.summary = "awl alert";
    n.body = text;
    n.urgency = AWL_NOTIF_CRITICAL;
    if (!awl_notifications_send( srv, &n ))
        awl_wrn_printf( "alert: no notification (the server isn't running)\n" );
}

/* ---- drawing ---- */

/** clicks go through the alert */
static bool accepts_input( struct wlr_scene_buffer* buffer, double* sx, double* sy ) {
    return false;
}

void awl_alert_init( awl_server_t *srv ) {
    srv->alerts = ecalloc( 1, sizeof *srv->alerts );
    srv->alerts->tree = wlr_scene_tree_create( &srv->scene->tree );
}

void awl_alert_addmon( awl_server_t* srv, Monitor* m ) {
    m->alert = wlr_scene_buffer_create( srv->alerts->tree, NULL );
    m->alert->point_accepts_input = accepts_input;
    wlr_scene_buffer_set_opacity( m->alert, srv->cfg->alert.opacity );
    wlr_scene_node_set_enabled( &m->alert->node, 0 );
    m->alert_version = (unsigned int)-1;
}

void awl_alert_configure( awl_server_t *srv ) {
    Monitor* m;
    wl_list_for_each( m, &srv->out.mons, link ) {
        if (!m->alert) continue;
        wlr_scene_buffer_set_opacity( m->alert, srv->cfg->alert.opacity );
        m->alert_version = (unsigned int)-1;
    }
    awl_alert_draw_all(srv);
}

void awl_alert_draw( awl_server_t* srv, Monitor* m ) {
    const pixman_color_t fg = color_8bit_to_16bit( srv->cfg->colors.alert_fg ),
                         bg = color_8bit_to_16bit( srv->cfg->colors.alert_bg );
    const int margin = srv->cfg->alert.margin;
    const char* text = awl_alert_text( srv );
    const float s = m->wlr_output->scale;
    struct fcft_font* font;
    pixman_image_t* pix;
    Buffer* buf;
    int pad, w, h;

    if (!m->alert) /* createmon() arranges before it makes it */
        return;
    if (m->alert_version == srv->alerts->version && m->alert_scale == s
            && wlr_box_equal( &m->alert_area, &m->w ))
        return;
    m->alert_version = srv->alerts->version;
    m->alert_scale = s;
    /* the window area: clear of the bar and of exclusive layer surfaces */
    m->alert_area = m->w;
    if (!text || !m->wlr_output->enabled || m->w.width <= 0) {
        wlr_scene_node_set_enabled( &m->alert->node, 0 );
        return;
    }

    /* the font only while drawing; the registry keeps it loaded */
    if (!(font = awl_font_get( srv->cfg->font, srv->cfg->fontsize * srv->cfg->alert.font_scale, s ))) {
        wlr_scene_node_set_enabled( &m->alert->node, 0 );
        return;
    }
    pad = font->height * 6 / 10;
    h = font->height + pad;
    w = (int)awl_draw_text_width( font, text ) + 2 * pad;
    if (w > (int)(m->w.width * s) - 2 * (int)(margin * s))
        w = (int)(m->w.width * s) - 2 * (int)(margin * s);
    if (w <= 0) {
        awl_font_release( font );
        wlr_scene_node_set_enabled( &m->alert->node, 0 );
        return;
    }

    buf = buffercreate( w, h );
    if ((pix = awl_draw_image( w, h, buf->data, buf->stride ))) {
        awl_draw_text( pix, font, 0, 0, w, h, pad, text, fg, bg );
        pixman_image_unref( pix );
    }
    awl_font_release( font );

    wlr_scene_buffer_set_buffer( m->alert, &buf->base );
    wlr_buffer_drop( &buf->base ); /* the scene holds it now */
    wlr_scene_buffer_set_dest_size( m->alert, (int)(w / s), (int)(h / s) );
    wlr_scene_node_set_position( &m->alert->node, m->w.x + margin,
            m->w.y + m->w.height - margin - (int)(h / s) );
    wlr_scene_node_raise_to_top( &srv->alerts->tree->node );
    wlr_scene_node_set_enabled( &m->alert->node, 1 );
}

void awl_alert_draw_all( awl_server_t *srv ) {
    Monitor* m;
    wl_list_for_each( m, &srv->out.mons, link )
        awl_alert_draw( srv, m );
}

void awl_alert_fini( awl_server_t *srv ) {
    /* the tree goes with the scene */
    free( srv->alerts );
    srv->alerts = NULL;
}
