/* The tray's icon theme for programs awl starts that ask for it
 * (awl_tray_app_env()): a dconf profile of their own, the one they would
 * have had otherwise plus a database that sets and locks
 * org.gnome.desktop.interface's icon-theme. A lock in a later database wins
 * over the values of the earlier ones, the user's included, so for them
 * that one key is the tray's and everything else stays as it is; the
 * desktop's setting isn't touched. GTK reads it from there (on Wayland,
 * without GTK_USE_PORTAL=1); a theme it lacks an icon in falls back as its
 * index.theme says, then to hicolor.
 *
 * Nothing goes to a file system: the profile and the database are sealed
 * in-memory files (memfd), close-on-exec in awl, which the program inherits
 * and opens as /proc/self/fd/N; they are gone once the last one holding
 * them exits. libawltray-env.so, next to awl and preloaded into the
 * program, keeps it from handing them, and the variables, on to its
 * children. libc only; main thread only. */

#define _GNU_SOURCE /* memfd_create() */
#include "awl_tray.h"
#include "../awl_log.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define KEY "/org/gnome/desktop/interface/icon-theme"
#define SHIM "libawltray-env.so"
#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static struct {
    /** the config's icon_theme, NULL: none */
    char *theme;
    /** what :c:func:`awl_tray_app_env` made, for theme made_for (NULL: nothing yet);
     * env.env NULL: nothing to give */
    char *made_for;
    awl_tray_app_env_t env;
    char *vars[5];
} E = { .env.fds = { -1, -1 } };

/** a malloc()ed printf; aborts awl like GLib did if there is no memory */
static char *
fmt(const char *f, ...)
{
    va_list ap;
    char *s;
    va_start(ap, f);
    int n = vasprintf(&s, f, ap);
    va_end(ap);
    if (n < 0) {
        awl_err_printf("tray: out of memory\n");
        abort();
    }
    return s;
}

static char *
copy(const char *s)
{
    return fmt("%s", s);
}

void
awl_tray_app_set_theme(const char *theme)
{
    free(E.theme);
    E.theme = theme && *theme ? copy(theme) : NULL;
}

/** forgets what :c:func:`awl_tray_app_env` made */
static void
drop(void)
{
    for (int i = 0; i < 2; i++) {
        if (E.env.fds[i] >= 0) close(E.env.fds[i]);
        E.env.fds[i] = -1;
    }
    for (size_t i = 0; i < LENGTH(E.vars); i++) {
        free(E.vars[i]);
        E.vars[i] = NULL;
    }
    E.env.env = NULL;
    free(E.made_for);
    E.made_for = NULL;
}

/** XDG_DATA_DIRS, or its default, as GLib has it: each in turn, while
 * each() returns 0 */
static int
each_data_dir(int (*each)(const char *dir, void *data), void *data)
{
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share/:/usr/share/";
    char *list = copy(dirs), *save = NULL;
    int r = 0;
    for (char *d = strtok_r(list, ":", &save); d && !r; d = strtok_r(NULL, ":", &save))
        r = each(d, data);
    free(list);
    return r;
}

/** XDG_DATA_HOME, or ~/.local/share; malloc()ed */
static char *
user_data_dir(void)
{
    const char *d = getenv("XDG_DATA_HOME");
    if (d && *d) return copy(d);
    const char *home = getenv("HOME");
    return fmt("%s/.local/share", home ? home : "");
}

static int
exists(const char *path)
{
    return !access(path, F_OK);
}

static int
theme_in(const char *dir, void *name)
{
    char *index = fmt("%s/icons/%s/index.theme", dir, (const char *)name);
    int found = exists(index);
    free(index);
    return found;
}

/** whether an icon theme name is installed where GTK looks for one */
static int
installed(const char *name)
{
    char *user = user_data_dir();
    const char *home = getenv("HOME");
    char *dot = fmt("%s/.icons/%s/index.theme", home ? home : "", name);
    int found = theme_in(user, (void *)name) || exists(dot)
            || each_data_dir(theme_in, (void *)name);
    free(dot);
    free(user);
    return found;
}

/** a file's contents, NUL terminated; NULL if it can't be read */
static char *
slurp(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    char *text = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    if (out) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f))) fwrite(buf, 1, n, out);
        fclose(out);
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        free(text);
        return NULL;
    }
    return text;
}

typedef struct {
    const char *name;
    char *text;
} profile_find_t;

static int
profile_in(const char *dir, void *data)
{
    profile_find_t *p = data;
    char *path = fmt("%s/dconf/profile/%s", dir, p->name);
    p->text = slurp(path);
    free(path);
    return !!p->text;
}

/** the dconf profile awl's programs get without us, as dconf finds it:
 * DCONF_PROFILE (a path, or a name), else "user", in /etc/dconf/profile
 * then the data dirs' dconf/profile; just the user's database if there is
 * none */
static char *
base_profile(void)
{
    const char *name = getenv("DCONF_PROFILE");
    char *text = NULL;
    if (name && name[0] == '/') {
        text = slurp(name);
    } else {
        if (!name || !*name) name = "user";
        char *p = fmt("/etc/dconf/profile/%s", name);
        text = slurp(p);
        free(p);
        if (!text) {
            profile_find_t find = { name, NULL };
            each_data_dir(profile_in, &find);
            text = find.text;
        }
    }
    if (!text) return copy("user-db:user\n");
    size_t n = strlen(text);
    if (n && text[n - 1] != '\n') {
        char *t = fmt("%s\n", text);
        free(text);
        text = t;
    }
    return text;
}

/* ---- the database: GLib's gvdb format (gvdb-format.h), as `dconf compile`
 * writes it, for one key and its lock ---- */

typedef struct {
    uint32_t hash, parent, key_start;
    uint16_t key_size;
    char type, unused;
    uint32_t start, end;
} gvdb_item_t;

typedef struct {
    uint8_t *data;
    size_t len, cap;
} bytes_t;

static void
grow(bytes_t *b, size_t n)
{
    if (b->len + n <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    uint8_t *data = realloc(b->data, cap);
    if (!data) {
        awl_err_printf("tray: out of memory\n");
        abort();
    }
    b->data = data;
    b->cap = cap;
}

/** gvdb's key hash (djb) */
static uint32_t
gvdb_hash(const char *key)
{
    uint32_t h = 5381;
    for (; *key; key++) h = h * 33 + (uint32_t)(signed char)*key;
    return h;
}

static void
put32(bytes_t *b, size_t at, uint32_t v)
{
    v = htole32(v);
    memcpy(b->data + at, &v, 4);
}

static size_t
append(bytes_t *b, const void *data, size_t n, size_t align)
{
    size_t pad = (align - b->len % align) % align;
    grow(b, pad + n);
    memset(b->data + b->len, 0, pad);
    b->len += pad;
    size_t at = b->len;
    if (n) memcpy(b->data + at, data, n);
    b->len += n;
    return at;
}

/** a hash table of n items at its place, all in one bucket and without a
 * bloom filter: returns where it starts, ``*end`` where it ends */
static size_t
table(bytes_t *b, int n, size_t *end)
{
    /* no bloom words, 1 bucket starting at item 0 */
    const uint32_t header[3] = { 0, htole32(1), 0 };
    size_t at = append(b, header, sizeof header, 4);
    gvdb_item_t blank = { 0 };
    for (int i = 0; i < n; i++) append(b, &blank, sizeof blank, 1);
    *end = b->len;
    return at;
}

static void
item(bytes_t *b, size_t table_at, int i, const char *key, size_t key_at, char type,
        size_t start, size_t end)
{
    size_t at = table_at + 12 + (size_t)i * sizeof(gvdb_item_t);
    put32(b, at, gvdb_hash(key));
    put32(b, at + 4, 0xffffffffu); /* no parent: the whole key is its own */
    put32(b, at + 8, (uint32_t)key_at);
    uint16_t size = htole16((uint16_t)strlen(key));
    memcpy(b->data + at + 12, &size, 2);
    b->data[at + 14] = (uint8_t)type;
    put32(b, at + 16, (uint32_t)start);
    put32(b, at + 20, (uint32_t)end);
}

/** the string s as a GVariant variant ("v" holding an "s"), serialized: the
 * string and its NUL, then a NUL and the child's type */
static size_t
value(bytes_t *b, const char *s, size_t *end)
{
    size_t n = strlen(s);
    size_t at = append(b, s, n + 1, 8);
    append(b, "\0s", 2, 1);
    *end = b->len;
    return at;
}

/** KEY set to theme, and locked: the root table holds it and .locks, a
 * table with the lock */
static bytes_t
database(const char *theme)
{
    bytes_t b = { 0 };
    /* "GVariant", version 0 */
    const uint32_t header[6] = { htole32(0x72615647), htole32(0x746e6169) };
    append(&b, header, sizeof header, 1);
    size_t root_end, locks_end, theme_end, lock_end;
    size_t root = table(&b, 2, &root_end);
    size_t locks = table(&b, 1, &locks_end);
    size_t locks_key = append(&b, ".locks", 6, 1);
    size_t key = append(&b, KEY, strlen(KEY), 1);
    size_t theme_at = value(&b, theme, &theme_end);
    size_t lock_at = value(&b, "", &lock_end);
    put32(&b, 16, (uint32_t)root);
    put32(&b, 20, (uint32_t)root_end);
    item(&b, root, 0, ".locks", locks_key, 'H', locks, locks_end);
    item(&b, root, 1, KEY, key, 'v', theme_at, theme_end);
    item(&b, locks, 0, KEY, key, 'v', lock_at, lock_end);
    return b;
}

/** a sealed in-memory file named name holding data, close-on-exec; -1 on
 * failure */
static int
memfile(const char *name, const void *data, size_t n)
{
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        awl_err_printf("tray: memfd_create: %s\n", strerror(errno));
        return -1;
    }
    const uint8_t *p = data;
    for (size_t left = n; left;) {
        ssize_t w = write(fd, p + (n - left), left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            awl_err_printf("tray: writing %s: %s\n", name, strerror(errno));
            close(fd);
            return -1;
        }
        left -= (size_t)w;
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
}

/** the shim, next to awl's executable; NULL if it isn't there */
static char *
shim(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return NULL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = '\0';
    char *path = fmt("%s/" SHIM, exe);
    if (access(path, R_OK)) {
        free(path);
        return NULL;
    }
    return path;
}

/** the profile, the database and the variables for E.theme */
static int
make(void)
{
    char *lib = shim();
    if (!lib) {
        awl_wrn_printf("tray: no " SHIM " next to awl, programs keep the desktop's icon theme\n");
        return 0;
    }
    bytes_t db = database(E.theme);
    E.env.fds[1] = memfile("awl-tray-dconf-db", db.data, db.len);
    free(db.data);
    if (E.env.fds[1] >= 0) {
        char *base = base_profile();
        char *text = fmt("%sfile-db:/proc/self/fd/%d\n", base, E.env.fds[1]);
        E.env.fds[0] = memfile("awl-tray-dconf-profile", text, strlen(text));
        free(text);
        free(base);
    }
    if (E.env.fds[0] < 0 || E.env.fds[1] < 0) {
        free(lib);
        return 0;
    }
    /* ours first, then what awl was started with */
    const char *preload = getenv("LD_PRELOAD");
    /* awl's own, for the shim to put back */
    const char *was = getenv("DCONF_PROFILE");
    E.vars[0] = fmt("DCONF_PROFILE=/proc/self/fd/%d", E.env.fds[0]);
    E.vars[1] = fmt("AWL_TRAY_ENV_FDS=%d,%d", E.env.fds[0], E.env.fds[1]);
    E.vars[2] = preload && *preload ? fmt("LD_PRELOAD=%s:%s", lib, preload)
                                    : fmt("LD_PRELOAD=%s", lib);
    if (was) E.vars[3] = fmt("AWL_TRAY_DCONF_PROFILE=%s", was);
    E.env.env = (const char *const *)E.vars;
    free(lib);
    return 1;
}

const awl_tray_app_env_t *
awl_tray_app_env(void)
{
    if (!E.theme) return NULL;
    if (E.made_for && !strcmp(E.made_for, E.theme)) return E.env.env ? &E.env : NULL;
    drop();
    E.made_for = copy(E.theme);
    if (!installed(E.theme)) {
        awl_wrn_printf("tray: icon theme %s isn't installed, programs keep the desktop's\n", E.theme);
        return NULL;
    }
    if (!make()) {
        drop();
        E.made_for = copy(E.theme);
        return NULL;
    }
    awl_log_printf("tray: icon theme %s for the programs that ask\n", E.theme);
    return &E.env;
}

void
awl_tray_app_cleanup(void)
{
    drop();
    free(E.theme);
    E.theme = NULL;
}
