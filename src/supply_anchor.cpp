// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "supply_anchor.h"
#include "sync.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// Independent cumulative-supply anchors.
//
// Every value below is satoshi-exact and was produced by replaying the chain's
// OWN block bytes (bi-supply-audit v2: valueOut/valueIn per transaction with
// prevouts resolved from the Store3A undo stream / block window; burned =
// OP_RETURN outputs of non-coinbase transactions), NOT by reading the node's
// nMoneySupply and NOT from wallet balances. See supply_anchor.h for the full
// provenance note.
//
// DO NOT "fix" an anchor to make a node pass: an anchor that disagrees with the
// persisted active record is the SIGNAL that the accumulator has been poisoned.
// If the chain legitimately reorganises across an anchored height, the block
// hash no longer matches and the anchor is skipped automatically.
// ---------------------------------------------------------------------------
static const MoneySupplyAnchor kMoneySupplyAnchors[] =
{
    {   8207223, "00000007462815ece0c143c9290e2887395aacc08c78f3d818e4c2008e744af8", INT64_C(1107086993003118) },
    {   8208074, "00000011476b10ad7e9a120669400360d81821b5c4cb69307b66aff13c6b5ca8", INT64_C(1107142566049614) },
    {   8208925, "4a589153e3bde87dccf9dd70a00f4f4e21240ce6c1b3ceb37101eee2e9a97f8d", INT64_C(1107158936005742) },
    {   8209776, "000000b65499b1a84bf6f7a9b008a0a86748ec117e972aa6e179f0a6f3b8655e", INT64_C(1107175589370097) },
    {   8210627, "00000005204a7741b17f4ad7e3321d30a3d574c87c521117834bc5ad9f9d3b9a", INT64_C(1107192590224891) },
    {   8211478, "00000070bbe508f355f365ec606aadb33b1ed66e29763a9e568e0c0123a2ec68", INT64_C(1107213201994749) },
    {   8212329, "000000034b4598f450fa4ab21cb08c400f7a62bd1a2d6ed6a05df5c343bcd52a", INT64_C(1107230206531735) },
    {   8213180, "00000011cd9998ed9f78256ed0c2a379fed891999855b61e7c5fa427ee5d8db8", INT64_C(1107253045282399) },
    {   8214031, "0000008e6d535880637d41c872a8f92db18389a28ca9ca70f12a9ffe9c5954f5", INT64_C(1107270016783766) },
    {   8214882, "000000ccd5220af133b26092868bd0608c4d0ed7adf4269d4a25091194a464ed", INT64_C(1107286952137189) },
    {   8215733, "0000009326aef27684cf640de9901327a7061848c107e5df5db07dedbfef5e91", INT64_C(1107303786893346) },
    {   8216584, "0000003fe35944d9b674ecf994cbe3c4666284ef6d52571d89437887e423f58a", INT64_C(1107320665128957) },
    {   8217435, "00000095cc81423eb3d99961d1ac0fa20695c001c4364f0a12acb9d6afacf343", INT64_C(1107337375233056) },
    {   8218286, "76069796fe95165b43f773ae6a0df133099321cf2039a335343c8b42d1f9f450", INT64_C(1107354163397432) },
    // The parent of the observed re-base: its record was (and is) correct.
    {   8219136, "00000224ada49b8152c82a2948c2ea03dab70b101026ba00fcff78d9380f0401", INT64_C(1107370942728933) },
    // The first poisoned height on the mainnet chain (record carries 0.2 INN).
    {   8219137, "00000058eb47a11c28ae2a72045a19f429ff0fa2f944be6d85cc305120b6a69e", INT64_C(1107370962728933) },
    {   8219654, "000000010e126b9c8b9e16bb2400cc41046f39f973d2ee65f853b9a0c4feb2f2", INT64_C(1107380882838510) },
    {   8220171, "0000000373d88b9254c5fcc1c756e2caadbec4d10af82b999e896af633ec4d9d", INT64_C(1107390985720694) },
    {   8220688, "00000008734e60e693791754f1779ce1999c83de1d6e214a24f51894853e8f4a", INT64_C(1107401050411098) },
    {   8221205, "0000000c12093845a67a1738214d9559eaa0b737ff222f140d37017d208d35c8", INT64_C(1107411077813831) },
    {   8221722, "0000007b2078637b7172bff8f3a0f7cce078a8ea45b57ad114819d8758c90057", INT64_C(1107421156937112) },
    {   8222239, "33768f75111ff6285d3083dc107e1ed06bfb56456b69127a116cc87195850bc2", INT64_C(1107431985232980) },
    {   8222756, "00000052b60da87520b80c67f49a9739e233ae69ea33bfb78fd3d6acdd80690c", INT64_C(1107441562158984) },
    {   8223273, "aa17628a6520c21b97d197edc63e3f443878c15bc9482f6735a56e8ed796e589", INT64_C(1107451565808287) },
    {   8223275, "000000b14aca3e6b1bce85e8b7a2bfca0aae0dd4e6bf8b03958b20f56a744cb4", INT64_C(1107451605808287) },
};

static const size_t kMoneySupplyAnchorCount =
    sizeof(kMoneySupplyAnchors) / sizeof(kMoneySupplyAnchors[0]);

const MoneySupplyAnchor* MoneySupplyAnchorTable(size_t* count)
{
    if (count) *count = kMoneySupplyAnchorCount;
    return kMoneySupplyAnchors;
}

const MoneySupplyAnchor* FindMoneySupplyAnchorAtOrBelow(int32_t height)
{
    const MoneySupplyAnchor* found = NULL;
    for (size_t i = 0; i < kMoneySupplyAnchorCount; ++i)
    {
        if (kMoneySupplyAnchors[i].height <= height)
            found = &kMoneySupplyAnchors[i];
        else
            break; // table is sorted
    }
    return found;
}

const MoneySupplyAnchor* FindMoneySupplyAnchorAbove(int32_t height)
{
    for (size_t i = 0; i < kMoneySupplyAnchorCount; ++i)
        if (kMoneySupplyAnchors[i].height > height)
            return &kMoneySupplyAnchors[i];
    return NULL;
}

const MoneySupplyAnchor* FindMoneySupplyAnchorBelow(int32_t height)
{
    const MoneySupplyAnchor* found = NULL;
    for (size_t i = 0; i < kMoneySupplyAnchorCount; ++i)
    {
        if (kMoneySupplyAnchors[i].height < height)
            found = &kMoneySupplyAnchors[i];
        else
            break;
    }
    return found;
}

const MoneySupplyAnchor* FindMoneySupplyAnchorInRange(int32_t lo, int32_t hi)
{
    const MoneySupplyAnchor* found = NULL;
    for (size_t i = 0; i < kMoneySupplyAnchorCount; ++i)
    {
        if (kMoneySupplyAnchors[i].height >= lo && kMoneySupplyAnchors[i].height <= hi)
            found = &kMoneySupplyAnchors[i];
        else if (kMoneySupplyAnchors[i].height > hi)
            break;
    }
    return found;
}

// ---- verification state ---------------------------------------------------
namespace {
CCriticalSection cs_supplyVerification;
int g_moneySupplyVerification = MONEY_SUPPLY_UNKNOWN;
std::string g_moneySupplyVerificationDetail;
}

void SetMoneySupplyVerification(int state, const std::string& detail)
{
    LOCK(cs_supplyVerification);
    g_moneySupplyVerification = state;
    g_moneySupplyVerificationDetail = detail;
}

int GetMoneySupplyVerification(std::string* detail)
{
    LOCK(cs_supplyVerification);
    if (detail) *detail = g_moneySupplyVerificationDetail;
    return g_moneySupplyVerification;
}

const char* MoneySupplyVerificationName(int state)
{
    switch (state)
    {
        case MONEY_SUPPLY_VERIFIED:   return "VERIFIED";
        case MONEY_SUPPLY_VIOLATION:  return "VIOLATION";
        case MONEY_SUPPLY_UNREADABLE: return "UNREADABLE";
        case MONEY_SUPPLY_UNKNOWN:
        default:                     return "UNVERIFIED";
    }
}
