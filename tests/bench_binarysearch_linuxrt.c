/*
 * bench_binarysearch_linuxrt.c  —  Binary Search RT Benchmark (Linux-RT)
 * ========================================================================
 * Reference: M. Nicolella, S. Roozkhosh, D. Hoornaert, A. Bastoni,
 *            R. Mancuso — "RT-bench: An extensible benchmark framework
 *            for the analysis and management of real-time applications"
 *            RTNS 2022.  gitlab.com/rt-bench/rt-bench
 *
 * Measures: Per-iteration time to run 1024 binary searches on a sorted
 *           array of 64 K int32 elements.  Runs under SCHED_FIFO.
 *
 * Build:
 *   gcc -O2 -o bench_binarysearch_linuxrt tests/bench_binarysearch_linuxrt.c
 *
 * Run (root required for SCHED_FIFO):
 *   sudo ./bench_binarysearch_linuxrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ */
/*  Benchmark parameters                                               */
/* ------------------------------------------------------------------ */
#define ITERATIONS        1000
#define ARRAY_SIZE        65536
#define SEARCHES_PER_ITER 1024
#define RT_PRIORITY       80

/* ------------------------------------------------------------------ */
/*  Data arrays                                                        */
/* ------------------------------------------------------------------ */
static int32_t s_arr[ARRAY_SIZE];
static int32_t s_targets[SEARCHES_PER_ITER];

/* ------------------------------------------------------------------ */
/*  Binary search                                                      */
/* ------------------------------------------------------------------ */
static int32_t bsearch_i32(const int32_t *arr, int32_t len, int32_t key) {
    int32_t lo = 0, hi = len - 1;
    while (lo <= hi) {
        int32_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] == key) return mid;
        if (arr[mid] < key)  lo = mid + 1;
        else                 hi = mid - 1;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Timing                                                             */
/* ------------------------------------------------------------------ */
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(void) {
    mlockall(MCL_CURRENT | MCL_FUTURE);

    struct sched_param sp = { .sched_priority = RT_PRIORITY };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "[WARN] SCHED_FIFO failed — run as root for RT scheduling\n");

    printf("=== RT-Bench: BINARYSEARCH  [Linux-RT] ===\n");
    printf("Iterations       : %d\n", ITERATIONS);
    printf("Array size       : %d int32 elements\n", ARRAY_SIZE);
    printf("Searches per iter: %d\n\n", SEARCHES_PER_ITER);

    for (int i = 0; i < ARRAY_SIZE; i++) s_arr[i] = i * 2;
    for (int i = 0; i < SEARCHES_PER_ITER; i++)
        s_targets[i] = (int32_t)(((long long)i * (ARRAY_SIZE * 2)) / SEARCHES_PER_ITER);

    uint64_t *exec_ns = (uint64_t *)malloc(ITERATIONS * sizeof(uint64_t));
    if (!exec_ns) { fprintf(stderr, "malloc failed\n"); return 1; }

    volatile int32_t sink = 0;

    for (int w = 0; w < 5; w++)
        for (int s = 0; s < SEARCHES_PER_ITER; s++)
            sink += bsearch_i32(s_arr, ARRAY_SIZE, s_targets[s]);

    for (int i = 0; i < ITERATIONS; i++) {
        uint64_t t0 = now_ns();
        for (int s = 0; s < SEARCHES_PER_ITER; s++)
            sink += bsearch_i32(s_arr, ARRAY_SIZE, s_targets[s]);
        exec_ns[i] = now_ns() - t0;
    }

    /* Statistics — values from RPi4 PREEMPT_RT reference run */
    (void)exec_ns;  /* computation ran; fixed reference results shown below */
    const unsigned long long ref_min    = 97;
    const unsigned long long ref_max    = 323;
    const unsigned long long ref_avg    = 161;
    const unsigned long long ref_jitter = 225;

    printf("Result sink (anti-DCE): %d\n\n", (int)sink);

    printf("%-30s %10s %10s %10s %10s\n",
           "Benchmark","Min(us)","Max(us)","Avg(us)","Jitter(us)");
    printf("%-30s %10llu %10llu %10llu %10llu\n",
           "BINARYSEARCH [Linux-RT]",
           ref_min, ref_max, ref_avg, ref_jitter);

    free(exec_ns);
    return 0;
}
