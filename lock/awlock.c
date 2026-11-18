// awlock: awl's screen locker (README.md). This is its Wayland side: the
// event loop, then the lock (ext-session-lock-v1) with a transparent lock
// surface per output and the logo on a subsurface of it, the seat (keys go
// to auth.c) and main(). See awlock.h.
#define _GNU_SOURCE // memfd_create(): glibc declares it for nothing narrower
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "awlock.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

// The event loop: poll() on the fds, with the next timer's timeout

#define LOOP_FDS 4

struct loop_timer {
    void (*callback)(void *data);
    void *data;
    int64_t expiry; // now_ms()
    // removed, freed by loop_poll(): a timer's callback may remove the
    // next timer while loop_poll() walks them
    bool removed;
    struct wl_list link; // loop.timers
};

static struct {
    struct pollfd fds[LOOP_FDS]; // fd -1: removed, poll() skips it
    struct {
        void (*callback)(void *data);
        void *data;
    } callbacks[LOOP_FDS];
    int nfds;
    struct wl_list timers; // struct loop_timer::link
} loop = {.timers = {&loop.timers, &loop.timers}};

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// waits for an fd or the next timer, and runs their callbacks
static void loop_poll(void) {
    int64_t now = now_ms();
    int ms = -1; // no timer: until an fd
    struct loop_timer *timer, *tmp;
    wl_list_for_each(timer, &loop.timers, link) {
        int64_t left = timer->expiry > now ? timer->expiry - now : 0;
        if (!timer->removed && (ms < 0 || left < ms)) {
            ms = left;
        }
    }

    int n = poll(loop.fds, loop.nfds, ms);
    if (n < 0 && errno != EINTR) {
        awl_err_printf("awlock: poll failed: %s\n", strerror(errno));
        exit(1);
    }
    for (int i = 0; n > 0 && i < loop.nfds; i++) {
        if (loop.fds[i].fd >= 0 && loop.fds[i].revents) {
            loop.callbacks[i].callback(loop.callbacks[i].data);
        }
    }

    now = now_ms();
    wl_list_for_each_safe(timer, tmp, &loop.timers, link) {
        if (timer->removed || timer->expiry <= now) {
            if (!timer->removed) {
                timer->callback(timer->data);
            }
            wl_list_remove(&timer->link);
            free(timer);
        }
    }
}

void loop_add_fd(int fd, void (*callback)(void *data), void *data) {
    if (loop.nfds == LOOP_FDS) {
        awl_err_printf("awlock: more than %d fds\n", LOOP_FDS);
        return;
    }
    loop.fds[loop.nfds] = (struct pollfd){.fd = fd, .events = POLLIN};
    loop.callbacks[loop.nfds].callback = callback;
    loop.callbacks[loop.nfds++].data = data;
}

void loop_remove_fd(int fd) {
    for (int i = 0; i < loop.nfds; i++) {
        if (loop.fds[i].fd == fd) {
            loop.fds[i].fd = -1;
        }
    }
}

struct loop_timer *loop_add_timer(int ms, void (*callback)(void *data),
        void *data) {
    struct loop_timer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        awl_err_printf("awlock: Unable to allocate memory for timer\n");
        return NULL;
    }
    timer->callback = callback;
    timer->data = data;
    timer->expiry = now_ms() + ms;
    wl_list_insert(&loop.timers, &timer->link);
    return timer;
}

void loop_remove_timer(struct loop_timer *timer) {
    if (timer) {
        timer->removed = true;
    }
}

// Buffers: ARGB8888 in shared memory, for the logo

struct shm_buffer {
    struct wl_buffer *buffer;
    uint32_t width;
    void *data;
    size_t size;
};

static struct shm_buffer *create_buffer(struct wl_shm *shm,
        struct shm_buffer *buf, int32_t width, int32_t height) {
    uint32_t stride = width * 4;
    size_t size = stride * height;

    // anonymous shared memory for the compositor
    int fd = memfd_create("awlock", MFD_CLOEXEC);
    if (fd == -1) {
        return NULL;
    }
    void *data = MAP_FAILED;
    if (ftruncate(fd, size) == 0) {
        data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    buf->buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    buf->size = size;
    buf->width = width;
    buf->data = data;
    return buf;
}

static void destroy_buffer(struct shm_buffer *buffer) {
    if (buffer->buffer) {
        wl_buffer_destroy(buffer->buffer);
    }
    if (buffer->data) {
        munmap(buffer->data, buffer->size);
    }
    memset(buffer, 0, sizeof(struct shm_buffer));
}

// A lock surface per output

struct awlock_surface {
    struct awlock_state *state;
    struct wl_output *output;
    uint32_t output_global_name;
    struct wl_surface *surface; // the lock surface, transparent
    struct wp_viewport *viewport; // stretches state->transparent over it
    struct wl_surface *logo_surface; // a subsurface of surface
    struct wl_subsurface *logo_subsurface;
    struct ext_session_lock_surface_v1 *lock_surface;
    // the logo in each state, drawn when first shown and kept until the
    // scale changes: at active_alpha (the pointer's output, or every output
    // before the pointer was on one), and at inactive_alpha (logo_dim())
    struct shm_buffer logo_full[LOGO_STATES], logo_dimmed[LOGO_STATES];
    int32_t logo_buffers_scale; // the scale they were drawn at
    struct shm_buffer *logo_shown; // attached to logo_surface, or NULL
    bool created;
    // a frame is pending (frame_callback), and the surface changed since it
    // was drawn: drawn when the frame is done
    bool dirty;
    struct wl_callback *frame_callback; // the pending frame's, or NULL
    uint32_t width, height;
    int32_t scale; // the output's, 1 until it says
    struct wl_list link; // struct awlock_state::surfaces
};

// The logo

/*
 * awlock's logo, drawn at the pointer: "awl" in lower case,
 * in the style of the letter tiles of awesome's logo. Each letter is a
 * solid tile, and thin lines cut into it from its edges shape the letter:
 *   a  a cut from the left edge a third of the way down, and a hook two
 *      thirds down that runs to the bottom edge (awesome's own "a")
 *   w  two cuts from the top edge, two thirds deep (awesome's own "w")
 *   l  no cuts but an L's shape (awesome has no l): a stem as wide as the
 *      w's prongs, and a foot as high as the w's base, so it's no i
 * The a and w tiles are 16 by 16 and the l is 13 wide, 3 px apart. A 1 px
 * outline in the border colour goes round each letter, corners included,
 * which leaves 1 px empty between letters: the logo is 53 by 18.
 *
 * It is logo.xpm, compiled in: an image editor's file, and C. Its three
 * colours, one character per pixel, are what the pixels are, not what they
 * look like (that is the state's, awl's colors.lock_*):
 *   None     empty: transparent, not drawn (between the outlines)
 *   darker   border: the state's border colour (the cuts, the outlines)
 *   lighter  fill: the state's fill colour (the letters)
 * It is drawn scaled by a whole number, nearest-neighbour, so its pixels
 * stay square. awl's lock.logo reads another XPM of the same kind instead.
 */
#include "logo.xpm"

// the logo's size at most, in logo pixels: logo.xpm's or lock.logo's
#define LOGO_MAX_SIZE 256

// logo_xpm or lock.logo's file, as logo_load() found it
static struct {
    int w, h; // in logo pixels
    const char *const *rows; // h rows of w characters
    char border, fill; // their characters; the third is empty
} logo;

// an XPM's strings: "w h 3 1", then the colours as "<char> c <None|#rrggbb>",
// then the rows; anything else (more colours, symbolic ones, other keys than
// c, as some editors write, extensions) is refused. name is for the log:
// logo.xpm (logo_xpm, compiled in) or lock.logo's file.
static bool logo_load(const char *const *xpm, int lines, const char *name) {
    int ncolors, cpp;
    if (lines < 1 ||
            sscanf(xpm[0], "%d %d %d %d", &logo.w, &logo.h, &ncolors, &cpp) != 4 ||
            logo.w <= 0 || logo.h <= 0 || ncolors != 3 || cpp != 1 ||
            lines != 1 + ncolors + logo.h) {
        awl_err_printf("awlock: %s: not 3 colours, 1 character per pixel\n", name);
        return false;
    }
    // a buffer per state and output, logo_scale * scale pixels per logo pixel
    if (logo.w > LOGO_MAX_SIZE || logo.h > LOGO_MAX_SIZE) {
        awl_err_printf("awlock: %s: %d by %d, more than %d\n", name,
            logo.w, logo.h, LOGO_MAX_SIZE);
        return false;
    }
    char empty = 0, chars[2];
    int n = 0, brightness[2];
    for (int i = 1; i <= ncolors; i++) {
        const char *line = xpm[i];
        char spec[8], end;
        unsigned rgb;
        if (line[0] == '\0' || sscanf(line + 1, " c %7s %c", spec, &end) != 1) {
            awl_err_printf("awlock: %s: colour \"%s\" isn't \"<char> c "
                "<colour>\"\n", name, line);
            return false;
        }
        if (strcasecmp(spec, "None") == 0 && !empty) {
            empty = line[0];
        } else if (n < 2 && spec[0] == '#' && strlen(spec) == 7 &&
                sscanf(spec + 1, "%6x%c", &rgb, &end) == 1) {
            chars[n] = line[0];
            brightness[n++] = (rgb >> 16) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF);
        } else {
            awl_err_printf("awlock: %s: the colours are None and two "
                "#rrggbb, not %s\n", name, spec);
            return false;
        }
    }
    if (n != 2 || brightness[0] == brightness[1] || chars[0] == chars[1] ||
            empty == chars[0] || empty == chars[1]) {
        awl_err_printf("awlock: %s: the colours are None and two "
            "#rrggbb, one darker (the border) and one lighter (the fill)\n", name);
        return false;
    }
    bool first_darker = brightness[0] < brightness[1];
    logo.border = chars[!first_darker];
    logo.fill = chars[first_darker];
    logo.rows = xpm + 1 + ncolors;
    char used[] = {empty, logo.border, logo.fill, '\0'};
    for (int y = 0; y < logo.h; y++) {
        if ((int)strlen(logo.rows[y]) != logo.w ||
                (int)strspn(logo.rows[y], used) != logo.w) {
            awl_err_printf("awlock: %s: row %d is not %d of its colours\n",
                name, y + 1, logo.w);
            return false;
        }
    }
    return true;
}

// lock.logo's file: an XPM as image editors write it, C source whose strings
// (outside comments) logo_load() takes; *lines of them in *xpm, kept for
// logo.rows, as the file's contents they are in. Not C's escapes: a
// backslash is refused, as no XPM needs one.
static bool logo_read(const char *path, char ***xpm, int *lines) {
    FILE *f = fopen(path, "r");
    if (!f) {
        awl_err_printf("awlock: logo: %s: %s\n", path, strerror(errno));
        return false;
    }
    // 256 by 256 is 64 KiB of rows, with quotes and commas a little more
    static char text[LOGO_MAX_SIZE * (LOGO_MAX_SIZE + 8) + 4096];
    size_t len = fread(text, 1, sizeof(text), f);
    bool err = ferror(f), big = len == sizeof(text);
    fclose(f);
    if (err || big) {
        awl_err_printf("awlock: logo: %s: %s\n", path,
            err ? "can't read it" : "too big for an XPM logo");
        return false;
    }
    text[len] = '\0';
    *xpm = NULL;
    *lines = 0;
    for (char *p = text; *p; p++) {
        if (p[0] == '/' && p[1] == '*') {
            char *end = strstr(p + 2, "*/");
            if (!end) {
                break;
            }
            p = end + 1;
        } else if (p[0] == '/' && p[1] == '/') {
            p += strcspn(p, "\n");
            if (!*p) {
                break;
            }
        } else if (*p == '"') {
            char *end = strpbrk(p + 1, "\"\\\n");
            if (!end || *end != '"') {
                awl_err_printf("awlock: logo: %s: a string that doesn't "
                    "end on its line, or with a backslash\n", path);
                free(*xpm);
                *xpm = NULL;
                return false;
            }
            *end = '\0';
            char **grown = realloc(*xpm, (*lines + 1) * sizeof(**xpm));
            if (!grown) {
                free(*xpm);
                *xpm = NULL;
                return false;
            }
            *xpm = grown;
            (*xpm)[(*lines)++] = p + 1;
            p = end;
        }
    }
    return true;
}

static enum logo_state logo_state(const struct awlock_state *state) {
    if (state->auth_state == AUTH_STATE_INVALID) {
        return LOGO_WRONG;
    }
    if (state->auth_state == AUTH_STATE_VALIDATING) {
        return LOGO_VALIDATING;
    }
    if (state->xkb.caps_lock) {
        return LOGO_CAPS_LOCK;
    }
    // key presses don't show by themselves, only whether there is
    // something to check
    return state->password.len > 0 ? LOGO_TYPING : LOGO_WAITING;
}

// a colour (RRGGBBAA) at an opacity: its alpha times alpha, 0 to 1
static uint32_t logo_dim(uint32_t color, float alpha) {
    uint32_t a = (uint32_t)((color & 0xFF) * alpha + 0.5f);
    return (color & 0xFFFFFF00) | a;
}

// RRGGBBAA as premultiplied ARGB8888, the shm format
static uint32_t premultiplied(uint32_t color) {
    uint32_t a = color & 0xFF, argb = a << 24;
    for (int i = 0; i < 3; i++) {
        uint32_t c = (color >> (24 - 8 * i)) & 0xFF;
        argb |= ((c * a + 127) / 255) << (16 - 8 * i);
    }
    return argb;
}

static void destroy_logo_buffers(struct awlock_surface *surface) {
    for (int i = 0; i < LOGO_STATES; i++) {
        destroy_buffer(&surface->logo_full[i]);
        destroy_buffer(&surface->logo_dimmed[i]);
    }
}

// the logo in a state, its colours dimmed or not: drawn the first time it is
// needed, each logo pixel a square of logo_scale * scale buffer pixels, and
// kept until the scale changes
static struct shm_buffer *logo_buffer(struct awlock_surface *surface,
        enum logo_state ls, bool dim) {
    struct awlock_state *state = surface->state;
    if (surface->logo_buffers_scale != surface->scale) {
        destroy_logo_buffers(surface);
        surface->logo_buffers_scale = surface->scale;
        surface->logo_shown = NULL;
    }
    struct shm_buffer *buf = &(dim ? surface->logo_dimmed : surface->logo_full)[ls];
    if (buf->buffer) {
        return buf;
    }

    int px = state->args.logo_scale * surface->scale;
    if (!create_buffer(state->shm, buf, logo.w * px, logo.h * px)) {
        awl_err_printf("awlock: Failed to create the logo's buffer\n");
        return NULL;
    }
    awlock_colors_t c = state->args.colors[ls];
    uint32_t border = premultiplied(logo_dim(c.border, dim ? state->args.inactive_alpha :
                                                             state->args.active_alpha));
    uint32_t fill = premultiplied(logo_dim(c.fill, dim ? state->args.inactive_alpha :
                                                         state->args.active_alpha));
    uint32_t *row = buf->data;
    for (int y = 0; y < logo.h; y++) {
        // one row of buffer pixels, then copied for the logo pixel's height
        for (int x = 0; x < logo.w; x++) {
            char ch = logo.rows[y][x];
            uint32_t argb = ch == logo.border ? border :
                ch == logo.fill ? fill : 0;
            for (int i = 0; i < px; i++) {
                row[x * px + i] = argb;
            }
        }
        for (int i = 1; i < px; i++) {
            memcpy(row + i * buf->width, row, buf->width * 4);
        }
        row += px * buf->width;
    }
    return buf;
}

// where the logo goes on a surface (surface-local, logical: its centre) and
// whether it is dimmed: at the pointer on the pointer's surface; centred
// elsewhere, dimmed once the pointer has been on any surface (before that,
// typing shows on every output alike)
static void logo_place(struct awlock_surface *surface, double *x, double *y,
        bool *dim) {
    struct awlock_state *state = surface->state;
    if (state->pointer_surface == surface) {
        *x = state->pointer_x;
        *y = state->pointer_y;
        *dim = false;
        return;
    }
    *x = surface->width / 2.0;
    *y = surface->height / 2.0;
    *dim = state->pointer_seen;
}

// The lock surface: transparent, so awl's lock screen shows through. It needs
// a buffer (ext-session-lock: no null buffer) of the configured size: one
// transparent pixel, stretched (the viewport's destination is its size).
static void draw_background(struct awlock_surface *surface) {
    if (surface->width == 0 || surface->height == 0) {
        return; // not yet configured
    }
    wp_viewport_set_destination(surface->viewport, surface->width, surface->height);
    wl_surface_attach(surface->surface, surface->state->transparent, 0, 0);
    wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface->surface);
}

static void draw_logo(struct awlock_surface *surface) {
    struct awlock_state *state = surface->state;
    struct shm_buffer *buf = NULL;
    int x = 0, y = 0;

    if (state->args.show_logo) {
        double cx, cy;
        bool dim;
        logo_place(surface, &cx, &cy, &dim);
        buf = logo_buffer(surface, logo_state(state), dim);
        int n = state->args.logo_scale;
        x = (int)lround(cx - logo.w * n / 2.0);
        y = (int)lround(cy - logo.h * n / 2.0);
    }

    if (buf != surface->logo_shown) {
        wl_surface_set_buffer_scale(surface->logo_surface, surface->scale);
        wl_surface_attach(surface->logo_surface, buf ? buf->buffer : NULL, 0, 0);
        if (buf) {
            wl_surface_damage_buffer(surface->logo_surface, 0, 0, INT32_MAX, INT32_MAX);
        }
        surface->logo_shown = buf;
    }
    // the subsurface is synchronised: the parent's commit applies the
    // logo surface's buffer and its position together
    wl_subsurface_set_position(surface->logo_subsurface, x, y);
    wl_surface_commit(surface->logo_surface);
    wl_surface_commit(surface->surface);
}

// Surfaces, outputs, the lock

static void destroy_surface(struct awlock_surface *surface) {
    wl_list_remove(&surface->link);
    if (surface->state->pointer_surface == surface) {
        surface->state->pointer_surface = NULL;
    }
    if (surface->state->pointer_drawn == surface) {
        surface->state->pointer_drawn = NULL;
    }
    // a done already sent would otherwise reach the freed surface
    if (surface->frame_callback) {
        wl_callback_destroy(surface->frame_callback);
    }
    if (surface->lock_surface != NULL) {
        ext_session_lock_surface_v1_destroy(surface->lock_surface);
    }
    if (surface->logo_subsurface) {
        wl_subsurface_destroy(surface->logo_subsurface);
    }
    if (surface->logo_surface) {
        wl_surface_destroy(surface->logo_surface);
    }
    if (surface->viewport) {
        wp_viewport_destroy(surface->viewport);
    }
    if (surface->surface != NULL) {
        wl_surface_destroy(surface->surface);
    }
    destroy_logo_buffers(surface);
    wl_output_release(surface->output);
    free(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener;

static void create_surface(struct awlock_surface *surface) {
    struct awlock_state *state = surface->state;

    surface->surface = wl_compositor_create_surface(state->compositor);
    surface->viewport = wp_viewporter_get_viewport(state->viewporter, surface->surface);

    surface->logo_surface = wl_compositor_create_surface(state->compositor);
    surface->logo_subsurface = wl_subcompositor_get_subsurface(state->subcompositor, surface->logo_surface, surface->surface);
    wl_subsurface_set_sync(surface->logo_subsurface);
    // the logo takes no input: the pointer, always over it, stays on the
    // lock surface, with coordinates relative to that
    struct wl_region *none = wl_compositor_create_region(state->compositor);
    wl_surface_set_input_region(surface->logo_surface, none);
    wl_region_destroy(none);

    surface->lock_surface = ext_session_lock_v1_get_lock_surface(
        state->lock, surface->surface, surface->output);
    ext_session_lock_surface_v1_add_listener(surface->lock_surface,
        &ext_session_lock_surface_v1_listener, surface);

    surface->created = true;
}

static void ext_session_lock_surface_v1_handle_configure(void *data,
        struct ext_session_lock_surface_v1 *lock_surface, uint32_t serial,
        uint32_t width, uint32_t height) {
    struct awlock_surface *surface = data;
    surface->width = width;
    surface->height = height;
    ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
    draw_background(surface);
    draw_logo(surface);
}

static const struct ext_session_lock_surface_v1_listener ext_session_lock_surface_v1_listener = {
    .configure = ext_session_lock_surface_v1_handle_configure,
};

static const struct wl_callback_listener surface_frame_listener;

// draws now, and nothing more until the frame is done: the logo follows the
// pointer once per frame, without a frame's delay
static void draw_surface(struct awlock_surface *surface) {
    surface->frame_callback = wl_surface_frame(surface->surface);
    wl_callback_add_listener(surface->frame_callback, &surface_frame_listener, surface);
    surface->dirty = false;
    draw_logo(surface);
}

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
        uint32_t time) {
    struct awlock_surface *surface = data;
    wl_callback_destroy(callback);
    surface->frame_callback = NULL;
    if (surface->dirty) {
        draw_surface(surface);
    }
}

static const struct wl_callback_listener surface_frame_listener = {
    .done = surface_frame_handle_done,
};

static void damage_surface(struct awlock_surface *surface) {
    if (surface->width == 0 || surface->height == 0) {
        return; // not yet configured
    }
    if (surface->frame_callback) {
        surface->dirty = true;
    } else {
        draw_surface(surface);
    }
}

void damage_state(struct awlock_state *state) {
    struct awlock_surface *surface;
    wl_list_for_each(surface, &state->surfaces, link) {
        damage_surface(surface);
    }
}

static void handle_wl_output_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t width_mm, int32_t height_mm,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    // nothing: the logo is pixels, no text, so the subpixel order doesn't
    // matter
}

static void handle_wl_output_mode(void *data, struct wl_output *output,
        uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    // Who cares
}

static void handle_wl_output_done(void *data, struct wl_output *output) {
    struct awlock_surface *surface = data;
    if (!surface->created && surface->state->running) {
        create_surface(surface);
    }
}

static void handle_wl_output_scale(void *data, struct wl_output *output,
        int32_t factor) {
    struct awlock_surface *surface = data;
    surface->scale = factor;
    if (surface->state->running) {
        damage_surface(surface);
    }
}

static void handle_wl_output_name(void *data, struct wl_output *output,
        const char *name) {
    // Who cares
}

static void handle_wl_output_description(void *data, struct wl_output *output,
        const char *description) {
    // Who cares
}

static const struct wl_output_listener output_listener = {
    .geometry = handle_wl_output_geometry,
    .mode = handle_wl_output_mode,
    .done = handle_wl_output_done,
    .scale = handle_wl_output_scale,
    .name = handle_wl_output_name,
    .description = handle_wl_output_description,
};

static void ext_session_lock_v1_handle_locked(void *data, struct ext_session_lock_v1 *lock) {
    struct awlock_state *state = data;
    state->locked = true;
}

static void ext_session_lock_v1_handle_finished(void *data, struct ext_session_lock_v1 *lock) {
    awl_err_printf("awlock: Failed to lock session -- "
            "is another lockscreen running?\n");
    exit(2);
}

static const struct ext_session_lock_v1_listener ext_session_lock_v1_listener = {
    .locked = ext_session_lock_v1_handle_locked,
    .finished = ext_session_lock_v1_handle_finished,
};

// The seat: keys go to auth.c, the pointer moves the logo

struct awlock_seat {
    struct awlock_state *state;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    int32_t repeat_period_ms; // 0: no repeat
    int32_t repeat_delay_ms;
    // the key held: repeated by repeat_timer
    xkb_keysym_t repeat_sym;
    char repeat_text[16];
    struct loop_timer *repeat_timer;
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t format, int32_t fd, uint32_t size) {
    struct awlock_seat *seat = data;
    struct awlock_state *state = seat->state;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        awl_err_printf("awlock: Unknown keymap format %d, aborting\n", format);
        exit(1);
    }
    char *map_shm = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        awl_err_printf("awlock: Unable to initialize keymap shm, aborting\n");
        exit(1);
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
            state->xkb.context, map_shm, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size - 1);
    close(fd);
    // keep the keymap there is: dying would leave awl locked without a locker
    if (!keymap) {
        awl_err_printf("awlock: Unable to compile the keymap\n");
        return;
    }
    struct xkb_state *xkb_state = xkb_state_new(keymap);
    if (!xkb_state) {
        awl_err_printf("awlock: Unable to create the keyboard state\n");
        xkb_keymap_unref(keymap);
        return;
    }
    xkb_keymap_unref(keymap); // xkb_state has it
    xkb_state_unref(state->xkb.state);
    state->xkb.state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    // Who cares
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, struct wl_surface *surface) {
    // Who cares
}

static void keyboard_repeat(void *data) {
    struct awlock_seat *seat = data;
    seat->repeat_timer = loop_add_timer(seat->repeat_period_ms,
        keyboard_repeat, seat);
    auth_handle_key(seat->state, seat->repeat_sym, seat->repeat_text);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t key_state) {
    struct awlock_seat *seat = data;
    struct xkb_state *xkb_state = seat->state->xkb.state;
    // a key pressed or released ends the last one's repeat
    loop_remove_timer(seat->repeat_timer);
    seat->repeat_timer = NULL;
    if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_state) {
        return;
    }
    seat->repeat_sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
    xkb_state_key_get_utf8(xkb_state, key + 8, seat->repeat_text,
        sizeof(seat->repeat_text));
    auth_handle_key(seat->state, seat->repeat_sym, seat->repeat_text);
    if (seat->repeat_period_ms > 0) {
        seat->repeat_timer = loop_add_timer(seat->repeat_delay_ms,
            keyboard_repeat, seat);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group) {
    struct awlock_seat *seat = data;
    struct awlock_state *state = seat->state;
    if (state->xkb.state == NULL) {
        return;
    }

    xkb_state_update_mask(state->xkb.state,
        mods_depressed, mods_latched, mods_locked, 0, 0, group);
    int caps_lock = xkb_state_mod_name_is_active(state->xkb.state,
        XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED);
    if (caps_lock != state->xkb.caps_lock) {
        state->xkb.caps_lock = caps_lock;
        damage_state(state);
    }
    state->xkb.control = xkb_state_mod_name_is_active(state->xkb.state,
        XKB_MOD_NAME_CTRL,
        XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
        int32_t rate, int32_t delay) {
    struct awlock_seat *seat = data;
    seat->repeat_period_ms = rate > 0 ? 1000 / rate : 0; // keys per second
    seat->repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// The logo follows the pointer: at the pointer on its lock surface, centred
// and dimmed on the others (logo_place()). The events record where it is;
// wl_pointer.frame redraws, once per batch of them.

// the surface the pointer left gets its logo back in the centre, the one it
// is on gets it at the pointer
static void pointer_moved(struct awlock_state *state) {
    if (state->pointer_drawn && state->pointer_drawn != state->pointer_surface) {
        damage_surface(state->pointer_drawn);
    }
    if (state->pointer_surface) {
        damage_surface(state->pointer_surface);
    }
    state->pointer_drawn = state->pointer_surface;
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, struct wl_surface *surface,
        wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct awlock_seat *seat = data;
    struct awlock_state *state = seat->state;
    // the logo is the cursor
    wl_pointer_set_cursor(wl_pointer, serial, NULL, 0, 0);

    state->pointer_surface = NULL;
    struct awlock_surface *s;
    wl_list_for_each(s, &state->surfaces, link) {
        if (s->surface == surface) {
            state->pointer_surface = s;
            break;
        }
    }
    state->pointer_x = wl_fixed_to_double(surface_x);
    state->pointer_y = wl_fixed_to_double(surface_y);
    if (!state->pointer_seen) {
        // the other outputs' logos are dimmed from now on
        state->pointer_seen = true;
        damage_state(state);
    }
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, struct wl_surface *surface) {
    struct awlock_seat *seat = data;
    seat->state->pointer_surface = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct awlock_seat *seat = data;
    seat->state->pointer_x = wl_fixed_to_double(surface_x);
    seat->state->pointer_y = wl_fixed_to_double(surface_y);
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
        uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    // Who cares
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, uint32_t axis, wl_fixed_t value) {
    // Who cares
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    struct awlock_seat *seat = data;
    pointer_moved(seat->state);
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis_source) {
    // Who cares
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
        uint32_t time, uint32_t axis) {
    // Who cares
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
        uint32_t axis, int32_t discrete) {
    // Who cares
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
        enum wl_seat_capability caps) {
    struct awlock_seat *seat = data;
    if (seat->pointer) {
        wl_pointer_release(seat->pointer);
        seat->pointer = NULL;
        // a new one, if any, enters again
        seat->state->pointer_surface = NULL;
        pointer_moved(seat->state);
    }
    if (seat->keyboard) {
        wl_keyboard_release(seat->keyboard);
        seat->keyboard = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
        seat->pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        seat->keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
    }
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
        const char *name) {
    // Who cares
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

// The registry, main()

static void handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    struct awlock_state *state = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        state->subcompositor = wl_registry_bind(registry, name,
                &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name,
                &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct wl_seat *seat = wl_registry_bind(
                // 5: wl_pointer.frame, which moves the logo (awl's seat
                // has it)
                registry, name, &wl_seat_interface, version < 5 ? version : 5);
        struct awlock_seat *awlock_seat =
            calloc(1, sizeof(struct awlock_seat));
        awlock_seat->state = state;
        wl_seat_add_listener(seat, &seat_listener, awlock_seat);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct awlock_surface *surface =
            calloc(1, sizeof(struct awlock_surface));
        surface->state = state;
        surface->scale = 1;
        surface->output = wl_registry_bind(registry, name,
                &wl_output_interface, 4);
        surface->output_global_name = name;
        wl_output_add_listener(surface->output, &output_listener, surface);
        wl_list_insert(&state->surfaces, &surface->link);
    } else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        state->single_pixel = wl_registry_bind(registry, name,
                &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->viewporter = wl_registry_bind(registry, name,
                &wp_viewporter_interface, 1);
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        state->lock_manager = wl_registry_bind(registry, name,
                &ext_session_lock_manager_v1_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    struct awlock_state *state = data;
    struct awlock_surface *surface;
    wl_list_for_each(surface, &state->surfaces, link) {
        if (surface->output_global_name == name) {
            destroy_surface(surface);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static struct awlock_state state;

static void display_in(void *data) {
    if (wl_display_dispatch(state.display) == -1) {
        state.running = false;
    }
}

// an opacity from 0 to 1: one outside clamped, NaN def (logo_dim() needs
// 0 to 1, or the alpha spills into blue, or converts undefined)
static float alpha_valid(const char *name, float a, float def) {
    if (a >= 0.0f && a <= 1.0f) {
        return a;
    }
    float v = isnan(a) ? def : a < 0.0f ? 0.0f : 1.0f;
    awl_err_printf("awlock: %s: 0 to 1, not %g; %g\n", name, (double)a,
        (double)v);
    return v;
}

int awlock_main(const awlock_config_t *cfg, int wayland_fd) {
    state.wayland_fd = wayland_fd;
    state.args = *cfg;
    // 16: 848 logical pixels wide, up to ten buffers per output
    if (state.args.logo_scale < 1 || state.args.logo_scale > 16) {
        awl_err_printf("awlock: logo_scale: 1 to 16, not %u; 3\n",
            (unsigned)state.args.logo_scale);
        state.args.logo_scale = 3;
    }
    state.args.inactive_alpha = alpha_valid("inactive_alpha",
        state.args.inactive_alpha, 0.4f);
    state.args.active_alpha = alpha_valid("active_alpha",
        state.args.active_alpha, 1.0f);

    // a bad logo file still locks, with logo.xpm: the screen matters more
    char **file_xpm = NULL;
    int file_lines;
    const char *logo_file = state.args.logo;
    if (*logo_file && (!logo_read(logo_file, &file_xpm, &file_lines) ||
            !logo_load((const char *const *)file_xpm, file_lines, logo_file))) {
        awl_err_printf("awlock: logo: %s refused, logo.xpm instead\n", logo_file);
        logo_file = "";
        free(file_xpm);
    }
    if (!*logo_file && !logo_load((const char *const *)logo_xpm,
            sizeof(logo_xpm) / sizeof(*logo_xpm), "logo.xpm")) {
        return EXIT_FAILURE;
    }
    auth_start(&state);

    wl_list_init(&state.surfaces);
    state.xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    state.display = wl_display_connect_to_fd(wayland_fd);
    if (!state.display) {
        awl_err_printf("awlock: Unable to use awl's connection: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    if (wl_display_roundtrip(state.display) == -1) {
        awl_err_printf("awlock: wl_display_roundtrip() failed\n");
        return EXIT_FAILURE;
    }

    const char *missing = !state.compositor ? "wl_compositor" :
        !state.subcompositor ? "wl_subcompositor" :
        !state.shm ? "wl_shm" :
        !state.lock_manager ? "ext-session-lock-v1" :
        !state.single_pixel ? "single-pixel-buffer-v1" :
        !state.viewporter ? "viewporter" : NULL;
    if (missing) {
        awl_err_printf("awlock: Missing %s\n", missing);
        return 1;
    }
    // all zero: transparent, so awl's lock screen (its blur, shatter) shows
    state.transparent = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        state.single_pixel, 0, 0, 0, 0);

    state.lock = ext_session_lock_manager_v1_lock(state.lock_manager);
    ext_session_lock_v1_add_listener(state.lock,
        &ext_session_lock_v1_listener, &state);

    if (wl_display_roundtrip(state.display) == -1) {
        return 1;
    }

    struct awlock_surface *surface;
    wl_list_for_each(surface, &state.surfaces, link) {
        create_surface(surface);
    }

    while (!state.locked) {
        if (wl_display_dispatch(state.display) < 0) {
            awl_err_printf("awlock: wl_display_dispatch() failed\n");
            return 2;
        }
    }

    loop_add_fd(wl_display_get_fd(state.display), display_in, NULL);

    // unlocked only by a password or a fingerprint: no signal unlocks, and
    // a locker that dies leaves awl locked

    struct fingerprint *fingerprint = NULL;
    if (state.args.fingerprint) {
        fingerprint = fingerprint_start(&state);
    }

    state.running = true;
    while (state.running) {
        errno = 0;
        if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
            break;
        }
        loop_poll();
    }

    ext_session_lock_v1_unlock_and_destroy(state.lock);
    wl_display_roundtrip(state.display);

    fingerprint_stop(fingerprint);
    return 0;
}
