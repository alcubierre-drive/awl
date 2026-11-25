#include "awl_capture.h"
#include "awl_client.h"
#include "awl_cursor.h"
#include "awl_log.h"
#include "awl_output.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/transform.h>

/** the largest side capture() renders, in device pixels */
#define CAPTURE_MAX_SIDE 32768

/** one paste in progress: what is left to write to its fd */
typedef struct {
    struct wl_list link;
    awl_server_t *srv;
    struct wl_event_source *source;
    int fd;
    /** the clipboard's bytes, shared (ClipData) */
    struct ClipData *bytes;
    size_t off;
} ClipWriter;

/** a copy's bytes, kept by its source and every writer */
typedef struct ClipData {
    int refs;
    size_t len;
    unsigned char data[];
} ClipData;

/** the clipboard's source while it is ours */
typedef struct {
    struct wlr_data_source base;
    awl_server_t *srv;
    ClipData *bytes;
} ClipSource;

struct awl_capture {
    /** the grab's callbacks and their data; mode is CurGrab while it holds */
    awl_grab_t grab;
    void *data;
    /** where motion() last reported the pointer */
    double x, y;
    /** the mark: shade around the box, its frame; made with the first */
    struct wlr_scene_tree *mark;
    struct wlr_scene_rect *shade[4], *frame[4];
    /** ClipWriter.link */
    struct wl_list writers;
};

static struct awl_capture *capture_state( awl_server_t *srv ) {
    if (!srv->capture) {
        srv->capture = calloc_or_die(1, sizeof(*srv->capture));
        wl_list_init(&srv->capture->writers);
    }
    return srv->capture;
}

/* ---- the grab ---- */

int grab_start( awl_server_t *srv, const awl_grab_t *grab, void *data, const char *cursor ) {
    if (!grab || srv->locked || (srv->cursor.mode != CurNormal && srv->cursor.mode != CurPressed))
        return 0;
    struct awl_capture *C = capture_state(srv);
    C->grab = *grab;
    C->data = data;
    srv->cursor.mode = CurGrab;
    /* no client sees the pointer meanwhile, nor sets its image */
    wlr_seat_pointer_notify_clear_focus(srv->seat);
    wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, cursor ? cursor : "crosshair");
    C->x = srv->cursor.wlr->x;
    C->y = srv->cursor.wlr->y;
    awl_vrb_printf("capture: grab at %.0f,%.0f\n", C->x, C->y);
    if (C->grab.motion)
        C->grab.motion(C->x, C->y, C->data);
    return 1;
}

/** ends the grab; the library's cancel if tell */
static void grab_stop( awl_server_t *srv, int tell ) {
    struct awl_capture *C = srv->capture;
    if (!C || srv->cursor.mode != CurGrab)
        return;
    awl_grab_t grab = C->grab;
    void *data = C->data;
    C->grab = (awl_grab_t){0};
    C->data = NULL;
    grab_mark(srv, NULL, 0, 0, 0);
    srv->cursor.mode = CurNormal;
    awl_vrb_printf("capture: grab %s\n", tell ? "cancelled" : "ended");
    /* what is under the pointer gets it back, and sets its image again (see
     * button_press()'s end of a move) */
    wlr_cursor_set_xcursor(srv->cursor.wlr, srv->cursor.mgr, "default");
    wlr_seat_pointer_notify_clear_focus(srv->seat);
    motion_notify(srv, 0, NULL, 0, 0, 0, 0);
    if (tell && grab.cancel)
        grab.cancel(data);
}

void grab_end( awl_server_t *srv ) {
    grab_stop(srv, 0);
}

void grab_cancel( awl_server_t *srv ) {
    grab_stop(srv, 1);
}

int grab_reap( awl_server_t *srv ) {
    if (srv->cursor.mode != CurGrab)
        return 0;
    grab_stop(srv, 0);
    return 1;
}

void grab_motion( awl_server_t *srv ) {
    struct awl_capture *C = srv->capture;
    if (srv->locked) {
        grab_cancel(srv);
        return;
    }
    /* a virtual pointer's motion comes as an internal one (time 0), and
     * those that only look again come too */
    if (srv->cursor.wlr->x == C->x && srv->cursor.wlr->y == C->y)
        return;
    C->x = srv->cursor.wlr->x;
    C->y = srv->cursor.wlr->y;
    if (C->grab.motion)
        C->grab.motion(C->x, C->y, C->data);
}

void grab_button( awl_server_t *srv, uint32_t button, int pressed, uint32_t mods ) {
    struct awl_capture *C = srv->capture;
    if (srv->locked) {
        grab_cancel(srv);
        return;
    }
    grab_motion(srv);
    /* motion() may have ended it */
    if (srv->cursor.mode == CurGrab && C->grab.button)
        C->grab.button(button, pressed, mods, C->data);
}

void grab_key( awl_server_t *srv, uint32_t sym, uint32_t mods ) {
    struct awl_capture *C = srv->capture;
    if (srv->locked) {
        grab_cancel(srv);
        return;
    }
    if (C->grab.key)
        C->grab.key(sym, mods, C->data);
}

/** color, 0xRRGGBBAA, premultiplied as the scene wants it */
static void premultiplied( uint32_t color, float out[4] ) {
    float a = (color & 0xff) / 255.0f;
    out[0] = ((color >> 24) & 0xff) / 255.0f * a;
    out[1] = ((color >> 16) & 0xff) / 255.0f * a;
    out[2] = ((color >> 8) & 0xff) / 255.0f * a;
    out[3] = a;
}

static void rect_set( struct wlr_scene_rect *r, int x, int y, int w, int h ) {
    wlr_scene_node_set_enabled(&r->node, w > 0 && h > 0);
    if (w <= 0 || h <= 0)
        return;
    wlr_scene_node_set_position(&r->node, x, y);
    wlr_scene_rect_set_size(r, w, h);
}

void grab_mark( awl_server_t *srv, const awl_box_t *box, uint32_t dim, uint32_t frame, int frame_width ) {
    struct awl_capture *C = srv->capture;
    struct wlr_box all = srv->out.screen_geom;
    float shade[4], line[4];
    int fw = MAX(frame_width, 0);

    if (!box || srv->cursor.mode != CurGrab) {
        if (C && C->mark)
            wlr_scene_node_set_enabled(&C->mark->node, 0);
        return;
    }
    if (!C->mark) {
        /* above everything; the locker's tree is there too, but no grab
         * holds while the screen is locked */
        C->mark = wlr_scene_tree_create(srv->layers[LyrBlock]);
        for (int i = 0; i < 4; i++) {
            C->shade[i] = wlr_scene_rect_create(C->mark, 0, 0, (float[4]){0});
            C->frame[i] = wlr_scene_rect_create(C->mark, 0, 0, (float[4]){0});
        }
    }
    wlr_scene_node_raise_to_top(&C->mark->node);
    wlr_scene_node_set_enabled(&C->mark->node, 1);
    premultiplied(dim, shade);
    premultiplied(frame, line);
    for (int i = 0; i < 4; i++) {
        wlr_scene_rect_set_color(C->shade[i], shade);
        wlr_scene_rect_set_color(C->frame[i], line);
    }

    if (box->w <= 0 || box->h <= 0) {
        rect_set(C->shade[0], all.x, all.y, all.width, all.height);
        for (int i = 1; i < 4; i++)
            rect_set(C->shade[i], 0, 0, 0, 0);
        for (int i = 0; i < 4; i++)
            rect_set(C->frame[i], 0, 0, 0, 0);
        return;
    }
    /* the frame around the box, the shade around the frame: above, below,
     * left and right of it */
    struct wlr_box b = { box->x - fw, box->y - fw, box->w + 2 * fw, box->h + 2 * fw };
    rect_set(C->shade[0], all.x, all.y, all.width, b.y - all.y);
    rect_set(C->shade[1], all.x, b.y + b.height, all.width, all.y + all.height - b.y - b.height);
    rect_set(C->shade[2], all.x, b.y, b.x - all.x, b.height);
    rect_set(C->shade[3], b.x + b.width, b.y, all.x + all.width - b.x - b.width, b.height);
    rect_set(C->frame[0], b.x, b.y, b.width, fw);
    rect_set(C->frame[1], b.x, box->y + box->h, b.width, fw);
    rect_set(C->frame[2], b.x, box->y, fw, box->h);
    rect_set(C->frame[3], box->x + box->w, box->y, fw, box->h);
}

/* ---- what is where ---- */

Client *client_at( awl_server_t *srv, double x, double y ) {
    Client *c = NULL;
    xy_to_node(srv, x, y, NULL, &c, NULL, NULL, NULL);
    return c;
}

Client *client_focused( awl_server_t *srv ) {
    return srv->out.sel_mon ? focus_top(srv, srv->out.sel_mon) : NULL;
}

int client_box( awl_server_t *srv, Client *c, int border, awl_box_t *box ) {
    (void)srv;
    if (!c)
        return 0;
    int bw = border ? 0 : (int)c->bw;
    *box = (awl_box_t){ c->geom.x + bw, c->geom.y + bw, c->geom.width - 2 * bw, c->geom.height - 2 * bw };
    return 1;
}

Monitor *monitor_at( awl_server_t *srv, double x, double y ) {
    return xy_to_mon(srv, x, y);
}

int monitor_box( awl_server_t *srv, Monitor *m, awl_box_t *box ) {
    struct wlr_box b = m ? m->m : srv->out.screen_geom;
    if (b.width <= 0 || b.height <= 0)
        return 0;
    *box = (awl_box_t){ b.x, b.y, b.width, b.height };
    return 1;
}

/* ---- capture ---- */

/** the pixman format of what wlr_texture_read_pixels() writes in drm; 0 if
 * none fits */
static pixman_format_code_t pixman_format( uint32_t drm ) {
    switch (drm) {
    case DRM_FORMAT_ARGB8888:    return PIXMAN_a8r8g8b8;
    case DRM_FORMAT_XRGB8888:    return PIXMAN_x8r8g8b8;
    case DRM_FORMAT_ABGR8888:    return PIXMAN_a8b8g8r8;
    case DRM_FORMAT_XBGR8888:    return PIXMAN_x8b8g8r8;
    case DRM_FORMAT_ARGB2101010: return PIXMAN_a2r10g10b10;
    case DRM_FORMAT_XRGB2101010: return PIXMAN_x2r10g10b10;
    case DRM_FORMAT_ABGR2101010: return PIXMAN_a2b10g10r10;
    case DRM_FORMAT_XBGR2101010: return PIXMAN_x2b10g10r10;
    default:                     return 0;
    }
}

/**
 * m's frame as it would show now, in its buffer's orientation and size: the
 * scene rendered again into a swapchain of our own, never committed. The
 * output's damage stays as it was (its damage ring tracks our buffer as
 * one more, which gets all of it), so its next frame is right; scenefx's
 * effect buffers have the output's size, as ours. A single client buffer
 * the scene would scan out is read as it is.
 */
static pixman_image_t *output_render( awl_server_t *srv, Monitor *m ) {
    struct wlr_output *o = m->wlr_output;
    struct wlr_output_state state;
    struct wlr_swapchain *sc;
    struct wlr_texture *tex = NULL;
    pixman_image_t *img = NULL;
    pixman_format_code_t pf;
    uint32_t fmt;

    if (!o->enabled || !o->swapchain || !o->allocator || !m->scene_output)
        return NULL;
    if (!(sc = wlr_swapchain_create(o->allocator, o->width, o->height, &o->swapchain->format)))
        return NULL;
    wlr_output_state_init(&state);
    if (!wlr_scene_output_build_state(m->scene_output, &state,
                &(struct wlr_scene_output_state_options){ .swapchain = sc })
            || !(state.committed & WLR_OUTPUT_STATE_BUFFER) || !state.buffer) {
        awl_err_printf("capture: %s: no frame\n", o->name);
        goto out;
    }
    if (!(tex = wlr_texture_from_buffer(srv->renderer, state.buffer)))
        goto out;
    fmt = wlr_texture_preferred_read_format(tex);
    if (!(pf = pixman_format(fmt))) {
        fmt = DRM_FORMAT_ARGB8888;
        pf = PIXMAN_a8r8g8b8;
    }
    if (!(img = pixman_image_create_bits(pf, (int)tex->width, (int)tex->height, NULL, 0)))
        goto out;
    if (!wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
                .data = pixman_image_get_data(img), .format = fmt,
                .stride = (uint32_t)pixman_image_get_stride(img) })) {
        awl_err_printf("capture: %s: can't read its pixels (format 0x%08x)\n", o->name, fmt);
        pixman_image_unref(img);
        img = NULL;
    }
out:
    if (tex)
        wlr_texture_destroy(tex);
    wlr_output_state_finish(&state);
    wlr_swapchain_destroy(sc);
    return img;
}

/**
 * src, m's frame (output_render()), into dst, which shows box at scale s:
 * the part of box m covers, turned upright by m's transform and scaled from
 * m's scale to s. The rect src gives each pixel of dst comes from one
 * affine map, logical pixels of dst to those of m's upright frame to its
 * buffer's (the scene's own, wlr_fbox_transform() by the inverted
 * transform on the upright size), which pixman samples at the pixels'
 * centers.
 */
static void compose( pixman_image_t *dst, pixman_image_t *src, Monitor *m, const awl_box_t *box, double s ) {
    struct wlr_output *o = m->wlr_output;
    struct wlr_box in, want = { box->x, box->y, box->w, box->h };
    enum wl_output_transform inv = wlr_output_transform_invert(o->transform);
    struct wlr_fbox q0, q1, q2;
    int tw = o->width, th = o->height;

    if (!wlr_box_intersection(&in, &m->m, &want))
        return;
    wlr_output_transform_coords(o->transform, &tw, &th);
    int x0 = (int)lround((in.x - box->x) * s);
    int y0 = (int)lround((in.y - box->y) * s);
    int x1 = (int)lround((in.x + in.width - box->x) * s);
    int y1 = (int)lround((in.y + in.height - box->y) * s);

    /* dst's pixels to the upright frame's: scale k, offset tx, ty */
    double k = o->scale / s;
    double tx = (box->x - m->m.x) * (double)o->scale;
    double ty = (box->y - m->m.y) * (double)o->scale;
    /* the upright frame's to the buffer's: where 0,0 and the unit vectors go */
    wlr_fbox_transform(&q0, &(struct wlr_fbox){ 0, 0, 0, 0 }, inv, tw, th);
    wlr_fbox_transform(&q1, &(struct wlr_fbox){ 1, 0, 0, 0 }, inv, tw, th);
    wlr_fbox_transform(&q2, &(struct wlr_fbox){ 0, 1, 0, 0 }, inv, tw, th);
    double a = q1.x - q0.x, b = q2.x - q0.x;
    double c = q1.y - q0.y, d = q2.y - q0.y;
    pixman_transform_t t = {{
        { pixman_double_to_fixed(a * k), pixman_double_to_fixed(b * k),
          pixman_double_to_fixed(a * tx + b * ty + q0.x) },
        { pixman_double_to_fixed(c * k), pixman_double_to_fixed(d * k),
          pixman_double_to_fixed(c * tx + d * ty + q0.y) },
        { 0, 0, pixman_fixed_1 },
    }};
    pixman_image_set_transform(src, &t);
    pixman_image_set_filter(src, k == 1.0 ? PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_BILINEAR, NULL, 0);
    /* the edges' pixels stand in for what is beyond, not transparency */
    pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);
    pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst, x0, y0, 0, 0, x0, y0, x1 - x0, y1 - y0);
}

pixman_image_t *capture( awl_server_t *srv, const awl_box_t *box, float scale ) {
    struct wlr_box in;
    pixman_image_t *dst;
    Monitor *m;
    double s = scale;
    int n = 0;

    if (!box || box->w <= 0 || box->h <= 0)
        return NULL;
    struct wlr_box want = { box->x, box->y, box->w, box->h };
    if (s <= 0) {
        s = 0;
        FOR_EACH_MON(m, srv)
            if (m->wlr_output->enabled && wlr_box_intersection(&in, &m->m, &want))
                s = MAX(s, (double)m->wlr_output->scale);
        if (s <= 0)
            return NULL;
    }
    int w = (int)lround(box->w * s);
    int h = (int)lround(box->h * s);
    if (w <= 0 || h <= 0 || w > CAPTURE_MAX_SIDE || h > CAPTURE_MAX_SIDE) {
        awl_err_printf("capture: %dx%d at %d,%d, scale %g: too large\n", box->w, box->h, box->x, box->y, s);
        return NULL;
    }
    /* cleared: transparent where no monitor is */
    if (!(dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0)))
        return NULL;
    FOR_EACH_MON(m, srv) {
        if (!m->wlr_output->enabled || !wlr_box_intersection(&in, &m->m, &want))
            continue;
        pixman_image_t *src = output_render(srv, m);
        if (!src)
            continue;
        compose(dst, src, m, box, s);
        pixman_image_unref(src);
        n++;
    }
    if (!n) {
        pixman_image_unref(dst);
        return NULL;
    }
    awl_vrb_printf("capture: %dx%d at %d,%d, scale %g: %dx%d from %d output(s)\n",
            box->w, box->h, box->x, box->y, s, w, h, n);
    return dst;
}

/* ---- the clipboard ---- */

static void clip_data_unref( ClipData *d ) {
    if (d && --d->refs == 0)
        free(d);
}

static void clip_writer_free( ClipWriter *w, int remove ) {
    if (remove && w->source)
        wl_event_source_remove(w->source);
    close(w->fd);
    clip_data_unref(w->bytes);
    wl_list_remove(&w->link);
    free(w);
}

/** a paste's fd can take more: as much of the rest as it takes */
static int clip_writable( int fd, uint32_t mask, void *data ) {
    ClipWriter *w = data;
    (void)mask;
    while (w->off < w->bytes->len) {
        ssize_t n = write(fd, w->bytes->data + w->off, w->bytes->len - w->off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return 0;
        if (n <= 0)
            break; /* the reader went away (EPIPE; SIGPIPE is ignored) */
        w->off += (size_t)n;
    }
    clip_writer_free(w, 1);
    return 0;
}

static void clip_send( struct wlr_data_source *base, const char *mime, int32_t fd ) {
    ClipSource *src = wl_container_of(base, src, base);
    struct awl_capture *C = capture_state(src->srv);
    int fl = fcntl(fd, F_GETFL);
    (void)mime;
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(fd);
        return;
    }
    ClipWriter *w = calloc_or_die(1, sizeof(*w));
    w->srv = src->srv;
    w->fd = fd;
    w->bytes = src->bytes;
    w->bytes->refs++;
    wl_list_insert(&C->writers, &w->link);
    /* never waits: a slow reader gets the rest as it reads */
    w->source = wl_event_loop_add_fd(src->srv->event_loop, fd, WL_EVENT_WRITABLE, clip_writable, w);
    if (!w->source)
        clip_writer_free(w, 0);
}

static void clip_destroy( struct wlr_data_source *base ) {
    ClipSource *src = wl_container_of(base, src, base);
    clip_data_unref(src->bytes);
    free(src);
}

static const struct wlr_data_source_impl clip_impl = {
    .send = clip_send,
    .destroy = clip_destroy,
};

int clipboard_set( awl_server_t *srv, const char *mime, const void *data, size_t len ) {
    char **p;
    if (!mime || !*mime || (!data && len))
        return 0;
    capture_state(srv);
    ClipSource *src = calloc_or_die(1, sizeof(*src));
    src->srv = srv;
    src->bytes = calloc_or_die(1, sizeof(*src->bytes) + len);
    src->bytes->refs = 1;
    src->bytes->len = len;
    if (len)
        memcpy(src->bytes->data, data, len);
    wlr_data_source_init(&src->base, &clip_impl);
    if (!(p = wl_array_add(&src->base.mime_types, sizeof(*p))) || !(*p = strdup(mime))) {
        if (p)
            *p = NULL; /* wlr_data_source_destroy() frees each */
        wlr_data_source_destroy(&src->base);
        return 0;
    }
    wlr_seat_set_selection(srv->seat, &src->base, wl_display_next_serial(srv->dpy));
    awl_vrb_printf("capture: clipboard: %zu bytes of %s\n", len, mime);
    return 1;
}

void capture_fini( awl_server_t *srv ) {
    ClipWriter *w, *tmp;
    if (!srv->capture)
        return;
    /* their event sources went with the display's loop */
    wl_list_for_each_safe(w, tmp, &srv->capture->writers, link)
        clip_writer_free(w, 0);
    /* the mark goes with the scene */
    free(srv->capture);
    srv->capture = NULL;
}
