#define _GNU_SOURCE
#include "readdir.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int filename_sort( const void* f1_, const void* f2_ ) {
    const Filename *f1 = f1_, *f2 = f2_;
    if (f1->is_dir && !f2->is_dir) return 1;
    if (!f1->is_dir && f2->is_dir) return -1;
    return strncmp( f1->name, f2->name, 127 );
}

int find_files( DesktopFiles* wp, const char* path ) {
    Filename files[128] = {0};
    int n_files = 0;
    // fill files/n_files
    DIR *d = opendir(path);
    struct dirent *dir = NULL;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strlen(dir->d_name) > 0) {
                if (strcmp(dir->d_name, ".") && strcmp(dir->d_name, "..")) {
                    files[n_files].is_dir = dir->d_type == DT_DIR;
                    files[n_files].is_broken = dir->d_type == DT_UNKNOWN;
                    files[n_files].is_hidden = dir->d_name[0] == '.';
                    strncpy( files[n_files].name, dir->d_name, sizeof(files[n_files].name)-1 );
                    n_files++;
                    if ((unsigned int)n_files >= sizeof(files)/sizeof(files[0]))
                        goto closedir;
                }
            }
        }
closedir:
        closedir(d);
    }
    qsort( files, n_files, sizeof(Filename), filename_sort );

    // update files/n_files
    int update = 0;
    if (wp->n_files != n_files) {
        update = 1;
    } else if (memcmp(wp->files, files, sizeof(files))) {
        update = 1;
    }
    if (update) {
        memcpy(wp->files, files, sizeof(files));
        wp->n_files = n_files;
    }

    return update;
}


