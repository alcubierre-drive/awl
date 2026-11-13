#pragma once

#include <stdbool.h>
#include <stdint.h>

/** A directory entry. */
typedef struct {
    char name[127];
    bool isdir:1;
    bool isbroken:1;
    bool ishidden:1;
} Filename;

/** A directory's entries, the first 128. */
typedef struct {
    Filename files[128];
    int n_files;
} DesktopFiles;

/** fills wp from path's entries; returns whether they have changed */
int findfiles( DesktopFiles* wp, const char* path );
