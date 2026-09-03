#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// Freestanding replacements for the handful of libc routines the kernel
// needs. The mem*/str* names are deliberate: GCC lowers struct assignments
// and array initialisations into calls to memcpy/memset even with
// -ffreestanding, so these symbols have to exist under exactly these names
// for the kernel to link at all.
void*  memset(void* dst, int c, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);

size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strchr(const char* s, int c);
char*  strstr(const char* hay, const char* needle);

int    kstricmp(const char* a, const char* b);
int    kstrnicmp(const char* a, const char* b, size_t n);

// Bounded formatter supporting %s %c %d %u %x %p and %% — enough for status
// lines, URLs and protocol headers. Always NUL-terminates.
int    ksnprintf(char* buf, size_t cap, const char* fmt, ...);
int    kvsnprintf(char* buf, size_t cap, const char* fmt, va_list ap);
