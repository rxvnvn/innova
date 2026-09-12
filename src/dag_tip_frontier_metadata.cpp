// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.1c — DAG tip frontier generation-lifecycle metadata implementation.

#include "dag_tip_frontier_metadata.h"

#include "dag_tip_frontier.h"            // BuildDagTipFrontier / reader
#include "dag_source_binding_verifier.h" // reusable digest contract
#include "fixed_blockindex_store.h"      // declared generation capability

#include <boost/filesystem.hpp>
#include <openssl/sha.h>

#include <cstring>
#include <fstream>

namespace {

static bool NdStr(std::string* e, const std::string& m) { if (e) *e = m; return false; }

// Frontier artifact binding bytes: generation(8 LE) || dagInputDigest(32) ||
// frontierDigest(32) || tipCount(8 LE).
void FrontBinding(unsigned char out[32], uint64_t generation,
                  const unsigned char dagInputDigest[32],
                  const unsigned char frontierDigest[32], uint64_t tipCount)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char b[8];
    uint64_t v = generation;
    for (int i = 0; i < 8; ++i) { b[i] = (unsigned char)(v & 0xff); v >>= 8; }
    SHA256_Update(&ctx, b, 8);
    SHA256_Update(&ctx, dagInputDigest, 32);
    SHA256_Update(&ctx, frontierDigest, 32);
    v = tipCount;
    for (int i = 0; i < 8; ++i) { b[i] = (unsigned char)(v & 0xff); v >>= 8; }
    SHA256_Update(&ctx, b, 8);
    SHA256_Final(out, &ctx);
}

} // namespace

bool EnsureDagTipFrontierMetadata(const std::string& dagLinksDir,
                                  const std::string& generationDir,
                                  uint64_t generationId,
                                  const unsigned char dagInputDigest[32],
                                  std::string* error)
{
    const boost::filesystem::path outPath =
        boost::filesystem::path(generationDir) / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    dag_tip_frontier::BuildOptions opts;
    opts.tempParent = boost::filesystem::temp_directory_path().string();
    dag_tip_frontier::BuildResult res;
    if (!dag_tip_frontier::BuildDagTipFrontier(dagLinksDir, dagInputDigest, generationId,
                                              outPath.string(), opts, &res))
    {
        std::string status = "build dag-tip-frontier failed";
        if (!res.error.empty()) status += ": " + res.error;
        return NdStr(error, status);
    }
    return true;
}

bool ComputeDagTipFrontierBinding(const std::string& generationDir,
                                  uint64_t generationId,
                                  const unsigned char dagInputDigest[32],
                                  unsigned char out[32],
                                  std::string* error)
{
    const boost::filesystem::path artifact =
        boost::filesystem::path(generationDir) / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    if (!boost::filesystem::exists(artifact))
        return NdStr(error, "dag-tip-frontier.dat absent (required capability)");
    dag_tip_frontier::TipFrontierReader r;
    std::string rerr;
    // Open validates generation + exact dagInputDigest self-binding and header integrity.
    if (!r.Open(artifact.string(), generationId, dagInputDigest, &rerr))
        return NdStr(error, "dag-tip-frontier open failed: " + rerr);
    unsigned char frontierDigest[32];
    if (!r.GetFrontierDigest(frontierDigest))
        return NdStr(error, "dag-tip-frontier digest unavailable");
    uint64_t tipCount = r.TipCount();
    FrontBinding(out, generationId, dagInputDigest, frontierDigest, tipCount);
    return true;
}

bool MixDagTipFrontierIntoDigest(const unsigned char dagInputDigest[32],
                                 const unsigned char frontierBinding[32],
                                 unsigned char mixedDigest[32])
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char dom = 0x86;
    SHA256_Update(&ctx, &dom, 1);
    SHA256_Update(&ctx, dagInputDigest, 32);
    SHA256_Update(&ctx, frontierBinding, 32);
    SHA256_Final(mixedDigest, &ctx);
    return true;
}

DagTipFrontierCapability QueryDagTipFrontierCapability(
    const std::string& generationDir, uint64_t expectedGeneration,
    uint32_t declaredGenerationCapability,
    const unsigned char expectedDagInputDigest[32], std::string* detail)
{
    const boost::filesystem::path artifact =
        boost::filesystem::path(generationDir) / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    if (!boost::filesystem::exists(artifact))
    {
        if (declaredGenerationCapability == BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER)
        {
            if (detail) *detail = "frontier-capable generation missing required dag-tip-frontier.dat";
            return DAG_TIP_FRONTIER_CAPABILITY_CORRUPT;
        }
        if (detail) *detail = "legacy generation: frontier capability not declared";
        return DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE;
    }
    dag_tip_frontier::TipFrontierReader r;
    std::string rerr;
    // Full binding check: generation id + dagInputDigest (manifest) must match.
    if (!r.Open(artifact.string(), expectedGeneration, expectedDagInputDigest, &rerr))
    {
        if (detail) *detail = "dag-tip-frontier binding/re-open failed: " + rerr;
        return DAG_TIP_FRONTIER_CAPABILITY_CORRUPT;
    }
    // Integrity: recompute the frontier digest over the streamed tips and compare
    // to the stored header digest.
    unsigned char stored[32];
    if (!r.GetFrontierDigest(stored))
    {
        if (detail) *detail = "dag-tip-frontier digest unavailable";
        return DAG_TIP_FRONTIER_CAPABILITY_CORRUPT;
    }
    r.ResetStream();
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    uint256 h;
    while (r.Next(&h))
        SHA256_Update(&ctx, h.begin(), 32);
    unsigned char recomputed[32];
    SHA256_Final(recomputed, &ctx);
    if (memcmp(stored, recomputed, 32) != 0)
    {
        if (detail) *detail = "dag-tip-frontier digest mismatch (integrity)";
        return DAG_TIP_FRONTIER_CAPABILITY_CORRUPT;
    }
    if (detail) detail->clear();
    return DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID;
}