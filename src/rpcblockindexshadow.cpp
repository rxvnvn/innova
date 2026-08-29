// Read-only diagnostic RPC for the Block Index V2 shadow (Phase A.7).
// No filesystem mutation, no publish/select, no authoritative cutover.
#include "blockindex_shadow_startup.h"
#include "blockindex_shadow_runtime.h"
#include "innovarpc.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"

// Declared in the registration table (see getblockindexv2info registration).
extern void RegisterBlockIndexV2ShadowRPC();

json_spirit::Value getblockindexv2info(const json_spirit::Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw std::runtime_error(
            "getblockindexv2info\n"
            "Returns read-only diagnostics for the Block Index V2 shadow generation.\n"
            "authoritative is always false in A.7 (legacy remains authoritative).");

    const BlockIndexV2ShadowState st = GetBlockIndexV2ShadowState();
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("enabled", st.enabled));
    obj.push_back(json_spirit::Pair("status", st.status));
    obj.push_back(json_spirit::Pair("authoritative", st.authoritative));
    obj.push_back(json_spirit::Pair("root", st.configuredRoot));
    obj.push_back(json_spirit::Pair("generation", st.generation));
    obj.push_back(json_spirit::Pair("compatibility", st.compatibility));
    obj.push_back(json_spirit::Pair("shadow_tip_height", st.shadowTipHeight));
    obj.push_back(json_spirit::Pair("shadow_tip_hash", st.shadowTipHash.ToString()));
    obj.push_back(json_spirit::Pair("legacy_tip_height_at_validation", st.legacyTipHeightAtValidation));
    obj.push_back(json_spirit::Pair("height_lag", st.heightLag));
    obj.push_back(json_spirit::Pair("structural_validation", st.structuralValidationOk));
    obj.push_back(json_spirit::Pair("tip_on_legacy_active_chain", st.tipOnLegacyActiveChain));
    obj.push_back(json_spirit::Pair("sample_hash_checks", st.sampleHashChecks));
    obj.push_back(json_spirit::Pair("sample_height_checks", st.sampleHeightChecks));
    obj.push_back(json_spirit::Pair("sample_mismatches", st.sampleMismatches));
    obj.push_back(json_spirit::Pair("open_ms", st.openDurationMs));
    obj.push_back(json_spirit::Pair("validation_ms", st.validationDurationMs));
    if (!st.lastError.empty())
        obj.push_back(json_spirit::Pair("last_error", st.lastError));

    // A.8: if exactly one read-only reader is retained (READY), report its
    // bounded cache statistics. Narrow: omitted entirely when no reader is
    // retained, so disabled / non-READY output is unchanged.
    const BlockIndexV2Reader* reader = GetBlockIndexV2ShadowReader();
    obj.push_back(json_spirit::Pair("reader_retained", reader != NULL));
    if (reader)
    {
        const BlockIndexV2ReaderCacheStats cache = GetBlockIndexV2ShadowReaderCacheStats();
        obj.push_back(json_spirit::Pair("reader_generation", (boost::uint64_t)reader->Generation()));
        obj.push_back(json_spirit::Pair("reader_cache_capacity_bytes", (boost::uint64_t)cache.capacityBytes));
        obj.push_back(json_spirit::Pair("reader_cache_entries", (boost::uint64_t)cache.entries));
        obj.push_back(json_spirit::Pair("reader_cache_hits", (boost::uint64_t)cache.hits));
        obj.push_back(json_spirit::Pair("reader_cache_misses", (boost::uint64_t)cache.misses));
        obj.push_back(json_spirit::Pair("reader_cache_evictions", (boost::uint64_t)cache.evictions));
        obj.push_back(json_spirit::Pair("reader_cache_bytes_estimated", (boost::uint64_t)cache.bytesEstimated));
        obj.push_back(json_spirit::Pair("reader_current_selection_changed",
                                        reader->CurrentSelectionChanged(NULL)));
    }
    return obj;
}

void RegisterBlockIndexV2ShadowRPC()
{
    // Intentionally empty: the command is registered in vRPCCommands (single
    // dispatch table). Kept as a stable extension point that the dispatch
    // table references, so the function name/symbol is always linked.
}