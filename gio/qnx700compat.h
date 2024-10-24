#warning "QNX700 compact included!"
#undef AT_FDCWD
#define openat(fd, path, flags) open(path, flags)
