#include "awl_input.h"
#include "awl_config.h"
#include "awl_cursor.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_popup.h"

typedef struct {
    struct wl_list link;
    struct wlr_keyboard_group *wlr_group;

    int nsyms;
    const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
    uint32_t mods; /* invalid if nsyms == 0 */
    /* repeating for the popup that has the keyboard, keycode's (xkb) */
    bool popupkey;
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

static void createkeyboard(awl_server_t *srv, struct wlr_keyboard *keyboard);
static struct xkb_keymap *compilekeymap(awl_server_t *srv);
static KeyboardGroup *createkeyboardgroup(awl_server_t *srv);
static void configurepointer(awl_server_t *srv, struct wlr_pointer *pointer);
static void createpointer(awl_server_t *srv, struct wlr_pointer *pointer);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static int keybinding(awl_server_t *srv, uint32_t mods, xkb_keysym_t sym);
static int keypopup(awl_server_t *srv, KeyboardGroup *group, uint32_t keycode, uint32_t mods);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void untrackinputdevice(struct wl_listener *listener, void *data);
static void trackinputdevice(awl_server_t *srv, struct wlr_input_device *device);

/** The input devices (:c:member:`awl_server_t.input`), from inputinit() to
 * inputfree() */
struct awl_input {
    /** the keyboards, all in one group */
    KeyboardGroup *kb_group;
    /** InputDevice.link */
    struct wl_list devices;
    /** What the keyboards were set up with, copied, to tell whether a reload
     * changed it */
    struct {
        char *xkb[5]; /* rules, model, layout, variant, options */
        int repeat_rate, repeat_delay;
    } applied;
};

void
checkidleinhibitor(awl_server_t *srv, struct wlr_surface *exclude)
{
    int inhibited = 0, unused_lx, unused_ly;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    wl_list_for_each(inhibitor, &srv->idle_inhibit_mgr->inhibitors, link) {
        struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
        struct wlr_scene_tree *tree = surface->data;
        if (exclude != surface && (srv->cfg->bypass_surface_visibility || (!tree
                || wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
            inhibited = 1;
            break;
        }
    }

    wlr_idle_notifier_v1_set_inhibited(srv->idle_notifier, inhibited);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_idle_inhibitor);
    struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
    IdleInhibitor *inhibitor = ecalloc(1, sizeof(*inhibitor));
    inhibitor->srv = srv;
    LISTEN(&idle_inhibitor->events.destroy, &inhibitor->destroy, destroyidleinhibitor);

    checkidleinhibitor(srv, NULL);
}

void
createkeyboard(awl_server_t *srv, struct wlr_keyboard *keyboard)
{
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(keyboard, srv->input->kb_group->wlr_group->keyboard.keymap);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(srv->input->kb_group->wlr_group, keyboard);
}

struct xkb_keymap *
compilekeymap(awl_server_t *srv)
{
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(context,
            &srv->cfg->keyboard.xkb, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    xkb_context_unref(context);
    return keymap;
}

KeyboardGroup *
createkeyboardgroup(awl_server_t *srv)
{
    KeyboardGroup *group = ecalloc(1, sizeof(*group));
    struct xkb_keymap *keymap;

    group->srv = srv;
    group->wlr_group = wlr_keyboard_group_create();
    group->wlr_group->data = group;

    /* Prepare an XKB keymap and assign it to the keyboard group: the one in
     * effect, which is the config's unless a reload brought a broken one */
    if (srv->input->kb_group)
        keymap = xkb_keymap_ref(srv->input->kb_group->wlr_group->keyboard.keymap);
    else if (!(keymap = compilekeymap(srv)))
        die("failed to compile keymap");

    wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
    xkb_keymap_unref(keymap);

    wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard,
            srv->input->kb_group ? srv->input->kb_group->wlr_group->keyboard.repeat_info.rate : srv->cfg->keyboard.repeat_rate,
            srv->input->kb_group ? srv->input->kb_group->wlr_group->keyboard.repeat_info.delay : srv->cfg->keyboard.repeat_delay);

    /* Set up listeners for keyboard events */
    LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
    LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

    group->key_repeat_source = wl_event_loop_add_timer(srv->event_loop, keyrepeat, group);

    /* A seat can only have one keyboard, but this is a limitation of the
     * Wayland protocol - not wlroots. We assign all connected keyboards to the
     * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
     * all of them. Set this combined wlr_keyboard as the seat keyboard.
     */
    wlr_seat_set_keyboard(srv->seat, &group->wlr_group->keyboard);
    return group;
}

void
configurepointer(awl_server_t *srv, struct wlr_pointer *pointer)
{
    struct libinput_device *device;
    if (wlr_input_device_is_libinput(&pointer->base)
            && (device = wlr_libinput_get_device_handle(&pointer->base))) {

        if (libinput_device_config_tap_get_finger_count(device)) {
            libinput_device_config_tap_set_enabled(device, srv->cfg->pointer.tap_to_click);
            libinput_device_config_tap_set_drag_enabled(device, srv->cfg->pointer.tap_and_drag);
            libinput_device_config_tap_set_drag_lock_enabled(device, srv->cfg->pointer.drag_lock);
            libinput_device_config_tap_set_button_map(device, srv->cfg->pointer.button_map);
        }

        if (libinput_device_config_scroll_has_natural_scroll(device))
            libinput_device_config_scroll_set_natural_scroll_enabled(device, srv->cfg->pointer.natural_scrolling);

        if (libinput_device_config_dwt_is_available(device))
            libinput_device_config_dwt_set_enabled(device, srv->cfg->pointer.disable_while_typing);

        if (libinput_device_config_left_handed_is_available(device))
            libinput_device_config_left_handed_set(device, srv->cfg->pointer.left_handed);

        if (libinput_device_config_middle_emulation_is_available(device))
            libinput_device_config_middle_emulation_set_enabled(device, srv->cfg->pointer.middle_button_emulation);

        if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
            libinput_device_config_scroll_set_method(device, srv->cfg->pointer.scroll_method);

        if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
            libinput_device_config_click_set_method(device, srv->cfg->pointer.click_method);

        if (libinput_device_config_send_events_get_modes(device))
            libinput_device_config_send_events_set_mode(device, srv->cfg->pointer.send_events_mode);

        if (libinput_device_config_accel_is_available(device)) {
            libinput_device_config_accel_set_profile(device, srv->cfg->pointer.accel_profile);
            libinput_device_config_accel_set_speed(device, srv->cfg->pointer.accel_speed);
        }
    }
}

void
createpointer(awl_server_t *srv, struct wlr_pointer *pointer)
{
    configurepointer(srv, pointer);
    wlr_cursor_attach_input_device(srv->cursor.wlr, &pointer->base);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
    IdleInhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
    /* `data` is the wlr_surface of the idle inhibitor being destroyed,
     * at this point the idle inhibitor is still in the list of the manager */
    checkidleinhibitor(inhibitor->srv, wlr_surface_get_root_surface(data));
    wl_list_remove(&inhibitor->destroy.link);
    free(inhibitor);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
    KeyboardGroup *group = wl_container_of(listener, group, destroy);
    wl_event_source_remove(group->key_repeat_source);
    wl_list_remove(&group->key.link);
    wl_list_remove(&group->modifiers.link);
    wl_list_remove(&group->destroy.link);
    wlr_keyboard_group_destroy(group->wlr_group);
    free(group);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_input_device);
    /* This event is raised by the backend when a new input device becomes
     * available. */
    struct wlr_input_device *device = data;
    uint32_t caps;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        createkeyboard(srv, wlr_keyboard_from_input_device(device));
        trackinputdevice(srv, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        createpointer(srv, wlr_pointer_from_input_device(device));
        trackinputdevice(srv, device);
        break;
    default:
        /* TODO handle other input device types */
        break;
    }

    /* We need to let the wlr_seat know what our capabilities are, which is
     * communiciated to the client. In awl we always have a cursor, even if
     * there are no pointer devices, so we always include that capability. */
    /* TODO do we actually require a cursor? */
    caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&srv->input->kb_group->wlr_group->devices))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(srv->seat, caps);
}

uint32_t
bindmods(awl_server_t *srv, uint32_t mod)
{
    return mod & MODKEY ? (mod & ~MODKEY) | srv->cfg->modkey : mod;
}

int
keybinding(awl_server_t *srv, uint32_t mods, xkb_keysym_t sym)
{
    /*
     * Here we handle compositor keybindings. This is when the compositor is
     * processing keys, rather than passing them on to the client for its own
     * processing.
     */
    const Key *k;
    for (k = srv->cfg->keys.v; k < srv->cfg->keys.v + srv->cfg->keys.n; k++) {
        if (CLEANMASK(mods) == CLEANMASK(bindmods(srv, k->mod))
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
int
keypopup(awl_server_t *srv, KeyboardGroup *group, uint32_t keycode, uint32_t mods)
{
    struct xkb_state *state = group->wlr_group->keyboard.xkb_state;
    const xkb_keysym_t *syms;
    int i, handled = 0, nsyms = xkb_state_key_get_syms(state, keycode, &syms);
    awl_window_t *win;
    char utf8[16];

    /* what it types, if anything: not Return's or Ctrl+W's control
     * character */
    xkb_state_key_get_utf8(state, keycode, utf8, sizeof(utf8));
    if ((unsigned char)utf8[0] < 0x20 || utf8[0] == 0x7f)
        utf8[0] = '\0';
    for (i = 0; i < nsyms; i++) {
        /* the callback may hide it, or take another key */
        if ((win = popupkeyboard(srv)) && awl_window_key(win, syms[i], utf8, mods))
            handled = 1;
        else
            handled = keybinding(srv, mods, syms[i]) || handled;
    }
    return handled;
}

void
keypress(struct wl_listener *listener, void *data)
{
    int i;
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

    wlr_idle_notifier_v1_notify_activity(srv->idle_notifier, srv->seat);
    awl_notifications_activity(srv);

    if (srv->locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        lockkeystroke(srv);

    /* A popup that has the keyboard (the launcher) gets every key before the
     * bindings, and no client gets any: none has the focus meanwhile. */
    if (!srv->locked && popupkeyboard(srv)) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED
                && keypopup(srv, group, keycode, mods)
                && group->wlr_group->keyboard.repeat_info.delay > 0) {
            group->popupkey = 1;
            group->keycode = keycode;
            group->mods = mods;
            group->nsyms = 1;
            wl_event_source_timer_update(group->key_repeat_source,
                    group->wlr_group->keyboard.repeat_info.delay);
        } else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED
                || !group->popupkey || keycode == group->keycode) {
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
        for (i = 0; i < nsyms; i++)
            handled = keybinding(srv, mods, syms[i]) || handled;
    }

    if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
        group->popupkey = 0;
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

void
keypressmod(struct wl_listener *listener, void *data)
{
    /* This event is raised when a modifier key, such as shift or alt, is
     * pressed. We simply communicate this to the client. */
    KeyboardGroup *group = wl_container_of(listener, group, modifiers);
    awl_server_t *srv = group->srv;

    wlr_seat_set_keyboard(srv->seat, &group->wlr_group->keyboard);
    /* Send modifiers to the client. */
    wlr_seat_keyboard_notify_modifiers(srv->seat,
            &group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
    KeyboardGroup *group = data;
    awl_server_t *srv = group->srv;
    int i;
    if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
        return 0;

    wl_event_source_timer_update(group->key_repeat_source,
            1000 / group->wlr_group->keyboard.repeat_info.rate);

    /* a binding's repeat doesn't go on into a popup's grab (see keypress()) */
    if (!group->popupkey && popupkeyboard(srv)) {
        group->nsyms = 0;
        wl_event_source_timer_update(group->key_repeat_source, 0);
        return 0;
    }
    if (group->popupkey) {
        /* stops with the popup, and when the screen is locked */
        if (srv->locked || !popupkeyboard(srv) || !keypopup(srv, group, group->keycode, group->mods)) {
            group->nsyms = 0;
            wl_event_source_timer_update(group->key_repeat_source, 0);
        }
        return 0;
    }
    for (i = 0; i < group->nsyms; i++)
        keybinding(srv, group->mods, group->keysyms[i]);

    return 0;
}

void
setpsel(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_set_psel);
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(srv->seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.request_set_sel);
    /* This event is raised by the seat when a client wants to set the selection,
     * usually when the user copies something. wlroots allows compositors to
     * ignore such requests if they so choose, but in awl we always honor them
     */
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(srv->seat, event->source, event->serial);
}

static void
untrackinputdevice(struct wl_listener *listener, void *data)
{
    InputDevice *d = wl_container_of(listener, d, destroy);
    wl_list_remove(&d->destroy.link);
    wl_list_remove(&d->link);
    free(d);
}

void
trackinputdevice(awl_server_t *srv, struct wlr_input_device *device)
{
    InputDevice *d = ecalloc(1, sizeof(*d));
    d->device = device;
    LISTEN(&device->events.destroy, &d->destroy, untrackinputdevice);
    wl_list_insert(&srv->input->devices, &d->link);
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *kb = data;
    /* virtual keyboards shouldn't share keyboard group */
    KeyboardGroup *group = createkeyboardgroup(srv);
    /* Set the keymap to match the group keymap */
    wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
    LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

    /* Add the new keyboard to the group */
    wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
    awl_server_t *srv = wl_container_of(listener, srv, listen.new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    struct wlr_input_device *device = &event->new_pointer->pointer.base;

    wlr_cursor_attach_input_device(srv->cursor.wlr, device);
    if (event->suggested_output)
        wlr_cursor_map_input_to_output(srv->cursor.wlr, device, event->suggested_output);
}

void
inputinit(awl_server_t *srv)
{
    srv->input = ecalloc(1, sizeof *srv->input);
    wl_list_init(&srv->input->devices);
    srv->input->kb_group = createkeyboardgroup(srv);
    wl_list_init(&srv->input->kb_group->destroy.link);
}

void
inputfini(awl_server_t *srv)
{
    destroykeyboardgroup(&srv->input->kb_group->destroy, NULL);
    srv->input->kb_group = NULL;
}

void
inputfree(awl_server_t *srv)
{
    int i;
    if (!srv->input)
        return;
    for (i = 0; i < (int)LENGTH(srv->input->applied.xkb); i++)
        free(srv->input->applied.xkb[i]);
    free(srv->input);
    srv->input = NULL;
}

void
inputconfigure(awl_server_t *srv)
{
    InputDevice *d;
    struct xkb_keymap *keymap;
    const char *xkb[5] = { srv->cfg->keyboard.xkb.rules, srv->cfg->keyboard.xkb.model,
        srv->cfg->keyboard.xkb.layout, srv->cfg->keyboard.xkb.variant, srv->cfg->keyboard.xkb.options };
    int i, changed;

    /* keyboards: the virtual ones keep the keymaps their clients gave them */
    for (i = changed = 0; i < (int)LENGTH(xkb); i++)
        changed |= !streqnull(xkb[i], srv->input->applied.xkb[i]);
    if (changed && !(keymap = compilekeymap(srv))) {
        awl_wrn_printf("plugins_config.c: can't compile the keymap, keeping the old one\n");
    } else if (changed) {
        wlr_keyboard_set_keymap(&srv->input->kb_group->wlr_group->keyboard, keymap);
        wl_list_for_each(d, &srv->input->devices, link)
            if (d->device->type == WLR_INPUT_DEVICE_KEYBOARD)
                wlr_keyboard_set_keymap(wlr_keyboard_from_input_device(d->device), keymap);
        xkb_keymap_unref(keymap);
        for (i = 0; i < (int)LENGTH(xkb); i++) {
            free(srv->input->applied.xkb[i]);
            srv->input->applied.xkb[i] = strdupnull(xkb[i]);
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
            configurepointer(srv, wlr_pointer_from_input_device(d->device));
}
