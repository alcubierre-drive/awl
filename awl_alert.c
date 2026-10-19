#include "awl_alert.h"
#include "awl_bar.h"
#include "awl_config.h"
#include "awl_log.h"
#include "awl_output.h"
#include "awl_redraw.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** the alert's look, see `awl_alert_draw()`: its font size relative to the
 * bar's, its distance from the bottom left corner in logical pixels, its
 * opacity */
#define ALERT_FONT_SCALE 1.5
#define ALERT_MARGIN 12
#define ALERT_ALPHA 0.9f

static void notify( const char* text );

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
    if (!texts[id][0]) notify( text );
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

/** a critical desktop notification of text */
static void notify( const char* text ) {
    spawn( &(Arg){ .v = (const char*[]){ "notify-send", "-a", "awl", "-u", "critical",
            "awl alert", text, NULL } } );
}

/* ---- drawing ---- */

/** the topmost tree, `awl_alert_init()`'s */
static struct wlr_scene_tree* tree;

/** clicks go through the alert */
static bool accepts_input( struct wlr_scene_buffer* buffer, double* sx, double* sy ) {
    return false;
}

void awl_alert_init( void ) {
    tree = wlr_scene_tree_create( &scene->tree );
}

void awl_alert_addmon( Monitor* m ) {
    m->alert = wlr_scene_buffer_create( tree, NULL );
    m->alert->point_accepts_input = accepts_input;
    wlr_scene_buffer_set_opacity( m->alert, ALERT_ALPHA );
    wlr_scene_node_set_enabled( &m->alert->node, 0 );
    m->alert_version = (unsigned int)-1;
}

void awl_alert_draw( Monitor* m ) {
    static const pixman_color_t fg = { 0xffff, 0xffff, 0xffff, 0xffff },
                                bg = { 0xc000, 0x0000, 0x0000, 0xffff };
    const char* text = awl_alert_text();
    const float s = m->wlr_output->scale;
    struct fcft_font* font;
    pixman_image_t* pix;
    Buffer* buf;
    int pad, w, h;

    if (!m->alert) /* createmon() arranges before it makes it */
        return;
    if (m->alert_version == version && m->alert_scale == s
            && wlr_box_equal( &m->alert_area, &m->m ))
        return;
    m->alert_version = version;
    m->alert_scale = s;
    m->alert_area = m->m;
    if (!text || !m->wlr_output->enabled || m->m.width <= 0) {
        wlr_scene_node_set_enabled( &m->alert->node, 0 );
        return;
    }

    /* the font only while drawing; the registry keeps it loaded */
    if (!(font = awl_font_get( cfg->font, cfg->fontsize * ALERT_FONT_SCALE, s ))) {
        wlr_scene_node_set_enabled( &m->alert->node, 0 );
        return;
    }
    pad = font->height * 6 / 10;
    h = font->height + pad;
    w = (int)awl_draw_text_width( font, text ) + 2 * pad;
    if (w > (int)(m->m.width * s) - 2 * (int)(ALERT_MARGIN * s))
        w = (int)(m->m.width * s) - 2 * (int)(ALERT_MARGIN * s);
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
    wlr_scene_node_set_position( &m->alert->node, m->m.x + ALERT_MARGIN,
            m->m.y + m->m.height - ALERT_MARGIN - (int)(h / s) );
    wlr_scene_node_raise_to_top( &tree->node );
    wlr_scene_node_set_enabled( &m->alert->node, 1 );
}

void awl_alert_draw_all( void ) {
    Monitor* m;
    wl_list_for_each( m, &mons, link )
        awl_alert_draw( m );
}
