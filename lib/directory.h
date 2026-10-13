#ifndef BARE_DIRECTORY_H
#define BARE_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv.h>

// Functions return 0 or a negative libuv error code. Names have already passed
// bare_directory_valid_name().

typedef uv_os_fd_t bare_directory_fd_t;

enum {
  bare_directory_type_file = 1,
  bare_directory_type_directory = 2,
  bare_directory_type_symlink = 3,
  bare_directory_type_other = 4,
};

enum {
  bare_directory_open_read = 1,
  bare_directory_open_write = 2,
  bare_directory_open_append = 4,
  bare_directory_open_create = 8,
  bare_directory_open_exclusive = 16,
  bare_directory_open_truncate = 32,
};

typedef struct {
  int type;
  uint64_t size;
  uint64_t ino;
  uint64_t nlink;
  int64_t atime;
  int64_t mtime;
  int64_t ctime;
} bare_directory_stat_t;

typedef int (*bare_directory_list_cb)(const char *name, int type, void *data);

bool
bare_directory_valid_name(const char *name, size_t len);

int
bare_directory_open(const char *path, bare_directory_fd_t *result);

int
bare_directory_open_directory(bare_directory_fd_t dir, const char *name, bare_directory_fd_t *result);

int
bare_directory_open_file(bare_directory_fd_t dir, const char *name, int flags, bare_directory_fd_t *result);

int
bare_directory_create_directory(bare_directory_fd_t dir, const char *name);

int
bare_directory_stat(bare_directory_fd_t dir, const char *name, bare_directory_stat_t *result);

int
bare_directory_fstat(bare_directory_fd_t fd, bare_directory_stat_t *result);

int
bare_directory_list(bare_directory_fd_t dir, bare_directory_list_cb cb, void *data);

int
bare_directory_remove(bare_directory_fd_t dir, const char *name, bool directory);

int
bare_directory_rename(bare_directory_fd_t from, const char *name, bare_directory_fd_t to, const char *target, bool replace);

int
bare_directory_read_link(bare_directory_fd_t dir, const char *name, char *buffer, size_t *len);

int
bare_directory_read(bare_directory_fd_t fd, void *buffer, size_t len, uint64_t position, size_t *result);

int
bare_directory_write(bare_directory_fd_t fd, const void *buffer, size_t len, uint64_t position, size_t *result);

int
bare_directory_append(bare_directory_fd_t fd, const void *buffer, size_t len, size_t *result);

int
bare_directory_truncate(bare_directory_fd_t fd, uint64_t size);

int
bare_directory_sync(bare_directory_fd_t fd);

int
bare_directory_set_times(bare_directory_fd_t fd, const int64_t *atime, const int64_t *mtime);

int
bare_directory_duplicate(bare_directory_fd_t fd, bare_directory_fd_t *result);

void
bare_directory_close(bare_directory_fd_t fd);

#endif // BARE_DIRECTORY_H
