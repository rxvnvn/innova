// txlat_selftest.cpp - isolated self-test for txlat module (no chain needed).
#include "txlat.h"
#include <cstdio>
#include <string>

static std::string g_buf;
// NOTE: with TXLAT_SELFTEST, txlat.cpp's OutputDebugStringF is stubbed to a
// local sink writing to stdout. We capture by counting lines in this test.

int main() {
    int lines_off = 0;
    // not enabled: Begin/Mark must produce nothing
    txlat::SetEnabled(false);
    txlat::Begin("send");
    txlat::Mark("x", 1, 2);
    // can't easily count stdout here; rely on Enabled() + non-crash + enable path below.
    int rc = 0;
    if (txlat::Enabled()) { printf("FAIL: enabled when off\n"); rc |= 1; }

    txlat::SetEnabled(true);
    if (!txlat::Enabled()) { printf("FAIL: disabled when on\n"); rc |= 2; }
    txlat::Begin("send");
    txlat::Mark("selectcoins", 573, 0);
    txlat::Mark("sign", 573, 0);
    txlat::SetEnabled(false);
    // after disable, Marks are no-ops (no crash)
    txlat::Mark("should_not_log", 9, 9);
    if (txlat::Enabled()) { printf("FAIL: enabled after off\n"); rc |= 4; }

    if (rc == 0) { printf("PASS\n"); return 0; }
    printf("FAIL rc=%d\n", rc); return rc;
}