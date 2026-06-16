#pragma once
#include <stddef.h>

/* Freestanding mini-libc string/mem helpers (libc/string.c). */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);  /* always NUL-terminates d[n-1] */
char  *strcat(char *d, const char *s);
char  *kstrlcat(char *d, const char *s, size_t cap);  /* cap = total size of d (strlcat-style) */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
int    atoi(const char *s);
