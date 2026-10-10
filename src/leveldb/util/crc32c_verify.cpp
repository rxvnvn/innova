// Bit-exactness harness: leveldb::crc32c::Extend (which on this CPU uses the new
// SSE4.2 path) MUST equal an independent, standard CRC-32C (Castagnoli) reference.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <vector>
#include "util/crc32c.h"

// Independent reference CRC-32C (Castagnoli, reflected, poly 0x82F63B78).
static uint32_t ref_crc32c(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256] = {0};
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t crc_ = crc ^ 0xffffffffu;
  for (size_t i = 0; i < n; ++i) crc_ = table[(crc_ ^ p[i]) & 0xff] ^ (crc_ >> 8);
  return crc_ ^ 0xffffffffu;
}

int main() {
  std::mt19937 rng(12345);
  unsigned fails = 0, tests = 0;
  // Known vectors (all-zero runs of specific lengths) — must hold regardless of path.
  const char* z = "";
  if (leveldb::crc32c::Extend(0, z, 0) != 0) { printf("FAIL empty\n"); ++fails; }
  char b0[8] = {0,0,0,0,0,0,0,0};
  // NOTE: values below are machine-confirmed via the fuzz vs an independent
  // reference implementation; the authoritative pin is "123456789" below.
  if (leveldb::crc32c::Extend(0, b0, 4) != ref_crc32c(0, (const uint8_t*)b0, 4)) { printf("FAIL 4x0\n"); ++fails; }
  if (leveldb::crc32c::Extend(0, b0, 3) != ref_crc32c(0, (const uint8_t*)b0, 3)) { printf("FAIL 3x0\n"); ++fails; }
  const char* a = "123456789";
  if (leveldb::crc32c::Extend(0, a, 9) != 0xe3069283) { printf("FAIL check '123456789' 0x%08x\n", leveldb::crc32c::Extend(0,a,9)); ++fails; }
  // Random fuzz across sizes 0..1024, random starts, random initial crc, and
  // several buffer alignments (cast addresses) to exercise the byte + u64 tails.
  std::vector<uint8_t> data(2048);
  for (size_t round = 0; round < 20000; ++round) {
    size_t n = rng() % 1024;
    for (size_t i = 0; i < n; ++i) data[i] = rng() & 0xff;
    uint32_t initcrc = rng();
    size_t off = rng() % 8;             // vary unaligned start
    uint32_t sse = leveldb::crc32c::Extend(initcrc, (const char*)&data[off], n);
    uint32_t ref = ref_crc32c(initcrc, &data[off], n);
    ++tests;
    if (sse != ref) {
      if (++fails <= 10) printf("FAIL round=%zu n=%zu off=%zu init=0x%08x sse=0x%08x ref=0x%08x\n",
                                round, n, off, initcrc, sse, ref);
    }
  }
  printf("crc32c bit-exactness: %u mismatches / %u random cases\n", fails, tests);
  return fails ? 1 : 0;
}