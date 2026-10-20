#include "awl_window.h"
#include "awl_config.h"
#include "awl_draw.h"
#include "awl_util.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct awl_window {
    struct wlr_scene_tree* tree; /* at the window's top left */
    struct wlr_scene_blur* blur;
    struct wlr_scene_buffer* buffer;
    awl_window_style_t style;
    char* text;
    /** where it is shown, NULL while hidden */
    Monitor* m;
    /** how it is placed: by the bar (awl_window_show()), beside anchor
     * (awl_window_show_beside()) or at a spot of the window area
     * (awl_window_show_at()) */
    enum { PLACE_BAR, PLACE_BESIDE, PLACE_AT } placement;
    int center_x, top;
    int left;
    struct wlr_box anchor;
    int at, dx, dy;
    /** what the buffer was drawn for; drawn = 0 forces a redraw */
    int drawn;
    float scale;
    struct fcft_font* font;
    /** its size, logical pixels */
    int lw, lh;
    /** what was drawn, for the pointer */
    awl_window_layout_t layout;
    /** takes the pointer if click or scroll is set; hover is the
     * highlighted row (drawn only with click) */
    awl_window_click_t click;
    void* click_data;
    awl_window_scroll_t scroll;
    void* scroll_data;
    awl_window_hidden_t hidden;
    void* hidden_data;
    awl_window_hover_t hover_cb;
    void* hover_data;
    /** takes the keyboard while shown, if key is set */
    awl_window_key_t key;
    void* key_data;
    int hover;
    /** the pointer is on it (awl_window_pointer_motion() since the last
     * awl_window_pointer_leave()) */
    int pointer_in;
    /** draws itself instead of the text, if painter.paint is set */
    awl_window_painter_t painter;
    void* painter_data;
};

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

/** clicks and the pointer go through */
static bool accepts_input( struct wlr_scene_buffer* buffer, double* sx, double* sy ) {
    return false;
}

/** (re)does the blur, under the buffer, from win->style */
static void configure( awl_window_t* win ) {
    if (!win->style.blur) {
        if (win->blur) wlr_scene_node_destroy( &win->blur->node );
        win->blur = NULL;
        return;
    }
    if (!win->blur) {
        win->blur = wlr_scene_blur_create( win->tree, win->lw, win->lh );
        wlr_scene_node_lower_to_bottom( &win->blur->node );
        wlr_scene_blur_set_should_only_blur_bottom_layer( win->blur, 0 );
    }
    wlr_scene_blur_set_corner_radius( win->blur, win->style.radius );
    wlr_scene_blur_set_strength( win->blur, win->style.blur_strength );
    wlr_scene_blur_set_alpha( win->blur, win->style.blur_alpha );
}

awl_window_t* awl_window_create( struct wlr_scene_tree* parent, const awl_window_style_t* style ) {
    awl_window_t* win = ecalloc( 1, sizeof(*win) );
    win->style = *style;
    win->tree = wlr_scene_tree_create( parent );
    win->buffer = wlr_scene_buffer_create( win->tree, NULL );
    win->buffer->point_accepts_input = accepts_input;
    win->hover = -1;
    configure( win );
    wlr_scene_node_set_enabled( &win->tree->node, 0 );
    return win;
}

void awl_window_destroy( awl_window_t* win ) {
    if (!win) return;
    wlr_scene_node_destroy( &win->tree->node );
    free( win->text );
    free( win );
}

void awl_window_set_style( awl_window_t* win, const awl_window_style_t* style ) {
    win->style = *style;
    configure( win );
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_set_text( awl_window_t* win, const char* text ) {
    if (!win) return;
    if (!text) text = "";
    if (win->text && !strcmp( win->text, text )) return;
    free( win->text );
    win->text = strdup( text );
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_set_painter( awl_window_t* win, const awl_window_painter_t* painter, void* data ) {
    if (!win) return;
    win->painter = painter ? *painter : (awl_window_painter_t){ 0 };
    win->painter_data = data;
    win->hover = -1;
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_redraw( awl_window_t* win ) {
    if (!win) return;
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

/** what m's popups draw with: the popup font, else the bar's */
static struct fcft_font* font_of( Monitor* m ) {
    return m->popupfont ? m->popupfont : m->drw.font;
}

/** an AWL_WINDOW_AT_* that is a strip, AWL_WINDOW_AT_BAR's and the edges' */
static int is_strip( int at ) {
    return at == AWL_WINDOW_AT_BAR || at == AWL_WINDOW_AT_STRIP_TOP || at == AWL_WINDOW_AT_STRIP_BOTTOM;
}

/** shown as a strip */
static int as_strip( const awl_window_t* win ) {
    return win->placement == PLACE_AT && is_strip( win->at );
}

/** the style its frame is drawn in: the bar's spot has none */
static awl_window_style_t frame_style( const awl_window_t* win ) {
    awl_window_style_t st = win->style;
    if (as_strip( win )) st.border_width = st.radius = st.padding = 0;
    return st;
}

/** a painted window's layout, its frame around what the painter measures */
static int layout_painted( awl_window_t* win, awl_window_layout_t* L, struct fcft_font* font, float s,
                           int max_w, int max_h ) {
    const awl_window_style_t st = frame_style( win );
    const int inset = awl_window_inset( &st, s );
    int cw = 0, ch = 0;
    if (max_w - 2 * inset <= 0 || max_h - 2 * inset <= 0) return 0;
    if (!win->painter.measure( win->painter_data, font, s, max_w - 2 * inset, max_h - 2 * inset, &cw, &ch ))
        return 0;
    /* never more than allowed, whatever the painter says */
    if (cw > max_w - 2 * inset) cw = max_w - 2 * inset;
    if (ch > max_h - 2 * inset) ch = max_h - 2 * inset;
    return awl_window_layout_frame( L, &st, s, cw, ch );
}

/** the painter's content into data, its frame first */
static void paint_painted( awl_window_t* win, const awl_window_layout_t* L, struct fcft_font* font, float s,
                           uint32_t* data, int32_t stride ) {
    const awl_window_style_t st = frame_style( win );
    awl_window_paint_frame( L, &st, data, stride );
    pixman_image_t* pix = awl_draw_image( (unsigned)L->width, (unsigned)L->height, data, stride );
    if (!pix) return;
    win->painter.paint( win->painter_data, pix, font, s, L->x0, L->y0, win->click ? win->hover : -1 );
    pixman_image_unref( pix );
}

/** returns whether there is anything to show */
static int draw( awl_window_t* win ) {
    Monitor* m = win->m;
    struct fcft_font* font = font_of( m );
    const float s = m->wlr_output->scale;
    awl_window_layout_t L;
    const awl_window_style_t st = frame_style( win );
    /* room for awl_window_place()'s margins; the bar's size over it */
    int w = m->m.width - 2 * awl_window_margin( win->style.margin, m->m.width );
    int h = m->w.height - 2 * awl_window_margin( win->style.margin, m->w.height );
    if (as_strip( win )) {
        w = m->m.width;
        h = m->b.real_height;
    }
    if (win->painter.paint ? !layout_painted( win, &L, font, s, (int)(w * s), (int)(h * s) )
                           : !awl_window_layout( &L, win->text, &st, font, s, (int)(w * s), (int)(h * s) ))
        return 0;

    int32_t stride = awl_draw_stride( L.width );
    Buffer* b = ecalloc( 1, sizeof(Buffer) + (size_t)stride * L.height );
    b->stride = stride;
    b->w = L.width;
    b->h = L.height;
    wlr_buffer_init( &b->base, &buffer_impl, L.width, L.height );
    if (win->painter.paint)
        paint_painted( win, &L, font, s, b->data, stride );
    else
        awl_window_paint( &L, win->text, &st, font, win->click ? win->hover : -1, b->data, stride );
    win->layout = L;

    win->lw = (int)lroundf( L.width / s );
    win->lh = (int)lroundf( L.height / s );
    wlr_scene_buffer_set_buffer( win->buffer, &b->base );
    wlr_scene_buffer_set_dest_size( win->buffer, win->lw, win->lh );
    wlr_buffer_drop( &b->base ); /* the scene holds it now */
    if (win->blur) {
        wlr_scene_blur_set_size( win->blur, win->lw, win->lh );
        wlr_scene_blur_set_corner_radius( win->blur, st.radius );
    }
    return 1;
}

static void place( awl_window_t* win ) {
    Monitor* m = win->m;
    int x, y;
    if (as_strip( win )) {
        const int top = win->at == AWL_WINDOW_AT_BAR ? cfg->bar.top : win->at == AWL_WINDOW_AT_STRIP_TOP;
        x = m->m.x;
        y = top ? m->m.y : m->m.y + m->m.height - win->lh;
    } else if (win->placement == PLACE_AT)
        awl_window_place_at( m->m.x, m->m.width, m->w.y, m->w.height, win->lw, win->lh, win->at, win->dx,
                             win->dy, win->style.margin, &x, &y );
    else if (win->placement == PLACE_BESIDE)
        /* the content's top at the anchor's */
        awl_window_place_beside( m->m.x, m->m.width, m->w.y, m->w.height, win->lw, win->lh, win->anchor.x,
                                 win->anchor.y - (int)lroundf( win->layout.y0 / win->scale ), win->anchor.width,
                                 win->left, win->style.margin, &x, &y );
    else
        awl_window_place( m->m.x, m->m.width, m->w.y, m->w.height, win->lw, win->lh,
                          win->center_x, win->top, win->style.margin, &x, &y );
    wlr_scene_node_set_position( &win->tree->node, x, y );
}

void awl_window_update( awl_window_t* win, Monitor* m ) {
    if (!win || win->m != m || !m) return;
    if (!m->wlr_output->enabled || !m->drw.m || !font_of( m )) {
        wlr_scene_node_set_enabled( &win->tree->node, 0 );
        return;
    }
    const float s = m->wlr_output->scale;
    if (!win->drawn || win->scale != s || win->font != font_of( m )) {
        win->drawn = draw( win );
        win->scale = s;
        win->font = font_of( m );
    }
    if (!win->drawn) {
        wlr_scene_node_set_enabled( &win->tree->node, 0 );
        return;
    }
    place( win );
    wlr_scene_node_set_enabled( &win->tree->node, 1 );
}

void awl_window_show( awl_window_t* win, Monitor* m, int center_x, int top ) {
    if (win->m != m || as_strip( win )) win->drawn = 0;
    win->m = m;
    win->center_x = center_x;
    win->top = top;
    win->placement = PLACE_BAR;
    awl_window_update( win, m );
}

void awl_window_show_beside( awl_window_t* win, Monitor* m, const struct wlr_box* anchor, int left ) {
    if (win->m != m || as_strip( win )) win->drawn = 0;
    win->m = m;
    win->placement = PLACE_BESIDE;
    win->left = left;
    win->anchor = *anchor;
    awl_window_update( win, m );
}

void awl_window_show_at( awl_window_t* win, Monitor* m, int at, int dx, int dy ) {
    /* to, from or between strips: its size, frame or place change */
    if (win->m != m || ((as_strip( win ) || is_strip( at )) && (win->placement != PLACE_AT || win->at != at)))
        win->drawn = 0;
    win->m = m;
    win->placement = PLACE_AT;
    win->at = at;
    win->dx = dx;
    win->dy = dy;
    awl_window_update( win, m );
}

void awl_window_hide( awl_window_t* win ) {
    if (!win || !win->m) return;
    win->m = NULL;
    if (win->hover != -1) {
        win->hover = -1;
        win->drawn = 0;
    }
    wlr_scene_node_set_enabled( &win->tree->node, 0 );
    /* last: it may show or destroy the window */
    if (win->hidden) win->hidden( win, win->hidden_data );
}

Monitor* awl_window_monitor( const awl_window_t* win ) {
    return win ? win->m : NULL;
}

int awl_window_box( const awl_window_t* win, struct wlr_box* box ) {
    *box = (struct wlr_box){ win->tree->node.x, win->tree->node.y, win->lw, win->lh };
    return win->tree->node.enabled;
}

void awl_window_removemon( awl_window_t* win, Monitor* m ) {
    if (win && win->m == m) awl_window_hide( win );
}

void awl_window_set_click( awl_window_t* win, awl_window_click_t click, void* data ) {
    if (!win) return;
    win->click = click;
    win->click_data = data;
    if (!click) awl_window_pointer_leave( win );
}

void awl_window_set_hidden( awl_window_t* win, awl_window_hidden_t hidden, void* data ) {
    if (!win) return;
    win->hidden = hidden;
    win->hidden_data = data;
}

void awl_window_set_hover( awl_window_t* win, awl_window_hover_t hover, void* data ) {
    if (!win) return;
    win->hover_cb = hover;
    win->hover_data = data;
}

void awl_window_set_key( awl_window_t* win, awl_window_key_t key, void* data ) {
    if (!win) return;
    win->key = key;
    win->key_data = data;
}

int awl_window_takes_keys( const awl_window_t* win ) {
    return win->key && win->m;
}

int awl_window_key( awl_window_t* win, uint32_t sym, const char* utf8, uint32_t mods ) {
    return win->key && win->key( win, sym, utf8, mods, win->key_data );
}

void awl_window_set_scroll( awl_window_t* win, awl_window_scroll_t scroll, void* data ) {
    if (!win) return;
    win->scroll = scroll;
    win->scroll_data = data;
    if (!scroll && !win->click) awl_window_pointer_leave( win );
}

/** whether win is visible at lx, ly, or less than gx, gy beside it */
static int inside( const awl_window_t* win, double lx, double ly, int gx, int gy ) {
    if (!win->m || !win->drawn || !win->tree->node.enabled) return 0;
    struct wlr_box box = { win->tree->node.x - gx, win->tree->node.y - gy, win->lw + 2 * gx,
                           win->lh + 2 * gy };
    return wlr_box_contains_point( &box, lx, ly );
}

int awl_window_contains( const awl_window_t* win, double lx, double ly ) {
    return win->m && inside( win, lx, ly, awl_window_margin( win->style.margin, win->m->m.width ),
                             awl_window_margin( win->style.margin, win->m->w.height ) );
}

int awl_window_takes_pointer( const awl_window_t* win, double lx, double ly ) {
    return (win->click || win->scroll) && inside( win, lx, ly, 0, 0 );
}

/** the cell at layout coordinates lx, ly, see awl_window_cell_at() */
static int cell_at( const awl_window_t* win, double lx, double ly, int* row, int* col ) {
    *row = *col = -1;
    if (!win->m || !win->drawn) return 0;
    if (win->painter.paint) {
        const awl_window_layout_t* L = &win->layout;
        int px = (int)((lx - win->tree->node.x) * win->scale) - L->x0;
        int py = (int)((ly - win->tree->node.y) * win->scale) - L->y0;
        if (px < 0 || py < 0 || px >= L->width - 2 * L->x0 || py >= L->height - 2 * L->y0 || !win->painter.hit)
            return 0;
        *row = win->painter.hit( win->painter_data, px, py );
        if (*row < 0) *row = -1;
        else *col = 0;
        return *row >= 0;
    }
    return awl_window_cell_at( &win->layout, (int)((lx - win->tree->node.x) * win->scale),
                               (int)((ly - win->tree->node.y) * win->scale), row, col );
}

int awl_window_pointer_motion( awl_window_t* win, double lx, double ly ) {
    int row, col;
    cell_at( win, lx, ly, &row, &col );
    const int entered = !win->pointer_in;
    win->pointer_in = 1;
    if (row != win->hover) {
        win->hover = row;
        win->drawn = 0;
        awl_window_update( win, win->m );
    } else if (!entered)
        return row;
    /* last: it may hide or show windows, this one too */
    if (win->hover_cb) win->hover_cb( win, row, 1, win->hover_data );
    return row;
}

void awl_window_pointer_leave( awl_window_t* win ) {
    const int was_in = win->pointer_in;
    win->pointer_in = 0;
    if (win->hover != -1) {
        win->hover = -1;
        win->drawn = 0;
        if (win->m) awl_window_update( win, win->m );
    }
    if (was_in && win->hover_cb) win->hover_cb( win, -1, 0, win->hover_data );
}

void awl_window_pointer_button( awl_window_t* win, double lx, double ly, uint32_t button, int* row, int* col ) {
    cell_at( win, lx, ly, row, col );
    if (win->click) win->click( win, *row, *col, button, win->click_data );
}

void awl_window_pointer_axis( awl_window_t* win, double lx, double ly, uint32_t orientation,
                              double delta, int32_t value120, int* row, int* col ) {
    cell_at( win, lx, ly, row, col );
    if (win->scroll) win->scroll( win, *row, *col, orientation, delta, value120, win->scroll_data );
}

int awl_window_hover_row( const awl_window_t* win ) {
    return win->hover;
}

int awl_window_part_box( const awl_window_t* win, int part, struct wlr_box* box ) {
    int x, y, w, h;
    if (!win->m || !win->drawn || !win->painter.paint || !win->painter.box ||
        !win->painter.box( win->painter_data, part, &x, &y, &w, &h ))
        return 0;
    const double s = win->scale;
    box->x = win->tree->node.x + (int)lround( (win->layout.x0 + x) / s );
    box->y = win->tree->node.y + (int)lround( (win->layout.y0 + y) / s );
    box->width = (int)lround( w / s );
    box->height = (int)lround( h / s );
    return 1;
}

int awl_window_cell_center( const awl_window_t* win, int row, int col, double* lx, double* ly ) {
    int x, y, w, h;
    if (win->painter.paint) {
        if (!win->m || !win->drawn || !win->painter.box || col != 0 ||
            !win->painter.box( win->painter_data, row, &x, &y, &w, &h ))
            return 0;
        *lx = win->tree->node.x + (win->layout.x0 + x + w / 2.0) / win->scale;
        *ly = win->tree->node.y + (win->layout.y0 + y + h / 2.0) / win->scale;
        return 1;
    }
    if (!win->m || !win->drawn || !awl_window_cell_box( &win->layout, row, col, &x, &y, &w, &h )) return 0;
    /* the cell itself, not the gaps after it */
    w = win->layout.widths[col];
    h = win->layout.fh;
    *lx = win->tree->node.x + (x + w / 2.0) / win->scale;
    *ly = win->tree->node.y + (y + h / 2.0) / win->scale;
    return 1;
}
