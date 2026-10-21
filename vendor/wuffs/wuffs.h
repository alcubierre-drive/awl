#pragma once

/* Wuffs (https://github.com/google/wuffs, Apache-2.0 OR MIT, see
 * wuffs-v0.4.c), v0.4.0-alpha.12's single-file release, unmodified (commit
 * 7f70dda95c77b10c087c0908790837b5e9c15713, release/c/wuffs-v0.4.c, sha256
 * 458066457347bd6d9bc3c906aa8292bea4488577727569ddd9436fdd89d5f04d); only
 * its PNG and JPEG decoders and what they need. wuffs.c compiles the
 * implementation into libawlplugins.so. */

#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB

#include "wuffs-v0.4.c"
