#pragma once

/**
 * Layouts, tags and views, part of awl: arranging a monitor's clients with
 * its layout, the layouts and their settings for each view
 * (:c:member:`Monitor.ptlt`), and the actions that change them (awl_layout.c).
 * Main thread only.
 */

#include "awl_config.h"

/** how many tags there are, cfg->ntags kept to 1..31 */
#define TAGCOUNT                (cfg->ntags < 1 ? 1 : cfg->ntags > 31 ? 31 : cfg->ntags)
/** the tags, as a mask */
#define TAGMASK                 ((1u << TAGCOUNT) - 1)

/** lays out m's windows with its layout */
void arrange(Monitor *m);
/** m's selected layout */
const Layout *sellayout(Monitor *m);
/** l's index in cfg->layouts, -1 if it isn't one of them */
int layoutindex(const Layout *l);
/** After cfg changed: the monitors' layouts point into the old table (which
 * may be about to be unloaded); same index in the new one, or its first */
void layoutsmoved(const Layout *old, size_t n_old);
/** After cfg changed: the views and windows on tags beyond :c:macro:`TAGCOUNT` go to
 * the first tag */
void tagsconfigure(void);
