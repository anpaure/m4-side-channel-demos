#pragma once
// Shared side-channel demo helpers for Apple Silicon (arm64, macOS).
// Timer: generic virtual counter CNTVCT_EL0 (~1 GHz on M4, calibrated at start).
// NOTE: DC CIVAC/IVAC are unusable from EL0 on macOS 15 (no-op / SIGILL),
// so all demos use eviction-based flushing instead (Evict+Reload style).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>

static double g_ns_per_tick = 1.0;
static mach_timebase_info_data_t g_tb = {1, 1};

static inline uint64_t timer_tick(void) {
  uint64_t t;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
  return t;
}

static void mach_tb_init(void) { mach_timebase_info(&g_tb); }
static inline uint64_t mach_ns(void) {
  uint64_t t = mach_absolute_time();
  return (uint64_t)(((__uint128_t)t * g_tb.numer) / g_tb.denom);
}
static inline uint64_t time_loads_m(volatile uint64_t *p, int n) {
  uint64_t t0, t1, s = 0;
  __asm__ volatile("dsb ish" ::: "memory");
  t0 = mach_ns();
  for (int i = 0; i < n; i++) s += *p;
  __asm__ volatile("dsb ish" ::: "memory");
  t1 = mach_ns();
  (void)s;
  return t1 - t0;
}
static inline uint64_t time_sweep_w(volatile uint8_t *b, size_t bytes) {
  uint64_t t0, t1, s = 0;
  __asm__ volatile("dsb ish" ::: "memory");
  t0 = mach_ns();
  for (size_t i = 0; i < bytes; i += 128) {
    b[i] = (uint8_t)(i >> 7);
    s += b[i];
  }
  __asm__ volatile("dsb ish" ::: "memory");
  t1 = mach_ns();
  __asm__ volatile("" ::"r"(s));
  return t1 - t0;
}
static void tb_calibrate(void) {
  uint64_t t0 = timer_tick();
  usleep(120000);
  uint64_t t1 = timer_tick();
  g_ns_per_tick = 120.0e6 / (double)(t1 - t0);
}
static inline uint64_t to_ns(uint64_t t) {
  return (uint64_t)((double)t * g_ns_per_tick);
}
static inline uint64_t now_ns(void) { return to_ns(timer_tick()); }

static inline void mem_barrier(void) { __asm__ volatile("dsb ish" ::: "memory"); }

static inline void set_utility_qos(void) {
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}

// Time `n` volatile loads of one word. Aggregation amplifies the
// L2-hit vs DRAM-miss difference (~250ns) well above timer noise.
static inline uint64_t time_loads(volatile uint64_t *p, int n) {
  uint64_t t0, t1, s = 0;
  __asm__ volatile("isb" ::: "memory");
  t0 = timer_tick();
  __asm__ volatile("isb" ::: "memory");
  for (int i = 0; i < n; i++) s += *p;
  __asm__ volatile("dsb ish" ::: "memory");
  t1 = timer_tick();
  (void)s;
  return t1 - t0;
}

// Allocate an eviction buffer (heap, resident) and evict L1/L2/SLC by
// walking it with a 128-byte stride.
static inline uint8_t *evbuf_alloc(size_t bytes) {
  uint8_t *b = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
  if (b == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  memset(b, 1, bytes);
  return b;
}
static inline void evict(uint8_t *b, size_t bytes) {
  uint64_t s = 0;
  for (size_t i = 0; i < bytes; i += 128) s += *(volatile uint8_t *)&b[i];
  __asm__ volatile("dsb ish" ::: "memory");
  __asm__ volatile("" ::"r"(s));
}

static inline void stream_w(volatile uint8_t *b, size_t bytes) {
  uint64_t s = 0;
  for (size_t i = 0; i < bytes; i += 128) {
    b[i] = (uint8_t)(i >> 7);
    s += b[i];
  }
  __asm__ volatile("dsb ish" ::: "memory");
  __asm__ volatile("" ::"r"(s));
}

static inline uint64_t time_stream_w(volatile uint8_t *b, size_t bytes) {
  uint64_t t0, t1, s = 0;
  __asm__ volatile("isb" ::: "memory");
  t0 = timer_tick();
  for (size_t i = 0; i < bytes; i += 128) {
    b[i] = (uint8_t)(i >> 7);
    s += b[i];
  }
  __asm__ volatile("dsb ish" ::: "memory");
  t1 = timer_tick();
  __asm__ volatile("" ::"r"(s));
  return t1 - t0;
}

static int u64cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}
static uint64_t median_u64(uint64_t *v, int n) {
  qsort(v, n, sizeof(uint64_t), u64cmp);
  return v[n / 2];
}