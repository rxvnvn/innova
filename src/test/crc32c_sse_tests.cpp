// Permanent regression gate for the LevelDB crc32c SSE4.2 acceleration
// (block-index V2 IBD performance, 2026-10-10).
//
// leveldb::crc32c::Extend gained a runtime-CPUID-gated SSE4.2 hardware path
// (~56% single-core CPU during IBD). This test verifies Extend is bit-identical
// to an independent standard CRC-32C (Castagnoli) reference across aligned and
// misaligned buffers and random initial CRCs -- so the accelerated path can
// never silently change on-disk checksums or consensus data.
#include <boost/test/unit_test.hpp>
#include <random>
#include <vector>
#include "util/crc32c.h"

BOOST_AUTO_TEST_SUITE(crc32c_sse_tests)

// Independent CRC-32C (Castagnoli, reflected, poly 0x82F63B78).
static uint32_t crc32c_ref(uint32_t crc, const uint8_t* p, size_t n) {
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

BOOST_AUTO_TEST_CASE(matches_standard_reference)
{
    // Canonical CRC-32C check vector.
    BOOST_CHECK_EQUAL(leveldb::crc32c::Extend(0, "123456789", 9), 0xe3069283u);

    std::mt19937 rng(20261010u);
    std::vector<uint8_t> buf(4096, 0);
    for (size_t round = 0; round < 5000; ++round) {
        size_t n = rng() % 2048;
        for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)rng();
        size_t off = rng() % 8;                 // vary alignment
        size_t init = (size_t)rng();            // <-- keeps value within uint32
        uint32_t c = leveldb::crc32c::Extend((uint32_t)init, (const char*)&buf[off], n);
        uint32_t r = crc32c_ref((uint32_t)init, &buf[off], n);
        BOOST_CHECK_EQUAL(c, r);
        if (c != r) return;                     // avoid drowning on a regression
    }
}

BOOST_AUTO_TEST_SUITE_END()