#pragma once

#include <stdbool.h>
#include <time.h>

// Kept apart from FatFs, whose DIR type collides with POSIX <dirent.h>.

typedef enum { WALK_FILE, WALK_DIR_ENTER, WALK_DIR_LEAVE } walk_event_t;

typedef int (*walk_fn)(void *ctx, walk_event_t event, const char *host_path, const char *rel_path, time_t mtime);

// Visits everything under host_dir, giving each entry its path relative to the root as rel_path.
int walk_tree(const char *host_dir, const char *rel_dir, walk_fn fn, void *ctx);
