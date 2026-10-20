#include "common.h"
#include <signal.h>
#include <sys/wait.h>

#define W_BYTES (32u << 20)
#define V_BYTES (512u << 20)
#define SLOT_MS 40
#define GUARD_MS 3
#define SWEEPS 5
#define WARMUPS 2

static uint8_t *sh;
static volatile uint64_t *ctrl;
static volatile uint8_t *wbuf;
static volatile uint8_t *vbuf;

static const char *msg = "M4MAX-SLC";
static int preamble[8] = {1, 0, 1, 0, 1, 0, 1, 0};

static int msg_bit(int i) {
  if (i < 8) return preamble[i];
  int c = i - 8;
  int ch = (unsigned char)msg[c / 8];
  return (ch >> (c % 8)) & 1;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  set_utility_qos();
  mach_tb_init();

  sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  if (sh == MAP_FAILED) { perror("mmap"); return 1; }
  ctrl = (volatile uint64_t *)sh;
  ctrl[0] = 0;

  wbuf = mmap(NULL, W_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  vbuf = mmap(NULL, V_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (wbuf == MAP_FAILED || vbuf == MAP_FAILED) { perror("mmap big"); return 1; }
  memset((void *)wbuf, 1, W_BYTES);
  memset((void *)vbuf, 1, V_BYTES);

  int nbits = 8 + (int)strlen(msg) * 8;
  int total_slots = nbits;

  pid_t kid = fork();
  if (kid == 0) {
    while (ctrl[0] == 0) {}
    uint64_t t0 = ctrl[0];
    for (int k = 0; k < total_slots; k++) {
      uint64_t end = t0 + (uint64_t)(k + 1) * SLOT_MS * 1000000ULL;
      if (msg_bit(k)) {
        const size_t chunk = 8u << 20;
        int stop = 0;
        while (!stop) {
          for (size_t off = 0; off < V_BYTES; off += chunk) {
            stream_w(vbuf + off, chunk);
            if (mach_ns() >= end) { stop = 1; break; }
          }
        }
      } else {
        while (mach_ns() < end) usleep(1000);
      }
    }
    _exit(0);
  }

  for (int i = 0; i < 4; i++) stream_w(wbuf, W_BYTES);
  uint64_t start = mach_ns() + 200000000ULL;
  ctrl[0] = start;

  double slot_us[256];
  memset(slot_us, 0, sizeof(slot_us));

  for (int k = 0; k < total_slots; k++) {
    uint64_t s0 = start + (uint64_t)k * SLOT_MS * 1000000ULL;
    uint64_t s1 = s0 + (uint64_t)SLOT_MS * 1000000ULL;
    while (mach_ns() < s0 + GUARD_MS * 1000000ULL) {}
    for (int i = 0; i < WARMUPS; i++) stream_w(wbuf, W_BYTES);
    uint64_t samples[8];
    int ns = 0;
    while (ns < SWEEPS && mach_ns() + 2000000ULL < s1) {
      samples[ns++] = time_sweep_w(wbuf, W_BYTES);
    }
    if (ns == 0) samples[ns++] = time_sweep_w(wbuf, W_BYTES);
    slot_us[k] = (double)median_u64(samples, ns) / 1000.0;
  }

  double lo = 0, hi = 0;
  int nlo = 0, nhi = 0;
  for (int k = 0; k < 8; k++) {
    if (preamble[k]) { hi += slot_us[k]; nhi++; }
    else { lo += slot_us[k]; nlo++; }
  }
  lo /= nlo; hi /= nhi;
  double thr = (lo + hi) / 2.0;

  printf("slot times (us) and decoded bits, threshold=%.1f us (lo=%.1f hi=%.1f)\n", thr, lo, hi);
  char decoded[32];
  int nchars = (int)strlen(msg);
  memset(decoded, 0, sizeof(decoded));
  int okbits = 0;
  for (int k = 0; k < total_slots; k++) {
    int bit = slot_us[k] > thr;
    if (bit == msg_bit(k)) okbits++;
    if (k >= 8) {
      int c = k - 8;
      if (bit) decoded[c / 8] |= (char)(1 << (c % 8));
    }
    printf("slot %3d: %6.1f us -> %d (true %d)%s\n", k, slot_us[k], bit,
      msg_bit(k), k < 8 ? " preamble" : "");
  }
  printf("true message:    \"%s\"\n", msg);
  printf("decoded message: \"%s\"\n", decoded);
  printf("bit accuracy: %d/%d (%.1f%%)\n", okbits, total_slots,
         100.0 * okbits / total_slots);

  kill(kid, SIGKILL);
  waitpid(kid, NULL, 0);
  return strcmp(decoded, msg) == 0 ? 0 : 1;
}
