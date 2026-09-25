#pragma once
#include <Arduino.h>

#include <cstdio>

// Print firmware logs to stderr so a failed prefetch job shows which step
// (admission, decode, fingerprint, cache) rejected it in CI output.
#define CROSSINK_TEST_LOG(level, tag, ...)          \
  do {                                              \
    std::fprintf(stderr, "[%s] [%s] ", level, tag); \
    std::fprintf(stderr, __VA_ARGS__);              \
    std::fputc('\n', stderr);                       \
  } while (0)
#define LOG_DBG(tag, ...) CROSSINK_TEST_LOG("DBG", tag, __VA_ARGS__)
#define LOG_INF(tag, ...) CROSSINK_TEST_LOG("INF", tag, __VA_ARGS__)
#define LOG_ERR(tag, ...) CROSSINK_TEST_LOG("ERR", tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) CROSSINK_TEST_LOG("WARN", tag, __VA_ARGS__)
