#include "awl_input.h"
#include "awl_bar.h"
#include "awl_capture.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_popup.h"
#include "awl_screensaver.h"

typedef struct {
    struct wl_list link;
    struct wlr_keyboard_group *wlr_group;

    int nsyms;
    const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
    uint32_t mods; /* invalid if nsyms == 0 */
    /* repeating for the popup that has the keyboard, keycode's (xkb) */
    bool popup_key;
    uint32_t keycode;
    struct wl_event_source *key_repeat_source;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
    awl_server_t *srv;
} KeyboardGroup;

/** an idle inhibitor's destroy listener */
typedef struct {
    struct wl_listener destroy;
    awl_server_t *srv;
} IdleInhibitor;

/** the keyboards and pointers, to apply a reloaded plugins_config.c to */
typedef struct {
    struct wlr_input_device *device;
    struct wl_listener destroy;
    struct wl_list link;
} InputDevice;

static void create_keyboard( awl_server_t *srv, struct wlr_keyboard *keyboard );
static struct xkb_keymap *compile_keymap( const awl_config_t *cfg );
static KeyboardGroup *create_keyboard_group( awl_server_t *srv );
static void configure_pointer( const awl_config_t *cfg, struct wlr_pointer *pointer );
static void create_pointer( awl_server_t *srv, struct wlr_pointer *pointer );
static void destroy_idle_inhibitor( struct wl_listener *listener, void *data );
static void destroy_keyboard_group( struct wl_listener *listener, void *data );
static int keybinding( awl_server_t *srv, uint32_t mods, xkb_keysym_t sym );
static int key_popup( awl_server_t *srv, KeyboardGroup *group, uint32_t keycode, uint32_t mods );
static void key_press( struct wl_listener *listener, void *data );
static void key_press_mod( struct wl_listener *listener, void *data );
static int key_repeat( void *data );
static void seat_caps( awl_server_t *srv );
static void untrack_input_device( struct wl_listener *listener, void *data );
static void track_input_device( awl_server_t *srv, struct wlr_input_device *device );

/** The input devices (:c:member:`awl_server_t.input`), from input_init() to
 * input_free() */
struct awl_input {
    /** the keyboards, all in one group */
    KeyboardGroup *kb_group;
    /** InputDevice.link */
    struct wl_list devices;
    /** the virtual keyboards (virtual_keyboard()), each in a group of its
     * own; they count for the seat's keyboard capability (seat_caps()) */
    int virtual_keyboards;
    /** What the keyboards were set up with, copied, to tell whether a reload
     * changed it */
    struct {
        char *xkb[5]; /* rules, model, layout, variant, options */
        int repeat_rate, repeat_delay;
    } applied;
    /** runs :c:member:`awl_config_t.on_idle` (:c:func:`idle_check`) */
    struct wl_event_source *idle_timer;
    /** the last :c:func:`activity` (CLOCK_MONOTONIC, ms) */
    int64_t last_activity;
    /** whether :c:func:`check_idle_inhibitor` found an inhibitor, the timer
     * is armed, :c:member:`awl_config_t.on_idle` ran since the last
     * :c:func:`activity`, the timeout passed while inhibited (logged), the
     * inhibitors are ignored (:c:func:`toggle_idle_inhibit`) */
    bool idle_inhibited:1, idle_armed:1, idle_done:1, idle_held:1, idle_ignored:1;
};

/** the idle check due :c:member:`awl_config_t.idle_seconds` after the last
 * :c:func:`activity`; not again before the next one once
 * :c:member:`awl_config_t.on_idle` ran, nor while inhibited once it passed */
static void idle_arm( awl_server_t *srv ) {
    struct awl_input *in = srv->input;
    const int64_t ms = (int64_t)(srv->cfg->idle_seconds * 1000.0f);
    if (!in->idle_timer || in->idle_armed || in->idle_held || in->idle_done || ms <= 0)
        return;
    const int64_t left = in->last_activity + ms - awl_now_ms();
    wl_event_source_timer_update(in->idle_timer, left < 1 ? 1 : left > INT_MAX ? INT_MAX : (int)left);
    in->idle_armed = 1;
}

/** :c:member:`awl_config_t.idle_seconds` since the last :c:func:`activity`:
 * :c:member:`awl_config_t.on_idle` runs, or, inhibited, that it doesn't is
 * logged; else checks again when that will be */
static int idle_check( void *data ) {
    awl_server_t *srv = data;
    struct awl_input *in = srv->input;
    const bool due = srv->cfg->idle_seconds > 0
            && awl_now_ms() - in->last_activity >= (int64_t)(srv->cfg->idle_seconds * 1000.0f);
    in->idle_armed = 0;
    if (due && in->idle_inhibited) {
        awl_log_printf("idle: %g s, inhibited, on_idle doesn't run\n", (double)srv->cfg->idle_seconds);
        in->idle_held = 1;
    } else if (due) {
        awl_log_printf("idle: %g s, on_idle runs\n", (double)srv->cfg->idle_seconds);
        in->idle_done = 1;
        for (size_t i = 0; i < srv->cfg->on_idle.n; i++)
            srv->cfg->on_idle.v[i].func(srv, &srv->cfg->on_idle.v[i].arg);
    }
    idle_arm(srv);
    return 0;
}

void check_idle_inhibitor( awl_server_t *srv, struct wlr_surface *exclude ) {
    int unused_lx, unused_ly;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    Client *c;
    int inhibited = awl_screensaver_inhibited(srv->screensaver);
    wl_list_for_each(c, &srv->clients.list, link)
        inhibited |= c->inhibit_idle && (srv->cfg->bypass_surface_visibility
                || wlr_scene_node_coords(&c->scene->node, &unused_lx, &unused_ly));
    wl_list_for_each(inhibitor, &srv->idle_inhibit_mgr->inhibitors, link) {
        struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
        struct wlr_scene_tree *tree = surface->data;
        if (exclude != surface && (srv->cfg->bypass_surface_visibility || (!tree
                || wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
            inhibited = 1;
            break;
        }
    }
    inhibited &= !srv->input->idle_ignored;

    wlr_idle_notifier_v1_set_inhibited(srv->idle_notifier, inhibited);
    /* like wlroots' notifier: the idle time counts from the inhibitor's end */
    if (srv->input->idle_inhibited && !inhibited) {
        srv->input->last_activity = awl_now_ms();
        srv->input->idle_held = 0;
    }
    srv->input->idle_inhibited = inhibited;
    idle_arm(srv);
}

void toggle_idle_inhibit( awl_server_t *srv, const Arg *arg ) {
    (void)arg;
    srv->input->idle_ignored = !srv->input->idle_ignored;
    awl_log_printf("idle: inhibitors %s\n", srv->input->idle_ignored ? "ignored" : "honored");
    check_idle_inhibitor(srv, NULL);
    bars_dirty(srv);
}

bool idle_inhibit_ignored( awl_server_t *srv ) {
    return srv->input->idle_ignored;
}

void activity( awl_server_t *srv ) {
    wlr_idle_notifier_v1_notify_activity(srv->idle_notifier, srv->seat);
    awl_notifications_activity(srv);
    srv->input->last_activity = awl_now_ms();
    srv->input->idle_done = srv->input->idle_held = 0;
    idle_arm(srv);
}

void create_idle_inhibitor( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
    IdleInhibitor *inhibitor = calloc_or_die(1, sizeof(*inhibitor));
    inhibitor->srv = srv;
    LISTEN(&idle_inhibitor->events.destroy, &inhibitor->destroy, destroy_idle_inhibitor);

    check_idle_inhibitor(srv, NULL);
}

void create_keyboard( awl_server_t *srv, struct wlr_keyboard *keyboard ) {
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(keyboard, srv->input->kb_group->wlr_group->keyboard.keymap);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(srv->input->kb_group->wlr_group, keyboard);
}

struct xkb_keymap *compile_keymap( const awl_config_t *cfg ) {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context,
            &cfg->keyboard.xkb, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    xkb_context_unref(context);
    return keymap;
}

KeyboardGroup *create_keyboard_group( awl_server_t *srv ) {
    KeyboardGroup *group = calloc_or_die(1, sizeof(*group));
    struct xkb_keymap *keymap;

    group->srv = srv;
    group->wlr_group = wlr_keyboard_group_create();
    group->wlr_group->data = group;

    /* Prepare an XKB keymap and assign it to the keyboard group: the one in
     * effect, which is the config's unless a reload brought a broken one */
    if (srv->input->kb_group)
        keymap = xkb_keymap_ref(srv->input->kb_group->wlr_group->keyboard.keymap);
    else if (!(keymap = compile_keymap(srv->cfg)))
        die("failed to compile keymap");

    wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
    xkb_keymap_unref(keymap);

    wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard,
            srv->input->kb_group ? srv->input->kb_group->wlr_group->keyboard.repeat_info.rate : srv->cfg->keyboard.repeat_rate,
            srv->input->kb_group ? srv->input->kb_group->wlr_group->keyboard.repeat_info.delay : srv->cfg->keyboard.repeat_delay);

    /* Set up listeners for keyboard events */
    LISTEN(&group->wlr_group->keyboard.events.key, &group->key, key_press);
    LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, key_press_mod);

    group->key_repeat_source = wl_event_loop_add_timer(srv->event_loop, key_repeat, group);

    /* A seat can only have one keyboard, but this is a limitation of the
     * Wayland protocol - not wlroots. We assign all connected keyboards to the
     * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
     * all of them. Set this combined wlr_keyboard as the seat keyboard.
     */
    wlr_seat_set_keyboard(srv->seat, &group->wlr_group->keyboard);
    return group;
}

void configure_pointer( const awl_config_t *cfg, struct wlr_pointer *pointer ) {
    struct libinput_device *device;
    if (wlr_input_device_is_libinput(&pointer->base)
            && (device = wlr_libinput_get_device_handle(&pointer->base))) {

        if (libinput_device_config_tap_get_finger_count(device)) {
            libinput_device_config_tap_set_enabled(device, cfg->pointer.tap_to_click);
            libinput_device_config_tap_set_drag_enabled(device, cfg->pointer.tap_and_drag);
            libinput_device_config_tap_set_drag_lock_enabled(device, cfg->pointer.drag_lock);
            libinput_device_config_tap_set_button_map(device, cfg->pointer.button_map);
        }

        if (libinput_device_config_scroll_has_natural_scroll(device))
            libinput_device_config_scroll_set_natural_scroll_enabled(device, cfg->pointer.natural_scrolling);

        if (libinput_device_config_dwt_is_available(device))
            libinput_device_config_dwt_set_enabled(device, cfg->pointer.disable_while_typing);

        if (libinput_device_config_left_handed_is_available(device))
            libinput_device_config_left_handed_set(device, cfg->pointer.left_handed);

        if (libinput_device_config_middle_emulation_is_available(device))
            libinput_device_config_middle_emulation_set_enabled(device, cfg->pointer.middle_button_emulation);

        if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
            libinput_device_config_scroll_set_method(device, cfg->pointer.scroll_method);

        if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
            libinput_device_config_click_set_method(device, cfg->pointer.click_method);

        if (libinput_device_config_send_events_get_modes(device))
            libinput_device_config_send_events_set_mode(device, cfg->pointer.send_events_mode);

        if (libinput_device_config_accel_is_available(device)) {
            libinput_device_config_accel_set_profile(device, cfg->pointer.accel_profile);
            libinput_device_config_accel_set_speed(device, cfg->pointer.accel_speed);
        }
    }
}

void create_pointer( awl_server_t *srv, struct wlr_pointer *pointer ) {
    configure_pointer(srv->cfg, pointer);
    wlr_cursor_attach_input_device(srv->cursor.wlr, &pointer->base);
}

void destroy_idle_inhibitor( struct wl_listener *listener, void *data ) {
    IdleInhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
    /* `data` is the wlr_surface of the idle inhibitor being destroyed,
     * at this point the idle inhibitor is still in the list of the manager */
    check_idle_inhibitor(inhibitor->srv, wlr_surface_get_root_surface(data));
    wl_list_remove(&inhibitor->destroy.link);
    free(inhibitor);
}

void destroy_keyboard_group( struct wl_listener *listener, void *data ) {
    KeyboardGroup *group = wl_container_of(listener, group, destroy);
    awl_server_t *srv = group->srv;
    /* a virtual keyboard's (virtual_keyboard()) */
    int virtual = group != srv->input->kb_group;
    (void)data;
    wl_event_source_remove(group->key_repeat_source);
    wl_list_remove(&group->key.link);
    wl_list_remove(&group->modifiers.link);
    wl_list_remove(&group->destroy.link);
    wlr_keyboard_group_destroy(group->wlr_group);
    free(group);
    if (virtual) {
        srv->input->virtual_keyboards--;
        seat_caps(srv);
    }
}

void input_device( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_input_device);
    /* This event is raised by the backend when a new input device becomes
     * available. */
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        create_keyboard(srv, wlr_keyboard_from_input_device(device));
        track_input_device(srv, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        create_pointer(srv, wlr_pointer_from_input_device(device));
        track_input_device(srv, device);
        break;
    default:
        /* touch screens, tablets and switches (a laptop's lid) aren't used */
        break;
    }
    seat_caps(srv);
}

/* What the seat offers its clients: always a pointer, as there is always a
 * cursor, even without a pointer device (awl's own popups and clients that
 * place things by it, such as awlock, rely on it), and a keyboard while there
 * is a physical or a virtual one. */
void seat_caps( awl_server_t *srv ) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&srv->input->kb_group->wlr_group->devices) || srv->input->virtual_keyboards)
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(srv->seat, caps);
}

uint32_t bind_mods( const awl_config_t *cfg, uint32_t mod ) {
    return mod & MODKEY ? (mod & ~MODKEY) | cfg->mod_key : mod;
}

int keybinding( awl_server_t *srv, uint32_t mods, xkb_keysym_t sym ) {
    /*
     * Here we handle compositor keybindings. This is when the compositor is
     * processing keys, rather than passing them on to the client for its own
     * processing.
     */
    for (const Key *k = srv->cfg->keys.v; k < srv->cfg->keys.v + srv->cfg->keys.n; k++) {
        if (CLEAN_MASK(mods) == CLEAN_MASK(bind_mods(srv->cfg, k->mod))
                && sym == k->keysym && k->func) {
            // awl_vrb_printf( "keybinding: (mod:%u key:%u func:%p arg:%li)\n", k->mod, k->keysym, k->func, k->arg.v );
            k->func(srv, &k->arg);
            return 1;
        }
    }
    return 0;
}

/** a press of keycode (xkb's) for the popup that has the keyboard, the
 * bindings for what it doesn't take; returns whether either took it */
int key_popup( awl_server_t *srv, KeyboardGroup *group, uint32_t keycode, uint32_t mods ) {
    struct xkb_state *state = group->wlr_group->keyboard.xkb_state;
    const xkb_keysym_t *syms;
    int handled = 0, nsyms = xkb_state_key_get_syms(state, keycode, &syms);
    char utf8[16];

    /* what it types, if anything: not Return's or Ctrl+W's control
     * character */
    xkb_state_key_get_utf8(state, keycode, utf8, sizeof(utf8));
    if ((unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7f)
        utf8[0] = '\0';
    for (int i = 0; i < nsyms; i++) {
        /* the callback may hide it, or take another key */
        awl_window_t *win = popup_keyboard(srv);
        if (win && awl_window_key(win, syms[i], utf8, mods))
            handled = 1;
        else
            handled = keybinding(srv, mods, syms[i]) || handled;
    }
    return handled;
}

void key_press( struct wl_listener *listener, void *data ) {
    /* This event is raised when a key is pressed or released. */
    KeyboardGroup *group = wl_container_of(listener, group, key);
    awl_server_t *srv = group->srv;
    struct wlr_keyboard_key_event *event = data;

    /* Translate libinput keycode -> xkbcommon */
    uint32_t keycode = event->keycode + 8;
    /* Get a list of keysyms based on the keymap for this keyboard */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
            group->wlr_group->keyboard.xkb_state, keycode, &syms);

    int handled = 0;
    uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

    activity(srv);

    if (srv->locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        lock_keystroke(srv);

    /* A grab gets the presses, without repeat, before a popup or a
     * binding; the releases go on to the client, as a binding's do. */
    if (!srv->locked && srv->cursor.mode == CurGrab && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        group->nsyms = 0;
        wl_event_source_timer_update(group->key_repeat_source, 0);
        /* the callback may end the grab */
        for (int i = 0; i < nsyms && srv->cursor.mode == CurGrab; i++)
            grab_key(srv, syms[i], mods);
        return;
    }

    /* A popup that has the keyboard (the launcher) gets every key before the
     * bindings, and no client gets any: none has the focus meanwhile. */
    if (!srv->locked && popup_keyboard(srv)) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED
                && key_popup(srv, group, keycode, mods)
                && group->wlr_group->keyboard.repeat_info.delay > 0) {
            group->popup_key = 1;
            group->keycode = keycode;
            group->mods = mods;
            group->nsyms = 1;
            wl_event_source_timer_update(group->key_repeat_source,
                    group->wlr_group->keyboard.repeat_info.delay);
        } else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED
                || !group->popup_key || keycode == group->keycode) {
            /* a release stops a binding's repeat too: the binding that
             * opened the popup (the launcher's) would toggle it again and
             * again while nothing else is pressed */
            group->nsyms = 0;
            wl_event_source_timer_update(group->key_repeat_source, 0);
        }
        return;
    }

    /* On _press_ if there is no active screen locker,
     * attempt to process a compositor keybinding. */
    if (!srv->locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++)
            handled = keybinding(srv, mods, syms[i]) || handled;
    }

    if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
        group->popup_key = 0;
        group->mods = mods;
        group->keysyms = syms;
        group->nsyms = nsyms;
        wl_event_source_timer_update(group->key_repeat_source,
                group->wlr_group->keyboard.repeat_info.delay);
    } else {
        group->nsyms = 0;
        wl_event_source_timer_update(group->key_repeat_source, 0);
    }

    if (handled)
        return;

    wlr_seat_set_keyboard(srv->seat, &group->wlr_group->keyboard);
    /* Pass unhandled keycodes along to the client. */
    wlr_seat_keyboard_notify_key(srv->seat, event->time_msec,
            event->keycode, event->state);
}

void key_press_mod( struct wl_listener *listener, void *data ) {
    /* This event is raised when a modifier key, such as shift or alt, is
     * pressed. We simply communicate this to the client. */
    KeyboardGroup *group = wl_container_of(listener, group, modifiers);
    awl_server_t *srv = group->srv;
    (void)data;

    wlr_seat_set_keyboard(srv->seat, &group->wlr_group->keyboard);
    /* Send modifiers to the client. */
    wlr_seat_keyboard_notify_modifiers(srv->seat,
            &group->wlr_group->keyboard.modifiers);
}

int key_repeat( void *data ) {
    KeyboardGroup *group = data;
    awl_server_t *srv = group->srv;
    if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
        return 0;

    wl_event_source_timer_update(group->key_repeat_source,
            1000 / group->wlr_group->keyboard.repeat_info.rate);

    /* a binding's repeat doesn't go on into a popup's grab (see key_press()) */
    if (!group->popup_key && popup_keyboard(srv)) {
        group->nsyms = 0;
        wl_event_source_timer_update(group->key_repeat_source, 0);
        return 0;
    }
    if (group->popup_key) {
        /* stops with the popup, and when the screen is locked */
        if (srv->locked || !popup_keyboard(srv) || !key_popup(srv, group, group->keycode, group->mods)) {
            group->nsyms = 0;
            wl_event_source_timer_update(group->key_repeat_source, 0);
        }
        return 0;
    }
    for (int i = 0; i < group->nsyms; i++)
        keybinding(srv, group->mods, group->keysyms[i]);

    return 0;
}

void set_primary_selection( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_set_psel);
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(srv->seat, event->source, event->serial);
}

void set_selection( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_set_sel);
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(srv->seat, event->source, event->serial);
}

static void untrack_input_device( struct wl_listener *listener, void *data ) {
    InputDevice *d = wl_container_of(listener, d, destroy);
    (void)data;
    wl_list_remove(&d->destroy.link);
    wl_list_remove(&d->link);
    free(d);
}

void track_input_device( awl_server_t *srv, struct wlr_input_device *device ) {
    InputDevice *d = calloc_or_die(1, sizeof(*d));
    d->device = device;
    LISTEN(&device->events.destroy, &d->destroy, untrack_input_device);
    wl_list_insert(&srv->input->devices, &d->link);
}

void virtual_keyboard( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *kb = data;
    /* virtual keyboards shouldn't share keyboard group */
    KeyboardGroup *group = create_keyboard_group(srv);
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
    LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroy_keyboard_group);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
    srv->input->virtual_keyboards++;
    seat_caps(srv);
}

void virtual_pointer( struct wl_listener *listener, void *data ) {
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    struct wlr_input_device *device = &event->new_pointer->pointer.base;

    wlr_cursor_attach_input_device(srv->cursor.wlr, device);
    if (event->suggested_output)
        wlr_cursor_map_input_to_output(srv->cursor.wlr, device, event->suggested_output);
}

void input_init( awl_server_t *srv ) {
    srv->input = calloc_or_die(1, sizeof *srv->input);
    wl_list_init(&srv->input->devices);
    srv->input->kb_group = create_keyboard_group(srv);
    wl_list_init(&srv->input->kb_group->destroy.link);
    srv->input->last_activity = awl_now_ms();
    srv->input->idle_timer = wl_event_loop_add_timer(srv->event_loop, idle_check, srv);
}

void input_fini( awl_server_t *srv ) {
    destroy_keyboard_group(&srv->input->kb_group->destroy, NULL);
    srv->input->kb_group = NULL;
    if (srv->input->idle_timer)
        wl_event_source_remove(srv->input->idle_timer);
    srv->input->idle_timer = NULL;
}

void input_free( awl_server_t *srv ) {
    if (!srv->input)
        return;
    for (int i = 0; i < (int)LENGTH(srv->input->applied.xkb); i++)
        free(srv->input->applied.xkb[i]);
    free(srv->input);
    srv->input = NULL;
}

void input_configure( awl_server_t *srv ) {
    InputDevice *d;
    struct xkb_keymap *keymap;
    const char *xkb[5] = { srv->cfg->keyboard.xkb.rules, srv->cfg->keyboard.xkb.model,
        srv->cfg->keyboard.xkb.layout, srv->cfg->keyboard.xkb.variant, srv->cfg->keyboard.xkb.options };
    int changed = 0;

    /* keyboards: the virtual ones keep the keymaps their clients gave them */
    for (int i = 0; i < (int)LENGTH(xkb); i++)
        changed |= !streq_null(xkb[i], srv->input->applied.xkb[i]);
    if (changed && !(keymap = compile_keymap(srv->cfg))) {
        awl_wrn_printf("plugins_config.c: can't compile the keymap, keeping the old one\n");
    } else if (changed) {
        wlr_keyboard_set_keymap(&srv->input->kb_group->wlr_group->keyboard, keymap);
        wl_list_for_each(d, &srv->input->devices, link)
            if (d->device->type == WLR_INPUT_DEVICE_KEYBOARD)
                wlr_keyboard_set_keymap(wlr_keyboard_from_input_device(d->device), keymap);
        xkb_keymap_unref(keymap);
        for (int i = 0; i < (int)LENGTH(xkb); i++) {
            free(srv->input->applied.xkb[i]);
            srv->input->applied.xkb[i] = strdup_null(xkb[i]);
        }
    }
    if (srv->cfg->keyboard.repeat_rate != srv->input->applied.repeat_rate || srv->cfg->keyboard.repeat_delay != srv->input->applied.repeat_delay) {
        wlr_keyboard_set_repeat_info(&srv->input->kb_group->wlr_group->keyboard,
                srv->cfg->keyboard.repeat_rate, srv->cfg->keyboard.repeat_delay);
        srv->input->applied.repeat_rate = srv->cfg->keyboard.repeat_rate;
        srv->input->applied.repeat_delay = srv->cfg->keyboard.repeat_delay;
    }

    wl_list_for_each(d, &srv->input->devices, link)
        if (d->device->type == WLR_INPUT_DEVICE_POINTER)
            configure_pointer(srv->cfg, wlr_pointer_from_input_device(d->device));
    /* again, for a changed idle_seconds */
    srv->input->idle_armed = 0;
    idle_arm(srv);
}
