// compat/string.h — the freestanding kernel has no libc, but third-party
// code (BearSSL) includes <string.h> for the handful of routines below.
// They are implemented in src/kstring.c under exactly these names.
#pragma once
#include <stddef.h>

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
