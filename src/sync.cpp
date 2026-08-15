// Copyright (c) 2011-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sync.h"

#include "util.h"

#include <boost/foreach.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <sstream>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace {
struct OwnerRecord
{
    std::string name;
    std::string id;
    std::string thread;
    std::string file;
    int line;
    int64_t start;
    uint64_t token;
    std::atomic<int> depth;
    std::atomic<OwnerRecord*>* slot;
    OwnerRecord() : line(0), start(0), token(0), depth(0), slot(NULL) {}
};
std::atomic<OwnerRecord*> mainOwner(NULL);
std::atomic<OwnerRecord*> walletOwner(NULL);
std::atomic<int> enabled(-1);
std::atomic<int> phaseEnabled(-1);
std::atomic<OwnerRecord*>* ownerSlot(const char* name)
{
    if (name && strcmp(name, "cs_main") == 0)
        return &mainOwner;
    if (name && strstr(name, "cs_wallet"))
        return &walletOwner;
    return NULL;
}
std::string ownerId()
{
    std::ostringstream stream;
    stream << boost::this_thread::get_id();
    return stream.str();
}
std::string ownerThreadName()
{
#ifdef __linux__
    char name[16] = {0};
    if (prctl(PR_GET_NAME, name, 0, 0, 0) == 0 && name[0])
        return std::string(name);
#endif
    return std::string("<unknown>");
}
}
bool SyncLockOwnerTrackingEnabled()
{
    int state = enabled.load(std::memory_order_acquire);
    if (state < 0)
    {
        const int configured = GetBoolArg("-synclockdiagnostics", false) ? 1 : 0;
        enabled.compare_exchange_strong(state, configured);
        state = enabled.load(std::memory_order_acquire);
    }
    return state == 1;
}
bool SyncLockPhaseDiagnosticsEnabled()
{
    int state = phaseEnabled.load(std::memory_order_acquire);
    if (state < 0)
    {
        const int configured = GetBoolArg("-synclockdiagnostics", false) ? 1 : 0;
        phaseEnabled.compare_exchange_strong(state, configured);
        state = phaseEnabled.load(std::memory_order_acquire);
    }
    return state == 1;
}
uint64_t SyncLockOwnerAcquired(void* mutex, const char* name, const char* file, int line)
{
    (void)mutex;
    if (!SyncLockOwnerTrackingEnabled())
        return 0;
    std::atomic<OwnerRecord*>* slot = ownerSlot(name);
    if (!slot)
        return 0;
    const std::string id = ownerId();
    OwnerRecord* current = slot->load(std::memory_order_acquire);
    if (current && current->id == id)
    {
        current->depth.fetch_add(1, std::memory_order_relaxed);
        return current->token;
    }
    OwnerRecord* record = new OwnerRecord();
    record->name = name;
    record->id = id;
    record->thread = ownerThreadName();
    record->file = file;
    record->line = line;
    record->start = GetTimeMicros();
    record->token = (uint64_t)(uintptr_t)record;
    record->slot = slot;
    record->depth.store(1, std::memory_order_relaxed);
    OwnerRecord* empty = NULL;
    if (!slot->compare_exchange_strong(empty, record,
                                       std::memory_order_release,
                                       std::memory_order_acquire))
    {
        delete record;
        if (empty && empty->id == id)
        {
            empty->depth.fetch_add(1, std::memory_order_relaxed);
            return empty->token;
        }
        return 0;
    }
    // Records are immutable after publication and retained while diagnostics run
    // so concurrent snapshots never dereference freed storage.
    return record->token;
}
void SyncLockOwnerReleased(void* mutex, uint64_t token)
{
    (void)mutex;
    if (!token || !SyncLockOwnerTrackingEnabled())
        return;
    OwnerRecord* record = (OwnerRecord*)(uintptr_t)token;
    if (record->depth.fetch_sub(1, std::memory_order_relaxed) == 1)
    {
        OwnerRecord* expected = record;
        record->slot->compare_exchange_strong(expected, (OwnerRecord*)NULL,
                                               std::memory_order_release,
                                               std::memory_order_relaxed);
    }
}
bool SyncLockPhaseLog(const char* scope, const char* phase, int64_t startMicros)
{
    if (!SyncLockPhaseDiagnosticsEnabled() || startMicros == 0)
        return false;
    const int64_t durationMicros = GetTimeMicros() - startMicros;
    const int64_t thresholdMicros = std::max<int64_t>(
        1, GetArg("-synclockphasethresholdms", 1)) * 1000;
    if (durationMicros < thresholdMicros)
        return false;
    printf("SYNCLOCK_PHASE scope=%s phase=%s duration_us=%lld threshold_us=%lld\n",
           scope ? scope : "<unknown>", phase ? phase : "<unknown>",
           (long long)durationMicros, (long long)thresholdMicros);
    return true;
}
void SyncLockPhaseResetForTesting()
{
    phaseEnabled.store(-1, std::memory_order_release);
}
CSyncLockPhase::CSyncLockPhase(const char* scopeIn, const char* phaseIn)
    : scope(scopeIn), phase(phaseIn),
      startMicros(SyncLockPhaseDiagnosticsEnabled() ? GetTimeMicros() : 0) {}
CSyncLockPhase::~CSyncLockPhase()
{
    SyncLockPhaseLog(scope, phase, startMicros);
}
std::vector<CSyncLockOwnerSnapshot> SyncLockOwnerSnapshots(const char* locks)
{
    std::vector<CSyncLockOwnerSnapshot> result;
    if (!SyncLockOwnerTrackingEnabled() || !locks)
        return result;
    const std::string lockList(locks);
    size_t begin = 0;
    while (begin <= lockList.size())
    {
        size_t end = lockList.find('+', begin);
        if (end == std::string::npos)
            end = lockList.size();
        const std::string lockName = lockList.substr(begin, end - begin);
        std::atomic<OwnerRecord*>* slot = ownerSlot(lockName.c_str());
        OwnerRecord* owner = slot ? slot->load(std::memory_order_acquire) : NULL;
        CSyncLockOwnerSnapshot snapshot;
        snapshot.lockName = lockName;
        if (owner)
        {
            snapshot.known = true;
            snapshot.lockName = owner->name;
            snapshot.ownerThreadId = owner->id;
            snapshot.ownerThreadName = owner->thread;
            snapshot.sourceFile = owner->file;
            snapshot.sourceLine = owner->line;
            snapshot.ownerStartTimeMicros = owner->start;
            snapshot.recursionDepth = owner->depth.load(std::memory_order_relaxed);
        }
        result.push_back(snapshot);
        if (end == lockList.size())
            break;
        begin = end + 1;
    }
    return result;
}
#ifdef DEBUG_LOCKCONTENTION
void PrintLockContention(const char* pszName, const char* pszFile, int nLine)
{
    printf("LOCKCONTENTION: %s\n", pszName);
    printf("Locker: %s:%d\n", pszFile, nLine);
}
#endif /* DEBUG_LOCKCONTENTION */

#ifdef DEBUG_LOCKORDER
//
// Early deadlock detection.
// Problem being solved:
//    Thread 1 locks  A, then B, then C
//    Thread 2 locks  D, then C, then A
//     --> may result in deadlock between the two threads, depending on when they run.
// Solution implemented here:
// Keep track of pairs of locks: (A before B), (A before C), etc.
// Complain if any thread tries to lock in a different order.
//

struct CLockLocation
{
    CLockLocation(const char* pszName, const char* pszFile, int nLine)
    {
        mutexName = pszName;
        sourceFile = pszFile;
        sourceLine = nLine;
    }

    std::string ToString() const
    {
        return mutexName+"  "+sourceFile+":"+itostr(sourceLine);
    }

    std::string MutexName() const { return mutexName; }

private:
    std::string mutexName;
    std::string sourceFile;
    int sourceLine;
};

typedef std::vector< std::pair<void*, CLockLocation> > LockStack;

static boost::mutex dd_mutex;
static std::map<std::pair<void*, void*>, LockStack> lockorders;
static boost::thread_specific_ptr<LockStack> lockstack;


static void potential_deadlock_detected(const std::pair<void*, void*>& mismatch, const LockStack& s1, const LockStack& s2)
{
    printf("POTENTIAL DEADLOCK DETECTED\n");
    printf("Previous lock order was:\n");
    BOOST_FOREACH(const PAIRTYPE(void*, CLockLocation)& i, s2)
    {
        if (i.first == mismatch.first) printf(" (1)");
        if (i.first == mismatch.second) printf(" (2)");
        printf(" %s\n", i.second.ToString().c_str());
    }
    printf("Current lock order is:\n");
    BOOST_FOREACH(const PAIRTYPE(void*, CLockLocation)& i, s1)
    {
        if (i.first == mismatch.first) printf(" (1)");
        if (i.first == mismatch.second) printf(" (2)");
        printf(" %s\n", i.second.ToString().c_str());
    }
}

static void push_lock(void* c, const CLockLocation& locklocation, bool fTry)
{
    if (lockstack.get() == NULL)
        lockstack.reset(new LockStack);

    if (fDebug) printf("Locking: %s\n", locklocation.ToString().c_str());
    dd_mutex.lock();

    (*lockstack).push_back(std::make_pair(c, locklocation));

    if (!fTry) {
        BOOST_FOREACH(const PAIRTYPE(void*, CLockLocation)& i, (*lockstack)) {
            if (i.first == c) break;

            std::pair<void*, void*> p1 = std::make_pair(i.first, c);
            if (lockorders.count(p1))
                continue;
            lockorders[p1] = (*lockstack);

            std::pair<void*, void*> p2 = std::make_pair(c, i.first);
            if (lockorders.count(p2))
            {
                potential_deadlock_detected(p1, lockorders[p2], lockorders[p1]);
                break;
            }
        }
    }
    dd_mutex.unlock();
}

static void pop_lock()
{
    if (fDebug)
    {
        const CLockLocation& locklocation = (*lockstack).rbegin()->second;
        printf("Unlocked: %s\n", locklocation.ToString().c_str());
    }
    dd_mutex.lock();
    (*lockstack).pop_back();
    dd_mutex.unlock();
}

void EnterCritical(const char* pszName, const char* pszFile, int nLine, void* cs, bool fTry)
{
    push_lock(cs, CLockLocation(pszName, pszFile, nLine), fTry);
}

void LeaveCritical()
{
    pop_lock();
}

std::string LocksHeld()
{
    std::string result;
    BOOST_FOREACH(const PAIRTYPE(void*, CLockLocation)&i, *lockstack)
        result += i.second.ToString() + std::string("\n");
    return result;
}

void AssertLockHeldInternal(const char *pszName, const char* pszFile, int nLine, void *cs)
{
    BOOST_FOREACH(const PAIRTYPE(void*, CLockLocation)&i, *lockstack)
        if (i.first == cs) return;

    printf("Assertion failed: lock %s not held in %s:%i; locks held:\n%s\n",
            pszName, pszFile, nLine, LocksHeld().c_str());
    fprintf(stderr, "Assertion failed: lock %s not held in %s:%i; locks held:\n%s",
            pszName, pszFile, nLine, LocksHeld().c_str());
    abort();
}

#endif /* DEBUG_LOCKORDER */
