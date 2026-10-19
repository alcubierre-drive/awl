#pragma once

/**
 * Layouts, tags and views, part of awl: arranging a monitor's clients with
 * its layout, the layouts and their settings for each view
 * (``Monitor.ptlt``), and the actions that change them (awl_layout.c).
 * Main thread only.
 */

#include "awl_config.h"

/** config_awl.h's tags, as a mask */
#define TAGMASK                 ((1u << LENGTH(tags)) - 1)

/** lays out m's windows with its layout */
void arrange(Monitor *m);
/** m's selected layout */
const Layout *sellayout(Monitor *m);
/** l's index in config_plugins.h's layouts, -1 if it isn't one of them */
int layoutindex(const Layout *l);
/** After cfg changed: the monitors' layouts point into the old table (which
 * may be about to be unloaded); same index in the new one, or its first */
void layoutsmoved(const Layout *old, size_t n_old);
