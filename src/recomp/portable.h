/* Small shims so the runtime builds with MinGW-w64 (no execinfo, no POSIX signals). */
#ifndef NEP_PORTABLE_H
#define NEP_PORTABLE_H
#ifdef _WIN32
#include <windows.h>
#include <unistd.h>      /* MinGW provides usleep */
static inline int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
static inline void backtrace_symbols_fd(void *const *b, int n, int fd) { (void)b; (void)n; (void)fd; }
#else
#include <execinfo.h>
#include <unistd.h>
#endif
#endif
