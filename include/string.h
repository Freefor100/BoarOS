#ifndef BOAROS_LIB_STRING_H
#define BOAROS_LIB_STRING_H

#include <stddef.h>

int memcmp(const void *left, const void *right, size_t size);
void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
int strcmp(const char *left, const char *right);
char *strcpy(char *destination, const char *source);
size_t strlen(const char *text);
int strncmp(const char *left, const char *right, size_t size);
char *strncpy(char *destination, const char *source, size_t size);

#endif
