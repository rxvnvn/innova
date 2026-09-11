#ifndef INNOVA_DAG_SOURCE_BINDING_VERIFIER_H
#define INNOVA_DAG_SOURCE_BINDING_VERIFIER_H

#include <stddef.h>
#include <stdint.h>
#include <string>

struct DagSourceBindingVerifierOptions
{
    size_t chunkBytes;
    size_t maxRecordsPerChunk;
    size_t maxOpenRuns;
    std::string tempParent;
    DagSourceBindingVerifierOptions()
        : chunkBytes(1024 * 1024), maxRecordsPerChunk(4096), maxOpenRuns(8), tempParent() {}
};

enum DagSourceBindingStatus
{
    DAG_SOURCE_BINDING_VERIFIED = 0,
    DAG_SOURCE_BINDING_DIGEST_MISMATCH,
    DAG_SOURCE_BINDING_SOURCE_UNAVAILABLE,
    DAG_SOURCE_BINDING_SOURCE_CORRUPT,
    DAG_SOURCE_BINDING_TEMP_IO_FAILURE,
    DAG_SOURCE_BINDING_DECODE_FAILURE,
    DAG_SOURCE_BINDING_AUTHORITY_FAILURE
};

struct DagSourceBindingResult
{
    DagSourceBindingStatus status;
    std::string error;
    uint64_t recordsProcessed;
    uint64_t bytesProcessed;
    uint64_t temporaryBytesWritten;
    uint64_t peakChunkBytes;
    uint64_t peakChunkRecords;
    uint64_t runCount;
    uint64_t mergePasses;
    uint64_t maxOpenRunsObserved;
    DagSourceBindingResult()
        : status(DAG_SOURCE_BINDING_AUTHORITY_FAILURE), recordsProcessed(0),
          bytesProcessed(0), temporaryBytesWritten(0), peakChunkBytes(0),
          peakChunkRecords(0), runCount(0), mergePasses(0), maxOpenRunsObserved(0) {}
};

// R1a: verifies the existing generation-builder dagInputDigest against the
// persisted daglinks source without loading the historical DAG into RAM.
class DagSourceBindingVerifier
{
public:
    static DagSourceBindingResult Verify(const std::string& dagLinksDir,
                                         const unsigned char expectedDigest[32],
                                         const DagSourceBindingVerifierOptions& options);
};

#endif
