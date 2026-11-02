#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "walk.h"

int walk_tree(const char *host_dir, const char *rel_dir, walk_fn fn, void *ctx)
{
    DIR *d = opendir(host_dir);
    if (!d) {
        fprintf(stderr, "mkcard: %s: %s\n", host_dir, strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char host_path[4096], rel_path[1024];
        snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, e->d_name);
        snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir, e->d_name);
        struct stat st;
        if (stat(host_path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (fn(ctx, WALK_DIR_ENTER, host_path, rel_path, st.st_mtime) != 0) {
                rc = -1;
                continue;
            }
            if (walk_tree(host_path, rel_path, fn, ctx) != 0)
                rc = -1;
            fn(ctx, WALK_DIR_LEAVE, host_path, rel_path, st.st_mtime);
        } else if (S_ISREG(st.st_mode)) {
            if (fn(ctx, WALK_FILE, host_path, rel_path, st.st_mtime) != 0)
                rc = -1;
        }
    }
    closedir(d);
    return rc;
}
