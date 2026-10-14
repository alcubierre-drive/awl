/** config_plugins.h, built into libawlplugins.so (see awl_config_t in
 * awl_plugin_abi.h). The functions config_plugins.h binds to keys and layouts
 * are awl's; here they are same-named wrappers calling them through the host
 * table, next to the library's own actions. None of them is static, so
 * config_plugins.h may leave any of them unbound; the library exports none. */
#include "awl.h"
#include "awl_plugin_abi.h"
#include "plugins.h"
#include "plugins/colors.h"

#define AWL_ACTION_WRAPPER( name ) \
    void name( const Arg* arg ) { awl_host->actions->name( arg ); }
AWL_ACTIONS( AWL_ACTION_WRAPPER )
#undef AWL_ACTION_WRAPPER

#define AWL_ARRANGE_WRAPPER( name ) \
    void name( Monitor* m ) { awl_host->arranges->name( m ); }
AWL_ARRANGES( AWL_ARRANGE_WRAPPER )
#undef AWL_ARRANGE_WRAPPER

void notifyconfig( const Arg* arg ) { awl_notify_config(); }
void backlighttoggle( const Arg* arg ) { awl_backlight_toggle(); }
#include "config_plugins.h"

const awl_config_t* awl_config( void ) {
    static awl_config_t c;
    c = AWL_CONFIG_TABLE;
    return &c;
}
