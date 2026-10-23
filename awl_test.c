#include "awl_test.h"
#include "awl_config.h"
#include "awl_lockscreen.h"
#include "awl_log.h"
#include "awl_notifications.h"
#include "awl_output.h"
#include "awl_popup.h"
#include <fcntl.h>
#include <limits.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_output.h>

/** AWL_TEST_WINDOWS' popups by number, see testwincmd(); :c:func:`popupdestroy`
 * clears a slot (:c:func:`testpopupdestroyed`), so a plugin reload leaves none
 * dangling */
static awl_window_t *testwins[8];

static void
testoutputadd_backend(struct wlr_backend *b, void *data)
{
    if (wlr_backend_is_headless(b))
        wlr_headless_add_output(b, 1280, 720);
    else if (wlr_backend_is_wl(b))
        wlr_wl_output_create(b);
}

/** the AWL_TEST_OUTPUTS hooks, see :c:func:`testinit` */
static int
testoutputadd(int signo, void *data)
{
    wlr_multi_for_each_backend(backend, testoutputadd_backend, NULL);
    return 0;
}

static int
testoutputremove(int signo, void *data)
{
    /* the newest monitor, createmon() inserts at the head; never the last one */
    Monitor *m;
    if (wl_list_length(&mons) > 1) {
        m = wl_container_of(mons.next, m, link);
        wlr_output_destroy(m->wlr_output);
    }
    return 0;
}

/** what testwincmd()'s dump reports as clicks and scrolls */
static char testclicks[2048];

/** input N on: N's click callback, data is N */
static void
testwinclick(awl_window_t *win, int row, int col, uint32_t button, void *data)
{
    size_t len = strlen(testclicks);
    snprintf(testclicks + len, sizeof(testclicks) - len, "click %d %d %d %u\n",
            (int)(intptr_t)data, row, col, button);
}

/** scroll N on: N's scroll callback, data is N */
static void
testwinscroll(awl_window_t *win, int row, int col, uint32_t orientation, double delta,
        int32_t value120, void *data)
{
    size_t len = strlen(testclicks);
    snprintf(testclicks + len, sizeof(testclicks) - len, "scroll %d %d %d %u %d %d\n",
            (int)(intptr_t)data, row, col, orientation, value120, delta > 0 ? 1 : delta < 0 ? -1 : 0);
}

/** \n, \t and \\ in s, in place */
static void
testunescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/** One line of AWL_TEST_WINDOWS' FIFO, driving the text popups the way a
 * plugin does through awl_host. N is a slot of testwins, MON an output's
 * name, X logical pixels from its left edge, TEXT the rest of the line with
 * \n, \t and \\ escapes:
 *
 *   show N MON X TEXT      its text, then :c:func:`popupshow`, creating N first if
 *                          it is empty
 *   toggle N MON X TEXT    the same, but hides it if it is shown on MON
 *   update N TEXT          its text, if it is shown
 *   hide N | destroy N | hideall
 *   input N on|off         N takes the pointer, its presses are recorded
 *   scroll N on|off        N takes the pointer, its scrolling is recorded
 *   persist N on|off       N is persistent (:c:func:`popupsetpersistent`)
 *   lock 1|0               what locking the screen does to the popups (sets
 *                          :c:var:`locked` without a locker: test only)
 *   restart                the plugin_restart action (:c:func:`pluginsreap`
 *                          destroys them all)
 *   nclose ID REASON       the notification server's host calls, as the
 *   ninvoke ID KEY [TOKEN]   library makes them (awl_notifications.h)
 *   nhold ID 0|1
 *   nforget SEQ            drops SEQ from the history, 0 all of it
 *   nshow SEQ              opens history entry SEQ again
 *   nidle                  the user away: the notifications' timeouts stop
 *                          as after :c:member:`awl_config_t.notifications.idle_pause`
 *   nactive                the user back (every command but nidle and dump
 *                          counts as that: they stand in for the user)
 *   nsend URGENCY TIMEOUT LINES WIDTH TAG|- SUMMARY\tBODY
 *                          awl's own (:c:func:`awl_notifications_send`), its look's
 *                          max_lines and width
 *   dump PATH              writes PATH.tmp, then renames it to PATH:
 *                            mon NAME X Y W H WX WY WW WH SCALE ENABLED
 *                            win N MON|- X Y W H VISIBLE HOVERROW PERSISTENT
 *                            cell N ROW COL X Y (a cell's center, visible
 *                              windows' first 8 rows)
 *                            click N ROW COL BUTTON (pressed since the last
 *                              dump, if it takes the pointer)
 *                            scroll N ROW COL ORIENTATION VALUE120 SIGN
 *                              (likewise; SIGN the delta's, -1, 0 or 1)
 *                            pwin I MON|- X Y W H VISIBLE PERSISTENT (the
 *                              plugins')
 *                            pcell I ROW X Y (the center of row ROW of
 *                              visible pwin I, a painter's part; first 8)
 *                            ppart I PART X Y (likewise, every part with
 *                              a box, of the first 1024)
 *                            hover MON X Y (the center of a bar widget
 *                              with callback_hover)
 *                            widget MON I X Y (the center of right-hand
 *                              widget I, the config's widgets' order, if
 *                              it has a width)
 *                            notif ID GENERATION URGENCY TIMEOUT VALUE
 *                              FLAGS NACTIONS IMAGE IMAGE_PATH APP_ICON
 *                              CATEGORY SUMMARY (the open notifications,
 *                              newest first; IMAGE the image data's
 *                              WxHxCHANNELS, an empty string -; the
 *                              first 256, all the server keeps)
 *                            notifs COUNT (how many are open)
 *                            past SEQ ID REASON SUMMARY (the history,
 *                              newest first)
 *                            pasts COUNT (how many it keeps)
 *                            persistent LAYER VISIBLE (:c:func:`popuppersistentlayer`:
 *                              overlay, top or -; whether in sight)
 *                            margin PX (cfg->popup.margin)
 *                            errors COUNT (lines rejected so far)
 */
static void
testwincmd(char *line)
{
    static int errors;
    char cmd[16] = "", mon[64] = "", tmp[PATH_MAX], onoff[4] = "";
    double cx, cy;
    int n = -1, x = 0, off = 0, args = 0, toggle;
    Monitor *m = NULL, *it;
    struct wlr_box box;
    Popup *p;
    awl_bar_t *d;
    unsigned int xpos;
    int bx, by, i, nn;
    unsigned int id;
    char key[64] = "", token[64] = "", img[48];
    static const awl_notif_t *notifs[256];
    static const awl_notif_past_t *past[100];
    FILE *f;

    if (sscanf(line, "%15s%n", cmd, &off) != 1)
        return;
    line += off;
    if (strcmp(cmd, "nidle") && strcmp(cmd, "dump"))
        awl_notifications_activity();
    if (*line == ' ')
        line++;
    toggle = !strcmp(cmd, "toggle");
    if (toggle || !strcmp(cmd, "show")) {
        if (sscanf(line, "%d %63s %d%n", &n, mon, &x, &args) != 3)
            goto bad;
    } else if (!strcmp(cmd, "update") || !strcmp(cmd, "hide") || !strcmp(cmd, "destroy")) {
        if (sscanf(line, "%d%n", &n, &args) != 1)
            goto bad;
    } else if (!strcmp(cmd, "input") || !strcmp(cmd, "scroll") || !strcmp(cmd, "persist")) {
        if (sscanf(line, "%d %3s%n", &n, onoff, &args) != 2
                || (strcmp(onoff, "on") && strcmp(onoff, "off")))
            goto bad;
    }
    if (n != -1 && (n < 0 || n >= (int)LENGTH(testwins)))
        goto bad;
    line += args;
    if (*line == ' ')
        line++;
    testunescape(line);

    if (toggle || !strcmp(cmd, "show")) {
        wl_list_for_each(it, &mons, link)
            if (!strcmp(it->wlr_output->name, mon))
                m = it;
        if (!m)
            goto bad;
        if (!testwins[n])
            testwins[n] = popupnew(0);
        if (toggle && awl_window_monitor(testwins[n]) == m) {
            awl_window_hide(testwins[n]);
        } else {
            awl_window_set_text(testwins[n], line);
            popupshow(testwins[n], m, x);
        }
    } else if (!strcmp(cmd, "update")) {
        if (awl_window_monitor(testwins[n]))
            awl_window_set_text(testwins[n], line);
    } else if (!strcmp(cmd, "hide")) {
        awl_window_hide(testwins[n]);
    } else if (!strcmp(cmd, "destroy")) {
        popupdestroy(testwins[n]);
    } else if (!strcmp(cmd, "input")) {
        if (!testwins[n])
            goto bad;
        awl_window_set_click(testwins[n], strcmp(onoff, "on") ? NULL : testwinclick, (void *)(intptr_t)n);
    } else if (!strcmp(cmd, "scroll")) {
        if (!testwins[n])
            goto bad;
        awl_window_set_scroll(testwins[n], strcmp(onoff, "on") ? NULL : testwinscroll, (void *)(intptr_t)n);
    } else if (!strcmp(cmd, "persist")) {
        if (!testwins[n])
            goto bad;
        popupsetpersistent(testwins[n], !strcmp(onoff, "on"));
    } else if (!strcmp(cmd, "lock")) {
        /* as awl_lockscreen.c does, without a locker */
        if (sscanf(line, "%d", &x) != 1)
            goto bad;
        if (x)
            popupshideall();
        locked = !!x;
        popupslocked();
    } else if (!strcmp(cmd, "hideall")) {
        popupshideall();
    } else if (!strcmp(cmd, "restart")) {
        plugin_restart(NULL);
    } else if (!strcmp(cmd, "nclose") || !strcmp(cmd, "nhold")) {
        if (sscanf(line, "%u %d", &id, &x) != 2)
            goto bad;
        if (!strcmp(cmd, "nclose"))
            awl_notifications_close(id, x);
        else
            awl_notifications_hold(id, x);
    } else if (!strcmp(cmd, "nsend")) {
        awl_notif_send_t s = AWL_NOTIF_SEND_INIT;
        char *body;
        int max_lines, width;
        if (sscanf(line, "%d %d %d %d %63s%n", &x, &s.timeout, &max_lines, &width, key,
                    &args) != 5)
            goto bad;
        /* 0: the config's */
        if (max_lines)
            AWL_SET(s.look.max_lines, max_lines);
        if (width)
            AWL_SET(s.look.width, width);
        s.urgency = x;
        s.tag = strcmp(key, "-") ? key : NULL;
        s.summary = line + args + (line[args] == ' ');
        if ((body = strchr(s.summary, '\t'))) {
            *body++ = 0;
            s.body = body;
        }
        if (!awl_notifications_send(&s))
            goto bad;
    } else if (!strcmp(cmd, "nforget")) {
        unsigned long long seq;
        if (sscanf(line, "%llu", &seq) != 1)
            goto bad;
        awl_notifications_forget(seq);
    } else if (!strcmp(cmd, "nshow")) {
        unsigned long long seq;
        if (sscanf(line, "%llu", &seq) != 1 || !awl_notifications_show(seq))
            goto bad;
    } else if (!strcmp(cmd, "nidle")) {
        awl_notifications_idle();
    } else if (!strcmp(cmd, "nactive")) {
        /* above */
    } else if (!strcmp(cmd, "ninvoke")) {
        if (sscanf(line, "%u %63s %63s", &id, key, token) < 2)
            goto bad;
        awl_notifications_invoke(id, key, *token ? token : NULL);
    } else if (!strcmp(cmd, "dump") && *line) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", line);
        if (!(f = fopen(tmp, "w")))
            goto bad;
        wl_list_for_each(it, &mons, link)
            fprintf(f, "mon %s %d %d %d %d %d %d %d %d %g %d\n", it->wlr_output->name,
                    it->m.x, it->m.y, it->m.width, it->m.height,
                    it->w.x, it->w.y, it->w.width, it->w.height,
                    it->wlr_output->scale, it->wlr_output->enabled);
        for (n = 0; n < (int)LENGTH(testwins); n++) {
            if (!testwins[n])
                continue;
            x = awl_window_box(testwins[n], &box);
            m = awl_window_monitor(testwins[n]);
            fprintf(f, "win %d %s %d %d %d %d %d %d %d\n", n, m ? m->wlr_output->name : "-",
                    box.x, box.y, box.width, box.height, x, awl_window_hover_row(testwins[n]),
                    popuppersistent(testwins[n]));
            for (int r = 0; x && r < 8 && awl_window_cell_center(testwins[n], r, 0, &cx, &cy); r++)
                for (int c = 0; awl_window_cell_center(testwins[n], r, c, &cx, &cy); c++)
                    fprintf(f, "cell %d %d %d %.0f %.0f\n", n, r, c, cx, cy);
        }
        fputs(testclicks, f);
        testclicks[0] = 0;
        n = 0;
        wl_list_for_each(p, &popups, link) {
            for (i = 0; i < (int)LENGTH(testwins) && testwins[i] != p->win; i++);
            if (i < (int)LENGTH(testwins))
                continue;
            x = awl_window_box(p->win, &box);
            m = awl_window_monitor(p->win);
            fprintf(f, "pwin %d %s %d %d %d %d %d %d\n", n, m ? m->wlr_output->name : "-",
                    box.x, box.y, box.width, box.height, x, p->persistent);
            for (int r = 0; x && r < 8 && awl_window_cell_center(p->win, r, 0, &cx, &cy); r++)
                fprintf(f, "pcell %d %d %.0f %.0f\n", n, r, cx, cy);
            for (int r = 0; x && r < 1024; r++)
                if (awl_window_cell_center(p->win, r, 0, &cx, &cy))
                    fprintf(f, "ppart %d %d %.0f %.0f\n", n, r, cx, cy);
            n++;
        }
        /* barwidgetat()'s layout of the right-hand widgets */
        wl_list_for_each(it, &mons, link) {
            d = &it->bar;
            if (!d->m || !it->scene_buffer || !it->wlr_output->enabled)
                continue;
            wlr_scene_node_coords(&it->scene_buffer->node, &bx, &by);
            xpos = d->center_widget_start + d->center_widget_space;
            for (i = d->n_widgets_right - 1; i >= 0; --i) {
                x = it->m.x + (int)((xpos + d->widgets_right[i].width / 2) / it->wlr_output->scale);
                if (d->widgets_right[i].callback_hover && d->widgets_right[i].width)
                    fprintf(f, "hover %s %d %d\n", it->wlr_output->name, x,
                            by + it->bar_area.real_height / 2);
                if (d->widgets_right[i].width)
                    fprintf(f, "widget %s %d %d %d\n", it->wlr_output->name, i, x,
                            by + it->bar_area.real_height / 2);
                xpos += d->widgets_right[i].width;
            }
        }
        nn = awl_notifications_list(notifs, LENGTH(notifs));
        for (i = 0; i < nn && i < (int)LENGTH(notifs); i++) {
            const awl_notif_t *o = notifs[i];
            if (o->image_data.data)
                snprintf(img, sizeof(img), "%dx%dx%d", o->image_data.w, o->image_data.h, o->image_data.channels);
            else
                snprintf(img, sizeof(img), "-");
            fprintf(f, "notif %u %llu %d %d %d %u %d %s %s %s %s %s\n", o->id,
                    (unsigned long long)o->generation, o->urgency, o->timeout, o->value, o->flags,
                    o->nactions, img, *o->image_path ? o->image_path : "-",
                    *o->app_icon ? o->app_icon : "-", *o->category ? o->category : "-", o->summary);
        }
        fprintf(f, "notifs %d\n", nn);
        nn = awl_notifications_history(past, (int)LENGTH(past));
        for (i = 0; i < nn && i < (int)LENGTH(past); i++)
            fprintf(f, "past %llu %u %d %s\n", (unsigned long long)past[i]->seq, past[i]->id,
                    past[i]->reason, past[i]->summary);
        fprintf(f, "pasts %d\n", nn);
        i = popuppersistentlayer(&x);
        fprintf(f, "persistent %s %d\n", i == LyrOverlay ? "overlay" : i == LyrTop ? "top" : "-", x);
        fprintf(f, "margin %d\n", cfg->popup.margin);
        fprintf(f, "errors %d\n", errors);
        fclose(f);
        rename(tmp, line);
    } else {
        goto bad;
    }
    return;
bad:
    errors++;
    awl_dbg_printf(awl_loglevel_err, "AWL_TEST_WINDOWS: bad command: %s %s\n", cmd, line);
}

/** AWL_TEST_WINDOWS' FIFO is readable: runs its complete lines */
static int
testwinctl(int fd, uint32_t mask, void *data)
{
    static char buf[4096];
    static size_t len;
    char *line, *nl;
    ssize_t r;

    while ((r = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += (size_t)r;
        buf[len] = 0;
        for (line = buf; (nl = strchr(line, '\n')); line = nl + 1) {
            *nl = 0;
            testwincmd(line);
        }
        len -= (size_t)(line - buf);
        memmove(buf, line, len);
        if (len == sizeof(buf) - 1) /* an overlong line */
            len = 0;
    }
    return 0;
}

void
testinit(void)
{
    /* for testing (test/live.sh): SIGUSR1 adds an output, SIGUSR2 removes
     * the newest one; headless and nested (wayland) backends only */
    if (getenv("AWL_TEST_OUTPUTS")) {
        wl_event_loop_add_signal(event_loop, SIGUSR1, testoutputadd, NULL);
        wl_event_loop_add_signal(event_loop, SIGUSR2, testoutputremove, NULL);
    }
    /* for testing (test/live.sh's win, test/windows.sh): a FIFO of commands
     * for the text popups, see testwincmd(); O_RDWR, so it never reads EOF
     * while no writer has it open */
    if (getenv("AWL_TEST_WINDOWS")) {
        int fd = open(getenv("AWL_TEST_WINDOWS"), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            die("AWL_TEST_WINDOWS %s:", getenv("AWL_TEST_WINDOWS"));
        wl_event_loop_add_fd(event_loop, fd, WL_EVENT_READABLE, testwinctl, NULL);
    }
}

void
testpopupdestroyed(awl_window_t *win)
{
    for (size_t i = 0; i < LENGTH(testwins); i++)
        if (testwins[i] == win)
            testwins[i] = NULL;
}
