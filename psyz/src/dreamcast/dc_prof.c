// Sampling profiler: TMU1 interrupts 1000 times a second and counts the
// interrupted program counter in 128-byte buckets of the code. DcProf_Report
// logs the busiest buckets ("prof:" lines); `sh-elf-nm -n` on the ELF names
// the functions. It costs about one interrupt per millisecond.
#include <psyz/log.h>
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
#define TOP 32

extern char etext[]; // end of .text, from the KOS linker script

static uint16_t* buckets;
static uint32_t bucket_count;
static volatile uint32_t samples, outside;

static void Sample(irq_t code, irq_context_t* ctx, void* data) {
    (void)code;
    (void)data;
    timer_clear(TMU1);
    uint32_t i = (ctx->pc - CODE_START) >> BUCKET_SHIFT;
    if (i < bucket_count) {
        if (buckets[i] != 0xFFFF) {
            buckets[i]++;
        }
    } else {
        outside++;
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
    if (!buckets) {
        return;
    }
    irq_set_handler(EXC_TMU1_TUNI1, Sample, NULL);
    timer_prime(TMU1, SAMPLE_HZ, 1);
    timer_start(TMU1);
}

void DcProf_Report(void) {
    if (!buckets || !samples) {
        return;
    }
    irq_disable_scoped();
    uint32_t total = samples, out = outside;
    struct {
        uint32_t index, count;
    } top[TOP];
    int n = 0;
    for (uint32_t i = 0; i < bucket_count; i++) {
        uint32_t c = buckets[i];
        if (!c) {
            continue;
        }
        int at = n < TOP ? n : TOP;
        while (at > 0 && top[at - 1].count < c) {
            at--;
        }
        if (at >= TOP) {
            continue;
        }
        int last = n < TOP ? n : TOP - 1;
        memmove(&top[at + 1], &top[at], (size_t)(last - at) * sizeof(top[0]));
        top[at].index = i;
        top[at].count = c;
        if (n < TOP) {
            n++;
        }
    }
    memset(buckets, 0, bucket_count * sizeof(*buckets));
    samples = outside = 0;
    char line[512];
    int len = 0;
    for (int k = 0; k < n; k++) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %08lx:%lu",
                        (unsigned long)(CODE_START +
                                        (top[k].index << BUCKET_SHIFT)),
                        (unsigned long)top[k].count);
        if (len > 400 || k == n - 1) {
            INFOF("prof: %lu samples, %lu outside code;%s",
                  (unsigned long)total, (unsigned long)out, line);
            len = 0;
        }
    }
}
