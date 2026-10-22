// atlas_sidebranch_probe.cpp — Stage 1 / Pass 8.
//
// Purpose: prove Fix B on REAL V2 interfaces against a real datadir copy that
// contains the network's 8180512' record. This probe does NOT reimplement DGW:
// it calls the production BlockIndexManager and the production
// ResolveNextTargetRequiredByValue(), and prints what they return.
//
// usage: atlas-sidebranch-probe <v2root>
//        where <v2root> is <datadir>/blockindex of the isolated copy.
#include "blockindex_authoritative_startup.h"
#include "blockindex_manager.h"
#include "main.h"
#include "uint256.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ui_interface.h"
#include "checkpoints.h"

// Globals required to link the shared engine OBJS (mirrors the other tools).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

// The engine reopens stdout during startup; keep the probe report on stderr so
// nothing is lost to stdio redirection.
#define printf(...) fprintf(stderr, __VA_ARGS__)

extern bool ResolveNextTargetRequiredByValue(const uint256& hashLast, bool fProofOfStake,
                                             unsigned int* pBitsOut, std::string* error);

static uint256 H(const char* hex)
{
    uint256 h;
    h.SetHex(hex);
    return h;
}

static const char* St(BlockIndexManagerStatus s)
{
    return BlockIndexManagerStatusName(s);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <v2root>   (v2root = <datadir>/blockindex)\n", argv[0]);
        return 2;
    }
    std::string err;
    if (!InitBlockIndexAuthoritative(argv[1], &err))
    {
        printf("INIT_FAIL error=%s\n", err.c_str());
        return 1;
    }
    BlockIndexManager& mgr = GetBlockIndexManager();
    printf("INIT_OK generation=%llu nTargetSpacing=%u\n",
           (unsigned long long)AuthoritativeGeneration(), nTargetSpacing);

    const uint256 side = H("000000243c3f4738a1bc5a4fa0ce9491d272b79a1aa6a0f623bc95cfdb0a197e");
    const uint256 ours = H("000000190bbff68dd0324f64ac18040c98addc56fb3e719a6bab02d507511df0");
    const uint256 missing = H("00000000000000000000000000000000000000000000000000000000deadbeef");

    // 1. active-only reader must refuse a verified non-active record
    {
        BlockIndexSnapshot s;
        std::string e;
        BlockIndexManagerStatus st = mgr.LookupByHash(side, &s, &e);
        printf("T1 LookupByHash(side)=%s err=%s\n", St(st), e.c_str());
    }
    // 2. difficulty reader must accept it and return the real record fields
    {
        BlockIndexSnapshot s;
        std::string e;
        BlockIndexManagerStatus st = mgr.LookupByHashForDifficulty(side, &s, &e);
        printf("T2 LookupByHashForDifficulty(side)=%s hash=%s prev=%s h=%d t=%u bits=%08x inMain=%d err=%s\n",
               St(st), s.hash.ToString().c_str(), s.hashPrev.ToString().c_str(), s.height,
               s.nTime, s.nBits, (int)s.fInMainChain, e.c_str());
    }
    // 3. DGW for the network parent (spacing 2 -> 0x1d1aa450)
    {
        unsigned int bits = 0;
        std::string e;
        bool ok = ResolveNextTargetRequiredByValue(side, false, &bits, &e);
        printf("T3 DGW(side) ok=%d bits=%08x expect=1d1aa450 match=%d err=%s\n",
               (int)ok, bits, (ok && bits == 0x1d1aa450u) ? 1 : 0, e.c_str());
    }
    // 4. DGW for our parent (spacing 7 -> 0x1d28a9f3)
    {
        unsigned int bits = 0;
        std::string e;
        bool ok = ResolveNextTargetRequiredByValue(ours, false, &bits, &e);
        printf("T4 DGW(ours) ok=%d bits=%08x expect=1d28a9f3 match=%d err=%s\n",
               (int)ok, bits, (ok && bits == 0x1d28a9f3u) ? 1 : 0, e.c_str());
    }
    // 5a. genuinely absent record stays a failure
    {
        BlockIndexSnapshot s;
        std::string e;
        BlockIndexManagerStatus st = mgr.LookupByHashForDifficulty(missing, &s, &e);
        printf("T5a LookupByHashForDifficulty(absent)=%s err=%s\n", St(st), e.c_str());
    }
    // 5b. injected authority failure stays a failure (test hook already in the code)
    {
        BlockIndexSnapshot s;
        std::string e;
        AuthoritativeActiveHashFailForTest(1);
        BlockIndexManagerStatus st = mgr.LookupByHashForDifficulty(side, &s, &e);
        printf("T5b LookupByHashForDifficulty(authority_failure)=%s err=%s\n", St(st), e.c_str());
    }
    // 6. the nBits check itself is unchanged and still rejects wrong bits
    {
        uint256 probeHash = side;
        // 0x1d00ffff is far harder than the block's claimed work -> must reject.
        bool wrongOk = CheckProofOfWork(probeHash, 0x1d00ffffu);
        // The block's own claimed bits: its hash DOES satisfy them -> must accept.
        bool claimedOk = CheckProofOfWork(probeHash, 0x1d3f1960u);
        // Easier than bnProofOfWorkLimit -> rejected as below minimum work.
        bool belowLimitOk = CheckProofOfWork(probeHash, 0x207fffffu);
        printf("T6 pow(wrong=1d00ffff)=%d expect=0 pow(claimed=1d3f1960)=%d expect=1 pow(below_limit=207fffff)=%d expect=0\n",
               (int)wrongOk, (int)claimedOk, (int)belowLimitOk);
    }
    return 0;
}
