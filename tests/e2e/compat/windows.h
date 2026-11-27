#pragma once
#ifdef _WIN32
#error "tests/e2e/compat/windows.h must never be included on _WIN32"
#endif

#include <stdint.h>
#include <time.h>

#ifndef BOOL
typedef int BOOL;
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef union _LARGE_INTEGER {
	struct {
		uint32_t LowPart;
		int32_t HighPart;
	};
	int64_t QuadPart;
} LARGE_INTEGER;

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) {
	if (lpFrequency)
		lpFrequency->QuadPart = 1000000000LL;
	return TRUE;
}

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
	struct timespec ts;
	if (lpPerformanceCount) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		lpPerformanceCount->QuadPart =
		    (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
	}
	return TRUE;
}

#ifndef sscanf_s
#define sscanf_s sscanf
#endif
