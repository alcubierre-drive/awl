#pragma once

#include <stdatomic.h>

/** The battery's charge and state. */
typedef struct awl_battery_t {
    _Atomic float charge;
    /** -1: invalid */
    atomic_int charging;
    /** 1: a Thunderbolt dock is plugged in and authorized, 0:
     * none, -1: no Thunderbolt bus */
    atomic_int docked;

    /** kernel uevents (netlink), -1 if unavailable */
    int uevent_fd;
} awl_battery_t;

awl_battery_t* bat_init( void );
/** Re-reads the battery; returns nonzero if charge or state changed. */
int bat_update( awl_battery_t* b );
/** Re-reads whether a dock is attached; returns nonzero if that changed. */
int bat_dock_update( awl_battery_t* b );
/** Call when ``uevent_fd`` is readable. Drains it and re-reads the battery if any
 * power_supply device (battery or AC adapter) changed, the dock if any
 * thunderbolt device did; returns nonzero if either changed. */
int bat_dispatch( awl_battery_t* b );
void bat_free( awl_battery_t* b );
