// Sampling profiler: TMU1 interrupts 1000 times a second and counts the
// interrupted program counter in 128-byte buckets of the code (and, inside
// memcpy, memset and memmove, the caller's). DcProf_Report logs the busiest
// buckets ("prof:" lines); `sh-elf-nm -n` on the ELF names the functions. It costs about one interrupt per millisecond.
#include <psyz/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos/irq.h>
#include <arch/timer.h>
#include "dc_prof.h"

#define CODE_START 0x8c010000u
#define BUCKET_SHIFT 7
#define SAMPLE_HZ 1000
#define TOP 64
#define CALLER_TOP 16
#define LIB_SPAN 0x300 // bytes of memcpy/memset/memmove counted as theirs

extern char etext[]; // end of .text, from the KOS linker script

static uint16_t* buckets;
// for samples inside memcpy, memset or memmove: where they were called from
static uint16_t* callers;
static uint32_t bucket_count;
static volatile uint32_t samples, outside;

static inline bool InLib(uint32_t pc) {
    return pc - (uint32_t)memcpy < LIB_SPAN ||
           pc - (uint32_t)memset < LIB_SPAN ||
           pc - (uint32_t)memmove < LIB_SPAN;
}

static inline void Count(uint16_t* table, uint32_t pc) {
    uint32_t i = (pc - CODE_START) >> BUCKET_SHIFT;
    if (i < bucket_count && table[i] != 0xFFFF) {
        table[i]++;
    }
}

static void Sample(irq_t code, irq_context_t* ctx, void* data) {
    (void)code;
    (void)data;
    timer_clear(TMU1);
    uint32_t pc = ctx->pc;
    if (pc - CODE_START >= bucket_count << BUCKET_SHIFT) {
        outside++;
    } else {
        Count(buckets, pc);
        if (InLib(pc)) {
            Count(callers, ctx->pr);
        }
    }
    samples++;
}

void DcProf_Start(void) {
    if (buckets) {
        return;
    }
    bucket_count = ((uint32_t)etext - CODE_START + (1u << BUCKET_SHIFT) - 1) >>
                   BUCKET_SHIFT;
    buckets = calloc(bucket_count, sizeof(*buckets));
    callers = calloc(bucket_count, sizeof(*callers));
    if (!buckets || !callers) {
        free(buckets);
        free(callers);
        buckets = callers = NULL;
        return;
    }
    irq_set_handler(EXC_TMU1_TUNI1, Sample, NULL);
    timer_prime(TMU1, SAMPLE_HZ, 1);
    timer_start(TMU1);
}

// logs the `top` largest buckets of table, several to a line
static void Log(const char* what, const uint16_t* table, int top) {
    struct {
        uint32_t index, count;
    } best[TOP];
    int n = 0;
    for (uint32_t i = 0; i < bucket_count; i++) {
        uint32_t c = table[i];
        if (!c || (n == top && c <= best[n - 1].count)) {
            continue;
        }
        int at = n < top ? n : top - 1;
        while (at > 0 && best[at - 1].count < c) {
            best[at] = best[at - 1];
            at--;
        }
        best[at].index = i;
        best[at].count = c;
        if (n < top) {
            n++;
        }
    }
    char line[512];
    int len = 0;
    for (int k = 0; k < n; k++) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %08lx:%lu",
                        (unsigned long)(CODE_START +
                                        (best[k].index << BUCKET_SHIFT)),
                        (unsigned long)best[k].count);
        if (len > 400 || k == n - 1) {
            INFOF("%s%s", what, line);
            len = 0;
        }
    }
}

void DcProf_Report(void) {
    if (!buckets || !samples) {
        return;
    }
    irq_disable_scoped();
    INFOF("prof: %lu samples, %lu outside code",
          (unsigned long)samples, (unsigned long)outside);
    Log("prof:", buckets, TOP);
    Log("prof callers:", callers, CALLER_TOP);
    memset(buckets, 0, bucket_count * sizeof(*buckets));
    memset(callers, 0, bucket_count * sizeof(*callers));
    samples = outside = 0;
}
