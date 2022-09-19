#pragma once

#define try(_expr, _act)                                                       \
    {                                                                          \
        if ((_expr) < 0) {                                                     \
            printf("try: %s failed, at line %d\n", #_expr, __LINE__); \
            _act;                                                              \
        }                                                                      \
    }

#ifdef FDEBUG
#define DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...)
#endif

