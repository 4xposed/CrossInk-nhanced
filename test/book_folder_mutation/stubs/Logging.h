#pragma once
#include <cstdio>
#define LOG_ERR(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
