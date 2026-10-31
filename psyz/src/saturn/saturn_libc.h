// Saturn backend: libc prototypes the libyaul headers lack. libyaul 6012f79
// libc/libc/stdlib.h declares strtol/strtoul but not getenv/strtoull, so core
// libgpu.c (:73, :209) called them implicitly (int return, LTO type mismatch).
// Definitions: saturn_misc.c. Force-included by saturn.mk (-include), so core
// sources see the prototypes without edits; declarations only, no stubs.
#ifndef PSYZ_SATURN_LIBC_H
#define PSYZ_SATURN_LIBC_H
char* getenv(const char* name);
unsigned long long strtoull(const char* s, char** end, int base);
#endif
