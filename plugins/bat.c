#include "bat.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/socket.h>
#include <linux/netlink.h>

static const char tb_devices[] = "/sys/bus/thunderbolt/devices";

/* The kernel sends a uevent whenever a power_supply device changes: the AC
 * adapter on plug/unplug, the battery on status changes and (on most
 * firmware) on every capacity step. These are the same events udev reads. */
static int uevent_open( void ) {
    int fd = socket( AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT );
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 /* kernel */ };
    if (bind( fd, (struct sockaddr*)&sa, sizeof sa ) < 0) {
        close( fd );
        return -1;
    }
    return fd;
}

awl_battery_t* bat_init( const char* dir, const char* ac_dir ) {
    awl_battery_t* b = calloc(1, sizeof(awl_battery_t));
    snprintf( b->dir, sizeof(b->dir), "%s", dir ? dir : "" );
    snprintf( b->ac_dir, sizeof(b->ac_dir), "%s", ac_dir ? ac_dir : "" );
    atomic_init( &b->charge, 0 );
    atomic_init( &b->charging, -1 );
    atomic_init( &b->docked, -1 );
    atomic_init( &b->ac, -1 );
    b->uevent_fd = uevent_open();
    return b;
}

void bat_free( awl_battery_t* b ) {
    if (b->uevent_fd >= 0) close( b->uevent_fd );
    free(b);
}

int bat_dispatch( awl_battery_t* b ) {
    int power = 0, tb = 0;
    char buf[8192];
    ssize_t n;
    // "action@devpath\0KEY=VALUE\0KEY=VALUE\0..."
    while ((n = recv( b->uevent_fd, buf, sizeof(buf)-1, 0 )) > 0) {
        buf[n] = '\0';
        for (char* kv = buf; kv < buf+n; kv += strlen(kv)+1)
            if (!strcmp( kv, "SUBSYSTEM=power_supply" )) power = 1;
            else if (!strcmp( kv, "SUBSYSTEM=thunderbolt" )) tb = 1;
    }
    // ENOBUFS: we lost events, so re-read to be safe
    if (n < 0 && errno == ENOBUFS) power = tb = 1;
    int changed = power ? bat_update( b ) : 0;
    if (tb) changed |= bat_dock_update( b );
    return changed;
}

/** reads up to n-1 bytes of a Thunderbolt device's attribute file into buf;
 * empty if it can't */
static void tb_attr( const char* dev, const char* attr, char* buf, size_t n ) {
    char path[sizeof(tb_devices) + 512];
    snprintf( path, sizeof(path), "%s/%s/%s", tb_devices, dev, attr );
    *buf = '\0';
    FILE* f = fopen( path, "r" );
    if (!f) return;
    size_t r = fread( buf, 1, n-1, f );
    buf[r] = '\0';
    fclose( f );
}

/* A device on the Thunderbolt bus is "<domain>-<route>", route 0 being the
 * host's own controller; one whose name says it's a dock, authorized, is
 * plugged in and in use. Names with a colon (retimers, XDomain services) and
 * the domains aren't devices. */
int bat_dock_update( awl_battery_t* b ) {
    int docked = -1;
    DIR* d = opendir( tb_devices );
    if (d) {
        docked = 0;
        struct dirent* e;
        while (!docked && (e = readdir( d ))) {
            unsigned domain;
            unsigned long long route;
            int len = 0;
            if (sscanf( e->d_name, "%u-%llx%n", &domain, &route, &len ) != 2 ||
                e->d_name[len] || !route)
                continue;
            char name[128], authorized[16];
            tb_attr( e->d_name, "device_name", name, sizeof(name) );
            if (!strstr( name, "Dock" ) && !strstr( name, "dock" )) continue;
            tb_attr( e->d_name, "authorized", authorized, sizeof(authorized) );
            docked = atoi( authorized ) > 0;
        }
        closedir( d );
    }
    return atomic_exchange( &b->docked, docked ) != docked;
}

int bat_update( awl_battery_t* b ) {
    char bat_file[sizeof(b->dir) + 16];

    FILE* f = NULL;
    unsigned long u = 0;
    int set = -3;

    float charge = 0.0;
    int charging = 0;
    snprintf( bat_file, sizeof(bat_file), "%senergy_now", b->dir );
    if ((f = fopen( bat_file, "r" ))) {
        if (fscanf(f, "%lu", &u) == 1) charge = u;
        fclose(f);
        set++;
    }
    snprintf( bat_file, sizeof(bat_file), "%senergy_full", b->dir );
    if ((f = fopen( bat_file, "r" ))) {
        if (fscanf(f, "%lu", &u) == 1) charge /= (u > 0) ? (float)u : 1.0;
        fclose(f);
        set++;
    }
    snprintf( bat_file, sizeof(bat_file), "%sstatus", b->dir );
    if ((f = fopen( bat_file, "r" ))) {
        memset( bat_file, 0, sizeof(bat_file) );
        if (fread( bat_file, 1, sizeof(bat_file)-1, f )) {}
        fclose(f);
        if (strstr(bat_file, "Charging")) charging = 1;
        set++;
    }

    if (set) charging = -1;

    int ac = -1;
    snprintf( bat_file, sizeof(bat_file), "%sonline", b->ac_dir );
    if (*b->ac_dir && (f = fopen( bat_file, "r" ))) {
        if (fscanf(f, "%d", &ac) != 1) ac = -1;
        fclose(f);
    }

    int changed = atomic_exchange( &b->charging, charging ) != charging;
    changed |= atomic_exchange( &b->charge, charge ) != charge;
    changed |= atomic_exchange( &b->ac, ac ) != ac;
    return changed;
}
