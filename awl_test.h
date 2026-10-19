#pragma once

/**
 * The AWL_TEST_OUTPUTS and AWL_TEST_WINDOWS hooks, for test/live.sh and
 * test/windows.sh (awl_test.c). Main thread only.
 */

#include "awl_server.h"
#include "awl_window.h"

/** With AWL_TEST_OUTPUTS, SIGUSR1 adds an output, SIGUSR2 removes the
 * newest one (headless and nested backends only; setup() blocks them for
 * the event loop). With AWL_TEST_WINDOWS, a FIFO of commands for the text
 * popups, see testwincmd(). After the backend is made. */
void testinit(void);
/** win, a popup, is gone */
void testpopupdestroyed(awl_window_t *win);
