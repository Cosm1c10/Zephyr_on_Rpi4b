# Sentinel-RT: QNX vs Zephyr Benchmark Test Plan

**Project:** Edge-Based Industrial Equipment Health Dashboard
**Comparison:** QNX Neutrino 8.0 vs Zephyr RTOS (rpi_4b)
**Hardware:** Raspberry Pi 4 Model B (BCM2711, Quad-core Cortex-A72 @ 1.8 GHz)

> This benchmark plan is derived from the methodology used in:
> *C.-F. Yang, Y. Shinjo — "Compounded Real-Time Operating Systems for Rich
> Real-Time Applications", IEEE Access.*
> That paper compared multiple RTOSs across scheduling jitter and RT task
> execution time jitter (Figures 18 and 19). We replicate those two core
> metrics for just QNX and Zephyr, then add four project-specific tests
> relevant to the industrial monitoring use case.

---

## Table of Contents

1. [Why These Benchmarks](#1-why-these-benchmarks)
2. [Test Environment Setup](#2-test-environment-setup)
3. [Benchmark 1 — Periodic Timer Jitter](#3-benchmark-1--periodic-timer-jitter)
4. [Benchmark 2 — Context Switch Time](#4-benchmark-2--context-switch-time)
5. [Benchmark 3 — GPIO Interrupt Latency](#5-benchmark-3--gpio-interrupt-latency)
6. [Benchmark 4 — 1 kHz Sensor Poll Jitter](#6-benchmark-4--1-khz-sensor-poll-jitter)
7. [Benchmark 5 — Network Round-Trip Latency](#7-benchmark-5--network-round-trip-latency)
8. [Benchmark 6 — TLS Handshake Time](#8-benchmark-6--tls-handshake-time)
9. [Benchmark 7 — Memory Footprint](#9-benchmark-7--memory-footprint)
10. [Results Table Template](#10-results-table-template)
11. [How to Plot the Results](#11-how-to-plot-the-results)
12. [Conclusion Criteria](#12-conclusion-criteria)

---

## 1. Why These Benchmarks

A real-time operating system makes two core guarantees:

| Guarantee | Measured by |
|-----------|------------|
| **Tasks start on time** (scheduling determinism) | Timer jitter — how much does a periodic task deviate from its intended wake-up time? |
| **Tasks finish in predictable time** (execution determinism) | Execution time jitter — how much does the same task's runtime vary run-to-run? |

The paper uses these two metrics across dozens of Linux benchmark programs to expose how well each RTOS isolates real-time tasks from background load. We apply the same idea to our own sensor monitoring workload.

Additionally, because Sentinel-RT is an industrial networked system, we also measure:
- GPIO interrupt latency (hardware responsiveness)
- 1 kHz polling consistency (core to our sensor accuracy)
- Network round-trip and TLS handshake time (end-to-end security overhead)
- Memory footprint (embedded resource efficiency)

---

## 2. Test Environment Setup

### Hardware (same for both tests)

- Raspberry Pi 4 Model B, 4 GB RAM
- Ethernet connected to LAN (no Wi-Fi during benchmarks)
- All sensors wired as per README hardware section
- USB-UART adapter on GPIO 14/15 for console output
- Logic analyser or oscilloscope on GPIO loopback pins (Benchmark 3)

### Software

| | QNX | Zephyr |
|-|-----|--------|
| RTOS version | QNX Neutrino 8.0 | Zephyr v3.x (latest stable) |
| Compiler | qcc (gcc_ntoaarch64le) | aarch64-zephyr-elf-gcc |
| TLS library | OpenSSL 1.1 | MbedTLS (bundled) |
| Build system | GNU Make | west / CMake |
| Timing API | `clock_nanosleep`, `ClockCycles()` | `k_uptime_ticks()`, `k_cycle_get_64()` |

### Ground rules

- Run each benchmark for **10,000 iterations minimum** (timer jitter: 60,000).
- Take measurements with **no other load** first, then repeat with a simulated background load (e.g., three concurrent `monitor` clients connected).
- Record **minimum, maximum, mean, and standard deviation** for every test.
- All timing values reported in **microseconds (µs)** unless otherwise noted.

---

## 3. Benchmark 1 — Periodic Timer Jitter

### What it measures

How accurately can the RTOS wake a sleeping thread at a fixed 1 ms interval?
This is the direct equivalent of **Figure 18** in the reference paper.

### Why it matters

The sensor polling thread in `drivers/sensor_manager.c` relies on a 1 ms sleep to achieve 1 kHz sampling. Any jitter here directly degrades sensor data quality.

### Method

A dedicated thread sleeps for exactly 1 ms using an absolute deadline timer, then records the actual wake-up time and computes the deviation from the expected time.

### Code — Zephyr (`tests/bench_timer_jitter_zephyr.c`)

```c
#include <zephyr/kernel.h>
#include <stdio.h>

#define ITERATIONS   60000
#define PERIOD_US    1000        /* 1 ms target period */

static int64_t max_jitter_us = 0;
static int64_t sum_jitter_us = 0;

int main(void) {
    printk("=== Benchmark 1: Periodic Timer Jitter (Zephyr) ===\n");
    printk("Iterations: %d  Period: %d us\n\n", ITERATIONS, PERIOD_US);

    int64_t expected_ticks = k_uptime_ticks();

    for (int i = 0; i < ITERATIONS; i++) {
        expected_ticks += k_us_to_ticks_ceil64(PERIOD_US);

        /* Absolute-deadline sleep — most accurate Zephyr sleep primitive */
        k_sleep(K_TIMEOUT_ABS_TICKS(expected_ticks));

        int64_t actual_ticks = k_uptime_ticks();
        int64_t delta_us = k_ticks_to_us_near64(actual_ticks - expected_ticks);
        if (delta_us < 0) delta_us = -delta_us;

        if (delta_us > max_jitter_us) max_jitter_us = delta_us;
        sum_jitter_us += delta_us;
    }

    printk("RESULT  max_jitter=%lld us  avg_jitter=%lld us\n",
           max_jitter_us, sum_jitter_us / ITERATIONS);
    return 0;
}
```

### Code — QNX (`tests/bench_timer_jitter_qnx.c`)

```c
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

#define ITERATIONS   60000
#define PERIOD_NS    1000000LL   /* 1 ms in nanoseconds */

int main(void) {
    printf("=== Benchmark 1: Periodic Timer Jitter (QNX) ===\n");
    printf("Iterations: %d  Period: 1 ms\n\n", ITERATIONS);

    struct timespec ts, actual;
    long long delta_ns, max_jitter = 0, sum_jitter = 0;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    for (int i = 0; i < ITERATIONS; i++) {
        /* Advance deadline by exactly 1 ms */
        ts.tv_nsec += PERIOD_NS;
        if (ts.tv_nsec >= 1000000000LL) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000LL;
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &actual);

        delta_ns = (actual.tv_sec  - ts.tv_sec)  * 1000000000LL
                 + (actual.tv_nsec - ts.tv_nsec);
        if (delta_ns < 0) delta_ns = -delta_ns;

        if (delta_ns > max_jitter) max_jitter = delta_ns;
        sum_jitter += delta_ns;
    }

    printf("RESULT  max_jitter=%lld us  avg_jitter=%lld us\n",
           max_jitter / 1000, (sum_jitter / ITERATIONS) / 1000);
    return 0;
}
```

### How to build and run

**Zephyr:**
```bash
# Add bench_timer_jitter_zephyr.c as the sole source in a minimal CMakeLists.txt
# inside tests/bench_timer_jitter/ then:
west build -b rpi_4b tests/bench_timer_jitter
# Copy zephyr.bin to SD card, boot, read output from UART console
```

**QNX:**
```bash
qcc -V gcc_ntoaarch64le -D_QNX_SOURCE -o bench_timer_jitter \
    tests/bench_timer_jitter_qnx.c
scp bench_timer_jitter qnxuser@<RPI_IP>:/tmp/
ssh qnxuser@<RPI_IP> "/tmp/bench_timer_jitter"
```

### Metrics to record

| Metric | QNX result | Zephyr result |
|--------|-----------|--------------|
| Max jitter (µs) | | |
| Avg jitter (µs) | | |
| Std deviation (µs) | | |
| Max jitter under load (µs) | | |

---

## 4. Benchmark 2 — Context Switch Time

### What it measures

Time for the CPU to switch execution from one thread to another.
This is the direct equivalent of **Figure 19** (RT task execution time jitter) in
the reference paper — a fast, consistent context switch means execution time
variation stays low.

### Why it matters

Sentinel-RT runs three concurrent thread types (polling thread, per-client worker,
main accept loop). Slow context switches increase response latency.

### Method — Semaphore ping-pong

Two threads of equal priority pass a token via semaphores.
The time between Thread A posting a semaphore and Thread B waking up equals
exactly one context switch.

```
Thread A posts sem_B  ──►  Thread B wakes (records T2)
T1 recorded here               │
                               │ posts sem_A
Thread A wakes ◄──────────────┘
Context switch time = T2 - T1
```

### Code — Zephyr (`tests/bench_ctx_switch_zephyr.c`)

```c
#include <zephyr/kernel.h>

#define ITERATIONS 10000

K_SEM_DEFINE(sem_a, 0, 1);
K_SEM_DEFINE(sem_b, 0, 1);

static volatile int64_t t_post;
static int64_t results[ITERATIONS];

void thread_b_fn(void *p1, void *p2, void *p3) {
    for (int i = 0; i < ITERATIONS; i++) {
        k_sem_take(&sem_b, K_FOREVER);
        int64_t t_wake = k_cycle_get_64();
        results[i] = k_cyc_to_ns_near64(t_wake - t_post);
        k_sem_give(&sem_a);
    }
}

K_THREAD_DEFINE(thread_b, 2048, thread_b_fn, NULL, NULL, NULL, 7, 0, 0);

int main(void) {
    printk("=== Benchmark 2: Context Switch Time (Zephyr) ===\n");

    int64_t max_ns = 0, sum_ns = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        t_post = k_cycle_get_64();
        k_sem_give(&sem_b);
        k_sem_take(&sem_a, K_FOREVER);
        if (results[i] > max_ns) max_ns = results[i];
        sum_ns += results[i];
    }

    printk("RESULT  max=%lld ns  avg=%lld ns\n",
           max_ns, sum_ns / ITERATIONS);
    return 0;
}
```

### Code — QNX (`tests/bench_ctx_switch_qnx.c`)

```c
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define ITERATIONS 10000

static sem_t sem_a, sem_b;
static volatile struct timespec t_post;
static long long results[ITERATIONS];

void *thread_b(void *arg) {
    struct timespec t_wake;
    for (int i = 0; i < ITERATIONS; i++) {
        sem_wait(&sem_b);
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        results[i] = (t_wake.tv_sec  - t_post.tv_sec)  * 1000000000LL
                   + (t_wake.tv_nsec - t_post.tv_nsec);
        sem_post(&sem_a);
    }
    return NULL;
}

int main(void) {
    printf("=== Benchmark 2: Context Switch Time (QNX) ===\n");

    sem_init(&sem_a, 0, 0);
    sem_init(&sem_b, 0, 0);

    pthread_t tid;
    pthread_create(&tid, NULL, thread_b, NULL);

    long long max_ns = 0, sum_ns = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, (struct timespec *)&t_post);
        sem_post(&sem_b);
        sem_wait(&sem_a);
        if (results[i] > max_ns) max_ns = results[i];
        sum_ns += results[i];
    }

    printf("RESULT  max=%lld ns  avg=%lld ns\n",
           max_ns, sum_ns / ITERATIONS);

    pthread_join(tid, NULL);
    sem_destroy(&sem_a);
    sem_destroy(&sem_b);
    return 0;
}
```

### Metrics to record

| Metric | QNX result | Zephyr result |
|--------|-----------|--------------|
| Max context switch (ns) | | |
| Avg context switch (ns) | | |
| Std deviation (ns) | | |

---

## 5. Benchmark 3 — GPIO Interrupt Latency

### What it measures

Time from a hardware GPIO edge event to the first instruction of the ISR executing.
This is the lowest-level real-time metric on embedded hardware.

### Why it matters

Vibration and sound sensors trigger digital edges on GPIO 17 and GPIO 27. The
faster and more consistently the OS responds to these edges, the more accurate
the event counting is at 1 kHz.

### Hardware setup

Connect two GPIO pins together with a short wire:
- **GPIO 23** (Physical Pin 16) → output (drives the pulse)
- **GPIO 24** (Physical Pin 18) → input with interrupt enabled

```
GPIO 23 (OUT) ──── short wire ──── GPIO 24 (IN + ISR)
```

No external components needed. Both pins are on the 40-pin header.

### Method

1. Record high-resolution timestamp T1 immediately before driving GPIO 23 high.
2. In the GPIO 24 ISR, record timestamp T2 as the very first instruction.
3. Latency = T2 − T1.
4. Repeat 10,000 times, record max and average.

### Code — Zephyr (`tests/bench_gpio_irq_zephyr.c`)

```c
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define GPIO_NODE   DT_NODELABEL(gpio0)
#define OUT_PIN     23
#define IN_PIN      24
#define ITERATIONS  10000

static const struct device *gpio_dev;
static struct gpio_callback cb_data;

static volatile int64_t t_trigger;
static int64_t latencies[ITERATIONS];
static volatile int isr_count = 0;

void gpio_isr(const struct device *dev,
              struct gpio_callback *cb, uint32_t pins) {
    int64_t t_isr = k_cycle_get_64();
    if (isr_count < ITERATIONS)
        latencies[isr_count] = k_cyc_to_ns_near64(t_isr - t_trigger);
    isr_count++;
}

int main(void) {
    printk("=== Benchmark 3: GPIO Interrupt Latency (Zephyr) ===\n");

    gpio_dev = DEVICE_DT_GET(GPIO_NODE);

    gpio_pin_configure(gpio_dev, OUT_PIN, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio_dev, IN_PIN,  GPIO_INPUT | GPIO_INT_EDGE_RISING);

    gpio_init_callback(&cb_data, gpio_isr, BIT(IN_PIN));
    gpio_add_callback(gpio_dev, &cb_data);
    gpio_pin_interrupt_configure(gpio_dev, IN_PIN, GPIO_INT_EDGE_RISING);

    for (int i = 0; i < ITERATIONS; i++) {
        isr_count = i;
        t_trigger = k_cycle_get_64();
        gpio_pin_set(gpio_dev, OUT_PIN, 1);
        k_busy_wait(200);   /* wait for ISR to fire */
        gpio_pin_set(gpio_dev, OUT_PIN, 0);
        k_busy_wait(200);
    }

    int64_t max_ns = 0, sum_ns = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        if (latencies[i] > max_ns) max_ns = latencies[i];
        sum_ns += latencies[i];
    }

    printk("RESULT  max=%lld ns  avg=%lld ns\n",
           max_ns, sum_ns / ITERATIONS);
    return 0;
}
```

### Code — QNX (`tests/bench_gpio_irq_qnx.c`)

```c
/*
 * QNX: uses InterruptAttach() for the GPIO 24 edge interrupt.
 * Requires root (ThreadCtl(_NTO_TCTL_IO, 0)) for GPIO memory mapping.
 */
#include <stdio.h>
#include <sys/neutrino.h>
#include <sys/mman.h>
#include <hw/inout.h>
#include <time.h>

#define GPIO_BASE_PHY  0xFE200000
#define GPIO_LEN       0x100
#define OUT_PIN        23
#define IN_PIN         24
#define ITERATIONS     10000

static uintptr_t gpio_base;

static inline void gpio_set(int pin, int val) {
    if (val) out32(gpio_base + 0x1C, 1u << pin);
    else     out32(gpio_base + 0x28, 1u << pin);
}

static inline int gpio_get(int pin) {
    return (in32(gpio_base + 0x34) >> pin) & 1;
}

int main(void) {
    printf("=== Benchmark 3: GPIO Interrupt Latency (QNX) ===\n");

    ThreadCtl(_NTO_TCTL_IO, 0);
    gpio_base = mmap_device_io(GPIO_LEN, GPIO_BASE_PHY);

    /* Configure OUT_PIN as output, IN_PIN as input */
    uint32_t fsel;
    fsel = in32(gpio_base + ((OUT_PIN / 10) * 4));
    fsel &= ~(7u << ((OUT_PIN % 10) * 3));
    fsel |=  (1u << ((OUT_PIN % 10) * 3));   /* output */
    out32(gpio_base + ((OUT_PIN / 10) * 4), fsel);

    fsel = in32(gpio_base + ((IN_PIN / 10) * 4));
    fsel &= ~(7u << ((IN_PIN % 10) * 3));    /* input */
    out32(gpio_base + ((IN_PIN / 10) * 4), fsel);

    struct timespec t1, t2;
    long long latencies[ITERATIONS], max_ns = 0, sum_ns = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        gpio_set(OUT_PIN, 1);

        /* Poll for the edge on IN_PIN (busy-wait — replace with
           InterruptAttach for a true interrupt-latency measurement) */
        while (!gpio_get(IN_PIN)) {}
        clock_gettime(CLOCK_MONOTONIC, &t2);

        latencies[i] = (t2.tv_sec  - t1.tv_sec)  * 1000000000LL
                     + (t2.tv_nsec - t1.tv_nsec);

        gpio_set(OUT_PIN, 0);
        while (gpio_get(IN_PIN)) {}   /* wait for line to clear */
    }

    for (int i = 0; i < ITERATIONS; i++) {
        if (latencies[i] > max_ns) max_ns = latencies[i];
        sum_ns += latencies[i];
    }

    printf("RESULT  max=%lld ns  avg=%lld ns\n",
           max_ns, sum_ns / ITERATIONS);
    return 0;
}
```

### Metrics to record

| Metric | QNX result | Zephyr result |
|--------|-----------|--------------|
| Max GPIO interrupt latency (ns) | | |
| Avg GPIO interrupt latency (ns) | | |
| Std deviation (ns) | | |

---

## 6. Benchmark 4 — 1 kHz Sensor Poll Jitter

### What it measures

How consistently the background polling thread in `drivers/sensor_manager.c`
fires at its 1 ms target interval. This is the **project-specific equivalent**
of Figure 18 and is the most directly relevant benchmark for industrial
monitoring accuracy.

### Why it matters

At 1 kHz, each missed or delayed sample means a vibration or sound event may
be miscounted. Jitter above ~100 µs starts to meaningfully affect event
accumulation accuracy over the 1-second evaluation window.

### Method

Instrument the existing `polling_thread()` in `drivers/sensor_manager.c` to
record the actual inter-wakeup delta and compare it to the 1000 µs target.

### Code — add to `drivers/sensor_manager.c`

```c
/* Add these at the top of the file */
#ifdef BENCH_POLL_JITTER
    #define BENCH_SAMPLES 60000   /* 60 seconds worth of 1 ms ticks */
    static int64_t  bench_deltas[BENCH_SAMPLES];
    static int      bench_idx = 0;
    static int64_t  bench_t_prev = 0;
#endif

/* Inside polling_thread(), replace the usleep(1000) block with: */
#ifdef BENCH_POLL_JITTER
    if (bench_t_prev != 0 && bench_idx < BENCH_SAMPLES) {
    #ifdef __ZEPHYR__
        int64_t now    = k_uptime_us();
    #else
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t now = ts_now.tv_sec * 1000000LL + ts_now.tv_nsec / 1000;
    #endif
        int64_t delta  = now - bench_t_prev - 1000;   /* deviation from 1000 µs */
        bench_deltas[bench_idx++] = (delta < 0) ? -delta : delta;
    }
    #ifdef __ZEPHYR__
        bench_t_prev = k_uptime_us();
    #else
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        bench_t_prev = _ts.tv_sec * 1000000LL + _ts.tv_nsec / 1000;
    #endif
#endif
    usleep(1000);

/* Add a reporting function callable after 60 seconds: */
#ifdef BENCH_POLL_JITTER
void bench_poll_report(void) {
    int64_t max_j = 0, sum_j = 0;
    for (int i = 0; i < bench_idx; i++) {
        if (bench_deltas[i] > max_j) max_j = bench_deltas[i];
        sum_j += bench_deltas[i];
    }
    printf("[BENCH] Poll jitter:  max=%lld us  avg=%lld us  samples=%d\n",
           max_j, sum_j / bench_idx, bench_idx);
}
#endif
```

### Build with benchmark flag enabled

```bash
# Zephyr — add to prj.conf:
CONFIG_EXTRA_CFLAGS="-DBENCH_POLL_JITTER"

# QNX — add to Makefile CFLAGS:
CFLAGS_QNX += -DBENCH_POLL_JITTER
```

### Metrics to record

| Metric | QNX result | Zephyr result |
|--------|-----------|--------------|
| Max poll jitter (µs) — idle | | |
| Avg poll jitter (µs) — idle | | |
| Max poll jitter (µs) — 3 clients connected | | |
| Avg poll jitter (µs) — 3 clients connected | | |

---

## 7. Benchmark 5 — Network Round-Trip Latency

### What it measures

Time from when the client sends the `get_sensors` command to when the last byte
of the server response arrives. This measures the combined latency of:
- Scheduler wake-up (server recv → thread schedule)
- I2C transaction (ADS1115 ADC read)
- TLS encrypt + send

### Method

Add timing to `apps/client.c` around the `get_sensors` exchange.

### Code — add to `apps/client.c`

```c
#include <sys/time.h>

/* Replace the SSL_write / SSL_read section for "get_sensors" with: */
struct timeval t_start, t_end;

gettimeofday(&t_start, NULL);
SSL_write(ssl, "get_sensors", 11);

/* Read until EOM marker (0x03) */
char buf[4096];
int total = 0, got_eom = 0;
while (!got_eom) {
    int n = SSL_read(ssl, buf + total, sizeof(buf) - total - 1);
    if (n <= 0) break;
    for (int i = total; i < total + n; i++) {
        if (buf[i] == '\x03') { got_eom = 1; break; }
    }
    total += n;
}
gettimeofday(&t_end, NULL);

long rtt_us = (t_end.tv_sec  - t_start.tv_sec)  * 1000000L
            + (t_end.tv_usec - t_start.tv_usec);
printf("RTT: %ld us\n", rtt_us);
```

Run 1,000 iterations and collect min/max/avg.

### Metrics to record

| Metric | QNX result | Zephyr result |
|--------|-----------|--------------|
| Min RTT (µs) | | |
| Max RTT (µs) | | |
| Avg RTT (µs) | | |
| Avg RTT under load (µs) | | |

---

## 8. Benchmark 6 — TLS Handshake Time

### What it measures

Time to complete a full mTLS handshake from TCP connect to first application
byte. This directly compares **OpenSSL (QNX)** vs **MbedTLS (Zephyr)** RSA
handshake performance on the same hardware.

### Method

Add timing to `apps/client.c` around the connect + SSL_connect sequence.

### Code — add to `apps/client.c`

```c
struct timeval t1, t2;

gettimeofday(&t1, NULL);

/* TCP connect */
connect(sock, (struct sockaddr *)&addr, sizeof(addr));

/* TLS handshake */
SSL_connect(ssl);

gettimeofday(&t2, NULL);

long handshake_ms = (t2.tv_sec  - t1.tv_sec)  * 1000L
                  + (t2.tv_usec - t1.tv_usec) / 1000L;
printf("TLS Handshake: %ld ms\n", handshake_ms);
```

Reconnect 100 times (disconnect with `quit` between each), record results.

### Metrics to record

| Metric | QNX (OpenSSL) | Zephyr (MbedTLS) |
|--------|--------------|-----------------|
| Min handshake time (ms) | | |
| Max handshake time (ms) | | |
| Avg handshake time (ms) | | |

---

## 9. Benchmark 7 — Memory Footprint

### What it measures

RAM usage and binary image size at idle. Smaller footprint = better fit for
constrained industrial embedded deployments.

### How to measure

**Zephyr — from the build output:**

```bash
west build -b rpi_4b .

# Image size (Flash):
ls -lh build/zephyr/zephyr.bin

# RAM usage — read from linker map:
cat build/zephyr/zephyr.map | grep -E "^(SRAM|bss|data)"

# Or read the summary printed at end of west build:
# Memory region   Used Size   Region Size   %age Used
#          FLASH:  412672 B        1 MB     39.36%
#           SRAM:   65536 B      128 MB      0.05%
```

**QNX — at runtime:**

```bash
# On the QNX Pi, after starting ims_server:
pidin mem | grep ims_server

# Or use top:
top -p $(pidin | grep ims_server | awk '{print $1}')
```

**Both — heap at runtime (add to server.c startup):**

```c
/* Zephyr */
#include <zephyr/sys/sys_heap.h>
struct sys_memory_stats stats;
sys_heap_runtime_stats_get(&_system_heap, &stats);
printk("[MEM] heap: allocated=%zu free=%zu\n",
       stats.allocated_bytes, stats.free_bytes);

/* QNX / Linux */
#include <malloc.h>
struct mallinfo mi = mallinfo();
printf("[MEM] heap: used=%d free=%d\n", mi.uordblks, mi.fordblks);
```

### Metrics to record

| Metric | QNX | Zephyr |
|--------|-----|--------|
| Binary image size (KB) | | |
| RAM at idle (KB) | | |
| Heap allocated at idle (KB) | | |
| RAM under 3-client load (KB) | | |

---

## 10. Results Table Template

Fill this in after running all benchmarks. Use it directly in your report.

```
╔══════════════════════════════════════════════════════════════════╗
║          SENTINEL-RT BENCHMARK RESULTS — QNX vs ZEPHYR          ║
║              Platform: Raspberry Pi 4 Model B                    ║
╠══════════════════════════╦═══════════════╦══════════════════════╣
║ Benchmark                ║ QNX Neutrino  ║ Zephyr RTOS          ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ Timer jitter — max (µs)  ║               ║                      ║
║ Timer jitter — avg (µs)  ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ Context switch — max(ns) ║               ║                      ║
║ Context switch — avg(ns) ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ GPIO IRQ latency max(ns) ║               ║                      ║
║ GPIO IRQ latency avg(ns) ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ Poll jitter idle max(µs) ║               ║                      ║
║ Poll jitter load max(µs) ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ Network RTT avg (µs)     ║               ║                      ║
║ Network RTT max (µs)     ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ TLS handshake avg (ms)   ║               ║                      ║
╠══════════════════════════╬═══════════════╬══════════════════════╣
║ Binary image size (KB)   ║               ║                      ║
║ RAM at idle (KB)         ║               ║                      ║
╚══════════════════════════╩═══════════════╩══════════════════════╝
```

---

## 11. How to Plot the Results

Use the following Python script to generate comparison charts from your
collected data. Save raw results as CSV files and run this script on your
workstation.

### CSV format

Create `results/timer_jitter.csv`:
```
iteration,qnx_jitter_us,zephyr_jitter_us
1,2.1,1.8
2,3.4,2.0
...
```

### Plot script (`results/plot_benchmarks.py`)

```python
import csv
import matplotlib.pyplot as plt
import numpy as np

def load_csv(filename, col):
    data = []
    with open(filename) as f:
        reader = csv.DictReader(f)
        for row in reader:
            data.append(float(row[col]))
    return np.array(data)

fig, axes = plt.subplots(2, 3, figsize=(15, 9))
fig.suptitle('Sentinel-RT: QNX vs Zephyr RTOS Benchmark\nRaspberry Pi 4 Model B',
             fontsize=14, fontweight='bold')

# --- Plot 1: Timer Jitter Distribution ---
ax = axes[0][0]
qnx_j    = load_csv('results/timer_jitter.csv',    'qnx_jitter_us')
zephyr_j = load_csv('results/timer_jitter.csv', 'zephyr_jitter_us')
ax.hist(qnx_j,    bins=50, alpha=0.6, label='QNX',    color='royalblue')
ax.hist(zephyr_j, bins=50, alpha=0.6, label='Zephyr', color='darkorange')
ax.set_title('Benchmark 1: Timer Jitter (1 ms period)')
ax.set_xlabel('Jitter (µs)')
ax.set_ylabel('Count')
ax.legend()
ax.axvline(qnx_j.max(),    color='royalblue',  linestyle='--',
           label=f'QNX max={qnx_j.max():.1f}µs')
ax.axvline(zephyr_j.max(), color='darkorange', linestyle='--',
           label=f'Zephyr max={zephyr_j.max():.1f}µs')

# --- Plot 2: Context Switch Time ---
ax = axes[0][1]
categories = ['Max', 'Avg', 'Std Dev']
qnx_ctx    = [0, 0, 0]    # fill in measured values (ns)
zephyr_ctx = [0, 0, 0]    # fill in measured values (ns)
x = np.arange(len(categories))
ax.bar(x - 0.2, qnx_ctx,    0.4, label='QNX',    color='royalblue')
ax.bar(x + 0.2, zephyr_ctx, 0.4, label='Zephyr', color='darkorange')
ax.set_title('Benchmark 2: Context Switch Time')
ax.set_ylabel('Time (ns)')
ax.set_xticks(x)
ax.set_xticklabels(categories)
ax.legend()

# --- Plot 3: GPIO Interrupt Latency ---
ax = axes[0][2]
qnx_irq    = load_csv('results/gpio_irq.csv',    'qnx_ns')
zephyr_irq = load_csv('results/gpio_irq.csv', 'zephyr_ns')
ax.boxplot([qnx_irq, zephyr_irq], labels=['QNX', 'Zephyr'],
           patch_artist=True,
           boxprops=dict(facecolor='lightblue'))
ax.set_title('Benchmark 3: GPIO IRQ Latency')
ax.set_ylabel('Latency (ns)')

# --- Plot 4: 1 kHz Poll Jitter ---
ax = axes[1][0]
labels  = ['Idle\nMax', 'Idle\nAvg', 'Load\nMax', 'Load\nAvg']
qnx_p   = [0, 0, 0, 0]    # fill in µs values
zep_p   = [0, 0, 0, 0]    # fill in µs values
x = np.arange(len(labels))
ax.bar(x - 0.2, qnx_p, 0.4, label='QNX',    color='royalblue')
ax.bar(x + 0.2, zep_p, 0.4, label='Zephyr', color='darkorange')
ax.set_title('Benchmark 4: 1 kHz Poll Jitter')
ax.set_ylabel('Jitter (µs)')
ax.set_xticks(x)
ax.set_xticklabels(labels)
ax.legend()

# --- Plot 5: Network RTT ---
ax = axes[1][1]
qnx_rtt    = load_csv('results/network_rtt.csv',    'qnx_us')
zephyr_rtt = load_csv('results/network_rtt.csv', 'zephyr_us')
ax.plot(qnx_rtt[:200],    alpha=0.7, label='QNX',    color='royalblue')
ax.plot(zephyr_rtt[:200], alpha=0.7, label='Zephyr', color='darkorange')
ax.set_title('Benchmark 5: Network RTT (first 200 samples)')
ax.set_xlabel('Sample')
ax.set_ylabel('Round-trip time (µs)')
ax.legend()

# --- Plot 6: Memory Footprint ---
ax = axes[1][2]
mem_labels = ['Image\nSize (KB)', 'RAM Idle\n(KB)', 'RAM Load\n(KB)']
qnx_mem    = [0, 0, 0]    # fill in KB values
zep_mem    = [0, 0, 0]    # fill in KB values
x = np.arange(len(mem_labels))
bars_q = ax.bar(x - 0.2, qnx_mem, 0.4, label='QNX',    color='royalblue')
bars_z = ax.bar(x + 0.2, zep_mem, 0.4, label='Zephyr', color='darkorange')
ax.set_title('Benchmark 7: Memory Footprint')
ax.set_ylabel('Size (KB)')
ax.set_xticks(x)
ax.set_xticklabels(mem_labels)
ax.legend()

plt.tight_layout()
plt.savefig('results/benchmark_comparison.png', dpi=150, bbox_inches='tight')
plt.show()
print("Chart saved to results/benchmark_comparison.png")
```

Run it:
```bash
mkdir -p results
python3 results/plot_benchmarks.py
```

---

## 12. Conclusion Criteria

Use this table to summarise which RTOS wins each category and why.

| Category | Better RTOS | Reasoning |
|----------|------------|-----------|
| **Scheduling determinism** (timer jitter) | Expected: QNX | QNX is a mature commercial RTOS with a fully preemptible microkernel designed for determinism. Zephyr is catching up but was originally designed for smaller MCUs. |
| **Context switch speed** | Expected: QNX | QNX's microkernel IPC is highly optimised for thread handoffs. |
| **GPIO interrupt latency** | Expected: QNX | QNX uses direct memory-mapped I/O with minimal driver overhead. |
| **1 kHz poll jitter** | To be measured | Depends on scheduler preemptibility under network load. |
| **Network round-trip** | Expected: Zephyr | Zephyr's lightweight network stack has less overhead than QNX's POSIX layer. |
| **TLS handshake** | Expected: similar | Both use RSA-2048 on the same CPU. MbedTLS vs OpenSSL may differ slightly. |
| **Memory footprint** | **Zephyr** | Zephyr is bare-metal with a link-time configurable kernel. QNX carries process model + IPC infrastructure overhead. |
| **Ease of deployment** | **Zephyr** | SD card flash vs QNX SDP licence + SSH deployment workflow. |
| **Open source / cost** | **Zephyr** | Zephyr is fully open source (Apache 2.0). QNX requires a commercial licence. |

### Overall conclusion framing

> If the benchmark results show QNX has lower timer jitter and interrupt latency,
> the conclusion is: **QNX wins on hard real-time determinism**, which matters
> for safety-critical systems with strict deadline requirements.
>
> If Zephyr's jitter is within an acceptable margin (e.g., < 50 µs max jitter
> for a 1 ms period), the conclusion is: **Zephyr is a viable open-source
> alternative** for industrial monitoring applications where cost, footprint,
> and deployment simplicity are priorities alongside real-time performance.

---

*Reference: C.-F. Yang, Y. Shinjo — "Compounded Real-Time Operating Systems
for Rich Real-Time Applications", IEEE Access.*
*Benchmark methodology adapted from isol-bench and Zephyr kernel benchmark suite.*
