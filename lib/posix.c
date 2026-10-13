#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uv.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "directory.h"

// Names are single components, so refusing a link as the last component is
// enough where O_NOFOLLOW_ANY isn't available.
#if defined(__APPLE__)
#define BARE_DIRECTORY_NOFOLLOW O_NOFOLLOW_ANY
#else
#define BARE_DIRECTORY_NOFOLLOW O_NOFOLLOW
#endif

#if defined(__APPLE__)
#define BARE_DIRECTORY_ATIME(st) ((st).st_atimespec)
#define BARE_DIRECTORY_MTIME(st) ((st).st_mtimespec)
#define BARE_DIRECTORY_CTIME(st) ((st).st_ctimespec)
#else
#define BARE_DIRECTORY_ATIME(st) ((st).st_atim)
#define BARE_DIRECTORY_MTIME(st) ((st).st_mtim)
#define BARE_DIRECTORY_CTIME(st) ((st).st_ctim)
#endif

// openat2() also confines opens in the kernel, in case an invalid name gets
// through. Android kills apps that call it, so it's never used there.
#if defined(__linux__) && !defined(__ANDROID__) && defined(SYS_openat2)
#define BARE_DIRECTORY_OPENAT2

#define BARE_DIRECTORY_RESOLVE_NO_MAGICLINKS 0x02
#define BARE_DIRECTORY_RESOLVE_NO_SYMLINKS   0x04
#define BARE_DIRECTORY_RESOLVE_BENEATH       0x08

typedef struct {
  uint64_t flags;
  uint64_t mode;
  uint64_t resolve;
} bare_directory_open_how_t;

static atomic_bool bare_directory__openat2_unsupported = false;
#endif

static int
bare_directory__error(void) {
  return uv_translate_sys_error(errno);
}

static int
bare_directory__type(mode_t mode) {
  if (S_ISREG(mode)) return bare_directory_type_file;
  if (S_ISDIR(mode)) return bare_directory_type_directory;
  if (S_ISLNK(mode)) return bare_directory_type_symlink;

  return bare_directory_type_other;
}

static int64_t
bare_directory__time(struct timespec ts) {
  return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void
bare_directory__to_stat(const struct stat *st, bare_directory_stat_t *result) {
  result->type = bare_directory__type(st->st_mode);
  result->size = st->st_size;
  result->ino = st->st_ino;
  result->nlink = st->st_nlink;
  result->atime = bare_directory__time(BARE_DIRECTORY_ATIME(*st));
  result->mtime = bare_directory__time(BARE_DIRECTORY_MTIME(*st));
  result->ctime = bare_directory__time(BARE_DIRECTORY_CTIME(*st));
}

static int
bare_directory__openat(int dir, const char *name, int flags, mode_t mode) {
  flags |= O_CLOEXEC | O_NOCTTY | BARE_DIRECTORY_NOFOLLOW;

  int fd;

#if defined(BARE_DIRECTORY_OPENAT2)
  if (!atomic_load(&bare_directory__openat2_unsupported)) {
    bare_directory_open_how_t how = {
      .flags = flags,
      .mode = (flags & O_CREAT) ? mode : 0,
      .resolve = BARE_DIRECTORY_RESOLVE_BENEATH | BARE_DIRECTORY_RESOLVE_NO_SYMLINKS | BARE_DIRECTORY_RESOLVE_NO_MAGICLINKS,
    };

    do {
      fd = syscall(SYS_openat2, dir, name, &how, sizeof(how));
    } while (fd == -1 && errno == EINTR);

    if (fd != -1 || errno != ENOSYS) return fd;

    atomic_store(&bare_directory__openat2_unsupported, true);
  }
#endif

  do {
    fd = openat(dir, name, flags, mode);
  } while (fd == -1 && errno == EINTR);

  return fd;
}

int
bare_directory_open(const char *path, bare_directory_fd_t *result) {
  int fd;

  do {
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (fd == -1 && errno == EINTR);

  if (fd == -1) return bare_directory__error();

  *result = fd;

  return 0;
}

int
bare_directory_open_directory(bare_directory_fd_t dir, const char *name, bare_directory_fd_t *result) {
  int fd = bare_directory__openat(dir, name, O_RDONLY | O_DIRECTORY, 0);

  if (fd == -1) {
    int err = bare_directory__error();

    // Linux reports a link as ENOTDIR when a directory is asked for.
    struct stat st;

    if (err == UV_ENOTDIR && fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
      err = UV_ELOOP;
    }

    return err;
  }

  *result = fd;

  return 0;
}

int
bare_directory_open_file(bare_directory_fd_t dir, const char *name, int flags, bare_directory_fd_t *result) {
  int err;

  // Opening a device or FIFO can have side effects, so the entry is checked
  // before opening and again after.
  struct stat before;
  err = fstatat(dir, name, &before, AT_SYMLINK_NOFOLLOW);

  bool exists = err == 0;

  if (exists) {
    if (S_ISDIR(before.st_mode)) return UV_EISDIR;
    if (S_ISLNK(before.st_mode)) return UV_ELOOP;
    if (!S_ISREG(before.st_mode)) return UV_ENOTSUP;
  } else if (errno != ENOENT || !(flags & bare_directory_open_create)) {
    return bare_directory__error();
  }

  int mode;

  if ((flags & bare_directory_open_read) && (flags & (bare_directory_open_write | bare_directory_open_append))) {
    mode = O_RDWR;
  } else if (flags & (bare_directory_open_write | bare_directory_open_append)) {
    mode = O_WRONLY;
  } else {
    mode = O_RDONLY;
  }

  if (flags & bare_directory_open_append) mode |= O_APPEND;
  if (flags & bare_directory_open_create) mode |= O_CREAT;
  if (flags & bare_directory_open_exclusive) mode |= O_EXCL;
  if (flags & bare_directory_open_truncate) mode |= O_TRUNC;

  int fd = bare_directory__openat(dir, name, mode | O_NONBLOCK, 0666);

  if (fd == -1) return bare_directory__error();

  struct stat after;
  err = fstat(fd, &after);

  if (err == -1 || !S_ISREG(after.st_mode) || (exists && (after.st_dev != before.st_dev || after.st_ino != before.st_ino))) {
    err = err == -1 ? bare_directory__error() : S_ISDIR(after.st_mode) ? UV_EISDIR
                                                                       : UV_ENOTSUP;

    close(fd);

    return err;
  }

  int status = fcntl(fd, F_GETFL);

  if (status == -1 || fcntl(fd, F_SETFL, status & ~O_NONBLOCK) == -1) {
    err = bare_directory__error();

    close(fd);

    return err;
  }

  *result = fd;

  return 0;
}

int
bare_directory_create_directory(bare_directory_fd_t dir, const char *name) {
  if (mkdirat(dir, name, 0777) == -1) return bare_directory__error();

  return 0;
}

int
bare_directory_stat(bare_directory_fd_t dir, const char *name, bare_directory_stat_t *result) {
  struct stat st;

  if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == -1) return bare_directory__error();

  bare_directory__to_stat(&st, result);

  return 0;
}

int
bare_directory_fstat(bare_directory_fd_t fd, bare_directory_stat_t *result) {
  struct stat st;

  if (fstat(fd, &st) == -1) return bare_directory__error();

  bare_directory__to_stat(&st, result);

  return 0;
}

int
bare_directory_list(bare_directory_fd_t dir, bare_directory_list_cb cb, void *data) {
  int err;

  // The stream takes over its descriptor and offset, so it gets its own.
  int fd = bare_directory__openat(dir, ".", O_RDONLY | O_DIRECTORY, 0);

  if (fd == -1) return bare_directory__error();

  DIR *stream = fdopendir(fd);

  if (stream == NULL) {
    err = bare_directory__error();

    close(fd);

    return err;
  }

  err = 0;

  for (;;) {
    errno = 0;

    struct dirent *entry = readdir(stream);

    if (entry == NULL) {
      if (errno != 0) err = bare_directory__error();

      break;
    }

    const char *name = entry->d_name;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

    int type;

    switch (entry->d_type) {
    case DT_REG:
      type = bare_directory_type_file;
      break;
    case DT_DIR:
      type = bare_directory_type_directory;
      break;
    case DT_LNK:
      type = bare_directory_type_symlink;
      break;
    case DT_UNKNOWN: {
      struct stat st;

      if (fstatat(dirfd(stream), name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
        err = bare_directory__error();

        goto done;
      }

      type = bare_directory__type(st.st_mode);
      break;
    }
    default:
      type = bare_directory_type_other;
    }

    err = cb(name, type, data);

    if (err != 0) break;
  }

done:
  closedir(stream);

  return err;
}

int
bare_directory_remove(bare_directory_fd_t dir, const char *name, bool directory) {
  if (unlinkat(dir, name, directory ? AT_REMOVEDIR : 0) == -1) return bare_directory__error();

  return 0;
}

int
bare_directory_rename(bare_directory_fd_t from, const char *name, bare_directory_fd_t to, const char *target, bool replace) {
  int err;

  if (replace) {
    err = renameat(from, name, to, target);
  } else {
#if defined(__APPLE__)
    err = renameatx_np(from, name, to, target, RENAME_EXCL);
#elif defined(__linux__)
    err = syscall(SYS_renameat2, from, name, to, target, 1 /* RENAME_NOREPLACE */);
#else
#error "Unsupported platform"
#endif
  }

  if (err == -1) return bare_directory__error();

  return 0;
}

int
bare_directory_read_link(bare_directory_fd_t dir, const char *name, char *buffer, size_t *len) {
  ssize_t n = readlinkat(dir, name, buffer, *len);

  if (n == -1) return bare_directory__error();

  if ((size_t) n == *len) return UV_ENAMETOOLONG;

  *len = n;

  return 0;
}

int
bare_directory_read(bare_directory_fd_t fd, void *buffer, size_t len, uint64_t position, size_t *result) {
  ssize_t n;

  do
    n = pread(fd, buffer, len, position);
  while (n == -1 && errno == EINTR);

  if (n == -1) return bare_directory__error();

  *result = n;

  return 0;
}

int
bare_directory_write(bare_directory_fd_t fd, const void *buffer, size_t len, uint64_t position, size_t *result) {
  ssize_t n;

  do
    n = pwrite(fd, buffer, len, position);
  while (n == -1 && errno == EINTR);

  if (n == -1) return bare_directory__error();

  *result = n;

  return 0;
}

int
bare_directory_append(bare_directory_fd_t fd, const void *buffer, size_t len, size_t *result) {
  ssize_t n;

  do
    n = write(fd, buffer, len);
  while (n == -1 && errno == EINTR);

  if (n == -1) return bare_directory__error();

  *result = n;

  return 0;
}

int
bare_directory_truncate(bare_directory_fd_t fd, uint64_t size) {
  int err;

  do
    err = ftruncate(fd, size);
  while (err == -1 && errno == EINTR);

  if (err == -1) return bare_directory__error();

  return 0;
}

int
bare_directory_sync(bare_directory_fd_t fd) {
  if (fsync(fd) == -1) return bare_directory__error();

  return 0;
}

static struct timespec
bare_directory__timespec(const int64_t *time) {
  if (time == NULL) return (struct timespec){.tv_nsec = UTIME_OMIT};

  int64_t sec = *time / 1000000000;
  int64_t nsec = *time % 1000000000;

  if (nsec < 0) {
    sec -= 1;
    nsec += 1000000000;
  }

  return (struct timespec){.tv_sec = sec, .tv_nsec = nsec};
}

int
bare_directory_set_times(bare_directory_fd_t fd, const int64_t *atime, const int64_t *mtime) {
  struct timespec times[2] = {bare_directory__timespec(atime), bare_directory__timespec(mtime)};

  if (futimens(fd, times) == -1) return bare_directory__error();

  return 0;
}

int
bare_directory_duplicate(bare_directory_fd_t fd, bare_directory_fd_t *result) {
  int dup = fcntl(fd, F_DUPFD_CLOEXEC, 0);

  if (dup == -1) return bare_directory__error();

  *result = dup;

  return 0;
}

void
bare_directory_close(bare_directory_fd_t fd) {
  close(fd);
}
