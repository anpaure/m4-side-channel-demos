#include "common.h"
#include <signal.h>
#include <sys/wait.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

typedef struct {
  uint32_t key;
  struct { uint8_t major, minor, build, reserved; uint16_t release; } vers;
  struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } pLimitData;
  struct { uint32_t dataSize, dataType; uint8_t dataAttributes; } keyInfo;
  uint8_t result, status, data8;
  uint32_t data32;
  uint8_t bytes[32];
} SMCKeyData_t;

static io_connect_t smc_conn;
static uint32_t pstr_size;

static kern_return_t smc_call(SMCKeyData_t *in, SMCKeyData_t *out) {
  size_t sz = sizeof(SMCKeyData_t);
  return IOConnectCallStructMethod(smc_conn, 2, in, sizeof(SMCKeyData_t), out, &sz);
}
static int smc_open(void) {
  io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                 IOServiceMatching("AppleSMC"));
  if (!svc) return -1;
  if (IOServiceOpen(svc, mach_task_self(), 0, &smc_conn) != KERN_SUCCESS) return -1;
  SMCKeyData_t in, out;
  memset(&in, 0, sizeof(in));
  in.key = 'PSTR';
  in.data8 = 9;
  if (smc_call(&in, &out) != KERN_SUCCESS || out.result) return -1;
  pstr_size = out.keyInfo.dataSize;
  return 0;
}
static int smc_power(float *w) {
  SMCKeyData_t in, out;
  memset(&in, 0, sizeof(in));
  in.key = 'PSTR';
  in.keyInfo.dataSize = pstr_size;
  in.data8 = 5;
  if (smc_call(&in, &out) != KERN_SUCCESS || out.result) return -1;
  memcpy(w, out.bytes, 4);
  return 0;
}

#define SLOT_MS 4000
#define SAMPLE_FROM_MS 2500
#define SAMPLE_TO_MS 100
#define NTHREADS 8

static const char *msg = "M4";
static int preamble[4] = {1, 0, 1, 0};
static int msg_bit(int i) {
  if (i < 4) return preamble[i];
  int c = i - 4;
  return ((unsigned char)msg[c / 8] >> (c % 8)) & 1;
}

static uint8_t *sh;
static volatile uint64_t *ctrl;
static volatile int g_active, g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static void *worker(void *arg) {
  (void)arg;
  volatile double d = 1.000001;
  pthread_mutex_lock(&g_mu);
  while (!g_stop) {
    while (!g_active && !g_stop) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    while (g_active) {
      for (int i = 0; i < 20000; i++) d = d * 1.0000001 + 1e-7;
    }
    pthread_mutex_lock(&g_mu);
  }
  pthread_mutex_unlock(&g_mu);
  return NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  set_utility_qos();
  mach_tb_init();
  if (smc_open() != 0) { printf("cant open SMC\n"); return 1; }

  sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
  if (sh == MAP_FAILED) { perror("mmap"); return 1; }
  ctrl = (volatile uint64_t *)sh;
  ctrl[0] = 0;

  int nbits = 4 + (int)strlen(msg) * 8;

  pid_t kid = fork();
  if (kid == 0) {
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker, NULL);
    while (ctrl[0] == 0) {}
    uint64_t t0 = ctrl[0];
    for (int k = 0; k < nbits; k++) {
      uint64_t end = t0 + (uint64_t)(k + 1) * SLOT_MS * 1000000ULL;
      pthread_mutex_lock(&g_mu);
      g_active = msg_bit(k);
      pthread_cond_broadcast(&g_cv);
      pthread_mutex_unlock(&g_mu);
      while (mach_ns() < end) usleep(2000);
    }
    pthread_mutex_lock(&g_mu);
    g_active = 0;
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    _exit(0);
  }

  uint64_t start = mach_ns() + 200000000ULL;
  ctrl[0] = start;

  double slot_w[256];
  memset(slot_w, 0, sizeof(slot_w));

  for (int k = 0; k < nbits; k++) {
    uint64_t s0 = start + (uint64_t)k * SLOT_MS * 1000000ULL;
    uint64_t s1 = s0 + (uint64_t)SLOT_MS * 1000000ULL;
    while (mach_ns() < s0 + SAMPLE_FROM_MS * 1000000ULL) {}
    double sum = 0, mn = 1e9, mx = 0;
    int n = 0;
    while (mach_ns() < s1 - SAMPLE_TO_MS * 1000000ULL) {
      float w;
      if (smc_power(&w) != 0) continue;
      sum += w;
      if (w < mn) mn = w;
      if (w > mx) mx = w;
      n++;
    }
    slot_w[k] = n ? sum / n : 0;
    int bit = slot_w[k] > 0;
    if (n == 0) printf("slot %2d: no samples\n", k);
    else printf("slot %2d: avg %6.2f W (min %.2f max %.2f, n=%d) true %d\n",
                k, slot_w[k], mn, mx, n, msg_bit(k));
    (void)bit;
  }

  double lo = 0, hi = 0;
  int nlo = 0, nhi = 0;
  for (int k = 0; k < 4; k++) {
    if (preamble[k]) { hi += slot_w[k]; nhi++; }
    else { lo += slot_w[k]; nlo++; }
  }
  lo /= nlo; hi /= nhi;
  double thr = (lo + hi) / 2.0;

  char decoded[32];
  memset(decoded, 0, sizeof(decoded));
  int okbits = 0;
  for (int k = 0; k < nbits; k++) {
    int bit = slot_w[k] > thr;
    if (bit == msg_bit(k)) okbits++;
    if (k >= 4) {
      int c = k - 4;
      if (bit) decoded[c / 8] |= (char)(1 << (c % 8));
    }
  }
  printf("idle=%.2f W busy=%.2f W threshold=%.2f W\n", lo, hi, thr);
  printf("true message:    \"%s\"\n", msg);
  printf("decoded message: \"%s\"\n", decoded);
  printf("bit accuracy: %d/%d (%.1f%%)\n", okbits, nbits, 100.0 * okbits / nbits);

  kill(kid, SIGKILL);
  waitpid(kid, NULL, 0);
  return strcmp(decoded, msg) == 0 ? 0 : 1;
}
