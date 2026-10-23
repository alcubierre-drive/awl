#include "temp.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static int read_line( const char* path, char* buf, size_t n ) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, n, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\n")] = 0;
    return ok;
}

int temp_find_hwmon( const char* name, const char* label, char* out, size_t n ) {
    // hwmon numbering is not stable across boots, so match by name and label
    DIR* d = opendir("/sys/class/hwmon");
    if (!d) return 0;
    int found = 0;
    struct dirent* e;
    char path[512], buf[64];
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5)) continue;
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", e->d_name);
        if (!read_line(path, buf, sizeof(buf)) || strcmp(buf, name)) continue;
        for (int i=1; i<64 && !found; ++i) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
            if (!read_line(path, buf, sizeof(buf)) || strncmp(buf, label, strlen(label))) continue;
            found = snprintf(out, n, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i) < (int)n;
        }
    }
    closedir(d);
    return found;
}

void temp_init( awl_temperature_t* temp ) {
    for (int i=0; i<16; ++i)
        atomic_init( &temp->temps[i], AWL_TEMP_NONE );
}

int temp_update( awl_temperature_t* temp ) {
    int changed = 0;
    for (int i=0; i<temp->f_ntemps; ++i) {
        float t = AWL_TEMP_NONE;
        FILE* f = fopen(temp->f_files[i], "r");
        if (f) {
            long u = 0;
            if (fscanf(f, "%li", &u) == 1) t = (float)u/(float)1000.0;
            fclose(f);
        }
        changed |= atomic_exchange( &temp->temps[i], t ) != t;
    }
    return changed;
}
