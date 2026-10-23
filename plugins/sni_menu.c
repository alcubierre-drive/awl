/* The items' menus over sd-bus, see sni_menu.h. */

#include "sni_menu.h"
#include "../awl_host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_IFACE "com.canonical.dbusmenu"
/** the size icons are looked up at: any will do (the main thread fits them
 * to the menu's font), it only picks among a theme's fixed sizes */
#define LOOKUP_PX 32
/** a layout deeper or larger than this is cut there: tray_menu_window.c shows 8
 * levels, and no app has thousands of entries */
#define MAX_DEPTH 8
#define MAX_NODES 4096
/** IconThemePath entries kept */
#define MAX_PATHS 8

struct sni_menu {
    uint64_t serial;
    sd_bus* bus;
    char *bus_name, *path, *theme_path;
    /** the menu's own IconThemePath, NULL terminated */
    char* paths[MAX_PATHS + 1];
    sd_bus_slot* match;
    /** the main thread shows it ("opened" of id 0 until "closed") */
    int open;
    /** the request the snapshots answer */
    uint32_t gen;
    /** a GetLayout is out; again: one more once it is back */
    int fetching, again;
    sni_menu_t* next;
};

static sni_menu_t* menus;
static uint64_t next_serial = 1;

/** async calls get the serial, not the menu: it may be gone by then */
#define SERIAL_DATA( s ) ((void*)(uintptr_t)(s))
#define DATA_SERIAL( p ) ((uint64_t)(uintptr_t)(p))

sni_menu_t* sni_menu_find( uint64_t serial ) {
    for (sni_menu_t* m = menus; m; m = m->next)
        if (m->serial == serial) return m;
    return NULL;
}

uint64_t sni_menu_serial( const sni_menu_t* menu ) {
    return menu->serial;
}

const char* sni_menu_path( const sni_menu_t* menu ) {
    return menu->path;
}

/* ---- snapshots ---- */

void awl_tray_menu_snap_free( awl_tray_menu_snap_t* snap ) {
    if (!snap) return;
    for (int i = 0; i < snap->nnodes; i++) {
        free( snap->nodes[i].label );
        free( snap->nodes[i].shortcut );
        free( snap->nodes[i].icon );
    }
    free( snap->nodes );
    free( snap );
}

/** an entry while the layout is parsed */
typedef struct tnode {
    awl_tray_menu_node_t n;
    struct tnode** kids;
    int nkids, cap;
} tnode;

static void tnode_free( tnode* t ) {
    if (!t) return;
    free( t->n.label );
    free( t->n.shortcut );
    free( t->n.icon );
    for (int i = 0; i < t->nkids; i++) tnode_free( t->kids[i] );
    free( t->kids );
    free( t );
}

static int tnode_add( tnode* t, tnode* k ) {
    if (t->nkids == t->cap) {
        int cap = t->cap ? 2 * t->cap : 8;
        tnode** kids = realloc( t->kids, (size_t)cap * sizeof(*kids) );
        if (!kids) return -1;
        t->kids = kids;
        t->cap = cap;
    }
    t->kids[t->nkids++] = k;
    return 0;
}

/** the label without mnemonics ("_File" is "File", "__" an underscore), on
 * one line */
static char* plain_label( const char* s ) {
    char* out = malloc( strlen( s ) + 1 );
    if (!out) return NULL;
    char* o = out;
    for (; *s; s++) {
        if (*s == '_') {
            if (s[1] == '_') *o++ = *s++;
            continue;
        }
        *o++ = (*s == '\n' || *s == '\t' || *s == '\r') ? ' ' : *s;
    }
    *o = 0;
    return out;
}

/** the first of the shortcuts (aas, the variant entered), as "Ctrl+Shift+Q" */
static char* shortcut_text( sd_bus_message* m ) {
    char buf[128] = "";
    size_t len = 0;
    if (sd_bus_message_enter_container( m, 'a', "as" ) <= 0) return NULL;
    if (sd_bus_message_enter_container( m, 'a', "s" ) > 0) {
        const char* k;
        while (sd_bus_message_read( m, "s", &k ) > 0) {
            char key[64];
            if (!strcmp( k, "Control" )) snprintf( key, sizeof key, "Ctrl" );
            /* a letter as GTK shows it: "q" is Q */
            else if (k[0] && !k[1]) snprintf( key, sizeof key, "%c", toupper( (unsigned char)k[0] ) );
            else snprintf( key, sizeof key, "%s", k );
            int r = snprintf( buf + len, sizeof buf - len, "%s%s", len ? "+" : "", key );
            if (r > 0 && (size_t)r < sizeof buf - len) len += (size_t)r;
        }
        sd_bus_message_exit_container( m );
    }
    /* the other shortcuts: skipped */
    while (sd_bus_message_skip( m, "as" ) > 0)
        ;
    sd_bus_message_exit_container( m );
    return len ? strdup( buf ) : NULL;
}

/** where one layout's icons are looked up, and what they found */
typedef struct {
    awl_icons_t* themes;
    /** the item's and the menu's IconThemePath, NULL terminated */
    const char* extra[MAX_PATHS + 2];
    /** name -> its file's contents (NULL: not found), as looked up */
    struct {
        char* name;
        unsigned char* data;
        size_t len;
        uint32_t flags;
    }* files;
    int nfiles, cap;
} icons_t;

static void icons_init( icons_t* ic, sni_menu_t* menu ) {
    memset( ic, 0, sizeof *ic );
    ic->themes = sni_themes();
    int n = 0;
    if (menu->theme_path && *menu->theme_path) ic->extra[n++] = menu->theme_path;
    for (char** p = menu->paths; *p; p++)
        if (**p) ic->extra[n++] = *p;
    ic->extra[n] = NULL;
}

static void icons_fini( icons_t* ic ) {
    for (int i = 0; i < ic->nfiles; i++) {
        free( ic->files[i].name );
        free( ic->files[i].data );
    }
    free( ic->files );
}

/** a copy of the file of icon name (not larger than AWL_ICON_MAX_BYTES): a
 * path as it is, else from the item's folders, then the tray's themes; NULL
 * if there's none. ``*flags``: :c:enumerator:`AWL_TRAY_MENU_SYMBOLIC` and :c:enumerator:`AWL_TRAY_MENU_FILE`
 * as they apply. */
static unsigned char* icon_file( icons_t* ic, const char* name, size_t* len, uint32_t* flags ) {
    int i = 0;
    for (; i < ic->nfiles && strcmp( ic->files[i].name, name ); i++)
        ;
    if (i == ic->nfiles) {
        if (ic->nfiles == ic->cap) {
            int cap = ic->cap ? 2 * ic->cap : 8;
            void* f = realloc( ic->files, (size_t)cap * sizeof(*ic->files) );
            if (!f) return NULL;
            ic->files = f;
            ic->cap = cap;
        }
        unsigned f = 0;
        char* path = ic->themes ? awl_icons_lookup( ic->themes, name, LOOKUP_PX, ic->extra, &f ) : NULL;
        size_t n = 0;
        void* data = path ? awl_icon_read( path, &n ) : NULL;
        free( path );
        if (!data) awl_vrb_printf( "sni: menu icon '%s' not found\n", name );
        ic->files[i].name = strdup( name );
        ic->files[i].data = data;
        ic->files[i].len = n;
        ic->files[i].flags = data ? AWL_TRAY_MENU_FILE | (f & AWL_ICON_SYMBOLIC ? AWL_TRAY_MENU_SYMBOLIC : 0) : 0;
        if (!ic->files[i].name) {
            free( data );
            return NULL;
        }
        ic->nfiles++;
    }
    if (!ic->files[i].data) return NULL;
    unsigned char* copy = malloc( ic->files[i].len );
    if (!copy) return NULL;
    memcpy( copy, ic->files[i].data, ic->files[i].len );
    *len = ic->files[i].len;
    *flags = ic->files[i].flags;
    return copy;
}

/** whether the variant m is at holds sig; it's skipped if not */
static int variant_is( sd_bus_message* m, const char* sig ) {
    const char* contents = NULL;
    if (sd_bus_message_peek_type( m, NULL, &contents ) > 0 && contents && !strcmp( contents, sig )) return 1;
    sd_bus_message_skip( m, "v" );
    return 0;
}

/** The entry m is at, (ia{sv}av), its children too; NULL if hidden or
 * broken (``*err`` then set), or past ``*budget`` entries. */
static tnode* parse( sd_bus_message* m, icons_t* ic, int depth, int* budget, int* err ) {
    int32_t id = 0;
    int visible = 1, enabled = 1, state = -1;
    const char *type = NULL, *label = "", *icon_name = NULL, *toggle = NULL, *display = NULL;
    const void* data = NULL;
    size_t data_len = 0;
    char* shortcut = NULL;
    tnode* t = NULL;

    if (sd_bus_message_enter_container( m, 'r', "ia{sv}av" ) <= 0 || sd_bus_message_read( m, "i", &id ) < 0 ||
        sd_bus_message_enter_container( m, 'a', "{sv}" ) <= 0)
        goto broken;
    while (sd_bus_message_enter_container( m, 'e', "sv" ) > 0) {
        const char* key;
        int r = sd_bus_message_read( m, "s", &key );
        if (r < 0) goto broken;
        if (!strcmp( key, "visible" ) && variant_is( m, "b" )) r = sd_bus_message_read( m, "v", "b", &visible );
        else if (!strcmp( key, "enabled" ) && variant_is( m, "b" )) r = sd_bus_message_read( m, "v", "b", &enabled );
        else if (!strcmp( key, "type" ) && variant_is( m, "s" )) r = sd_bus_message_read( m, "v", "s", &type );
        else if (!strcmp( key, "label" ) && variant_is( m, "s" )) r = sd_bus_message_read( m, "v", "s", &label );
        else if (!strcmp( key, "icon-name" ) && variant_is( m, "s" ))
            r = sd_bus_message_read( m, "v", "s", &icon_name );
        else if (!strcmp( key, "toggle-type" ) && variant_is( m, "s" ))
            r = sd_bus_message_read( m, "v", "s", &toggle );
        else if (!strcmp( key, "toggle-state" ) && variant_is( m, "i" ))
            r = sd_bus_message_read( m, "v", "i", &state );
        else if (!strcmp( key, "children-display" ) && variant_is( m, "s" ))
            r = sd_bus_message_read( m, "v", "s", &display );
        else if (!strcmp( key, "icon-data" ) && variant_is( m, "ay" )) {
            if ((r = sd_bus_message_enter_container( m, 'v', "ay" )) > 0 &&
                (r = sd_bus_message_read_array( m, 'y', &data, &data_len )) >= 0)
                r = sd_bus_message_exit_container( m );
        } else if (!strcmp( key, "shortcut" ) && variant_is( m, "aas" )) {
            if ((r = sd_bus_message_enter_container( m, 'v', "aas" )) > 0) {
                free( shortcut );
                shortcut = shortcut_text( m );
                r = sd_bus_message_exit_container( m );
            }
        } else if (!strcmp( key, "visible" ) || !strcmp( key, "enabled" ) || !strcmp( key, "type" ) ||
                   !strcmp( key, "label" ) || !strcmp( key, "icon-name" ) || !strcmp( key, "toggle-type" ) ||
                   !strcmp( key, "toggle-state" ) || !strcmp( key, "children-display" ) ||
                   !strcmp( key, "icon-data" ) || !strcmp( key, "shortcut" ))
            ; /* of another type: variant_is() skipped it */
        else
            r = sd_bus_message_skip( m, "v" );
        if (r < 0 || sd_bus_message_exit_container( m ) < 0) goto broken;
    }
    if (sd_bus_message_exit_container( m ) < 0) goto broken;

    if (visible && --*budget >= 0) {
        if (!(t = calloc( 1, sizeof(*t) )) || !(t->n.label = plain_label( label ))) goto broken;
        t->n.id = id;
        if (type && !strcmp( type, "separator" )) t->n.flags |= AWL_TRAY_MENU_SEPARATOR;
        if (!enabled) t->n.flags |= AWL_TRAY_MENU_DISABLED;
        if (toggle && (!strcmp( toggle, "checkmark" ) || !strcmp( toggle, "radio" ))) {
            t->n.flags |= !strcmp( toggle, "radio" ) ? AWL_TRAY_MENU_RADIO : AWL_TRAY_MENU_CHECK;
            if (state == 1) t->n.flags |= AWL_TRAY_MENU_CHECKED;
            else if (state != 0) t->n.flags |= AWL_TRAY_MENU_MIXED;
        }
        if (display && !strcmp( display, "submenu" )) t->n.flags |= AWL_TRAY_MENU_SUBMENU;
        t->n.shortcut = shortcut;
        shortcut = NULL;
        /* icon-data, a PNG, comes first, as libdbusmenu has it */
        if (data && data_len && data_len <= AWL_ICON_MAX_BYTES && (t->n.icon = malloc( data_len ))) {
            memcpy( t->n.icon, data, data_len );
            t->n.icon_len = data_len;
        }
        if (!t->n.icon && icon_name && *icon_name) {
            uint32_t flags = 0;
            if ((t->n.icon = icon_file( ic, icon_name, &t->n.icon_len, &flags ))) t->n.flags |= flags;
        }
    }

    /* the children, separators collapsed; all of them read, even of a
     * hidden entry */
    if (sd_bus_message_enter_container( m, 'a', "v" ) <= 0) goto broken;
    for (;;) {
        const char* contents = NULL;
        int r = sd_bus_message_peek_type( m, NULL, &contents );
        if (r < 0) goto broken;
        if (!r) break;
        if (!contents || strcmp( contents, "(ia{sv}av)" ) || depth >= MAX_DEPTH) {
            if (sd_bus_message_skip( m, "v" ) < 0) goto broken;
            continue;
        }
        if (sd_bus_message_enter_container( m, 'v', "(ia{sv}av)" ) <= 0) goto broken;
        tnode* k = parse( m, ic, depth + 1, budget, err );
        if (*err || sd_bus_message_exit_container( m ) < 0) {
            tnode_free( k );
            goto broken;
        }
        if (!k || !t) {
            tnode_free( k );
            continue;
        }
        int sep = !!(k->n.flags & AWL_TRAY_MENU_SEPARATOR);
        tnode* last = t->nkids ? t->kids[t->nkids - 1] : NULL;
        if ((sep && (!last || (last->n.flags & AWL_TRAY_MENU_SEPARATOR))) || tnode_add( t, k )) tnode_free( k );
    }
    if (sd_bus_message_exit_container( m ) < 0 || sd_bus_message_exit_container( m ) < 0) goto broken;
    if (t && t->nkids && (t->kids[t->nkids - 1]->n.flags & AWL_TRAY_MENU_SEPARATOR))
        tnode_free( t->kids[--t->nkids] );
    /* children-display alone too: Qt's and Electron's submenus are empty
     * until their AboutToShow */
    if (t && t->nkids) t->n.flags |= AWL_TRAY_MENU_SUBMENU;
    return t;
broken:
    *err = 1;
    free( shortcut );
    tnode_free( t );
    return NULL;
}

/** the tree as nodes, breadth first: each one's children side by side */
static awl_tray_menu_snap_t* flatten( tnode* root, int count ) {
    tnode** order = malloc( (size_t)count * sizeof(*order) );
    awl_tray_menu_snap_t* snap = calloc( 1, sizeof(*snap) );
    if (!order || !snap || !(snap->nodes = calloc( (size_t)count, sizeof(*snap->nodes) ))) {
        free( order );
        free( snap );
        return NULL;
    }
    int n = 0;
    order[n++] = root;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < order[i]->nkids && n < count; k++) order[n++] = order[i]->kids[k];
    snap->nnodes = n;
    int next = 1;
    for (int i = 0; i < n; i++) {
        tnode* t = order[i];
        awl_tray_menu_node_t* node = &snap->nodes[i];
        *node = t->n;
        /* the snapshot owns them now */
        t->n.label = t->n.shortcut = NULL;
        t->n.icon = NULL;
        node->first = next;
        node->n = t->nkids;
        if (i == 0) node->parent = -1;
        for (int k = 0; k < node->n; k++) snap->nodes[next + k].parent = i;
        next += node->n;
    }
    free( order );
    return snap;
}

/** the entries parse() made of tree t */
static int count( const tnode* t ) {
    int n = 1;
    for (int i = 0; i < t->nkids; i++) n += count( t->kids[i] );
    return n;
}

/* ---- D-Bus ---- */

static void fetch( sni_menu_t* menu );

/** GetLayout -> (u revision, (ia{sv}av) layout) */
static int layout_done( sd_bus_message* m, void* data, sd_bus_error* ret_error ) {
    (void)ret_error;
    sni_menu_t* menu = sni_menu_find( DATA_SERIAL( data ) );
    if (!menu) return 0;
    menu->fetching = 0;
    const sd_bus_error* e = sd_bus_message_get_error( m );
    uint32_t revision = 0;
    if (e) {
        awl_wrn_printf( "sni: menu %s%s: GetLayout: %s\n", menu->bus_name, menu->path,
                        e->message ? e->message : e->name );
    } else if (sd_bus_message_read( m, "u", &revision ) >= 0) {
        icons_t ic;
        icons_init( &ic, menu );
        int err = 0, budget = MAX_NODES;
        tnode* root = parse( m, &ic, 0, &budget, &err );
        icons_fini( &ic );
        if (err) awl_wrn_printf( "sni: menu %s%s: a broken layout\n", menu->bus_name, menu->path );
        awl_tray_menu_snap_t* snap = root ? flatten( root, count( root ) ) : NULL;
        tnode_free( root );
        if (snap) {
            snap->serial = menu->serial;
            snap->gen = menu->gen;
            snap->revision = revision;
            sni_to_main_snap( snap );
        }
    }
    if (menu->again) {
        menu->again = 0;
        fetch( menu );
    }
    return 0;
}

/** GetLayout of everything, its snapshot to the main thread */
static void fetch( sni_menu_t* menu ) {
    if (menu->fetching) {
        menu->again = 1;
        return;
    }
    sd_bus_message* call = NULL;
    if (sd_bus_message_new_method_call( menu->bus, &call, menu->bus_name, menu->path, MENU_IFACE, "GetLayout" ) < 0 ||
        sd_bus_message_append( call, "ii", 0, -1 ) < 0 || sd_bus_message_append_strv( call, (char*[]){ NULL } ) < 0 ||
        sd_bus_call_async( menu->bus, NULL, call, layout_done, SERIAL_DATA( menu->serial ), 0 ) < 0)
        awl_wrn_printf( "sni: menu %s%s: can't ask for its layout\n", menu->bus_name, menu->path );
    else
        menu->fetching = 1;
    sd_bus_message_unref( call );
}

/** Some menus (Qt's, Electron's) fill themselves in only when asked
 * whether they need to: with update, fetch the layout once they answered,
 * whatever they say (an error included; the layout itself may still be
 * fine); else only if they say it changed. The serial in the upper bits,
 * update in the lowest. */
static int about_to_show_done( sd_bus_message* m, void* data, sd_bus_error* ret_error ) {
    (void)ret_error;
    uint64_t v = DATA_SERIAL( data );
    sni_menu_t* menu = sni_menu_find( v >> 1 );
    int need = 0;
    int ok = !sd_bus_message_is_method_error( m, NULL ) && sd_bus_message_read( m, "b", &need ) >= 0;
    if (menu && ((v & 1) || (ok && need))) fetch( menu );
    return 0;
}

static void about_to_show( sni_menu_t* menu, int32_t id, int update ) {
    if (sd_bus_call_method_async( menu->bus, NULL, menu->bus_name, menu->path, MENU_IFACE, "AboutToShow",
                                  about_to_show_done, SERIAL_DATA( menu->serial << 1 | !!update ), "i", id ) < 0 &&
        update)
        fetch( menu );
}

/** LayoutUpdated, ItemsPropertiesUpdated: a new layout, if it's shown */
static int on_signal( sd_bus_message* m, void* data, sd_bus_error* ret_error ) {
    (void)ret_error;
    sni_menu_t* menu = sni_menu_find( DATA_SERIAL( data ) );
    if (menu && menu->open &&
        (sd_bus_message_is_signal( m, MENU_IFACE, "LayoutUpdated" ) ||
         sd_bus_message_is_signal( m, MENU_IFACE, "ItemsPropertiesUpdated" )))
        fetch( menu );
    return 0;
}

/** Get( IconThemePath ) -> v(as) */
static int paths_done( sd_bus_message* m, void* data, sd_bus_error* ret_error ) {
    (void)ret_error;
    sni_menu_t* menu = sni_menu_find( DATA_SERIAL( data ) );
    if (!menu || sd_bus_message_is_method_error( m, NULL ) ||
        sd_bus_message_enter_container( m, 'v', "as" ) <= 0 || sd_bus_message_enter_container( m, 'a', "s" ) <= 0)
        return 0;
    const char* p;
    for (int n = 0; n < MAX_PATHS && sd_bus_message_read( m, "s", &p ) > 0;) {
        if (*p != '/' || !(menu->paths[n] = strdup( p ))) continue;
        n++;
    }
    return 0;
}

/* ---- the menus ---- */

sni_menu_t* sni_menu_new( sd_bus* bus, const char* bus_name, const char* path, const char* theme_path ) {
    sni_menu_t* menu = calloc( 1, sizeof(*menu) );
    if (!menu) return NULL;
    menu->serial = next_serial++;
    menu->bus = bus;
    menu->bus_name = strdup( bus_name );
    menu->path = strdup( path );
    menu->theme_path = strdup( theme_path ? theme_path : "" );
    if (!menu->bus_name || !menu->path || !menu->theme_path) {
        free( menu->bus_name );
        free( menu->path );
        free( menu->theme_path );
        free( menu );
        return NULL;
    }
    char rule[1024];
    snprintf( rule, sizeof rule, "type='signal',sender='%s',path='%s',interface='" MENU_IFACE "'", bus_name, path );
    sd_bus_add_match_async( bus, &menu->match, rule, on_signal, NULL, SERIAL_DATA( menu->serial ) );
    sd_bus_call_method_async( bus, NULL, bus_name, path, "org.freedesktop.DBus.Properties", "Get", paths_done,
                              SERIAL_DATA( menu->serial ), "ss", MENU_IFACE, "IconThemePath" );
    menu->next = menus;
    menus = menu;
    return menu;
}

void sni_menu_free( sni_menu_t* menu ) {
    if (!menu) return;
    for (sni_menu_t** p = &menus; *p; p = &(*p)->next)
        if (*p == menu) {
            *p = menu->next;
            break;
        }
    /* the main thread may show it, or wait for it */
    sni_to_main_gone( menu->serial );
    sd_bus_slot_unref( menu->match );
    for (char** p = menu->paths; *p; p++) free( *p );
    free( menu->bus_name );
    free( menu->path );
    free( menu->theme_path );
    free( menu );
}

void sni_menu_set_theme_path( sni_menu_t* menu, const char* theme_path ) {
    char* p = strdup( theme_path ? theme_path : "" );
    if (!p) return;
    free( menu->theme_path );
    menu->theme_path = p;
}

void sni_menu_request( sni_menu_t* menu, uint32_t gen ) {
    menu->gen = gen;
    about_to_show( menu, 0, 1 );
}

void sni_menu_event( sni_menu_t* menu, int32_t id, const char* event ) {
    if (id == 0 && !strcmp( event, "opened" )) menu->open = 1;
    if (id == 0 && !strcmp( event, "closed" )) menu->open = 0;
    /* Event( i id, s event, v data, u timestamp ), no reply wanted */
    sd_bus_call_method_async( menu->bus, NULL, menu->bus_name, menu->path, MENU_IFACE, "Event", NULL, NULL,
                              "isvu", id, event, "i", 0, 0 );
    /* a submenu fills itself in, as the root does */
    if (id != 0 && !strcmp( event, "opened" )) about_to_show( menu, id, 0 );
}
