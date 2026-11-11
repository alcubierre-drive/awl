#pragma once

#include <stdatomic.h>
#include <stddef.h>

/** The battery's charge and state. */
typedef struct awl_battery_t {
    _Atomic float charge;
    /** -1: invalid */
    atomic_int charging;
    /** 1: a Thunderbolt dock is plugged in and authorized, 0:
     * none, -1: no Thunderbolt bus */
    atomic_int docked;
    /** 1: the AC adapter is online, 0: offline, -1: unknown */
    atomic_int ac;

    /** kernel uevents (netlink), -1 if unavailable */
    int uevent_fd;
    /** the battery's and the AC adapter's sysfs directories, with a trailing '/' */
    char dir[256], ac_dir[256];
} awl_battery_t;

/** reads the battery in dir and the AC adapter in ac_dir
 * (:c:member:`awl_plugin_config_t.battery`) */
awl_battery_t* bat_init( const char* dir, const char* ac_dir );
/** Re-reads the battery and the AC adapter; returns nonzero if charge or
 * state changed. */
int bat_update( awl_battery_t* b );
/** Re-reads whether a dock is attached; returns nonzero if that changed. */
int bat_dock_update( awl_battery_t* b );
/** Call when ``uevent_fd`` is readable. Drains it and re-reads the battery if any
 * power_supply device (battery or AC adapter) changed, the dock if any
 * thunderbolt device did; returns nonzero if either changed. */
int bat_dispatch( awl_battery_t* b );
void bat_free( awl_battery_t* b );

/** The charge thresholds (charge_control_start_threshold and
 * charge_control_end_threshold in its directory), percent, into ``*start``
 * and ``*end``; -1 for one the battery doesn't have. Reads sysfs: main
 * thread, when needed. */
void bat_thresholds( const awl_battery_t* b, int* start, int* end );
/** Writes a threshold, end if end else start; returns 0 or an errno
 * (EACCES: the files are root's, see the udev rule in README.md's
 * Configuration). */
int bat_threshold_set( const awl_battery_t* b, int end, int percent );
/** the battery's status ("Charging", "Not charging", "Full"...) into buf,
 * "" if unknown */
void bat_status( const awl_battery_t* b, char* buf, size_t n );
