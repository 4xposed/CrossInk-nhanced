#pragma once
#include <Arduino.h>

#include <cstdio>
#define LOG_ERR(tag, ...)         \
  do {                            \
    fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr);          \
  } while (0)
#define LOG_INF(tag, ...) \
  do {                    \
  } while (0)
