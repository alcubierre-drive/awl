#pragma once

/* Wuffs (https://github.com/google/wuffs, Apache-2.0, see wuffs-v0.3.c),
 * v0.3.5's single-file release, unmodified; only its PNG decoder and what
 * that needs. wuffs.c compiles the implementation into libawlplugins.so. */

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs-v0.3.c"
