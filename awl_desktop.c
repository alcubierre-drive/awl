#include "awl_desktop.h"
#include "awl_draw.h"
#include "awl_util.h"
#include "awl_plugin_abi.h"
#include "awl_plugin_host.h"

/** everything a panel's pixels depend on; zeroed before filling, so the
 * padding compares equal too */
typedef struct {
    uint64_t version;  /* the library's, see awl_plugin_api_t */
    unsigned reloads;
    int enabled;
    struct wlr_box w;
    float scale;
} DesktopKey;

/** the desktop panel on one monitor: its scene nodes and what it was drawn for */
typedef struct DesktopView {
    Monitor* m;
    struct wl_list link;
    struct wlr_scene_tree* tree; /* at the panel's top left */
    struct wlr_scene_blur* blur;
    struct wlr_scene_buffer* buffer;
    DesktopKey key;
    int drawn;
} DesktopView;

static struct {
    struct wlr_scene_tree* tree;
    desktop_style_t cfg;
    struct wl_list views; /* DesktopView.link */
    unsigned reloads;     /* versions of different libraries don't compare */
} desk;

void desktop_init( struct wlr_scene_tree* tree, const desktop_style_t* cfg ) {
    desk.tree = tree;
    desk.cfg = *cfg;
    wl_list_init( &desk.views );
}

/** (re)does v's blur, under its buffer, from desk.cfg */
static void configure( DesktopView* v ) {
    if (!desk.cfg.blur) {
        if (v->blur) wlr_scene_node_destroy( &v->blur->node );
        v->blur = NULL;
        return;
    }
    if (!v->blur) {
        v->blur = wlr_scene_blur_create( v->tree, 0, 0 );
        wlr_scene_node_lower_to_bottom( &v->blur->node );
        wlr_scene_blur_set_should_only_blur_bottom_layer( v->blur, 0 );
    }
    wlr_scene_blur_set_corner_radius( v->blur, desk.cfg.radius );
    wlr_scene_blur_set_strength( v->blur, desk.cfg.blur_strength );
    wlr_scene_blur_set_alpha( v->blur, desk.cfg.blur_alpha );
}

static DesktopView* view_of( Monitor* m ) {
    DesktopView* v;
    wl_list_for_each( v, &desk.views, link )
        if (v->m == m) return v;
    return NULL;
}

void desktop_configure( const desktop_style_t* cfg ) {
    desk.cfg = *cfg;
    DesktopView* v;
    wl_list_for_each( v, &desk.views, link ) {
        configure( v );
        v->drawn = 0; /* the radius is drawn into the buffer too */
        desktop_update( v->m );
    }
}

void desktop_addmon( Monitor* m ) {
    DesktopView* v = ecalloc( 1, sizeof(*v) );
    v->m = m;
    v->tree = wlr_scene_tree_create( desk.tree );
    v->buffer = wlr_scene_buffer_create( v->tree, NULL );
    configure( v );
    wlr_scene_node_set_enabled( &v->tree->node, 0 );
    wl_list_insert( &desk.views, &v->link );
    desktop_update( m );
}

void desktop_removemon( Monitor* m ) {
    DesktopView* v = view_of( m );
    if (!v) return;
    wlr_scene_node_destroy( &v->tree->node );
    wl_list_remove( &v->link );
    free( v );
}

/** like the bar's buffers (awl_bar.c): read-only for the renderer */
static void buffer_destroy( struct wlr_buffer* wb ) {
    Buffer* b = wl_container_of( wb, b, base );
    free( b );
}

static bool buffer_begin( struct wlr_buffer* wb, uint32_t flags, void** data,
                          uint32_t* format, size_t* stride ) {
    Buffer* b = wl_container_of( wb, b, base );
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
    *data = b->data;
    *stride = b->stride;
    *format = DRM_FORMAT_ARGB8888;
    return true;
}

static void buffer_end( struct wlr_buffer* wb ) {
}

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .begin_data_ptr_access = buffer_begin,
    .end_data_ptr_access = buffer_end,
};

/** what m's panel draws in: :c:member:`Monitor.desktopfont`, else the bar's; NULL if none */
static struct fcft_font* font_of( Monitor* m ) {
    return m->desktopfont ? m->desktopfont : m->bar.font;
}

/** returns whether there is anything to show */
static int draw( DesktopView* v, const awl_plugin_api_t* api ) {
    Monitor* m = v->m;
    struct fcft_font* font = font_of( m );
    const float s = m->wlr_output->scale;
    const int radius = (int)lroundf( desk.cfg.radius * s );
    int x, y, width, height;
    api->desktop_measure( font, (int)(m->w.width * s), (int)(m->w.height * s), radius, s,
                          &x, &y, &width, &height );
    if (width <= 0 || height <= 0) return 0;

    int32_t stride = awl_draw_stride( width );
    Buffer* b = ecalloc( 1, sizeof(Buffer) + (size_t)stride * height );
    b->stride = stride;
    b->w = width;
    b->h = height;
    wlr_buffer_init( &b->base, &buffer_impl, width, height );

    pixman_image_t* pix = awl_draw_image( width, height, b->data, stride );
    if (pix) {
        api->desktop_draw( pix, font, b->data, stride, width, height, radius, s );
        pixman_image_unref( pix );
    }

    const int lw = (int)lroundf( width / s ), lh = (int)lroundf( height / s );
    wlr_scene_buffer_set_buffer( v->buffer, &b->base );
    wlr_scene_buffer_set_dest_size( v->buffer, lw, lh );
    wlr_buffer_drop( &b->base ); /* the scene holds it now */
    if (v->blur) wlr_scene_blur_set_size( v->blur, lw, lh );
    wlr_scene_node_set_position( &v->tree->node, m->w.x + (int)lroundf( x / s ),
                                 m->w.y + (int)lroundf( y / s ) );
    return 1;
}

void desktop_update( Monitor* m ) {
    DesktopView* v = view_of( m );
    if (!v) return;
    const awl_plugin_api_t* api = awl_plugins_api();

    DesktopKey key;
    memset( &key, 0, sizeof(key) );
    key.version = api ? api->desktop_version() : 0;
    key.reloads = desk.reloads;
    key.enabled = m->wlr_output->enabled;
    key.w = m->w;
    key.scale = m->wlr_output->scale;
    if (v->drawn && !memcmp( &key, &v->key, sizeof(key) )) return;
    v->key = key;
    v->drawn = 1;

    int on = key.version && key.enabled && font_of( m ) && m->w.width > 0 && m->w.height > 0
          && draw( v, api );
    wlr_scene_node_set_enabled( &v->tree->node, on );
}

void desktop_fontchanged( Monitor* m ) {
    DesktopView* v = view_of( m );
    if (!v) return;
    v->drawn = 0;
    desktop_update( m );
}

void desktop_update_all( void ) {
    DesktopView* v;
    wl_list_for_each( v, &desk.views, link )
        desktop_update( v->m );
}

void desktop_reloaded( void ) {
    desk.reloads++;
    desktop_update_all();
}

int desktop_click( int button, uint32_t mods ) {
    const awl_plugin_api_t* api = awl_plugins_api();
    if (!api || !api->desktop_click( button, mods )) return 0;
    desktop_update_all();
    return 1;
}
