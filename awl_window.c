#include "awl_window.h"
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
    int center_x, top;
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
    int hover;
    /** draws itself instead of the text, if painter.paint is set */
    awl_window_painter_t painter;
    void* painter_data;
};

/** like the bar's buffers in awl.c: read-only for the renderer */
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
    if (!text) text = "";
    if (win->text && !strcmp( win->text, text )) return;
    free( win->text );
    win->text = strdup( text );
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_set_painter( awl_window_t* win, const awl_window_painter_t* painter, void* data ) {
    win->painter = painter ? *painter : (awl_window_painter_t){ 0 };
    win->painter_data = data;
    win->hover = -1;
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

void awl_window_redraw( awl_window_t* win ) {
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
}

/** what m's popups draw with: the popup font, else the bar's */
static struct fcft_font* font_of( Monitor* m ) {
    return m->popupfont ? m->popupfont : m->drw.font;
}

/** a painted window's layout, its frame around what the painter measures */
static int layout_painted( awl_window_t* win, awl_window_layout_t* L, struct fcft_font* font, float s,
                           int max_w, int max_h ) {
    const int inset = awl_window_inset( &win->style, s );
    int cw = 0, ch = 0;
    if (max_w - 2 * inset <= 0 || max_h - 2 * inset <= 0) return 0;
    if (!win->painter.measure( win->painter_data, font, s, max_w - 2 * inset, max_h - 2 * inset, &cw, &ch ))
        return 0;
    /* never more than allowed, whatever the painter says */
    if (cw > max_w - 2 * inset) cw = max_w - 2 * inset;
    if (ch > max_h - 2 * inset) ch = max_h - 2 * inset;
    return awl_window_layout_frame( L, &win->style, s, cw, ch );
}

/** the painter's content into data, its frame first */
static void paint_painted( awl_window_t* win, const awl_window_layout_t* L, struct fcft_font* font, float s,
                           uint32_t* data, int32_t stride ) {
    awl_window_paint_frame( L, &win->style, data, stride );
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
    /* room for awl_window_place()'s margins */
    const int w = m->m.width - 2 * awl_window_margin( win->style.margin, m->m.width );
    const int h = m->w.height - 2 * awl_window_margin( win->style.margin, m->w.height );
    if (win->painter.paint ? !layout_painted( win, &L, font, s, (int)(w * s), (int)(h * s) )
                           : !awl_window_layout( &L, win->text, &win->style, font, s, (int)(w * s), (int)(h * s) ))
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
        awl_window_paint( &L, win->text, &win->style, font, win->click ? win->hover : -1, b->data, stride );
    win->layout = L;

    win->lw = (int)lroundf( L.width / s );
    win->lh = (int)lroundf( L.height / s );
    wlr_scene_buffer_set_buffer( win->buffer, &b->base );
    wlr_scene_buffer_set_dest_size( win->buffer, win->lw, win->lh );
    wlr_buffer_drop( &b->base ); /* the scene holds it now */
    if (win->blur) wlr_scene_blur_set_size( win->blur, win->lw, win->lh );
    return 1;
}

static void place( awl_window_t* win ) {
    Monitor* m = win->m;
    int x, y;
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
    if (win->m != m) win->drawn = 0;
    win->m = m;
    win->center_x = center_x;
    win->top = top;
    awl_window_update( win, m );
}

void awl_window_hide( awl_window_t* win ) {
    if (!win->m) return;
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
    return win->m;
}

int awl_window_box( const awl_window_t* win, struct wlr_box* box ) {
    *box = (struct wlr_box){ win->tree->node.x, win->tree->node.y, win->lw, win->lh };
    return win->tree->node.enabled;
}

void awl_window_removemon( awl_window_t* win, Monitor* m ) {
    if (win && win->m == m) awl_window_hide( win );
}

void awl_window_set_click( awl_window_t* win, awl_window_click_t click, void* data ) {
    win->click = click;
    win->click_data = data;
    if (!click) awl_window_pointer_leave( win );
}

void awl_window_set_hidden( awl_window_t* win, awl_window_hidden_t hidden, void* data ) {
    win->hidden = hidden;
    win->hidden_data = data;
}

void awl_window_set_scroll( awl_window_t* win, awl_window_scroll_t scroll, void* data ) {
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
    if (row != win->hover) {
        win->hover = row;
        win->drawn = 0;
        awl_window_update( win, win->m );
    }
    return row;
}

void awl_window_pointer_leave( awl_window_t* win ) {
    if (win->hover == -1) return;
    win->hover = -1;
    win->drawn = 0;
    if (win->m) awl_window_update( win, win->m );
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
