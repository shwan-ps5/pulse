// libc functions the console's libc does not export but the GL stack reaches (ps5-opengl's
// runtime_shims.c already supplies __assert).
//
// Mesa's shader lexer, a ps5-opengl screen-dump path and libc++'s printing reach these, and the
// console's libc exports neither: the import would jump to address 0. There is no terminal, and
// no temporary-file support to offer.
#include <errno.h>

int isatty(int fd) {
  (void)fd;
  errno = ENOTTY;
  return 0;
}

int mkstemp(char *path) {
  (void)path;
  errno = ENOSYS;
  return -1;
}
