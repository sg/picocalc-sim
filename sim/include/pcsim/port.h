#pragma once

// POSIX file calls that the Windows C runtime lacks or spells differently; runtime.c supplies the
// missing ones.

#include <stdint.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>

ssize_t pread(int fd, void *buf, size_t len, int64_t off);
ssize_t pwrite(int fd, const void *buf, size_t len, int64_t off);
#define fsync _commit
#define pcsim_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>

#define O_BINARY 0
#define pcsim_mkdir(path) mkdir(path, 0755)
#endif
