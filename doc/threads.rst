Threads
=======

awl's main thread runs the Wayland event loop and must never block: a
stalled main thread freezes every window, the cursor included. Everything
that may wait, on the disk, the network, D-Bus or a decoder, happens on
another thread.

The threads
-----------

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Thread
     - Where
     - What it does
   * - main
     - awl.c
     - wlroots, input, drawing the bars and panels, taking the wallpaper
   * - ``poller``
     - plugins/poller.c
     - one PulseAudio main loop for all status plugins: CPU, memory,
       temperature and clock once per second; battery and IP address on
       kernel events; volume and the backlight-tooler daemon on their own events
   * - ``desktop``
     - plugins_desktop_panel.c
     - watches ``desktop_dir`` with inotify and lists it
   * - ``wallpaper``
     - plugins/wallpaper.c
     - lists the wallpaper directory, picks one, decodes it
   * - ``logind``
     - plugins/logind.c
     - listens to systemd-logind on the system bus: asks awl to lock on
       ``Lock`` and before suspend, holds the delay inhibitor that makes
       suspend wait for it
   * - tray
     - tray/
     - the GTK main loop: StatusNotifier host and watcher, the tray windows,
       the calendar

The rules
---------

The main thread never touches the file system.
   On a hung mount, e.g. a remote one, any file system call can block
   indefinitely, ``stat()`` and ``open()`` included. So the desktop panel's
   scanner and the wallpaper's decoder do all of it, and the main thread
   only gets their results.

Results are handed over, not shared.
   A thread publishes its result whole, through an atomic pointer or
   atomic values, so neither side ever waits for the other's lock. The
   status plugins keep their readings in atomics (e.g. `awl_stats_t`), the
   scanner hands over a copy of its list, the decoder a finished
   `awl_image_t`.

Changes are announced with a redraw request.
   `awl_redraw_request()` may be called from any thread; the libraries call
   it as ``awl_host->redraw_request()``. Logging, too, is safe everywhere. The main loop
   handles all requests since its last wakeup at once. Lock requests and the
   logind thread's state take the same way through their own eventfd
   (awl_lock.h).

Alerts are awl's.
   What the user must not miss, such as no lock before suspend, is raised
   by awl's main thread (awl_alert.h), never by a library thread: the
   library may be the very thing that is missing. awl watches for what the
   thread should report, and raises the alert when it doesn't.

Threads are woken, never cancelled.
   Every library thread is an `awl_thread_t`: it sleeps in ``poll()`` on its
   own eventfd among others, and `awl_thread_stop()` sets a flag and makes
   the eventfd readable. The thread returns when it next looks.

A stuck thread is left behind.
   A thread blocked in the kernel, e.g. on a dead mount, can't look. The
   scanner and the decoder are given 500 ms to stop; after that, their
   state is abandoned with them, never freed, and since their code must stay
   mapped, the old library is never unloaded (`awl_plugin_api_t.fini`
   reports it). A reload still starts fresh threads in the new library.

Signals belong to the main thread.
   `awl_thread_start()` blocks all signals in the threads it starts.
   Children are spawned with ``vfork()``, not ``fork()``, since a fork can
   inherit a lock another thread held at that moment.

Who talks to whom
-----------------

Solid arrows are data handed over, dashed ones wakeups. The main thread
wakes the library's threads through their eventfds; they and the tray wake it
through the redraw eventfd, with ``awl_host->redraw_request()``, and it then
picks up whatever they published.

.. mermaid::

   flowchart TB
     subgraph lib["libawlplugins.so"]
       direction LR
       poller["poller thread<br/>stats, temp, clock, battery,<br/>IP, volume, backlight"] -- "readings" --> atomics[("atomics<br/>awl_stats_t, ...")]
       scanner["desktop thread<br/>desktop_dir"] -- "changed list" --> pending[("pending<br/>DesktopFiles*")]
       changer["wallpaper thread<br/>pick, decode"] -- "decoded image" --> image[("pending<br/>awl_image_t*")]
     end
     tray["tray thread<br/>GTK, libawltray.so"]
     logind["logind thread<br/>plugins/logind.c"]
     redraw(["redraw eventfd<br/>awl_redraw.c"])
     lockfd(["lock eventfd<br/>awl_lock.c"])
     subgraph main["main thread (awl.c)"]
       direction LR
       loop["Wayland event loop"] --> fire["redraw_fire()"]
       fire --> bars["drawbars()"]
       fire --> panels["desktop_update_all()"]
       fire --> wp["wallpapertake()"]
       loop --> lockfire["lockfire()"]
       lockfire --> lockscreen["lockscreen()"]
       lockfire --> logindupdate["logindupdate():<br/>the alert"]
     end

     lib -. "awl_host->redraw_request()" .-> redraw
     tray -. "awl_host->redraw_request():<br/>a tray's width changed" .-> redraw
     redraw -. "readable" .-> loop
     logind -. "awl_host->lock_request(),<br/>awl_host->logind_state()" .-> lockfd
     lockfd -. "readable" .-> loop
     main -. "session_covered()" .-> logind
     atomics -- "widgets read them" --> bars
     pending -- "awl_desktop_version()" --> panels
     image -- "awl_wallpaper_take()" --> wp
     main -. "wake_fd" .-> lib

The life of a thread
--------------------

Every thread of the library is an `awl_thread_t`, started and stopped the
same way. Only the poller is waited for without a limit: the only files it
reads are in ``/proc`` and ``/sys``, which can't hang like a remote mount.
The logind thread touches no files at all; it gets 2 s.

.. mermaid::

   flowchart LR
     subgraph T["the thread"]
       start["awl_thread_start()<br/>wake_fd = eventfd<br/>all signals blocked<br/>pthread_create()"] --> sleep["poll() on wake_fd<br/>and its own fds"]
       sleep -- "own fd, timeout" --> work["its work"] --> sleep
       sleep -- "wake_fd" --> woken{"awl_thread_woken():<br/>quit set?"}
       woken -- "no: a request" --> work
       woken -- yes --> ret(["returns"])
     end
     subgraph M["main thread: awl_thread_stop(t, timeout)"]
       stop["quit = 1<br/>write wake_fd"] --> join{"exited within<br/>the timeout?"}
       join -- yes --> freed["wake_fd closed,<br/>state freed"]
       join -- "no: stuck in the kernel" --> left["detached,<br/>state and code kept"]
     end
     stop -.-> woken
     ret -.-> join

The poller
----------

One PulseAudio main loop drives all status plugins. The 1 s tick is aligned
to the wall-clock second, so the clock never lags; battery and IP address
only wake the loop when the kernel reports a change, with a slow re-read on
the tick as a safety net.

.. mermaid::

   flowchart TD
     run["poller_run()<br/>pa_mainloop_run()"] --> wait{"next event"}

     wait -- "1 s tick" --> fresh{"fresh?<br/>(just resumed)"}
     fresh -- yes --> all["update_all():<br/>every plugin re-reads"]
     fresh -- no --> tick["stats, temperature, clock<br/>battery every 60th tick<br/>IP every 5th, if without netlink"]
     all --> req1["awl_host->redraw_request()"]
     tick --> req1
     req1 --> rearm["re-arm for the next<br/>full second"] --> wait

     wait -- "uevent fd" --> bat["bat_dispatch()"] --> chg1{"changed and<br/>not paused?"}
     wait -- "rtnetlink fd" --> ip["ip_dispatch()"] --> chg1
     chg1 -- yes --> req2["awl_host->redraw_request()"] --> wait
     chg1 -- no --> wait

     wait -- "PulseAudio, D-Bus" --> other["pulsetest.c, backlight.c<br/>event sources"] --> wait

     wait -- "wake_fd" --> woken{"awl_thread_woken():<br/>quit?"}
     woken -- yes --> quit["quit the loop,<br/>the thread returns"]
     woken -- no --> sync["sync_paused()"]
     sync -- "paused" --> stoptick["free the tick"] --> wait
     sync -- "resumed" --> starttick["new tick, due now,<br/>fresh = 1"] --> wait

Pausing comes from the main thread, whenever no bar is visible (locked, or
every output off):

.. mermaid::

   sequenceDiagram
     participant M as main thread
     participant P as poller thread
     M->>M: updatepluginpause()
     M->>P: awl_plugins_set_paused(1)<br/>poller_set_paused(): paused = 1, wake
     P->>P: wake_callback(): sync_paused()<br/>frees the tick
     Note over P: only battery and IP events<br/>still wake it, without redraws
     M->>P: awl_plugins_set_paused(0)<br/>paused = 0, wake
     P->>P: sync_paused(): new tick, due now
     P->>P: tick: update_all()
     P-->>M: awl_host->redraw_request()

A wallpaper change
------------------

Requests are counters, so several in a row cost one change, and
`awl_wallpaper_settled()` can tell whether the wallpaper shown is the one
all of them end on.

.. mermaid::

   sequenceDiagram
     participant M as main thread
     participant W as wallpaper thread
     M->>W: awl_wallpaper_step(+1)<br/>steps += 1, asked += 1, wake
     W->>W: takes asked, then steps, random, back
     W->>W: change(): lists the directory,<br/>picks, sets wallpaper_index,<br/>where = (cur, n, rand_next), done = asked
     W->>W: decode() the PNG (Wuffs)
     W->>W: pending = image<br/>(frees one not taken yet)
     W-->>M: awl_host->redraw_request()
     M->>M: redraw_fire(): wallpapertake()
     M->>M: awl_wallpaper_take(): pending = NULL
     M->>M: background_set(): fade in
     Note over M: settled while done == asked:<br/>then plugins.c tells "x/N"

The desktop scanner
-------------------

.. mermaid::

   sequenceDiagram
     participant M as main thread
     participant S as desktop thread
     S->>S: inotify on desktop_dir<br/>(retried every 5 s while missing)
     Note over S: an event: wait 50 ms for more,<br/>500 ms at most
     S->>S: findfiles(): the list
     S->>S: pending = copy, if it changed
     S-->>M: awl_host->redraw_request()
     M->>M: desktop_update_all()
     M->>M: awl_desktop_version(): takes pending,<br/>version + 1 if it differs
     M->>M: redraws the panels whose version changed

Locking before suspend
----------------------

The logind thread holds a delay inhibitor whenever config_awl.h's
``lock_on_suspend`` is set, so logind sends ``PrepareForSleep(true)`` and then
waits, up to its ``InhibitDelayMaxSec`` (5 s by default), for the inhibitor's
fd to be closed. "Covered" means every enabled output has shown a frame with
the locker's surface (`lockcovered()`), or ``LOCK_COVER_MS`` passed.

.. mermaid::

   sequenceDiagram
     participant L as logind
     participant T as logind thread
     participant M as main thread
     participant K as locker
     T->>L: Inhibit(sleep, delay) at start and after each resume
     L-->>T: inhibitor fd
     L->>T: PrepareForSleep(true)
     T-->>M: awl_host->lock_request()
     M->>M: lockfire(): lockscreen()
     M->>K: spawn config_awl.h's locker
     K->>M: ext-session-lock: lock, surfaces
     M->>M: locksession(), then rendermon():<br/>a frame with its surface on every output
     M->>T: lockcovered(): session_covered(1), wake
     T->>L: close(inhibitor fd), SetLockedHint(true)
     Note over L: suspends
     L->>T: PrepareForSleep(false)
     T->>L: Inhibit(sleep, delay)

``loginctl lock-session`` takes the first half of this: the session's
``Lock`` signal, ``lock_request()``, and the locker. A request while a lock
is in place, or while the locker started last hasn't locked yet, does
nothing.

The thread tells awl what it listens for (`awl_logind_flags`), on every
change. While that is less than awl needs (``Lock``, plus ``PrepareForSleep``
with an inhibitor if ``lock_on_suspend``) for ``LOGIND_GRACE_MS``, awl raises
the logind alert: a red box in every output's bottom left corner and a
critical notification.
That covers a thread that never reports at all, too, e.g. without a plugin
library; a plugin reload or a reconnect stays within the grace period.
