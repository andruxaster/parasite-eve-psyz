// Sega Dreamcast: host primitives behind the emulated PS1 kernel (os.h),
// on KallistiOS threads, mutexes and semaphores.
#include <psyz/log.h>
#include <stdlib.h>
#include <kos/mutex.h>
#include <kos/sem.h>
#include <kos/thread.h>
#include <kos/timer.h>
#include "../psyz/os.h"

// KOS runs lower numbers first; the game thread uses PRIO_DEFAULT.
#define HELPER_THREAD_PRIORITY (PRIO_DEFAULT - 1)

uint64_t Psyz_OsNowUs(void) { return timer_us_gettime64(); }

uintptr_t Psyz_OsThreadSelf(void) { return (uintptr_t)thd_get_current(); }

void* Psyz_OsLockCreate(void) {
    mutex_t* lock = malloc(sizeof(*lock));
    if (lock && mutex_init(lock, MUTEX_TYPE_NORMAL) != 0) {
        free(lock);
        lock = NULL;
    }
    return lock;
}

void Psyz_OsLock(void* lock) { mutex_lock((mutex_t*)lock); }

void Psyz_OsUnlock(void* lock) { mutex_unlock((mutex_t*)lock); }

void* Psyz_OsBellCreate(void) {
    semaphore_t* bell = malloc(sizeof(*bell));
    if (bell && sem_init(bell, 0) != 0) {
        free(bell);
        bell = NULL;
    }
    return bell;
}

// Auto-reset: a ring while nobody waits is kept once, not counted.
void Psyz_OsBellRing(void* bell) {
    semaphore_t* sem = bell;
    if (sem_count(sem) <= 0) {
        sem_signal(sem);
    }
}

void Psyz_OsBellWait(void* bell, uint32_t timeout_us) {
    semaphore_t* sem = bell;
    if (timeout_us == PSYZ_OS_FOREVER) {
        sem_wait(sem);
        return;
    }
    // KOS times out in milliseconds, and 0 means forever.
    unsigned int ms = (timeout_us + 999u) / 1000u;
    sem_wait_timed(sem, ms ? ms : 1);
}

static void* ThreadEntry(void* arg) {
    ((void (*)(void))arg)();
    return NULL;
}

int Psyz_OsThreadStart(void (*fn)(void), const char* name) {
    kthread_attr_t attr = {0};
    attr.create_detached = true;
    attr.prio = HELPER_THREAD_PRIORITY;
    attr.label = name;
    if (!thd_create_ex(&attr, ThreadEntry, (void*)fn)) {
        WARNF("cannot start thread %s", name);
        return -1;
    }
    return 0;
}
