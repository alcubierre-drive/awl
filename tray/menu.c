/* The tray thread's half of the items' menus: see menu.h. GTK thread only,
 * apart from awl_tray_menu_request() and awl_tray_menu_event(), which only
 * queue work for it. */

#include "menu.h"
#include "dbus-menu.h"
#include "../awl_log.h"

#include <gtk/gtk.h>

#include <stdlib.h>
#include <string.h>

struct awl_tray_menu {
    uint64_t serial;
    char *bus, *path, *theme_path;
    /** NULL until connected */
    SnComCanonicalDbusmenu* proxy;
    GCancellable* cancel;
    /** the main thread shows it ("opened" of id 0 until "closed") */
    int open;
    /** the request the snapshots answer; wanted: it came before the proxy */
    uint32_t gen;
    int wanted;
    /** a GetLayout is out; again: one more once it is back */
    int fetching, again;
};

/** serial -> menu, of the menus that exist */
static GHashTable* menus;
static uint64_t next_serial = 1;

/** async callbacks get the serial, not the menu: it may be gone by then */
#define SERIAL_DATA( s ) ((gpointer)(uintptr_t)(s))
#define DATA_SERIAL( p ) ((uint64_t)(uintptr_t)(p))

static awl_tray_menu_t* lookup( uint64_t serial ) {
    return menus ? g_hash_table_lookup( menus, &serial ) : NULL;
}

/* ---- to the main thread ---- */

typedef struct {
    uint64_t serial;
    char* monitor;
    int center_x;
} toggle_t;

static void main_toggle( void* data ) {
    toggle_t* t = data;
    awl_tray_menu_window_toggle( t->serial, t->monitor, t->center_x );
    free( t->monitor );
    free( t );
}

static void main_snap( void* data ) {
    awl_tray_menu_window_snap( data );
}

static void main_gone( void* data ) {
    awl_tray_menu_window_gone( DATA_SERIAL( data ) );
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
    GPtrArray* kids;
} tnode;

static void tnode_free( gpointer p ) {
    tnode* t = p;
    if (!t) return;
    free( t->n.label );
    free( t->n.shortcut );
    free( t->n.icon );
    if (t->kids) g_ptr_array_unref( t->kids );
    free( t );
}

/** the label without mnemonics ("_File" is "File", "__" an underscore), on
 * one line */
static char* plain_label( const char* s ) {
    char* out = malloc( strlen( s ) + 1 );
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

/** the first of the shortcuts (aas), as "Ctrl+Shift+Q" */
static char* shortcut_text( GVariant* v ) {
    if (!g_variant_is_of_type( v, G_VARIANT_TYPE( "aas" ) ) || g_variant_n_children( v ) == 0) return NULL;
    GVariant* keys = g_variant_get_child_value( v, 0 );
    GString* s = g_string_new( NULL );
    for (gsize i = 0; i < g_variant_n_children( keys ); i++) {
        const char* k;
        g_variant_get_child( keys, i, "&s", &k );
        if (s->len) g_string_append_c( s, '+' );
        if (!strcmp( k, "Control" )) g_string_append( s, "Ctrl" );
        /* a letter as GTK shows it: "q" is Q */
        else if (k[0] && !k[1]) g_string_append_c( s, g_ascii_toupper( k[0] ) );
        else g_string_append( s, k );
    }
    g_variant_unref( keys );
    if (!s->len) {
        g_string_free( s, TRUE );
        return NULL;
    }
    char* out = strdup( s->str );
    g_string_free( s, TRUE );
    return out;
}

/** the icon themes one layout's icons are looked up in, and what they found */
typedef struct {
    GtkIconTheme* own; /* the item's and the menu's IconThemePath, or NULL */
    GHashTable* files; /* name -> GBytes, NULL if not found */
} icons_t;

static void icons_init( icons_t* ic, awl_tray_menu_t* menu ) {
    GPtrArray* paths = g_ptr_array_new();
    if (menu->theme_path && *menu->theme_path) g_ptr_array_add( paths, menu->theme_path );
    const gchar* const* more = menu->proxy ? sn_com_canonical_dbusmenu_get_icon_theme_path( menu->proxy ) : NULL;
    for (; more && *more; more++)
        if (**more) g_ptr_array_add( paths, (gpointer)*more );
    ic->own = NULL;
    if (paths->len) {
        ic->own = gtk_icon_theme_new();
        gtk_icon_theme_set_search_path( ic->own, (const gchar**)paths->pdata, (gint)paths->len );
    }
    g_ptr_array_free( paths, TRUE );
    ic->files = g_hash_table_new_full( g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref );
}

static void icons_fini( icons_t* ic ) {
    if (ic->own) g_object_unref( ic->own );
    g_hash_table_unref( ic->files );
}

static GBytes* read_file( const char* path ) {
    gchar* data;
    gsize len;
    if (!g_file_get_contents( path, &data, &len, NULL )) return NULL;
    return g_bytes_new_take( data, len );
}

/** the file of icon name: a path as it is, else from the item's themes, then
 * the default one. Any size will do (the main thread scales it): the
 * lookup's size only picks among a theme's sizes, preferring what scales. */
static GBytes* icon_file( icons_t* ic, const char* name ) {
    gpointer found;
    if (g_hash_table_lookup_extended( ic->files, name, NULL, &found )) return found;
    GBytes* bytes = NULL;
    if (name[0] == '/') {
        bytes = read_file( name );
    } else {
        GtkIconTheme* themes[2] = { ic->own, gtk_icon_theme_get_default() };
        for (int i = 0; i < 2 && !bytes; i++) {
            if (!themes[i]) continue;
            GtkIconInfo* info = gtk_icon_theme_lookup_icon( themes[i], name, 32, 0 );
            if (!info) continue;
            const gchar* file = gtk_icon_info_get_filename( info );
            if (file) bytes = read_file( file );
            g_object_unref( info );
        }
    }
    if (!bytes) awl_vrb_printf( "tray: menu icon '%s' not found\n", name );
    g_hash_table_insert( ic->files, g_strdup( name ), bytes );
    return bytes;
}

static void take_bytes( awl_tray_menu_node_t* n, GBytes* b ) {
    gsize len;
    const void* data = b ? g_bytes_get_data( b, &len ) : NULL;
    if (!data || !len) return;
    n->icon = malloc( len );
    memcpy( n->icon, data, len );
    n->icon_len = len;
}

/** The entry of a layout's (ia{sv}av), its children too; NULL if hidden. */
static tnode* parse( GVariant* v, icons_t* ic ) {
    gint32 id;
    GVariant *props, *kids;
    if (!g_variant_is_of_type( v, G_VARIANT_TYPE( "(ia{sv}av)" ) )) return NULL;
    g_variant_get( v, "(i@a{sv}@av)", &id, &props, &kids );

    gboolean visible = TRUE, enabled = TRUE;
    const char *type = NULL, *label = "", *icon_name = NULL, *toggle = NULL, *display = NULL;
    gint32 state = -1;
    g_variant_lookup( props, "visible", "b", &visible );
    tnode* t = NULL;
    if (!visible) goto out;

    g_variant_lookup( props, "enabled", "b", &enabled );
    g_variant_lookup( props, "type", "&s", &type );
    g_variant_lookup( props, "label", "&s", &label );
    g_variant_lookup( props, "icon-name", "&s", &icon_name );
    g_variant_lookup( props, "toggle-type", "&s", &toggle );
    g_variant_lookup( props, "toggle-state", "i", &state );
    g_variant_lookup( props, "children-display", "&s", &display );

    t = calloc( 1, sizeof(*t) );
    t->n.id = id;
    t->n.label = plain_label( label );
    if (type && !strcmp( type, "separator" )) t->n.flags |= AWL_TRAY_MENU_SEPARATOR;
    if (!enabled) t->n.flags |= AWL_TRAY_MENU_DISABLED;
    if (toggle && (!strcmp( toggle, "checkmark" ) || !strcmp( toggle, "radio" ))) {
        t->n.flags |= !strcmp( toggle, "radio" ) ? AWL_TRAY_MENU_RADIO : AWL_TRAY_MENU_CHECK;
        if (state == 1) t->n.flags |= AWL_TRAY_MENU_CHECKED;
        else if (state != 0) t->n.flags |= AWL_TRAY_MENU_MIXED;
    }
    if (display && !strcmp( display, "submenu" )) t->n.flags |= AWL_TRAY_MENU_SUBMENU;
    GVariant* sc = g_variant_lookup_value( props, "shortcut", NULL );
    if (sc) {
        t->n.shortcut = shortcut_text( sc );
        g_variant_unref( sc );
    }
    /* icon-data, a PNG, comes first, as libdbusmenu has it */
    GVariant* data = g_variant_lookup_value( props, "icon-data", G_VARIANT_TYPE( "ay" ) );
    if (data) {
        GBytes* b = g_variant_get_data_as_bytes( data );
        take_bytes( &t->n, b );
        g_bytes_unref( b );
        g_variant_unref( data );
    }
    if (!t->n.icon && icon_name && *icon_name) take_bytes( &t->n, icon_file( ic, icon_name ) );

    /* the children, separators collapsed */
    t->kids = g_ptr_array_new_with_free_func( tnode_free );
    for (gsize i = 0; i < g_variant_n_children( kids ); i++) {
        GVariant* boxed = g_variant_get_child_value( kids, i );
        GVariant* child = g_variant_get_variant( boxed );
        tnode* k = parse( child, ic );
        g_variant_unref( child );
        g_variant_unref( boxed );
        if (!k) continue;
        int sep = !!(k->n.flags & AWL_TRAY_MENU_SEPARATOR);
        tnode* last = t->kids->len ? g_ptr_array_index( t->kids, t->kids->len - 1 ) : NULL;
        if (sep && (!last || (last->n.flags & AWL_TRAY_MENU_SEPARATOR))) {
            tnode_free( k );
            continue;
        }
        g_ptr_array_add( t->kids, k );
    }
    if (t->kids->len) {
        tnode* last = g_ptr_array_index( t->kids, t->kids->len - 1 );
        if (last->n.flags & AWL_TRAY_MENU_SEPARATOR) g_ptr_array_remove_index( t->kids, t->kids->len - 1 );
    }
    /* children-display alone too: Qt's and Electron's submenus are empty
     * until their AboutToShow */
    if (t->kids->len) t->n.flags |= AWL_TRAY_MENU_SUBMENU;
out:
    g_variant_unref( props );
    g_variant_unref( kids );
    return t;
}

/** the tree as nodes, breadth first: each one's children side by side */
static awl_tray_menu_snap_t* flatten( tnode* root ) {
    GPtrArray* order = g_ptr_array_new();
    g_ptr_array_add( order, root );
    for (guint i = 0; i < order->len; i++) {
        tnode* t = g_ptr_array_index( order, i );
        for (guint k = 0; k < t->kids->len; k++) g_ptr_array_add( order, g_ptr_array_index( t->kids, k ) );
    }
    awl_tray_menu_snap_t* snap = calloc( 1, sizeof(*snap) );
    snap->nnodes = (int)order->len;
    snap->nodes = calloc( order->len, sizeof(*snap->nodes) );
    int next = 1;
    for (guint i = 0; i < order->len; i++) {
        tnode* t = g_ptr_array_index( order, i );
        awl_tray_menu_node_t* n = &snap->nodes[i];
        *n = t->n;
        /* the snapshot owns them now */
        t->n.label = t->n.shortcut = NULL;
        t->n.icon = NULL;
        n->first = next;
        n->n = (int)t->kids->len;
        if (i == 0) n->parent = -1;
        for (int k = 0; k < n->n; k++) snap->nodes[next + k].parent = (int)i;
        next += n->n;
    }
    g_ptr_array_free( order, TRUE );
    return snap;
}

/* ---- D-Bus ---- */

static void fetch( awl_tray_menu_t* menu );

static void layout_done( GObject* src, GAsyncResult* res, gpointer data ) {
    guint revision = 0;
    GVariant* layout = NULL;
    GError* err = NULL;
    gboolean ok = sn_com_canonical_dbusmenu_call_get_layout_finish( SN_COM_CANONICAL_DBUSMENU( src ), &revision,
                                                                    &layout, res, &err );
    awl_tray_menu_t* menu = lookup( DATA_SERIAL( data ) );
    if (!menu) goto out;
    menu->fetching = 0;
    if (!ok) {
        awl_wrn_printf( "tray: menu %s%s: GetLayout: %s\n", menu->bus, menu->path, err->message );
        goto out;
    }
    icons_t ic;
    icons_init( &ic, menu );
    tnode* root = parse( layout, &ic );
    icons_fini( &ic );
    if (root) {
        awl_tray_menu_snap_t* snap = flatten( root );
        tnode_free( root );
        snap->serial = menu->serial;
        snap->gen = menu->gen;
        snap->revision = revision;
        awl_host->post( main_snap, snap );
    }
    if (menu->again) {
        menu->again = 0;
        fetch( menu );
    }
out:
    if (layout) g_variant_unref( layout );
    g_clear_error( &err );
}

/** GetLayout of everything, its snapshot to the main thread */
static void fetch( awl_tray_menu_t* menu ) {
    static const gchar* const all[] = { NULL };
    if (!menu->proxy) return;
    if (menu->fetching) {
        menu->again = 1;
        return;
    }
    menu->fetching = 1;
    sn_com_canonical_dbusmenu_call_get_layout( menu->proxy, 0, -1, all, menu->cancel, layout_done,
                                               SERIAL_DATA( menu->serial ) );
}

/** Some menus (Qt's, Electron's) fill themselves in only when asked
 * whether they need to: with update, fetch the layout once they answered,
 * whatever they say (an error included; the layout itself may still be
 * fine); else only if they say it changed. */
typedef struct {
    uint64_t serial;
    int update;
} shown_t;

static void about_to_show_done( GObject* src, GAsyncResult* res, gpointer data ) {
    shown_t* s = data;
    gboolean need = FALSE;
    gboolean ok = sn_com_canonical_dbusmenu_call_about_to_show_finish( SN_COM_CANONICAL_DBUSMENU( src ), &need,
                                                                       res, NULL );
    awl_tray_menu_t* menu = lookup( s->serial );
    if (menu && (s->update || (ok && need))) fetch( menu );
    free( s );
}

static void about_to_show( awl_tray_menu_t* menu, int32_t id, int update ) {
    shown_t* s = malloc( sizeof(*s) );
    *s = (shown_t){ menu->serial, update };
    sn_com_canonical_dbusmenu_call_about_to_show( menu->proxy, id, menu->cancel, about_to_show_done, s );
}

static void layout_updated( SnComCanonicalDbusmenu* proxy, guint revision, gint parent, gpointer data ) {
    awl_tray_menu_t* menu = data;
    if (menu->open) fetch( menu );
}

static void properties_updated( SnComCanonicalDbusmenu* proxy, GVariant* updated, GVariant* removed,
                                gpointer data ) {
    awl_tray_menu_t* menu = data;
    if (menu->open) fetch( menu );
}

static void proxy_ready( GObject* src, GAsyncResult* res, gpointer data ) {
    GError* err = NULL;
    SnComCanonicalDbusmenu* proxy = sn_com_canonical_dbusmenu_proxy_new_finish( res, &err );
    awl_tray_menu_t* menu = lookup( DATA_SERIAL( data ) );
    if (!menu) {
        if (proxy) g_object_unref( proxy );
        g_clear_error( &err );
        return;
    }
    if (!proxy) {
        awl_wrn_printf( "tray: menu %s%s: %s\n", menu->bus, menu->path, err->message );
        g_error_free( err );
        if (menu->wanted) awl_host->post( main_gone, SERIAL_DATA( menu->serial ) );
        return;
    }
    menu->proxy = proxy;
    g_signal_connect( proxy, "layout-updated", G_CALLBACK( layout_updated ), menu );
    g_signal_connect( proxy, "items-properties-updated", G_CALLBACK( properties_updated ), menu );
    if (menu->wanted) {
        menu->wanted = 0;
        about_to_show( menu, 0, 1 );
    }
}

/* ---- the menus ---- */

awl_tray_menu_t* awl_tray_menu_new( const char* bus, const char* path, const char* theme_path ) {
    if (!menus) menus = g_hash_table_new( g_int64_hash, g_int64_equal );
    awl_tray_menu_t* menu = calloc( 1, sizeof(*menu) );
    menu->serial = next_serial++;
    menu->bus = strdup( bus );
    menu->path = strdup( path );
    menu->theme_path = strdup( theme_path ? theme_path : "" );
    menu->cancel = g_cancellable_new();
    g_hash_table_insert( menus, &menu->serial, menu );
    sn_com_canonical_dbusmenu_proxy_new_for_bus( G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, bus, path,
                                                 menu->cancel, proxy_ready, SERIAL_DATA( menu->serial ) );
    return menu;
}

void awl_tray_menu_free( awl_tray_menu_t* menu ) {
    if (!menu) return;
    g_hash_table_remove( menus, &menu->serial );
    /* the main thread may show it, or wait for it */
    awl_host->post( main_gone, SERIAL_DATA( menu->serial ) );
    g_cancellable_cancel( menu->cancel );
    g_object_unref( menu->cancel );
    if (menu->proxy) {
        g_signal_handlers_disconnect_by_data( menu->proxy, menu );
        g_object_unref( menu->proxy );
    }
    free( menu->bus );
    free( menu->path );
    free( menu->theme_path );
    free( menu );
}

void awl_tray_menu_set_theme_path( awl_tray_menu_t* menu, const char* theme_path ) {
    free( menu->theme_path );
    menu->theme_path = strdup( theme_path ? theme_path : "" );
}

void awl_tray_menu_toggle( awl_tray_menu_t* menu, const char* monitor, int center_x ) {
    toggle_t* t = malloc( sizeof(*t) );
    *t = (toggle_t){ menu->serial, strdup( monitor ? monitor : "" ), center_x };
    awl_host->post( main_toggle, t );
}

/* ---- from the main thread ---- */

typedef struct {
    uint64_t serial;
    uint32_t gen;
    int32_t id;
    /** NULL: a request */
    char* event;
} call_t;

static gboolean run_call( gpointer data ) {
    call_t* c = data;
    awl_tray_menu_t* menu = lookup( c->serial );
    if (!menu) {
        /* it went away before the main thread knew: let it know */
        if (!c->event) awl_host->post( main_gone, SERIAL_DATA( c->serial ) );
    } else if (!c->event) {
        menu->gen = c->gen;
        if (menu->proxy) about_to_show( menu, 0, 1 );
        else menu->wanted = 1;
    } else {
        if (c->id == 0 && !strcmp( c->event, "opened" )) menu->open = 1;
        if (c->id == 0 && !strcmp( c->event, "closed" )) menu->open = 0;
        if (menu->proxy) {
            sn_com_canonical_dbusmenu_call_event( menu->proxy, c->id, c->event,
                                                  g_variant_new_variant( g_variant_new_int32( 0 ) ), 0, NULL,
                                                  NULL, NULL );
            /* a submenu fills itself in, as the root does */
            if (c->id != 0 && !strcmp( c->event, "opened" )) about_to_show( menu, c->id, 0 );
        }
    }
    free( c->event );
    free( c );
    return G_SOURCE_REMOVE;
}

void awl_tray_menu_request( uint64_t serial, uint32_t gen ) {
    call_t* c = calloc( 1, sizeof(*c) );
    *c = (call_t){ serial, gen, 0, NULL };
    g_idle_add( run_call, c );
}

void awl_tray_menu_event( uint64_t serial, int32_t id, const char* event ) {
    call_t* c = calloc( 1, sizeof(*c) );
    *c = (call_t){ serial, 0, id, strdup( event ) };
    g_idle_add( run_call, c );
}
