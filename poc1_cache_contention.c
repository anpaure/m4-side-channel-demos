#include "common.h"
#include <signal.h>
#include <sys/wait.h>

#define LINE 128
#define NLINES 256
#define SECRET_LEN 16
#define SLOT_MS 100
#define BURST 50000
#define ROUNDS 2
#define GUARD_MS 4

static uint8_t *sh;
static volatile uint64_t *ctrl;
static volatile uint64_t *lines;

static uint64_t burst_ns(int cand) {
  return time_loads_m(&lines[(size_t)cand * (LINE / 8)], BURST);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  set_utility_qos();
  mach_tb_init();
  sh = mmap(NULL, 4096 + NLINES * LINE, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANON, -1, 0);
  if (sh == MAP_FAILED) { perror("mmap"); return 1; }
  ctrl = (volatile uint64_t *)sh;
  lines = (volatile uint64_t *)(sh + 4096);
  ctrl[0] = 0;
  for (int i = 0; i < NLINES; i++) lines[(size_t)i * (LINE / 8)] = 0;

  unsigned char secret[SECRET_LEN];
  arc4random_buf(secret, sizeof(secret));

  pid_t kid = fork();
  if (kid == 0) {
    while (ctrl[0] == 0) {}
    uint64_t t0 = ctrl[0];
    for (int r = 0; r < ROUNDS; r++) {
      for (int b = 0; b < SECRET_LEN; b++) {
        volatile uint64_t *line = &lines[(size_t)secret[b] * (LINE / 8)];
        uint64_t end = t0 + (uint64_t)(r * SECRET_LEN + b + 1) * SLOT_MS * 1000000ULL;
        while (mach_ns() < end) {
          uint64_t x = *line;
          *line = x + 1;
        }
      }
    }
    _exit(0);
  }

  uint64_t start = mach_ns() + 200000000ULL;
  ctrl[0] = start;
  while (mach_ns() < start + GUARD_MS * 1000000ULL) {}

  static uint16_t votes[SECRET_LEN][NLINES];
  memset(votes, 0, sizeof(votes));
  uint64_t total_busy_ns = 0;

  for (int r = 0; r < ROUNDS; r++) {
    for (int b = 0; b < SECRET_LEN; b++) {
      int slot = r * SECRET_LEN + b;
      uint64_t s0 = start + (uint64_t)slot * SLOT_MS * 1000000ULL;
      uint64_t s1 = s0 + (uint64_t)SLOT_MS * 1000000ULL - GUARD_MS * 1000000ULL;
      while (mach_ns() < s0 + GUARD_MS * 1000000ULL) {}
      while (mach_ns() + 3000000ULL < s1) {
        uint64_t best = 0;
        int bi = 0;
        for (int c = 0; c < NLINES; c++) {
          uint64_t t = burst_ns(c);
          total_busy_ns += t;
          if (t > best) { best = t; bi = c; }
        }
        votes[b][bi]++;
      }
    }
  }

  printf("true secret:     ");
  for (int i = 0; i < SECRET_LEN; i++) printf("%02x ", secret[i]);
  printf("\nrecovered secret: ");
  int ok = 0;
  for (int i = 0; i < SECRET_LEN; i++) {
    int best = 0;
    for (int c = 1; c < NLINES; c++)
      if (votes[i][c] > votes[i][best]) best = c;
    printf("%02x ", best);
    if (best == secret[i]) ok++;
  }
  printf("\nrecovered %d/%d bytes in %.1f s (%.1f ms spent probing)\n",
         ok, SECRET_LEN, (double)(mach_ns() - start) / 1e9,
         (double)total_busy_ns / 1e6);

  kill(kid, SIGKILL);
  waitpid(kid, NULL, 0);
  return ok == SECRET_LEN ? 0 : 1;
}
