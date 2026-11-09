// Read-only diagnostic RPC for the Block Index V2 shadow (Phase A.7).
// No filesystem mutation, no publish/select, no authoritative cutover.
#include "blockindex_shadow_startup.h"
#include "blockindex_shadow_runtime.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_manager.h"
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
            "Read-only diagnostics for Block Index V2.\n"
            "\n"
            "`enabled`, `authoritative`, `root`, `generation`, `shadow_*` describe the OPTIONAL\n"
            "read-only shadow (Phase A.7) and are NOT the production authority.\n"
            "`authority_*` describe the production authoritative startup actually in use:\n"
            "  authority_mode       true when V2 is the block-index authority for this process\n"
            "  authority_root       authoritative root path\n"
            "  authority_generation authoritative generation\n"
            "  authority_tip_height/authority_tip_hash  authoritative tip read BY VALUE (null if unavailable)");

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
    // ------------------------------------------------------------------
    // B26 (2026-11-09): the fields above describe the SHADOW only. Report the
    // production authority separately and by value, so an operator cannot read
    // "enabled=false" as "V2 is not in use" while V2 is in fact authoritative.
    // ------------------------------------------------------------------
    obj.push_back(json_spirit::Pair("authority_mode", g_fAuthoritativeStartup));
    obj.push_back(json_spirit::Pair("authority_root", AuthoritativeRootPath()));
    obj.push_back(json_spirit::Pair("authority_generation", (boost::uint64_t)AuthoritativeGeneration()));
    {
        BlockIndexSnapshot tip;
        std::string tipErr;
        if (GetBlockIndexManager().GetTip(&tip, &tipErr) == BLOCK_INDEX_MANAGER_OK && tip.found)
        {
            obj.push_back(json_spirit::Pair("authority_tip_height", tip.height));
            obj.push_back(json_spirit::Pair("authority_tip_hash", tip.hash.ToString()));
        }
        else
        {
            obj.push_back(json_spirit::Pair("authority_tip_height", json_spirit::Value::null));
            obj.push_back(json_spirit::Pair("authority_tip_hash", json_spirit::Value::null));
            if (!tipErr.empty())
                obj.push_back(json_spirit::Pair("authority_tip_error", tipErr));
        }
    }
    return obj;
}

void RegisterBlockIndexV2ShadowRPC()
{
    // Intentionally empty: the command is registered in vRPCCommands (single
    // dispatch table). Kept as a stable extension point that the dispatch
    // table references, so the function name/symbol is always linked.
}