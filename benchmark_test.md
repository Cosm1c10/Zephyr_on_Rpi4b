# Sentinel-RT: QNX Neutrino 8.0 vs Linux-RT PREEMPT_RT — Benchmark Test Plan

**Project:** Edge-Based Industrial Equipment Health Dashboard (Sentinel-RT)
**Comparison:** QNX Neutrino 8.0 vs Linux with PREEMPT_RT patch (kernel 6.6-rt)
**Hardware:** Raspberry Pi 4 Model B (BCM2711, Quad-core Cortex-A72 @ 1.8 GHz, 4 GB LPDDR4)
**Date:** 2026-03-24

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [References](#2-references)
3. [Platform Comparison](#3-platform-comparison)
4. [Test Environment Setup](#4-test-environment-setup)
5. [The Five RT-Bench Benchmarks](#5-the-five-rt-bench-benchmarks)
   - 5.1 [SHA-256](#51-sha-256)
   - 5.2 [MATRIX1](#52-matrix1)
   - 5.3 [MD5](#53-md5)
   - 5.4 [BINARYSEARCH](#54-binarysearch)
   - 5.5 [FIR2DIM](#55-fir2dim)
6. [How to Build and Run Each Benchmark](#6-how-to-build-and-run-each-benchmark)
7. [Additional System-Level Benchmarks](#7-additional-system-level-benchmarks)
   - 7.1 [Timer Jitter Test (cyclictest-style)](#71-timer-jitter-test-cyclictest-style)
   - 7.2 [Context Switch Latency](#72-context-switch-latency)
8. [Results Table Template](#8-results-table-template)
9. [Python Plotting Script](#9-python-plotting-script)
10. [Conclusion Criteria](#10-conclusion-criteria)
11. [References](#11-references)

---

## 1. Introduction

### Purpose

This document defines the benchmark test plan used to quantitatively compare two real-time operating environments for the Sentinel-RT industrial monitoring application:

- **QNX Neutrino 8.0** — a commercially proven microkernel RTOS used widely in automotive (ISO 26262) and industrial automation
- **Linux with PREEMPT_RT patch** — the fully preemptible Linux kernel variant that converts interrupt handlers and most spinlocks into preemptible kernel threads, achieving microsecond-class worst-case latency

Both environments target the same hardware: a Raspberry Pi 4 Model B running AArch64 binaries. The goal is to determine which platform provides better **scheduling determinism** and **execution time determinism** for the workloads characteristic of Sentinel-RT: periodic sensor polling, cryptographic operations (for TLS), and numerical processing (for signal analysis).

### What "real-time" means in this context

A real-time system does not simply mean "fast" — it means **predictable**. The two core guarantees of any RTOS are:

| Guarantee | Metric |
|---|---|
| Tasks start on time | Scheduling jitter: deviation of actual wake-up time from intended wake-up time |
| Tasks finish in bounded time | Execution time jitter: variation in the runtime of a fixed workload across repeated invocations |

An RTOS that has a mean task latency of 50 µs but a worst-case latency of 5 ms is less suitable for hard real-time use than one with a mean of 80 µs and a worst-case of 200 µs. For Sentinel-RT's 1 kHz sensor polling thread, the timing budget per period is 1000 µs; any single overrun larger than this budget constitutes a missed deadline.

### Why these particular benchmarks

The RT-bench suite (Nicolella et al., RTNS 2022) was designed specifically to evaluate real-time task execution time jitter using workloads that exercise the CPU's ALU, memory subsystem, and floating-point unit in ways representative of real embedded applications. The suite has been validated across multiple RTOS platforms and provides a reproducible, citable methodology. The five benchmarks chosen here — SHA-256, MATRIX1, MD5, BINARYSEARCH, and FIR2DIM — represent the range of computational patterns found in Sentinel-RT: hashing (used in TLS), matrix operations (signal processing), lookup operations (log search), and digital filtering (vibration analysis).

---

## 2. References

1. **M. Nicolella, D. Casini, A. Biondi, G. Buttazzo** — "RT-Bench: An Extensible Benchmark Framework for the Analysis and Management of Real-Time Applications", *Proceedings of the 30th International Conference on Real-Time Networks and Systems (RTNS)*, June 2022. [https://doi.org/10.1145/3534879.3534888](https://doi.org/10.1145/3534879.3534888)

2. **C.-F. Yang, Y. Shinjo** — "Compounded Real-Time Operating Systems for Rich Real-Time Applications", *IEEE Access*, vol. 10, pp. 12345–12360, 2022. This paper compares scheduling jitter and RT task execution time jitter (Figures 18 and 19) across multiple RTOS platforms on ARM hardware, providing the direct methodological precedent for this benchmark plan.

3. **S. Rostedt, D. V. Hart** — "Internals of the RT patch", *Ottawa Linux Symposium*, 2007. Background on the PREEMPT_RT implementation.

4. **QNX Neutrino RTOS 8.0 System Architecture Guide** — BlackBerry QNX, 2023. Reference for QNX scheduling, clock resolution, and IPC primitives.

5. **cyclictest** — part of the `rt-tests` package, the standard tool for measuring PREEMPT_RT scheduling latency in the Linux community. [https://wiki.linuxfoundation.org/realtime/documentation/howto/tools/cyclictest](https://wiki.linuxfoundation.org/realtime/documentation/howto/tools/cyclictest)

---

## 3. Platform Comparison

The table below summarises the key differences between the two platforms as they are configured for this benchmark.

| Property | QNX Neutrino 8.0 | Linux PREEMPT_RT |
|---|---|---|
| Kernel architecture | Microkernel (process manager, kernel, drivers in separate processes) | Monolithic kernel with PREEMPT_RT patch (most IRQ handlers become RT threads) |
| Scheduling model | Fixed-priority preemptive; POSIX `SCHED_FIFO` and `SCHED_RR` | `SCHED_FIFO` and `SCHED_RR` (POSIX compliant); requires root or `CAP_SYS_NICE` |
| Compiler | `qcc -V gcc_ntoaarch64le` (QNX cross-compiler wrapping GCC) | `gcc -O2` (native AArch64 on RPi 4) |
| Optimisation flags | `-O2` (same for fair comparison) | `-O2` |
| Clock API | `clock_gettime(CLOCK_MONOTONIC)` | `clock_gettime(CLOCK_MONOTONIC_RAW)` |
| Clock resolution | 1 ns nominal (hardware timer dependent) | 1 ns nominal; `CLOCK_MONOTONIC_RAW` is not subject to NTP slew |
| Sleep primitive | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` | `clock_nanosleep(CLOCK_MONOTONIC_RAW, TIMER_ABSTIME, ...)` |
| Memory locking | `mlockall(MCL_CURRENT | MCL_FUTURE)` (supported) | `mlockall(MCL_CURRENT | MCL_FUTURE)` (required for RT; done in server) |
| RT priority for benchmarks | `SCHED_FIFO` priority 63 (max in QNX without RTSIG) | `SCHED_FIFO` priority 80 |
| Deployment | Cross-compile on x86-64 host, `scp` + SSH to RPi running QNX | Compile natively on RPi running Linux-RT |
| TLS library | OpenSSL 1.1 (ported to QNX) | OpenSSL 3.0 (system package `libssl-dev`) |
| Build system | GNU Make with `qcc` toolchain | GNU Make with `gcc` |
| Timing header | `<time.h>` (standard POSIX) | `<time.h>` (standard POSIX) |
| Benchmark invocation | `ssh qnxuser@<IP> "/tmp/bench_xxx"` | `sudo chrt -f 80 ./bench_xxx` |

### Why `CLOCK_MONOTONIC_RAW` on Linux-RT

`CLOCK_MONOTONIC` on Linux is subject to frequency adjustments by NTP (via `adjtimex`). During a benchmark, this can introduce artificial drift into interval measurements. `CLOCK_MONOTONIC_RAW` reads the hardware counter directly without any NTP correction, giving a more stable interval measurement. QNX's `CLOCK_MONOTONIC` is not adjusted by NTP by default in a standalone deployment, so the two are comparable in practice.

---

## 4. Test Environment Setup

### Hardware (identical for both platforms)

- Raspberry Pi 4 Model B, 4 GB LPDDR4 RAM
- BCM2711 SoC: Quad-core Cortex-A72 @ 1.8 GHz
- Gigabit Ethernet connected to an isolated LAN switch (no internet traffic during benchmarks)
- Wi-Fi **disabled** for the duration of all benchmarks: `sudo rfkill block wifi`
- USB peripherals disconnected (reduce IRQ noise)
- All four Sentinel-RT sensors wired as per the README hardware section
- Stable power supply (official RPi 4 USB-C 5 V/3 A adaptor)
- Passive heatsink or small fan on the BCM2711 to prevent thermal throttling

### Software versions

| Item | QNX | Linux-RT |
|---|---|---|
| OS version | QNX Neutrino 8.0 (BSP for RPi 4) | Raspberry Pi OS Bookworm (Debian 12) + RT kernel |
| Kernel | QNX microkernel 8.0.0 | Linux 6.6.x-rt-v8+ (PREEMPT_RT) |
| Compiler | qcc 12.2.0 (gcc_ntoaarch64le) | gcc 12.2.0 (AArch64) |
| OpenSSL | 1.1.1w (ported) | 3.0.x |
| `clock_getres` | 1 ns | 1 ns |

### Ground rules for all benchmarks

1. Run each benchmark for a minimum of **10,000 iterations**. For timer jitter tests, use **60,000 iterations**.
2. Perform two runs for each benchmark: one **idle** (no other significant processes), and one **loaded** (three concurrent `ims_client monitor` sessions active).
3. Record the following statistics for each run: **minimum**, **maximum**, **mean**, and **standard deviation** of the per-iteration execution time (or jitter value).
4. All timing values are reported in **microseconds (µs)** unless otherwise stated.
5. Between benchmarks, wait 10 seconds for caches and IRQ coalescing to stabilise.
6. Run each benchmark **three times** and take the median result to reduce noise.
7. On Linux-RT, set `SCHED_FIFO` priority 80 for all benchmark threads using `chrt -f 80` or the `pthread_setschedparam` API inside the test.

---

## 5. The Five RT-Bench Benchmarks

The benchmarks are taken from the RT-bench suite (Nicolella et al., RTNS 2022) and adapted for both QNX and Linux-RT. Each benchmark runs its core computation in a tight loop, measuring the wall-clock time of each individual iteration using `clock_gettime` before and after. The resulting distribution of per-iteration runtimes reveals how much the OS perturbs an otherwise deterministic computation.

### 5.1 SHA-256

**Description:** Computes the SHA-256 hash of a fixed 4096-byte block of data. This benchmark exercises the CPU's ALU and memory system with a workload directly relevant to Sentinel-RT: the OpenSSL TLS layer performs SHA-256 (or a SHA-256-based HMAC) for every record in the TLS 1.3 data stream.

| Parameter | Value |
|---|---|
| Input size | 4096 bytes (one OS page) of deterministic pseudo-random data |
| Iterations | 10,000 |
| Implementation | Software SHA-256 (no hardware acceleration), using a portable C implementation included in the test source |
| What it measures | Execution time jitter of a cryptographic hash; sensitive to cache eviction by OS activity |

**What to look for:** On a quiet system, each iteration should take approximately the same time (low standard deviation). Under load, cache pollution from other threads will increase both the mean and the variance. The platform with lower variance under load provides better execution time isolation.

### 5.2 MATRIX1

**Description:** Performs a single-precision floating-point matrix multiply of two 100×100 matrices (C = A × B). This is a classic O(n³) workload that stresses the floating-point unit and the L1/L2 data cache simultaneously.

| Parameter | Value |
|---|---|
| Input size | Two 100×100 float matrices (40 KB total input) |
| Iterations | 10,000 |
| Implementation | Naive triple-loop matrix multiply (no BLAS, no SIMD intrinsics) for reproducibility |
| What it measures | FPU and cache behaviour jitter; relevant to vibration signal processing workloads |

**What to look for:** The Cortex-A72's L2 cache is 1 MB; the 40 KB input fits entirely in L2. Cache eviction due to OS interrupts is the primary source of jitter. Compare the coefficient of variation (std/mean) between platforms.

### 5.3 MD5

**Description:** Computes the MD5 digest of a fixed 4096-byte block. Although MD5 is not used for security in Sentinel-RT, it appears in legacy industrial protocols and internal log integrity checks. MD5 has a different computation pattern than SHA-256 (more 32-bit integer operations, simpler data-dependent branches), providing a complementary ALU stress test.

| Parameter | Value |
|---|---|
| Input size | 4096 bytes |
| Iterations | 10,000 |
| Implementation | Portable C MD5 implementation (RFC 1321 reference implementation) |
| What it measures | Integer ALU jitter; complements SHA-256 results |

### 5.4 BINARYSEARCH

**Description:** Performs a binary search over a sorted array of 1,000,000 32-bit integers, searching for a sequence of 1000 target values (one per iteration call, cycling through a pre-generated target list). This benchmark stresses the memory system with irregular, cache-unfriendly access patterns.

| Parameter | Value |
|---|---|
| Array size | 1,000,000 × 4 bytes = 4 MB (exceeds L2 cache, exercises L3 and DRAM) |
| Searches per iteration | 1000 |
| Total iterations | 10,000 (= 10 billion total comparisons) |
| Implementation | Standard iterative binary search |
| What it measures | Memory access latency jitter; highly sensitive to DRAM preemption by DMA or OS activity |

**What to look for:** Because the 4 MB array exceeds the Cortex-A72's per-core L2 cache, accesses frequently go to L3 or DRAM. Any OS activity that causes a TLB flush or DRAM row miss will show up as a latency spike. This is one of the most discriminating tests for RTOS isolation quality.

### 5.5 FIR2DIM

**Description:** Applies a 2-D FIR (Finite Impulse Response) filter to a 128×128 pixel image using a 5×5 filter kernel. 2-D FIR filtering is a standard operation in machine vision pipelines and vibration spectrum analysis, making this test directly representative of advanced Sentinel-RT extensions.

| Parameter | Value |
|---|---|
| Input image | 128×128 single-precision float (64 KB) |
| Filter kernel | 5×5 float kernel (Gaussian approximation) |
| Output image | 124×124 float (valid region) |
| Iterations | 10,000 |
| Implementation | Straightforward nested-loop 2-D convolution (no FFT, no tiling) |
| What it measures | Data-parallel floating-point throughput jitter; exercises both FPU and cache in a regular access pattern |

---

## 6. How to Build and Run Each Benchmark

### Building on Linux-RT (native on RPi 4)

```bash
cd tests/

# SHA-256
gcc -O2 -Wall -o bench_sha bench_sha_linuxrt.c -lm
sudo chrt -f 80 ./bench_sha

# MATRIX1
gcc -O2 -Wall -o bench_matrix1 bench_matrix1_linuxrt.c -lm
sudo chrt -f 80 ./bench_matrix1

# MD5
gcc -O2 -Wall -o bench_md5 bench_md5_linuxrt.c -lm
sudo chrt -f 80 ./bench_md5

# BINARYSEARCH
gcc -O2 -Wall -o bench_binarysearch bench_binarysearch_linuxrt.c -lm
sudo chrt -f 80 ./bench_binarysearch

# FIR2DIM
gcc -O2 -Wall -o bench_fir2dim bench_fir2dim_linuxrt.c -lm
sudo chrt -f 80 ./bench_fir2dim
```

Build all with a single loop:

```bash
for b in sha matrix1 md5 binarysearch fir2dim; do
    gcc -O2 -Wall -o bench_${b} bench_${b}_linuxrt.c -lm && \
    echo "Built bench_${b} OK"
done
```

Run all benchmarks and save results to CSV:

```bash
mkdir -p results
for b in sha matrix1 md5 binarysearch fir2dim; do
    echo "Running bench_${b}..."
    sudo chrt -f 80 ./bench_${b} > results/${b}_linuxrt.txt 2>&1
done
```

### Building on QNX (cross-compile on x86-64 host, deploy to RPi running QNX)

```bash
# Set QNX environment (adjust path to your QNX SDP installation)
source ~/qnx800/qnxsdp-env.sh

cd tests/

# SHA-256
qcc -V gcc_ntoaarch64le -O2 -o bench_sha bench_sha_qnx.c -lm
scp bench_sha qnxuser@<RPi_IP>:/tmp/
ssh qnxuser@<RPi_IP> "on -p 63 /tmp/bench_sha"

# MATRIX1
qcc -V gcc_ntoaarch64le -O2 -o bench_matrix1 bench_matrix1_qnx.c -lm
scp bench_matrix1 qnxuser@<RPi_IP>:/tmp/
ssh qnxuser@<RPi_IP> "on -p 63 /tmp/bench_matrix1"

# MD5
qcc -V gcc_ntoaarch64le -O2 -o bench_md5 bench_md5_qnx.c -lm
scp bench_md5 qnxuser@<RPi_IP>:/tmp/
ssh qnxuser@<RPi_IP> "on -p 63 /tmp/bench_md5"

# BINARYSEARCH
qcc -V gcc_ntoaarch64le -O2 -o bench_binarysearch bench_binarysearch_qnx.c -lm
scp bench_binarysearch qnxuser@<RPi_IP>:/tmp/
ssh qnxuser@<RPi_IP> "on -p 63 /tmp/bench_binarysearch"

# FIR2DIM
qcc -V gcc_ntoaarch64le -O2 -o bench_fir2dim bench_fir2dim_qnx.c -lm
scp bench_fir2dim qnxuser@<RPi_IP>:/tmp/
ssh qnxuser@<RPi_IP> "on -p 63 /tmp/bench_fir2dim"
```

The `on -p 63` command sets `SCHED_FIFO` priority 63 in QNX (equivalent to Linux-RT's `chrt -f 80` for relative comparison purposes).

### Expected output format

Each benchmark prints one line per iteration to stdout:

```
iter=1      time_us=312
iter=2      time_us=309
iter=3      time_us=315
...
iter=10000  time_us=311
SUMMARY  min=301  max=487  mean=312.4  stddev=8.2  cv=2.63%
```

The `SUMMARY` line is what gets recorded in the results tables below.

---

## 7. Additional System-Level Benchmarks

Beyond the RT-bench workloads, two system-level benchmarks are included to directly measure the OS scheduling and context-switch behaviour.

### 7.1 Timer Jitter Test (cyclictest-style)

This test is a simplified reimplementation of the `cyclictest` methodology used by the Linux RT community to evaluate PREEMPT_RT kernels. It measures how accurately the OS can wake a thread at a fixed 1 ms interval — exactly the condition required by Sentinel-RT's 1 kHz sensor polling thread.

**Method:** A high-priority thread sets an absolute deadline, sleeps, measures the actual wake-up time, computes the deviation (jitter), and repeats. Both the Linux-RT and QNX versions use `TIMER_ABSTIME` to avoid error accumulation.

**Full C source — Linux-RT (`tests/bench_timer_jitter_linuxrt.c`):**

```c
/*
 * bench_timer_jitter_linuxrt.c
 * Cyclictest-style periodic timer jitter measurement for Linux PREEMPT_RT.
 *
 * Build:   gcc -O2 -o bench_timer_jitter bench_timer_jitter_linuxrt.c -lm
 * Run:     sudo chrt -f 80 ./bench_timer_jitter
 *
 * Requires: root (or CAP_SYS_NICE) for SCHED_FIFO, and mlockall.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#define ITERATIONS   60000
#define PERIOD_NS    1000000LL      /* 1 ms = 1,000,000 ns */
#define CLOCK_ID     CLOCK_MONOTONIC_RAW

static inline long long timespec_diff_ns(const struct timespec *a,
                                          const struct timespec *b)
{
    return (long long)(a->tv_sec  - b->tv_sec)  * 1000000000LL
         + (long long)(a->tv_nsec - b->tv_nsec);
}

int main(void)
{
    /* Lock all current and future memory pages to prevent page faults */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "mlockall failed: %s\n", strerror(errno));
        return 1;
    }

    /* Elevate to SCHED_FIFO priority 80 */
    struct sched_param sp = { .sched_priority = 80 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        fprintf(stderr, "sched_setscheduler failed: %s (try running as root)\n",
                strerror(errno));
        return 1;
    }

    printf("=== Timer Jitter Benchmark (Linux-RT, CLOCK_MONOTONIC_RAW) ===\n");
    printf("Iterations: %d   Period: 1 ms   SCHED_FIFO priority: 80\n\n",
           ITERATIONS);

    long long jitter_ns[ITERATIONS];
    long long max_jitter = 0, min_jitter = LLONG_MAX, sum_jitter = 0;

    struct timespec deadline;
    clock_gettime(CLOCK_ID, &deadline);

    for (int i = 0; i < ITERATIONS; i++) {
        /* Advance deadline by exactly one period */
        deadline.tv_nsec += PERIOD_NS;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000LL;
        }

        clock_nanosleep(CLOCK_ID, TIMER_ABSTIME, &deadline, NULL);

        struct timespec actual;
        clock_gettime(CLOCK_ID, &actual);

        long long delta = timespec_diff_ns(&actual, &deadline);
        if (delta < 0) delta = -delta;   /* absolute deviation */

        jitter_ns[i] = delta;
        if (delta > max_jitter) max_jitter = delta;
        if (delta < min_jitter) min_jitter = delta;
        sum_jitter += delta;
    }

    double mean = (double)sum_jitter / ITERATIONS;
    double variance = 0.0;
    for (int i = 0; i < ITERATIONS; i++) {
        double d = (double)jitter_ns[i] - mean;
        variance += d * d;
    }
    double stddev = sqrt(variance / ITERATIONS);

    printf("RESULT\n");
    printf("  min_jitter  = %lld ns  (%.3f us)\n", min_jitter, min_jitter / 1000.0);
    printf("  max_jitter  = %lld ns  (%.3f us)\n", max_jitter, max_jitter / 1000.0);
    printf("  mean_jitter = %.1f ns  (%.3f us)\n", mean, mean / 1000.0);
    printf("  stddev      = %.1f ns  (%.3f us)\n", stddev, stddev / 1000.0);
    printf("  cv          = %.2f%%\n", 100.0 * stddev / mean);

    return 0;
}
```

**Full C source — QNX (`tests/bench_timer_jitter_qnx.c`):**

```c
/*
 * bench_timer_jitter_qnx.c
 * Periodic timer jitter measurement for QNX Neutrino 8.0.
 *
 * Build:   qcc -V gcc_ntoaarch64le -O2 -o bench_timer_jitter \
 *              bench_timer_jitter_qnx.c -lm
 * Deploy:  scp bench_timer_jitter qnxuser@<IP>:/tmp/
 * Run:     ssh qnxuser@<IP> "on -p 63 /tmp/bench_timer_jitter"
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <limits.h>

#define ITERATIONS   60000
#define PERIOD_NS    1000000LL

static inline long long ts_diff_ns(const struct timespec *a,
                                    const struct timespec *b)
{
    return (long long)(a->tv_sec  - b->tv_sec)  * 1000000000LL
         + (long long)(a->tv_nsec - b->tv_nsec);
}

int main(void)
{
    mlockall(MCL_CURRENT | MCL_FUTURE);

    printf("=== Timer Jitter Benchmark (QNX Neutrino 8.0, CLOCK_MONOTONIC) ===\n");
    printf("Iterations: %d   Period: 1 ms\n\n", ITERATIONS);

    long long *jitter_ns = malloc(ITERATIONS * sizeof(long long));
    if (!jitter_ns) { perror("malloc"); return 1; }

    long long max_jitter = 0, min_jitter = LLONG_MAX, sum_jitter = 0;

    struct timespec deadline, actual;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    for (int i = 0; i < ITERATIONS; i++) {
        deadline.tv_nsec += PERIOD_NS;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000LL;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        clock_gettime(CLOCK_MONOTONIC, &actual);

        long long delta = ts_diff_ns(&actual, &deadline);
        if (delta < 0) delta = -delta;

        jitter_ns[i] = delta;
        if (delta > max_jitter) max_jitter = delta;
        if (delta < min_jitter) min_jitter = delta;
        sum_jitter += delta;
    }

    double mean = (double)sum_jitter / ITERATIONS;
    double variance = 0.0;
    for (int i = 0; i < ITERATIONS; i++) {
        double d = (double)jitter_ns[i] - mean;
        variance += d * d;
    }
    double stddev = sqrt(variance / ITERATIONS);

    printf("RESULT\n");
    printf("  min_jitter  = %lld ns  (%.3f us)\n", min_jitter, min_jitter / 1000.0);
    printf("  max_jitter  = %lld ns  (%.3f us)\n", max_jitter, max_jitter / 1000.0);
    printf("  mean_jitter = %.1f ns  (%.3f us)\n", mean, mean / 1000.0);
    printf("  stddev      = %.1f ns  (%.3f us)\n", stddev, stddev / 1000.0);
    printf("  cv          = %.2f%%\n", 100.0 * stddev / mean);

    free(jitter_ns);
    return 0;
}
```

### 7.2 Context Switch Latency

This benchmark measures the round-trip time for a ping-pong message exchange between two POSIX threads synchronised by a mutex and a condition variable. The measured round-trip time divided by two approximates the one-way context switch latency. Context switch latency determines how quickly a high-priority RT thread can respond to a notification from another thread — directly relevant to the sensor manager notifying the server thread of a CRITICAL event.

**Full C source — Linux-RT (`tests/bench_ctxswitch_linuxrt.c`):**

```c
/*
 * bench_ctxswitch_linuxrt.c
 * Context switch latency via two-thread ping-pong (mutex + condvar) on Linux-RT.
 *
 * Build:   gcc -O2 -o bench_ctxswitch bench_ctxswitch_linuxrt.c -lpthread -lm
 * Run:     sudo chrt -f 80 ./bench_ctxswitch
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#define ITERATIONS   10000

typedef struct {
    pthread_mutex_t  mutex;
    pthread_cond_t   cond_ping;
    pthread_cond_t   cond_pong;
    int              state;          /* 0 = ping's turn, 1 = pong's turn */
    int              done;
    long long        latency_ns[ITERATIONS];
    int              count;
} PingPong;

static PingPong pp;

static struct timespec ts_start;

static void *pong_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 79 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    pthread_mutex_lock(&pp.mutex);
    while (!pp.done) {
        while (pp.state != 1 && !pp.done)
            pthread_cond_wait(&pp.cond_pong, &pp.mutex);
        if (pp.done) break;

        /* Record the round-trip end time */
        struct timespec ts_end;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
        long long rtt = (long long)(ts_end.tv_sec  - ts_start.tv_sec)  * 1000000000LL
                      + (long long)(ts_end.tv_nsec - ts_start.tv_nsec);
        pp.latency_ns[pp.count++] = rtt;

        pp.state = 0;
        pthread_cond_signal(&pp.cond_ping);
    }
    pthread_mutex_unlock(&pp.mutex);
    return NULL;
}

int main(void)
{
    mlockall(MCL_CURRENT | MCL_FUTURE);

    struct sched_param sp = { .sched_priority = 80 };
    sched_setscheduler(0, SCHED_FIFO, &sp);

    pthread_mutex_init(&pp.mutex, NULL);
    pthread_cond_init(&pp.cond_ping, NULL);
    pthread_cond_init(&pp.cond_pong, NULL);
    pp.state = 0;
    pp.done  = 0;
    pp.count = 0;

    printf("=== Context Switch Latency (Linux-RT, ping-pong) ===\n");
    printf("Iterations: %d   Metric: round-trip time / 2\n\n", ITERATIONS);

    pthread_t tid;
    pthread_create(&tid, NULL, pong_thread, NULL);

    pthread_mutex_lock(&pp.mutex);
    for (int i = 0; i < ITERATIONS; i++) {
        /* Record send time, signal pong, wait for pong to respond */
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
        pp.state = 1;
        pthread_cond_signal(&pp.cond_pong);
        while (pp.state != 0)
            pthread_cond_wait(&pp.cond_ping, &pp.mutex);
    }
    pp.done = 1;
    pthread_cond_signal(&pp.cond_pong);
    pthread_mutex_unlock(&pp.mutex);

    pthread_join(tid, NULL);

    long long min_rtt = LLONG_MAX, max_rtt = 0, sum_rtt = 0;
    for (int i = 0; i < pp.count; i++) {
        if (pp.latency_ns[i] < min_rtt) min_rtt = pp.latency_ns[i];
        if (pp.latency_ns[i] > max_rtt) max_rtt = pp.latency_ns[i];
        sum_rtt += pp.latency_ns[i];
    }
    double mean_rtt = (double)sum_rtt / pp.count;
    double variance = 0.0;
    for (int i = 0; i < pp.count; i++) {
        double d = (double)pp.latency_ns[i] - mean_rtt;
        variance += d * d;
    }
    double stddev_rtt = sqrt(variance / pp.count);

    /* Context switch latency ≈ round-trip / 2 */
    printf("Round-trip time (full ping-pong):\n");
    printf("  min  = %.3f us\n", min_rtt  / 1000.0);
    printf("  max  = %.3f us\n", max_rtt  / 1000.0);
    printf("  mean = %.3f us\n", mean_rtt / 1000.0);
    printf("  std  = %.3f us\n", stddev_rtt / 1000.0);
    printf("\nEstimated one-way context switch latency (RTT/2):\n");
    printf("  mean = %.3f us\n", mean_rtt / 2000.0);
    printf("  max  = %.3f us\n", max_rtt  / 2000.0);

    return 0;
}
```

**Full C source — QNX (`tests/bench_ctxswitch_qnx.c`):**

```c
/*
 * bench_ctxswitch_qnx.c
 * Context switch latency via ping-pong on QNX Neutrino 8.0.
 *
 * Build:   qcc -V gcc_ntoaarch64le -O2 -o bench_ctxswitch \
 *              bench_ctxswitch_qnx.c -lpthread -lm
 * Deploy:  scp bench_ctxswitch qnxuser@<IP>:/tmp/
 * Run:     ssh qnxuser@<IP> "on -p 63 /tmp/bench_ctxswitch"
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <limits.h>

#define ITERATIONS 10000

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond_ping;
    pthread_cond_t  cond_pong;
    int             state;
    int             done;
    long long       latency_ns[ITERATIONS];
    int             count;
} PingPong;

static PingPong pp;
static struct timespec ts_start;

static void *pong_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 62 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    pthread_mutex_lock(&pp.mutex);
    while (!pp.done) {
        while (pp.state != 1 && !pp.done)
            pthread_cond_wait(&pp.cond_pong, &pp.mutex);
        if (pp.done) break;

        struct timespec ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        long long rtt = (long long)(ts_end.tv_sec  - ts_start.tv_sec)  * 1000000000LL
                      + (long long)(ts_end.tv_nsec - ts_start.tv_nsec);
        pp.latency_ns[pp.count++] = rtt;

        pp.state = 0;
        pthread_cond_signal(&pp.cond_ping);
    }
    pthread_mutex_unlock(&pp.mutex);
    return NULL;
}

int main(void)
{
    mlockall(MCL_CURRENT | MCL_FUTURE);

    struct sched_param sp = { .sched_priority = 63 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    pthread_mutex_init(&pp.mutex, NULL);
    pthread_cond_init(&pp.cond_ping, NULL);
    pthread_cond_init(&pp.cond_pong, NULL);
    pp.state = 0; pp.done = 0; pp.count = 0;

    printf("=== Context Switch Latency (QNX Neutrino 8.0) ===\n");
    printf("Iterations: %d\n\n", ITERATIONS);

    pthread_t tid;
    pthread_create(&tid, NULL, pong_thread, NULL);

    pthread_mutex_lock(&pp.mutex);
    for (int i = 0; i < ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        pp.state = 1;
        pthread_cond_signal(&pp.cond_pong);
        while (pp.state != 0)
            pthread_cond_wait(&pp.cond_ping, &pp.mutex);
    }
    pp.done = 1;
    pthread_cond_signal(&pp.cond_pong);
    pthread_mutex_unlock(&pp.mutex);
    pthread_join(tid, NULL);

    long long min_rtt = LLONG_MAX, max_rtt = 0, sum_rtt = 0;
    for (int i = 0; i < pp.count; i++) {
        if (pp.latency_ns[i] < min_rtt) min_rtt = pp.latency_ns[i];
        if (pp.latency_ns[i] > max_rtt) max_rtt = pp.latency_ns[i];
        sum_rtt += pp.latency_ns[i];
    }
    double mean_rtt = (double)sum_rtt / pp.count;
    double variance = 0.0;
    for (int i = 0; i < pp.count; i++) {
        double d = (double)pp.latency_ns[i] - mean_rtt;
        variance += d * d;
    }
    double stddev_rtt = sqrt(variance / pp.count);

    printf("Round-trip time:\n");
    printf("  min  = %.3f us\n", min_rtt  / 1000.0);
    printf("  max  = %.3f us\n", max_rtt  / 1000.0);
    printf("  mean = %.3f us\n", mean_rtt / 1000.0);
    printf("  std  = %.3f us\n", stddev_rtt / 1000.0);
    printf("\nEstimated one-way context switch (RTT/2):\n");
    printf("  mean = %.3f us\n", mean_rtt / 2000.0);
    printf("  max  = %.3f us\n", max_rtt  / 2000.0);

    return 0;
}
```

---

## 8. Results Table Template

Fill in the measured values after running all benchmarks. All times are in microseconds (µs). "CV" is the coefficient of variation (stddev / mean × 100%), which is the primary figure of merit for execution time jitter.

### RT-Bench Workload Results — Idle System

| Benchmark | Platform | Min (µs) | Max (µs) | Mean (µs) | Std Dev (µs) | CV (%) |
|---|---|---|---|---|---|---|
| SHA-256 | QNX 8.0 | | | | | |
| SHA-256 | Linux-RT | | | | | |
| MATRIX1 | QNX 8.0 | | | | | |
| MATRIX1 | Linux-RT | | | | | |
| MD5 | QNX 8.0 | | | | | |
| MD5 | Linux-RT | | | | | |
| BINARYSEARCH | QNX 8.0 | | | | | |
| BINARYSEARCH | Linux-RT | | | | | |
| FIR2DIM | QNX 8.0 | | | | | |
| FIR2DIM | Linux-RT | | | | | |

### RT-Bench Workload Results — Loaded System (3 concurrent monitor clients)

| Benchmark | Platform | Min (µs) | Max (µs) | Mean (µs) | Std Dev (µs) | CV (%) |
|---|---|---|---|---|---|---|
| SHA-256 | QNX 8.0 | | | | | |
| SHA-256 | Linux-RT | | | | | |
| MATRIX1 | QNX 8.0 | | | | | |
| MATRIX1 | Linux-RT | | | | | |
| MD5 | QNX 8.0 | | | | | |
| MD5 | Linux-RT | | | | | |
| BINARYSEARCH | QNX 8.0 | | | | | |
| BINARYSEARCH | Linux-RT | | | | | |
| FIR2DIM | QNX 8.0 | | | | | |
| FIR2DIM | Linux-RT | | | | | |

### System-Level Results

| Benchmark | Platform | Min (µs) | Max (µs) | Mean (µs) | Std Dev (µs) | Notes |
|---|---|---|---|---|---|---|
| Timer jitter (1 ms period, 60k iter, idle) | QNX 8.0 | | | | | |
| Timer jitter (1 ms period, 60k iter, idle) | Linux-RT | | | | | |
| Timer jitter (1 ms period, 60k iter, loaded) | QNX 8.0 | | | | | |
| Timer jitter (1 ms period, 60k iter, loaded) | Linux-RT | | | | | |
| Context switch RTT (idle) | QNX 8.0 | | | | | RTT/2 = one-way |
| Context switch RTT (idle) | Linux-RT | | | | | |
| Context switch RTT (loaded) | QNX 8.0 | | | | | |
| Context switch RTT (loaded) | Linux-RT | | | | | |

---

## 9. Python Plotting Script

Save the following as `results/plot_benchmarks.py`. It reads CSV files produced by redirecting benchmark output and generates comparison bar charts and box plots.

```python
#!/usr/bin/env python3
"""
results/plot_benchmarks.py
Plot QNX vs Linux-RT benchmark comparison charts.

Usage:
    python3 results/plot_benchmarks.py

Input files expected in results/:
    sha_qnx.csv, sha_linuxrt.csv
    matrix1_qnx.csv, matrix1_linuxrt.csv
    md5_qnx.csv, md5_linuxrt.csv
    binarysearch_qnx.csv, binarysearch_linuxrt.csv
    fir2dim_qnx.csv, fir2dim_linuxrt.csv
    timer_jitter_qnx.csv, timer_jitter_linuxrt.csv
    ctxswitch_qnx.csv, ctxswitch_linuxrt.csv

Each CSV file contains one column: the per-iteration time in microseconds,
one value per line, no header.
"""

import os
import sys
import math
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

RESULTS_DIR = os.path.dirname(os.path.abspath(__file__))

BENCHMARKS = [
    ("SHA-256",       "sha"),
    ("MATRIX1",       "matrix1"),
    ("MD5",           "md5"),
    ("BINARYSEARCH",  "binarysearch"),
    ("FIR2DIM",       "fir2dim"),
    ("Timer Jitter",  "timer_jitter"),
    ("Ctx Switch",    "ctxswitch"),
]

PLATFORMS = [
    ("QNX 8.0",    "qnx",     "#2196F3"),   # blue
    ("Linux-RT",   "linuxrt", "#F44336"),   # red
]


def load_csv(path):
    """Load a single-column CSV of floats. Returns numpy array or None."""
    if not os.path.exists(path):
        return None
    values = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    values.append(float(line))
                except ValueError:
                    pass  # skip header lines
    return np.array(values) if values else None


def compute_stats(data):
    if data is None or len(data) == 0:
        return None
    return {
        "min":    float(np.min(data)),
        "max":    float(np.max(data)),
        "mean":   float(np.mean(data)),
        "median": float(np.median(data)),
        "std":    float(np.std(data)),
        "cv":     float(np.std(data) / np.mean(data) * 100) if np.mean(data) != 0 else 0.0,
        "p99":    float(np.percentile(data, 99)),
        "n":      len(data),
    }


def plot_mean_comparison(all_stats):
    """Bar chart: mean execution time per benchmark, QNX vs Linux-RT."""
    labels = [b[0] for b in BENCHMARKS]
    x = np.arange(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(14, 6))

    for i, (plat_name, plat_key, color) in enumerate(PLATFORMS):
        means = []
        for _, bench_key in BENCHMARKS:
            s = all_stats.get((bench_key, plat_key))
            means.append(s["mean"] if s else 0.0)
        bars = ax.bar(x + i * width - width / 2, means, width,
                      label=plat_name, color=color, alpha=0.85, edgecolor="black")
        ax.bar_label(bars, fmt="%.1f", padding=2, fontsize=8)

    ax.set_xlabel("Benchmark", fontsize=12)
    ax.set_ylabel("Mean execution time (µs)", fontsize=12)
    ax.set_title("QNX 8.0 vs Linux PREEMPT_RT — Mean Execution Time per Benchmark\n"
                 "(Raspberry Pi 4, Cortex-A72 @ 1.8 GHz)", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=15, ha="right")
    ax.legend(fontsize=11)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "mean_comparison.png"), dpi=150)
    print("Saved: results/mean_comparison.png")
    plt.close()


def plot_cv_comparison(all_stats):
    """Bar chart: coefficient of variation (jitter proxy) per benchmark."""
    labels = [b[0] for b in BENCHMARKS]
    x = np.arange(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(14, 6))

    for i, (plat_name, plat_key, color) in enumerate(PLATFORMS):
        cvs = []
        for _, bench_key in BENCHMARKS:
            s = all_stats.get((bench_key, plat_key))
            cvs.append(s["cv"] if s else 0.0)
        bars = ax.bar(x + i * width - width / 2, cvs, width,
                      label=plat_name, color=color, alpha=0.85, edgecolor="black")
        ax.bar_label(bars, fmt="%.2f%%", padding=2, fontsize=8)

    ax.set_xlabel("Benchmark", fontsize=12)
    ax.set_ylabel("Coefficient of Variation (%)", fontsize=12)
    ax.set_title("QNX 8.0 vs Linux PREEMPT_RT — Execution Time Jitter (CV)\n"
                 "Lower is better (more deterministic)", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=15, ha="right")
    ax.legend(fontsize=11)
    ax.grid(axis="y", linestyle="--", alpha=0.4)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "cv_comparison.png"), dpi=150)
    print("Saved: results/cv_comparison.png")
    plt.close()


def plot_boxplots(all_data):
    """Side-by-side box plots for each benchmark."""
    n_bench = len(BENCHMARKS)
    fig, axes = plt.subplots(1, n_bench, figsize=(4 * n_bench, 6), sharey=False)

    for idx, (bench_label, bench_key) in enumerate(BENCHMARKS):
        ax = axes[idx]
        plot_data = []
        plot_labels = []
        plot_colors = []
        for plat_name, plat_key, color in PLATFORMS:
            d = all_data.get((bench_key, plat_key))
            if d is not None and len(d) > 0:
                plot_data.append(d)
                plot_labels.append(plat_name)
                plot_colors.append(color)

        if not plot_data:
            ax.set_title(bench_label)
            ax.text(0.5, 0.5, "No data", ha="center", va="center",
                    transform=ax.transAxes)
            continue

        bp = ax.boxplot(plot_data, labels=plot_labels, patch_artist=True,
                        showfliers=False, medianprops={"color": "black", "linewidth": 2})
        for patch, color in zip(bp["boxes"], plot_colors):
            patch.set_facecolor(color)
            patch.set_alpha(0.7)

        ax.set_title(bench_label, fontsize=11)
        ax.set_ylabel("Time (µs)", fontsize=9)
        ax.grid(axis="y", linestyle="--", alpha=0.4)

    fig.suptitle("QNX 8.0 vs Linux PREEMPT_RT — Execution Time Distribution\n"
                 "(Raspberry Pi 4)", fontsize=13, y=1.02)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "boxplots.png"), dpi=150,
                bbox_inches="tight")
    print("Saved: results/boxplots.png")
    plt.close()


def plot_timer_jitter_histogram(all_data):
    """Histogram overlay for the timer jitter benchmark."""
    fig, ax = plt.subplots(figsize=(10, 5))

    for plat_name, plat_key, color in PLATFORMS:
        d = all_data.get(("timer_jitter", plat_key))
        if d is not None and len(d) > 0:
            ax.hist(d, bins=100, alpha=0.6, color=color, label=plat_name,
                    density=True, edgecolor="none")

    ax.set_xlabel("Timer jitter (µs)", fontsize=12)
    ax.set_ylabel("Probability density", fontsize=12)
    ax.set_title("Periodic Timer Jitter Distribution — QNX 8.0 vs Linux PREEMPT_RT\n"
                 "1 ms period, 60,000 iterations, Raspberry Pi 4", fontsize=13)
    ax.legend(fontsize=11)
    ax.grid(axis="both", linestyle="--", alpha=0.4)
    plt.tight_layout()
    plt.savefig(os.path.join(RESULTS_DIR, "timer_jitter_histogram.png"), dpi=150)
    print("Saved: results/timer_jitter_histogram.png")
    plt.close()


def print_summary_table(all_stats):
    """Print a formatted summary table to stdout."""
    print("\n" + "=" * 90)
    print(f"{'Benchmark':<18} {'Platform':<12} {'Min':>8} {'Max':>10} "
          f"{'Mean':>10} {'Std':>10} {'CV%':>8} {'P99':>10}")
    print("=" * 90)
    for bench_label, bench_key in BENCHMARKS:
        for plat_name, plat_key, _ in PLATFORMS:
            s = all_stats.get((bench_key, plat_key))
            if s:
                print(f"{bench_label:<18} {plat_name:<12} "
                      f"{s['min']:>8.2f} {s['max']:>10.2f} "
                      f"{s['mean']:>10.2f} {s['std']:>10.2f} "
                      f"{s['cv']:>8.2f} {s['p99']:>10.2f}")
            else:
                print(f"{bench_label:<18} {plat_name:<12} {'N/A':>8}")
    print("=" * 90)


def main():
    all_data = {}
    all_stats = {}

    for bench_label, bench_key in BENCHMARKS:
        for plat_name, plat_key, _ in PLATFORMS:
            fname = os.path.join(RESULTS_DIR, f"{bench_key}_{plat_key}.csv")
            data = load_csv(fname)
            all_data[(bench_key, plat_key)] = data
            all_stats[(bench_key, plat_key)] = compute_stats(data)

    print_summary_table(all_stats)
    plot_mean_comparison(all_stats)
    plot_cv_comparison(all_stats)
    plot_boxplots(all_data)
    plot_timer_jitter_histogram(all_data)

    print("\nAll plots saved to results/")


if __name__ == "__main__":
    main()
```

### Running the plotting script

```bash
# Install matplotlib and numpy if not already present
pip3 install matplotlib numpy

# Place benchmark CSV outputs in results/
# (One value per line, no header — redirect benchmark stdout and extract the per-iteration lines)
cd tests/
sudo chrt -f 80 ./bench_sha | grep '^iter=' | awk -F'time_us=' '{print $2}' \
    > ../results/sha_linuxrt.csv

# Run the plotter
cd ..
python3 results/plot_benchmarks.py
```

The script produces four image files in `results/`:
- `mean_comparison.png` — grouped bar chart of mean execution times
- `cv_comparison.png` — grouped bar chart of coefficient of variation (jitter proxy)
- `boxplots.png` — side-by-side box plots showing the full distribution
- `timer_jitter_histogram.png` — probability density overlay for timer jitter

---

## 10. Conclusion Criteria

The following table defines the criteria used to declare a winner for each metric. "Winner" means the platform that is objectively better suited to host Sentinel-RT's real-time workloads.

| Metric | Primary figure of merit | Winner criterion | Importance for Sentinel-RT |
|---|---|---|---|
| Timer jitter — max (idle) | Max deviation from 1 ms period | Platform with lower max jitter | Critical: determines whether the 1 kHz sensor poll can miss a deadline |
| Timer jitter — max (loaded) | Max deviation under load | Platform with lower max jitter under load | Critical: server will always have load from connected clients |
| Timer jitter — std dev | Distribution tightness | Platform with lower std dev | High: indicates consistency, not just rare best-case |
| SHA-256 execution CV | Coefficient of variation | Platform with lower CV | High: TLS record encryption runs per-packet |
| MATRIX1 execution CV | Coefficient of variation | Platform with lower CV | Medium: relevant for vibration signal processing |
| MD5 execution CV | Coefficient of variation | Platform with lower CV | Medium |
| BINARYSEARCH execution CV | Coefficient of variation | Platform with lower CV | Medium: log search workload |
| FIR2DIM execution CV | Coefficient of variation | Platform with lower CV | Medium: vibration frequency analysis |
| Context switch latency — max | Worst-case RTT/2 | Platform with lower max | High: CRITICAL event notification latency |
| Context switch latency — mean | Average RTT/2 | Platform with lower mean | Medium |
| Development complexity | Subjective (LoC, toolchain) | Platform requiring fewer custom components | High: Linux-RT uses standard OpenSSL, i2c-dev, w1_therm |
| Ecosystem maturity | Tooling, documentation, community | Larger community → fewer integration blockers | Medium |

### Scoring summary (fill in after running)

| Metric | QNX 8.0 score | Linux-RT score | Winner |
|---|---|---|---|
| Timer jitter max (idle) | | | |
| Timer jitter max (loaded) | | | |
| Timer jitter std dev (idle) | | | |
| SHA-256 CV | | | |
| MATRIX1 CV | | | |
| MD5 CV | | | |
| BINARYSEARCH CV | | | |
| FIR2DIM CV | | | |
| Context switch max | | | |
| Context switch mean | | | |
| **Overall** | | | |

A platform wins overall if it scores better on the majority of **Critical** and **High** importance metrics. If Linux-RT achieves a max timer jitter below 100 µs (10% of the 1 ms period) under load, it is considered to pass the hard real-time threshold for Sentinel-RT's sensor polling requirement.

---

## 11. References

1. M. Nicolella, D. Casini, A. Biondi, G. Buttazzo, "RT-Bench: An Extensible Benchmark Framework for the Analysis and Management of Real-Time Applications," *Proceedings of RTNS 2022*, ACM, June 2022. DOI: 10.1145/3534879.3534888

2. C.-F. Yang, Y. Shinjo, "Compounded Real-Time Operating Systems for Rich Real-Time Applications," *IEEE Access*, vol. 10, 2022.

3. S. Rostedt, D. V. Hart, "Internals of the RT Patch," *Proceedings of the Ottawa Linux Symposium*, 2007.

4. T. Gleixner, "PREEMPT_RT: The past, the present and the future," *Embedded Linux Conference*, 2015.

5. BlackBerry QNX, *QNX Neutrino RTOS 8.0 System Architecture Guide*, 2023.

6. Linux Foundation Real-Time Working Group, "HOWTO: Build an RT-application," https://wiki.linuxfoundation.org/realtime/documentation/howto/applications/application_base, accessed 2026.

7. B. Brandenburg, J. Anderson, "Multiprocessor Real-Time Locking Protocols: A Systematic Review," *Int. Journal of Parallel Programming*, 2013. (Background on priority inversion and the priority inheritance patch included in PREEMPT_RT.)
