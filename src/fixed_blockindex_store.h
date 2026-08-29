#ifndef INNOVA_FIXED_BLOCKINDEX_STORE_H
#define INNOVA_FIXED_BLOCKINDEX_STORE_H

#include "blockindex_accessor.h"

#include <boost/filesystem/path.hpp>
#include <stdint.h>
#include <string>
#include <vector>

static const uint32_t BLOCK_INDEX_FORMAT_VERSION = 1;
static const uint32_t BLOCK_INDEX_RECORD_VERSION = 1;
static const uint32_t BLOCK_INDEX_RECORD_SIZE_V1 = 228;
static const uint32_t BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 = 40;
static const uint32_t BLOCK_INDEX_MANIFEST_SIZE_V1 = 88;

static const char* const BLOCK_INDEX_RECORDS_FILE_NAME = "records.dat";
static const char* const BLOCK_INDEX_MANIFEST_FILE_NAME = "MANIFEST";

struct BlockIndexRecord
{
    uint256 hash;
    uint256 hashPrev;
    uint256 hashMerkleRoot;
    uint256 hashProof;
    COutPoint prevoutStake;

    int32_t height;
    uint32_t nFile;
    uint32_t nBlockPos;
    uint32_t nFlags;
    int32_t nVersion;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    int64_t nMint;
    int64_t nMoneySupply;
    uint64_t nStakeModifier;
    uint32_t nStakeTime;

    BlockIndexRecord()
        : hash(0),
          hashPrev(0),
          hashMerkleRoot(0),
          hashProof(0),
          prevoutStake(),
          height(0),
          nFile(0),
          nBlockPos(0),
          nFlags(0),
          nVersion(0),
          nTime(0),
          nBits(0),
          nNonce(0),
          nMint(0),
          nMoneySupply(0),
          nStakeModifier(0),
          nStakeTime(0)
    {
    }
};

enum BlockIndexManifestState
{
    BLOCK_INDEX_MANIFEST_BUILDING = 1,
    BLOCK_INDEX_MANIFEST_COMPLETE = 2,
};

struct FixedBlockIndexManifest
{
    uint32_t formatVersion;
    uint32_t recordVersion;
    uint32_t manifestSize;
    uint32_t recordSize;
    uint64_t generation;
    uint64_t recordCount;
    BlockIndexId committedTipId;
    int32_t committedTipHeight;
    uint32_t state;
    uint256 committedTipHash;

    FixedBlockIndexManifest()
        : formatVersion(BLOCK_INDEX_FORMAT_VERSION),
          recordVersion(BLOCK_INDEX_RECORD_VERSION),
          manifestSize(BLOCK_INDEX_MANIFEST_SIZE_V1),
          recordSize(BLOCK_INDEX_RECORD_SIZE_V1),
          generation(0),
          recordCount(0),
          committedTipId(BLOCK_INDEX_ID_INVALID),
          committedTipHeight(-1),
          state(BLOCK_INDEX_MANIFEST_BUILDING),
          committedTipHash(0)
    {
    }
};

struct FixedBlockIndexOpenOptions
{
    bool requireCompleteManifest;

    FixedBlockIndexOpenOptions()
        : requireCompleteManifest(true)
    {
    }
};

bool EncodeBlockIndexRecordV1(const BlockIndexRecord& record, std::vector<unsigned char>* out, std::string* error);
bool DecodeBlockIndexRecordV1(const unsigned char* data, size_t size, BlockIndexRecord* out, std::string* error);

class FixedBlockIndexStore
{
public:
    FixedBlockIndexStore();

    static bool Create(const std::string& dir, uint64_t generation, FixedBlockIndexStore* out, std::string* error);
    static bool OpenReadOnly(const std::string& dir, const FixedBlockIndexOpenOptions& options, FixedBlockIndexStore* out, std::string* error);

    bool Append(const BlockIndexRecord& record, BlockIndexId* outId, std::string* error);
    bool Read(BlockIndexId id, BlockIndexRecord* out, std::string* error) const;
    bool WriteManifest(const FixedBlockIndexManifest& manifest, std::string* error);

    uint64_t CommittedRecordCount() const;
    uint64_t PhysicalRecordCount() const;
    const FixedBlockIndexManifest& GetManifest() const;

private:
    boost::filesystem::path dirPath;
    boost::filesystem::path recordsPath;
    boost::filesystem::path manifestPath;
    uint64_t generation;
    uint64_t physicalRecordCount;
    FixedBlockIndexManifest manifest;
    bool writable;
    bool open;

    bool InitializePaths(const std::string& dir, std::string* error);
    bool WriteRecordsHeader(std::string* error);
    bool LoadRecordsHeader(uint64_t* fileSize, std::string* error);
    bool LoadManifest(const FixedBlockIndexOpenOptions& options, std::string* error);
    bool ValidateManifest(const FixedBlockIndexManifest& candidate, std::string* error) const;
    bool ValidateCommittedRegion(uint64_t fileSize, std::string* error) const;
};

#endif
