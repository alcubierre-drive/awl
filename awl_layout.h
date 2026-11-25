#pragma once

/**
 * Layouts, tags and views, part of awl: arranging a monitor's clients with
 * its layout, the layouts and their settings for each view
 * (:c:member:`Monitor.tag_layouts`), and the actions that change them (awl_layout.c).
 * Main thread only.
 */

#include "awl_config.h"

/** how many tags there are, cfg->ntags kept to 1..31 */
#define TAG_COUNT(cfg)           ((cfg)->ntags < 1 ? 1 : (cfg)->ntags > 31 ? 31 : (cfg)->ntags)
/** the tags, as a mask */
#define TAG_MASK(cfg)            ((1u << TAG_COUNT(cfg)) - 1)

/** lays out m's windows with its layout */
void arrange( awl_server_t *srv, Monitor *m );
/** m's selected layout */
const Layout *selected_layout( Monitor *m );
/** l's index in :c:member:`cfg->layouts <awl_config_t.layouts>`, -1 if it isn't one of them */
int layout_index( const awl_config_t *cfg, const Layout *l );
/** After cfg changed: the monitors' layouts point into the old table (which
 * may be about to be unloaded); same index in the new one, or its first */
void layouts_moved( awl_server_t *srv, const Layout *old, size_t n_old );
/** After cfg changed: the views and windows on tags beyond :c:macro:`TAG_COUNT` go to
 * the first tag */
void tags_configure( awl_server_t *srv );
