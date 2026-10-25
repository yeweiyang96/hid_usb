#ifndef TEST_PICO_CRITICAL_SECTION_H
#define TEST_PICO_CRITICAL_SECTION_H

/* Host-only replacement for the Pico lock primitive. All bridge operations run
 * unchanged against real exclusive Windows locks. This verifies cross-thread
 * exclusion and state transitions, not RP2350 IRQ masking or IRQ timing. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
    SRWLOCK lock;
} critical_section_t;

static inline void critical_section_init(critical_section_t *section) {
    InitializeSRWLock(&section->lock);
}

static inline void critical_section_enter_blocking(critical_section_t *section) {
    AcquireSRWLockExclusive(&section->lock);
}

static inline void critical_section_exit(critical_section_t *section) {
    ReleaseSRWLockExclusive(&section->lock);
}

#endif
