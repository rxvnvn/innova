// Statistical study of the coin-selection randomness: compares the three candidate bit sources
// (LCG from bdc4238d, xoshiro128**, buffered CSPRNG), their bit-level quality, their cost, and
// the distribution of what the subset sampler actually picks.
//
// This is a measurement harness, not a pass/fail gate: it prints one machine-readable line per
// measurement (prefix RNGSTUDY) so the results can be archived as evidence, plus loose sanity
// assertions that would only fire on a gross defect.
#include <boost/test/unit_test.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <stdint.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

#include "wallet.h"
#include "util.h"

BOOST_AUTO_TEST_SUITE(selectcoins_rng_study_tests)

namespace {

// ---------------------------------------------------------------- bit sources
// A. The LCG shipped in bdc4238d: bit 19 of each LCG step. Bytes are assembled from 8 such
//    bits, so the studied stream is bit-identical to that commit's sampler input.
struct LcgSource {
    uint32_t state;
    explicit LcgSource(uint32_t seed) : state(seed ? seed : 0x9e3779b9u) {}
    bool Bit() { state = state * 1664525u + 1013904223u; return ((state >> 19) & 1u) != 0; }
};

// B. xoshiro128** -- a modern, fast, well-tested non-cryptographic PRNG.
struct XoshiroSource {
    uint32_t s[4];
    explicit XoshiroSource(uint32_t seed) {
        // splitmix32 seeding, so different seeds give uncorrelated states
        uint32_t x = seed ? seed : 1u;
        for (int i = 0; i < 4; ++i) {
            x += 0x9e3779b9u;
            uint32_t z = x;
            z = (z ^ (z >> 16)) * 0x85ebca6bu;
            z = (z ^ (z >> 13)) * 0xc2b2ae35u;
            s[i] = z ^ (z >> 16);
        }
        if ((s[0] | s[1] | s[2] | s[3]) == 0) s[0] = 1;
    }
    static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    uint32_t Next() {
        const uint32_t result = rotl(s[1] * 5u, 7) * 9u;
        const uint32_t t = s[1] << 9;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
        s[3] = rotl(s[3], 11);
        return result;
    }
};

// C. Buffered CSPRNG (the shipped variant): plain GetRandBytes in 4 KiB chunks.
//    No state of our own -- this is exactly the production source, GetRandBytes.

// Generic bit-reader over a byte producer, mirroring CoinSelectionRng/ByteSourceBits.
template <typename Producer>
struct BitReader {
    Producer p;
    unsigned char buf[4096];
    size_t pos;
    explicit BitReader(uint32_t seed) : p(seed), pos(sizeof(buf) * 8) {}
    bool Bit() {
        if (pos == sizeof(buf) * 8) { p.Fill(buf, sizeof(buf)); pos = 0; }
        const bool f = ((buf[pos >> 3] >> (pos & 7)) & 1u) != 0;
        ++pos;
        return f;
    }
};

struct LcgProducer {
    LcgSource src;
    explicit LcgProducer(uint32_t seed) : src(seed) {}
    void Fill(unsigned char* out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            unsigned char b = 0;
            for (int k = 0; k < 8; ++k) if (src.Bit()) b |= (unsigned char)(1u << k);
            out[i] = b;
        }
    }
};

struct XoshiroProducer {
    XoshiroSource src;
    explicit XoshiroProducer(uint32_t seed) : src(seed) {}
    void Fill(unsigned char* out, size_t n) {
        size_t i = 0;
        while (i < n) {
            uint32_t v = src.Next();
            for (int k = 0; k < 4 && i < n; ++k, ++i) out[i] = (unsigned char)(v >> (8 * k));
        }
    }
};

struct CsprngProducer {
    explicit CsprngProducer(uint32_t) {}
    void Fill(unsigned char* out, size_t n) { GetRandBytes(out, (int)n); }
};

// ---------------------------------------------------------------- byte-level statistics
struct ByteStats {
    double ones;          // fraction of 1 bits
    double chi2_bytes;    // chi-square over 256 byte values, df=255
    double max_abs_corr;  // max |serial correlation| over lags 1..32
    double runs_z;        // z-score of the runs test on the bit stream
};

ByteStats AnalyzeStream(unsigned char* data, size_t n) {
    ByteStats st;
    const size_t totalBits = n * 8;
    size_t ones = 0;
    double counts[256] = {0};
    for (size_t i = 0; i < n; ++i) {
        counts[data[i]] += 1.0;
        for (int k = 0; k < 8; ++k) if ((data[i] >> k) & 1u) ++ones;
    }
    st.ones = (double)ones / (double)totalBits;

    const double expected = (double)n / 256.0;
    double chi2 = 0;
    for (int i = 0; i < 256; ++i) {
        const double d = counts[i] - expected;
        chi2 += d * d / expected;
    }
    st.chi2_bytes = chi2;

    // serial correlation of the bit stream at several lags
    st.max_abs_corr = 0;
    double mean = st.ones;
    for (int lag = 1; lag <= 32; ++lag) {
        double sxy = 0, sxx = 0, syy = 0;
        size_t cnt = 0;
        for (size_t i = lag; i < totalBits; i += 7) { // strided for speed, unbiased
            int a = (data[i >> 3] >> (i & 7)) & 1;
            size_t j = i - lag;
            int b = (data[j >> 3] >> (j & 7)) & 1;
            sxy += (a - mean) * (b - mean);
            sxx += (a - mean) * (a - mean);
            syy += (b - mean) * (b - mean);
            ++cnt;
        }
        if (cnt > 1 && sxx > 0 && syy > 0) {
            double c = sxy / std::sqrt(sxx * syy);
            if (std::fabs(c) > st.max_abs_corr) st.max_abs_corr = std::fabs(c);
        }
    }

    // runs test (Wald-Wolfowitz) on the bit stream
    size_t runs = 1;
    for (size_t i = 1; i < totalBits; ++i) {
        int a = (data[i >> 3] >> (i & 7)) & 1;
        int b = (data[(i - 1) >> 3] >> ((i - 1) & 7)) & 1;
        if (a != b) ++runs;
    }
    const double n1 = (double)ones, n0 = (double)(totalBits - ones);
    const double expRuns = 2.0 * n1 * n0 / (double)totalBits + 1.0;
    const double varRuns = 2.0 * n1 * n0 * (2.0 * n1 * n0 - (double)totalBits) /
                           ((double)totalBits * (double)totalBits * ((double)totalBits - 1.0));
    st.runs_z = varRuns > 0 ? (double)(runs - expRuns) / std::sqrt(varRuns) : 0.0;
    return st;
}

// ---------------------------------------------------------------- sampler outcomes
struct SampleStats {
    double mean_coins, sd_coins;
    int min_coins, max_coins;
    double mean_overshoot, max_overshoot;
    int exact_hits;
    double mean_best;
};

// Deterministic value pools. Identical pools are fed to every source, so any difference in the
// outcomes comes from the randomness alone.
std::vector<int64_t> PoolOfEqualCoins(int nCoins, int64_t value)
{
    return std::vector<int64_t>(nCoins, value);
}

// Values spread over two orders of magnitude: this is the case where the sampler actually has
// to search (few exact hits), so the number of iterations matters and any bias in the bits would
// show up as a skewed overshoot or coin-count distribution.
std::vector<int64_t> PoolOfVariedCoins(int nCoins, uint32_t seed)
{
    std::vector<int64_t> v;
    uint32_t s = seed;
    for (int i = 0; i < nCoins; ++i) {
        s = s * 1664525u + 1013904223u;
        const int64_t lo = 1000000;            // 0.01 INN
        const int64_t hi = 50000000;           // 0.50 INN
        v.push_back(lo + (int64_t)((uint64_t)(s >> 8) * (uint64_t)(hi - lo) >> 24));
    }
    return v;
}

// Shape of the benchmark wallet: a long tail of 0.07 INN coins plus a few large ones.
std::vector<int64_t> PoolOfBenchmarkShape(int nCoins, int nLarge)
{
    std::vector<int64_t> v(nCoins, 7000000);
    for (int i = 0; i < nLarge && i < nCoins; ++i)
        v[i] = (i % 2 == 0) ? 500000000 : 1000000000;
    return v;
}

SampleStats RunSamplerStudy(const std::vector<int64_t>& values, int64_t nTarget, int nSeeds,
                            void (*randbytes)(unsigned char*, int), long long& total_us)
{
    std::vector<double> coinCounts;
    double sumOvershoot = 0;
    double maxOvershoot = 0;
    int exact = 0;
    double sumBest = 0;
    total_us = 0;

    for (int s = 0; s < nSeeds; ++s) {
        std::vector<char> vfBest;
        int64_t nBest = 0;
        boost::posix_time::ptime t0 = boost::posix_time::microsec_clock::universal_time();
        selectcoins_test_RunSampler(values, nTarget, 1000, randbytes, vfBest, nBest);
        boost::posix_time::ptime t1 = boost::posix_time::microsec_clock::universal_time();
        total_us += (t1 - t0).total_microseconds();

        double cnt = 0;
        for (size_t i = 0; i < vfBest.size(); ++i) if (vfBest[i]) cnt += 1;
        coinCounts.push_back(cnt);
        sumBest += (double)nBest;
        const double over = (double)(nBest - nTarget);
        sumOvershoot += over;
        if (over > maxOvershoot) maxOvershoot = over;
        if (nBest == nTarget) ++exact;
    }

    SampleStats st;
    double mean = 0;
    for (size_t i = 0; i < coinCounts.size(); ++i) mean += coinCounts[i];
    mean /= (double)coinCounts.size();
    double var = 0;
    for (size_t i = 0; i < coinCounts.size(); ++i) var += (coinCounts[i] - mean) * (coinCounts[i] - mean);
    var /= (double)coinCounts.size();
    st.mean_coins = mean;
    st.sd_coins = std::sqrt(var);
    st.min_coins = (int)*std::min_element(coinCounts.begin(), coinCounts.end());
    st.max_coins = (int)*std::max_element(coinCounts.begin(), coinCounts.end());
    st.mean_overshoot = sumOvershoot / (double)nSeeds;
    st.max_overshoot = maxOvershoot;
    st.exact_hits = exact;
    st.mean_best = sumBest / (double)nSeeds;
    return st;
}

// Deterministic byte producers for the sampler study (the seam is function-pointer based).
// Each call advances a process-wide stream, so successive runs see different randomness --
// which is what a distribution study wants.
void LcgBytes(unsigned char* buf, int n) { static LcgProducer p(0xC0FFEEu); p.Fill(buf, (size_t)n); }
void XoshiroBytes(unsigned char* buf, int n) { static XoshiroProducer p(0xC0FFEEu); p.Fill(buf, (size_t)n); }

// Fixed-stream generator used by the determinism gate (resettable, so two runs see the same
// stream -- otherwise the gate would compare different sequences).
uint32_t g_detStudyState = 0x1234ABCDu;
void ResetDeterministicForStudy() { g_detStudyState = 0x1234ABCDu; }
void DeterministicForStudy(unsigned char* buf, int n)
{
    for (int i = 0; i < n; ++i) {
        g_detStudyState = g_detStudyState * 1664525u + 1013904223u;
        buf[i] = (unsigned char)(g_detStudyState >> 24);
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(bit_quality_of_candidate_sources)
{
    const size_t kBytes = 512 * 1024; // 4 Mbit
    std::vector<unsigned char> buf(kBytes);

    {
        LcgProducer p(12345);
        p.Fill(&buf[0], kBytes);
        ByteStats st = AnalyzeStream(&buf[0], kBytes);
        fprintf(stderr, "RNGSTUDY bits source=lcg ones=%.6f chi2_bytes=%.1f max_abs_corr=%.6f runs_z=%.3f\n",
                st.ones, st.chi2_bytes, st.max_abs_corr, st.runs_z);
    }
    {
        XoshiroProducer p(12345);
        p.Fill(&buf[0], kBytes);
        ByteStats st = AnalyzeStream(&buf[0], kBytes);
        fprintf(stderr, "RNGSTUDY bits source=xoshiro ones=%.6f chi2_bytes=%.1f max_abs_corr=%.6f runs_z=%.3f\n",
                st.ones, st.chi2_bytes, st.max_abs_corr, st.runs_z);
    }
    {
        CsprngProducer p(12345);
        p.Fill(&buf[0], kBytes);
        ByteStats st = AnalyzeStream(&buf[0], kBytes);
        fprintf(stderr, "RNGSTUDY bits source=csprng ones=%.6f chi2_bytes=%.1f max_abs_corr=%.6f runs_z=%.3f\n",
                st.ones, st.chi2_bytes, st.max_abs_corr, st.runs_z);
    }
}

BOOST_AUTO_TEST_CASE(cost_of_random_bits)
{
    const size_t kBytes = 192 * 1024; // 1.5 Mbit: one sampler pass over a 1.5k-coin pool
    std::vector<unsigned char> buf(kBytes);

    {
        boost::posix_time::ptime t0 = boost::posix_time::microsec_clock::universal_time();
        LcgProducer p(7); p.Fill(&buf[0], kBytes);
        boost::posix_time::ptime t1 = boost::posix_time::microsec_clock::universal_time();
        fprintf(stderr, "RNGSTUDY cost source=lcg bytes=%zu us=%lld\n", kBytes,
                (long long)(t1 - t0).total_microseconds());
    }
    {
        boost::posix_time::ptime t0 = boost::posix_time::microsec_clock::universal_time();
        XoshiroProducer p(7); p.Fill(&buf[0], kBytes);
        boost::posix_time::ptime t1 = boost::posix_time::microsec_clock::universal_time();
        fprintf(stderr, "RNGSTUDY cost source=xoshiro bytes=%zu us=%lld\n", kBytes,
                (long long)(t1 - t0).total_microseconds());
    }
    {
        boost::posix_time::ptime t0 = boost::posix_time::microsec_clock::universal_time();
        CsprngProducer p(7); p.Fill(&buf[0], kBytes);
        boost::posix_time::ptime t1 = boost::posix_time::microsec_clock::universal_time();
        fprintf(stderr, "RNGSTUDY cost source=csprng bytes=%zu us=%lld\n", kBytes,
                (long long)(t1 - t0).total_microseconds());
    }
}

BOOST_AUTO_TEST_CASE(sampler_outcomes_across_pools_and_sources)
{
    struct Src { const char* name; void (*fn)(unsigned char*, int); };
    const Src sources[3] = {
        {"lcg", LcgBytes},
        {"xoshiro", XoshiroBytes},
        {"csprng", GetRandBytes},
    };

    // Three pool shapes x three sizes. Targets are NOT multiples of the coin values, so the
    // sampler has to approximate and the iterations actually run to completion.
    struct Fixture { const char* name; std::vector<int64_t> values; int64_t target; int seeds; };
    std::vector<Fixture> fixtures;
    {
        Fixture f; f.name = "equal724";   f.values = PoolOfEqualCoins(724, 7000000);  f.target = 4004000000LL; f.seeds = 40; fixtures.push_back(f);
        Fixture g; g.name = "varied724";  g.values = PoolOfVariedCoins(724, 11);       g.target = 4000000000LL; g.seeds = 40; fixtures.push_back(g);
        Fixture h; h.name = "varied1382"; h.values = PoolOfVariedCoins(1382, 12);      h.target = 4000000000LL; h.seeds = 30; fixtures.push_back(h);
        Fixture k; k.name = "varied2526"; k.values = PoolOfVariedCoins(2526, 13);      k.target = 4000000000LL; k.seeds = 20; fixtures.push_back(k);
        Fixture m; m.name = "bench2526";  m.values = PoolOfBenchmarkShape(2526, 26);   m.target = 4004000000LL; m.seeds = 20; fixtures.push_back(m);
    }

    for (size_t fi = 0; fi < fixtures.size(); ++fi) {
        for (int si = 0; si < 3; ++si) {
            long long total_us = 0;
            SampleStats st = RunSamplerStudy(fixtures[fi].values, fixtures[fi].target,
                                             fixtures[fi].seeds, sources[si].fn, total_us);
            fprintf(stderr, "RNGSTUDY sampler fixture=%s source=%s seeds=%d mean_coins=%.2f sd_coins=%.2f "
                   "min=%d max=%d mean_overshoot_inn=%.9f max_overshoot_inn=%.9f exact=%d "
                   "us_per_pass=%.1f\n",
                   fixtures[fi].name, sources[si].name, fixtures[fi].seeds, st.mean_coins, st.sd_coins,
                   st.min_coins, st.max_coins, st.mean_overshoot / 1e8, st.max_overshoot / 1e8,
                   st.exact_hits, (double)total_us / (double)fixtures[fi].seeds);
        }
    }
}

BOOST_AUTO_TEST_CASE(sampler_always_reaches_target_and_is_reproducible)
{
    // Sanity gates: the sampler must always reach the target, and a fixed byte stream must
    // reproduce the same result (that is what the parity tests rely on).
    std::vector<int64_t> values = PoolOfVariedCoins(300, 99);
    const int64_t target = 4000000000LL;

    std::vector<char> a, b;
    int64_t bestA = 0, bestB = 0;
    ResetDeterministicForStudy();
    selectcoins_test_RunSampler(values, target, 1000, DeterministicForStudy, a, bestA);
    ResetDeterministicForStudy();
    selectcoins_test_RunSampler(values, target, 1000, DeterministicForStudy, b, bestB);
    BOOST_CHECK_EQUAL(bestA, bestB);
    BOOST_CHECK(a == b);
    BOOST_CHECK(bestA >= target);
}

BOOST_AUTO_TEST_SUITE_END()
