/* sni_menu: a tray item (StatusNotifierItem) with a com.canonical.dbusmenu
 * menu, for testing the tray's menus in test/live.sh's session:
 *
 *   live.sh run env DBUS_SESSION_BUS_ADDRESS=... test/sni_menu
 *
 * The menu has what menus have: an icon by name and one as PNG data, a
 * check box and radio buttons (clicking them toggles them and sends
 * LayoutUpdated, as real menus do), a disabled entry, separators (also ones
 * the tray has to collapse), a shortcut, a mnemonic, and a submenu that is
 * empty until its AboutToShow, as Qt's are, and "Network", a submenu with
 * one of its own. Every call is printed to stdout: "event <id> <name>",
 * "about-to-show <id>", "get-layout <parent>", "item <method>".
 *
 * The environment changes it:
 *   SNI_MENU_REBUILD=<ms>  every ms, rebuilds "Network" as nm-applet's
 *                          rescans do: a layout without it, 100 ms later
 *                          one with it again, its entries' ids all new
 *   SNI_MENU_IS_MENU=1     ItemIsMenu true (a left click opens the menu)
 *   SNI_MENU_NO_ACTIVATE=1 Activate fails, as for items that have none */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char item_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconThemePath' type='s' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "</interface></node>";

static const char menu_xml[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='as' direction='in'/><arg type='u' direction='out'/>"
    "<arg type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/>"
    "<arg type='a(ia{sv})' direction='out'/></method>"
    "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "<arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ia{s})'/></signal>"
    "</interface></node>";

enum { ID_OPEN = 1, ID_PNG, ID_CHECK, ID_RADIO_A, ID_RADIO_B, ID_DISABLED, ID_SEP1, ID_SEP2, ID_SEP3, ID_MORE,
       ID_SUB_A, ID_SUB_B, ID_QUIT };

static GDBusConnection* bus;
static guint revision = 1;
static int checked = 1, radio = ID_RADIO_A, sub_filled = 0;
/** SNI_MENU_REBUILD: "Network" is being rebuilt; its ids' generation */
static int rebuilding = 0, net_gen = 0;
static GBytes* png;

static GVariant* entry( int id, GVariantBuilder* props, GVariant* kids ) {
    if (!kids) kids = g_variant_new_array( G_VARIANT_TYPE_VARIANT, NULL, 0 );
    return g_variant_new( "(i@a{sv}@av)", id, g_variant_builder_end( props ), kids );
}

static GVariantBuilder* props( const char* label ) {
    GVariantBuilder* b = g_variant_builder_new( G_VARIANT_TYPE( "a{sv}" ) );
    if (label) g_variant_builder_add( b, "{sv}", "label", g_variant_new_string( label ) );
    return b;
}

static GVariant* separator( int id ) {
    GVariantBuilder* p = props( NULL );
    g_variant_builder_add( p, "{sv}", "type", g_variant_new_string( "separator" ) );
    GVariant* v = entry( id, p, NULL );
    g_variant_builder_unref( p );
    return v;
}

static GVariant* layout( void ) {
    GVariantBuilder kids;
    GVariantBuilder* p;
    g_variant_builder_init( &kids, G_VARIANT_TYPE( "av" ) );

    /* separators first and twice in a row: the tray drops them */
    g_variant_builder_add( &kids, "v", separator( ID_SEP1 ) );

    p = props( "_Open file" );
    g_variant_builder_add( p, "{sv}", "icon-name", g_variant_new_string( "document-open" ) );
    g_variant_builder_add( &kids, "v", entry( ID_OPEN, p, NULL ) );
    g_variant_builder_unref( p );

    p = props( "Icon from PNG data" );
    g_variant_builder_add( p, "{sv}", "icon-data",
                           g_variant_new_from_bytes( G_VARIANT_TYPE( "ay" ), png, TRUE ) );
    g_variant_builder_add( &kids, "v", entry( ID_PNG, p, NULL ) );
    g_variant_builder_unref( p );

    g_variant_builder_add( &kids, "v", separator( ID_SEP2 ) );
    g_variant_builder_add( &kids, "v", separator( ID_SEP3 ) );

    p = props( "Check box" );
    g_variant_builder_add( p, "{sv}", "toggle-type", g_variant_new_string( "checkmark" ) );
    g_variant_builder_add( p, "{sv}", "toggle-state", g_variant_new_int32( checked ) );
    g_variant_builder_add( &kids, "v", entry( ID_CHECK, p, NULL ) );
    g_variant_builder_unref( p );

    for (int id = ID_RADIO_A; id <= ID_RADIO_B; id++) {
        p = props( id == ID_RADIO_A ? "Radio A" : "Radio B" );
        g_variant_builder_add( p, "{sv}", "toggle-type", g_variant_new_string( "radio" ) );
        g_variant_builder_add( p, "{sv}", "toggle-state", g_variant_new_int32( radio == id ) );
        g_variant_builder_add( &kids, "v", entry( id, p, NULL ) );
        g_variant_builder_unref( p );
    }

    p = props( "Disabled" );
    g_variant_builder_add( p, "{sv}", "enabled", g_variant_new_boolean( FALSE ) );
    g_variant_builder_add( &kids, "v", entry( ID_DISABLED, p, NULL ) );
    g_variant_builder_unref( p );

    /* empty until its AboutToShow */
    GVariantBuilder sub;
    g_variant_builder_init( &sub, G_VARIANT_TYPE( "av" ) );
    if (sub_filled) {
        GVariantBuilder* q = props( "Sub entry A" );
        g_variant_builder_add( &sub, "v", entry( ID_SUB_A, q, NULL ) );
        g_variant_builder_unref( q );
        q = props( "Sub entry B, a long one" );
        g_variant_builder_add( &sub, "v", entry( ID_SUB_B, q, NULL ) );
        g_variant_builder_unref( q );
    }
    p = props( "More" );
    g_variant_builder_add( p, "{sv}", "children-display", g_variant_new_string( "submenu" ) );
    g_variant_builder_add( &kids, "v", entry( ID_MORE, p, g_variant_builder_end( &sub ) ) );
    g_variant_builder_unref( p );

    /* a submenu with a submenu, filled from the start; its ids change
     * with each rebuild */
    if (!rebuilding) {
        const int base = 1000 + 100 * net_gen;
        GVariantBuilder net, deep;
        g_variant_builder_init( &net, G_VARIANT_TYPE( "av" ) );
        g_variant_builder_init( &deep, G_VARIANT_TYPE( "av" ) );
        const char* deep_labels[] = { "Deep 1", "Deep 2", "Deep 3" };
        for (int i = 0; i < 3; i++) {
            GVariantBuilder* q = props( deep_labels[i] );
            g_variant_builder_add( &deep, "v", entry( base + 10 + i, q, NULL ) );
            g_variant_builder_unref( q );
        }
        const char* net_labels[] = { "Wi-Fi A", "Wi-Fi B", NULL, "Wi-Fi C" };
        for (int i = 0; i < 4; i++) {
            if (!net_labels[i]) {
                g_variant_builder_add( &net, "v", separator( base + 1 + i ) );
                continue;
            }
            GVariantBuilder* q = props( net_labels[i] );
            g_variant_builder_add( &net, "v", entry( base + 1 + i, q, NULL ) );
            g_variant_builder_unref( q );
        }
        GVariantBuilder* q = props( "Deeper" );
        g_variant_builder_add( q, "{sv}", "children-display", g_variant_new_string( "submenu" ) );
        g_variant_builder_add( &net, "v", entry( base + 9, q, g_variant_builder_end( &deep ) ) );
        g_variant_builder_unref( q );
        p = props( "Network" );
        g_variant_builder_add( p, "{sv}", "children-display", g_variant_new_string( "submenu" ) );
        g_variant_builder_add( &kids, "v", entry( base, p, g_variant_builder_end( &net ) ) );
        g_variant_builder_unref( p );
    }

    g_variant_builder_add( &kids, "v", separator( ID_SEP1 + 100 ) );

    p = props( "_Quit" );
    GVariantBuilder sc;
    g_variant_builder_init( &sc, G_VARIANT_TYPE( "aas" ) );
    const char* keys[] = { "Control", "q", NULL };
    g_variant_builder_add( &sc, "^as", keys );
    g_variant_builder_add( p, "{sv}", "shortcut", g_variant_builder_end( &sc ) );
    g_variant_builder_add( &kids, "v", entry( ID_QUIT, p, NULL ) );
    g_variant_builder_unref( p );

    p = props( NULL );
    g_variant_builder_add( p, "{sv}", "children-display", g_variant_new_string( "submenu" ) );
    GVariant* root = entry( 0, p, g_variant_builder_end( &kids ) );
    g_variant_builder_unref( p );
    return root;
}

static void changed( void ) {
    revision++;
    g_dbus_connection_emit_signal( bus, NULL, "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated",
                                   g_variant_new( "(ui)", revision, 0 ), NULL );
}

static void menu_call( GDBusConnection* c, const char* sender, const char* path, const char* iface,
                       const char* method, GVariant* args, GDBusMethodInvocation* inv, gpointer data ) {
    if (!strcmp( method, "GetLayout" )) {
        gint32 parent, depth;
        g_variant_get( args, "(ii^a&s)", &parent, &depth, NULL );
        printf( "get-layout %d\n", parent );
        g_dbus_method_invocation_return_value( inv, g_variant_new( "(u@(ia{sv}av))", revision, layout() ) );
    } else if (!strcmp( method, "AboutToShow" )) {
        gint32 id;
        g_variant_get( args, "(i)", &id );
        printf( "about-to-show %d\n", id );
        int need = id == ID_MORE && !sub_filled;
        if (need) {
            sub_filled = 1;
            changed();
        }
        g_dbus_method_invocation_return_value( inv, g_variant_new( "(b)", need ) );
    } else if (!strcmp( method, "Event" )) {
        gint32 id;
        const char* name;
        g_variant_get( args, "(i&svu)", &id, &name, NULL, NULL );
        printf( "event %d %s\n", id, name );
        if (!strcmp( name, "clicked" )) {
            if (id == ID_CHECK) checked = !checked;
            if (id == ID_RADIO_A || id == ID_RADIO_B) radio = id;
            if (id == ID_CHECK || id == ID_RADIO_A || id == ID_RADIO_B) changed();
        }
        g_dbus_method_invocation_return_value( inv, NULL );
    } else if (!strcmp( method, "GetGroupProperties" )) {
        g_dbus_method_invocation_return_value( inv, g_variant_new( "(@a(ia{sv}))",
                                                g_variant_new_array( G_VARIANT_TYPE( "(ia{sv})" ), NULL, 0 ) ) );
    } else {
        g_dbus_method_invocation_return_dbus_error( inv, "org.freedesktop.DBus.Error.UnknownMethod", method );
    }
    fflush( stdout );
}

static GVariant* menu_prop( GDBusConnection* c, const char* sender, const char* path, const char* iface,
                            const char* name, GError** err, gpointer data ) {
    if (!strcmp( name, "Version" )) return g_variant_new_uint32( 3 );
    if (!strcmp( name, "IconThemePath" )) return g_variant_new_strv( NULL, 0 );
    if (!strcmp( name, "TextDirection" )) return g_variant_new_string( "ltr" );
    return g_variant_new_string( "normal" );
}

static void item_call( GDBusConnection* c, const char* sender, const char* path, const char* iface,
                       const char* method, GVariant* args, GDBusMethodInvocation* inv, gpointer data ) {
    printf( "item %s\n", method );
    fflush( stdout );
    if (!strcmp( method, "Activate" ) && getenv( "SNI_MENU_NO_ACTIVATE" ))
        g_dbus_method_invocation_return_dbus_error( inv, "org.freedesktop.DBus.Error.UnknownMethod", method );
    else
        g_dbus_method_invocation_return_value( inv, NULL );
}

static gboolean rebuilt( gpointer data ) {
    rebuilding = 0;
    changed();
    return G_SOURCE_REMOVE;
}

static gboolean rebuild( gpointer data ) {
    printf( "rebuild\n" );
    fflush( stdout );
    rebuilding = 1;
    net_gen++;
    changed();
    g_timeout_add( 100, rebuilt, NULL );
    return G_SOURCE_CONTINUE;
}

static GVariant* item_prop( GDBusConnection* c, const char* sender, const char* path, const char* iface,
                            const char* name, GError** err, gpointer data ) {
    if (!strcmp( name, "Category" )) return g_variant_new_string( "ApplicationStatus" );
    if (!strcmp( name, "Id" )) return g_variant_new_string( "sni_menu" );
    if (!strcmp( name, "Title" )) return g_variant_new_string( "sni_menu test" );
    if (!strcmp( name, "Status" )) return g_variant_new_string( "Active" );
    if (!strcmp( name, "IconName" )) return g_variant_new_string( "dialog-information" );
    if (!strcmp( name, "IconThemePath" )) return g_variant_new_string( "" );
    if (!strcmp( name, "Menu" )) return g_variant_new_object_path( "/MenuBar" );
    if (!strcmp( name, "ItemIsMenu" )) return g_variant_new_boolean( getenv( "SNI_MENU_IS_MENU" ) != NULL );
    if (!strcmp( name, "ToolTip" ))
        return g_variant_new( "(s@a(iiay)ss)", "", g_variant_new_array( G_VARIANT_TYPE( "(iiay)" ), NULL, 0 ),
                              "sni_menu tooltip", "its <i>description</i>" );
    return NULL;
}

static void registered( GObject* src, GAsyncResult* res, gpointer data ) {
    GError* err = NULL;
    GVariant* v = g_dbus_connection_call_finish( G_DBUS_CONNECTION( src ), res, &err );
    if (v) {
        printf( "registered\n" );
        g_variant_unref( v );
    } else {
        printf( "register failed: %s\n", err->message );
        g_error_free( err );
    }
    fflush( stdout );
}

/** a 32 x 32 PNG: a red disk on transparency */
static GBytes* make_png( void ) {
    GdkPixbuf* pb = gdk_pixbuf_new( GDK_COLORSPACE_RGB, TRUE, 8, 32, 32 );
    guint8* px = gdk_pixbuf_get_pixels( pb );
    int rs = gdk_pixbuf_get_rowstride( pb );
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            guint8* p = px + y * rs + x * 4;
            int in = (x - 15.5) * (x - 15.5) + (y - 15.5) * (y - 15.5) < 14 * 14;
            p[0] = 0xe0, p[1] = 0x30, p[2] = 0x30, p[3] = in ? 0xff : 0;
        }
    gchar* buf;
    gsize len;
    gdk_pixbuf_save_to_buffer( pb, &buf, &len, "png", NULL, NULL );
    g_object_unref( pb );
    return g_bytes_new_take( buf, len );
}

int main( void ) {
    static const GDBusInterfaceVTable menu_vt = { menu_call, menu_prop, NULL, { 0 } };
    static const GDBusInterfaceVTable item_vt = { item_call, item_prop, NULL, { 0 } };
    GError* err = NULL;
    png = make_png();
    bus = g_bus_get_sync( G_BUS_TYPE_SESSION, NULL, &err );
    if (!bus) {
        fprintf( stderr, "sni_menu: %s\n", err->message );
        return 1;
    }
    GDBusNodeInfo* item = g_dbus_node_info_new_for_xml( item_xml, NULL );
    GDBusNodeInfo* menu = g_dbus_node_info_new_for_xml( menu_xml, NULL );
    g_dbus_connection_register_object( bus, "/StatusNotifierItem", item->interfaces[0], &item_vt, NULL, NULL, NULL );
    g_dbus_connection_register_object( bus, "/MenuBar", menu->interfaces[0], &menu_vt, NULL, NULL, NULL );

    char name[64];
    snprintf( name, sizeof name, "org.kde.StatusNotifierItem-%d-1", (int)getpid() );
    g_bus_own_name_on_connection( bus, name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL );
    g_dbus_connection_call( bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                            "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                            g_variant_new( "(s)", name ), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, registered,
                            NULL );
    const char* every = getenv( "SNI_MENU_REBUILD" );
    if (every && atoi( every ) > 0) g_timeout_add( (guint)atoi( every ), rebuild, NULL );
    printf( "sni_menu %s\n", name );
    fflush( stdout );
    g_main_loop_run( g_main_loop_new( NULL, FALSE ) );
    return 0;
}
