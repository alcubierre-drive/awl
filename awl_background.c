#include "awl_background.h"
#include "awl_util.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** one decoded image, read-only for the renderer */
typedef struct {
    struct wlr_buffer base;
    awl_image_t* img;
} Image;

/** an image on one monitor; its size kept here, since the scene buffer lets
 * go of the wlr_buffer once it has made a texture of it */
typedef struct {
    struct wlr_scene_buffer* node;
    int width, height;
} Shown;

typedef struct BackgroundView {
    Monitor* m;
    struct wl_list link;
    /* the current image and, while fading, the previous one below it */
    Shown cur, old;
    struct wlr_box box; /* what they were placed for; 0 if hidden */
} BackgroundView;

static struct {
    struct wlr_scene_tree* tree;
    struct wl_list views;  /* BackgroundView.link */
    Image* cur;            /* NULL until the first one */
    struct wl_event_source* fade_timer;
    struct timespec fade_start;
    unsigned int fade_ms;
} bg;

/** a frame's worth at 60 Hz */
static const int fade_tick_ms = 16;

static void image_destroy( struct wlr_buffer* wb ) {
    Image* i = wl_container_of( wb, i, base );
    free( i->img );
    free( i );
}

static bool image_begin( struct wlr_buffer* wb, uint32_t flags, void** data,
                         uint32_t* format, size_t* stride ) {
    Image* i = wl_container_of( wb, i, base );
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = i->img->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)i->img->stride;
    return true;
}

static void image_end( struct wlr_buffer* wb ) {
}

static const struct wlr_buffer_impl image_impl = {
    .destroy = image_destroy,
    .begin_data_ptr_access = image_begin,
    .end_data_ptr_access = image_end,
};

static BackgroundView* view_of( Monitor* m ) {
    BackgroundView* v;
    wl_list_for_each( v, &bg.views, link )
        if (v->m == m) return v;
    return NULL;
}

/** s's image, scaled to cover box and centered, the overhang cut off */
static void place( const Shown* s, struct wlr_box box ) {
    if (!s->node) return;
    double iw = s->width, ih = s->height;
    double k = MAX( box.width / iw, box.height / ih );
    double sw = box.width / k, sh = box.height / k;
    wlr_scene_buffer_set_source_box( s->node, &(struct wlr_fbox){ (iw - sw) / 2, (ih - sh) / 2, sw, sh } );
    wlr_scene_buffer_set_dest_size( s->node, box.width, box.height );
    wlr_scene_node_set_position( &s->node->node, box.x, box.y );
}

static Shown show( Image* i, struct wlr_box box ) {
    Shown s = { wlr_scene_buffer_create( bg.tree, &i->base ), i->img->width, i->img->height };
    place( &s, box );
    return s;
}

static void hide( Shown* s ) {
    if (s->node) wlr_scene_node_destroy( &s->node->node );
    *s = (Shown){0};
}

static double elapsed_ms( const struct timespec* since ) {
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return (now.tv_sec - since->tv_sec) * 1e3 + (now.tv_nsec - since->tv_nsec) / 1e6;
}

static int fade_step( void* data ) {
    BackgroundView* v;
    double t = bg.fade_ms ? elapsed_ms( &bg.fade_start ) / bg.fade_ms : 1;
    if (t > 1) t = 1;
    float a = (float)(t * t * (3 - 2 * t)); /* smoothstep */
    wl_list_for_each( v, &bg.views, link ) {
        if (v->cur.node) wlr_scene_buffer_set_opacity( v->cur.node, a );
        if (t >= 1) hide( &v->old );
    }
    if (t < 1) wl_event_source_timer_update( bg.fade_timer, fade_tick_ms );
    return 0;
}

void background_init( struct wlr_scene_tree* tree, struct wl_event_loop* loop ) {
    bg.tree = tree;
    wl_list_init( &bg.views );
    bg.fade_timer = wl_event_loop_add_timer( loop, fade_step, NULL );
}

void background_fini( void ) {
    if (bg.fade_timer) wl_event_source_remove( bg.fade_timer );
    bg.fade_timer = NULL;
    if (bg.cur) wlr_buffer_drop( &bg.cur->base );
    bg.cur = NULL;
}

void background_set( awl_image_t* img, unsigned int fade_ms ) {
    BackgroundView* v;
    Image* i = ecalloc( 1, sizeof(*i) );
    i->img = img;
    wlr_buffer_init( &i->base, &image_impl, img->width, img->height );

    /* a fade still running is cut short: its old image goes, its new one
     * shows fully and is what this one fades in over */
    int fade = 0;
    wl_list_for_each( v, &bg.views, link ) {
        hide( &v->old );
        if (v->cur.node) wlr_scene_buffer_set_opacity( v->cur.node, 1 );
        v->old = v->cur;
        v->cur = (Shown){0};
        if (!v->box.width) continue;
        v->cur = show( i, v->box );
        if (v->old.node && fade_ms) {
            wlr_scene_buffer_set_opacity( v->cur.node, 0 );
            fade = 1;
        } else {
            hide( &v->old );
        }
    }
    /* kept for monitors that show up later; the previous one is freed once
     * no node holds it anymore */
    if (bg.cur) wlr_buffer_drop( &bg.cur->base );
    bg.cur = i;

    if (fade) {
        clock_gettime( CLOCK_MONOTONIC, &bg.fade_start );
        bg.fade_ms = fade_ms;
        wl_event_source_timer_update( bg.fade_timer, fade_tick_ms );
    }
}

void background_addmon( Monitor* m ) {
    BackgroundView* v = ecalloc( 1, sizeof(*v) );
    v->m = m;
    wl_list_insert( &bg.views, &v->link );
    background_update( m );
}

void background_removemon( Monitor* m ) {
    BackgroundView* v = view_of( m );
    if (!v) return;
    hide( &v->old );
    hide( &v->cur );
    wl_list_remove( &v->link );
    free( v );
}

void background_update( Monitor* m ) {
    BackgroundView* v = view_of( m );
    if (!v) return;
    struct wlr_box box = m->m;
    if (!m->wlr_output->enabled || box.width <= 0 || box.height <= 0)
        box = (struct wlr_box){0};
    if (!memcmp( &box, &v->box, sizeof(box) )) return;
    v->box = box;

    if (!box.width) {
        hide( &v->old );
        hide( &v->cur );
        return;
    }
    if (!v->cur.node && bg.cur) v->cur = show( bg.cur, box );
    place( &v->cur, box );
    place( &v->old, box );
}
