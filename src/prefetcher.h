/** $lic$
 * Copyright (C) 2012-2013 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * This is an internal version, and is not under GPL. All rights reserved.
 * Only MIT and Stanford students and faculty are allowed to use this version.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2010) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 */

#ifndef PREFETCHER_H_
#define PREFETCHER_H_

#include <array>
#include <bitset>
#include "bithacks.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "stats.h"

/* Prefetcher models: Basic operation is to interpose between cache levels, issue additional accesses,
 * and keep a small table with delays; when the demand access comes, we do it and account for the
 * latency as when it was first fetched (to avoid hit latencies on partial latency overlaps).
 */

template <int32_t M, int32_t T, int32_t I>  // max value, threshold, initial
class SatCounter {
    private:
        int32_t count;
    public:
        SatCounter() : count(I) {}
        void reset() { count = I; }
        void dec() { count = MAX(count - 1, 0); }
        void inc() { count = MIN(count + 1, M); }
        bool pred() const { return count >= T; }
        uint32_t counter() const { return count; }
};

class PrefetchResponseEvent;

/* This is basically a souped-up version of the DLP L2 prefetcher in Nehalem: 16 stream buffers,
 * but (a) no up/down distinction, and (b) strided operation based on dominant stride detection
 * to try to subsume as much of the L1 IP/strided prefetcher as possible.
 *
 * FIXME: For now, mostly hardcoded; 64-line entries (4KB w/64-byte lines), fixed granularities, etc.
 * TODO: Adapt to use weave models
 */
class StreamPrefetcher : public BaseCache {
    private:
        struct Entry {
            // Two competing strides; at most one active
            int32_t stride;
            SatCounter<3, 2, 1> conf;

            struct AccessTimes {
                uint64_t startCycle;  // FIXME: Dead for now, we should use it for profiling
                uint64_t respCycle;

                void fill(uint32_t s, uint64_t r) { startCycle = s; respCycle = r; }
            };

            AccessTimes times[64];
            std::bitset<64> valid;

            // Weave-phase end-of-access event. Used to avoid early responses with weave models.
            // Self-cleaning (PrefetchResponseEvent sets this to nullptr when it fires),
            // so can't be stale.
            std::array<PrefetchResponseEvent*, 64> respEvents;

            uint32_t lastPos;
            uint32_t lastLastPos;
            uint32_t lastPrefetchPos;
            uint64_t lastCycle;  // updated on alloc and hit
            uint64_t ts;

            void alloc(uint64_t curCycle) {
                stride = 1;
                lastPos = 0;
                lastLastPos = 0;
                lastPrefetchPos = 0;
                conf.reset();
                valid.reset();
                respEvents.fill(nullptr);
                lastCycle = curCycle;
            }
        };

        uint64_t timestamp;  // for LRU
        Address* tag;
        Entry* array;

        Counter profAccesses, profPrefetches, profDoublePrefetches, profPageHits, profHits, profShortHits, profStrideSwitches, profLowConfAccs;

        MemObject* parent;
        BaseCache* child;
        uint32_t childId;
        g_string name;

        uint32_t numBuffers;
        bool partitionBuffers; // partition stream buffer among data classes
        uint32_t partitions;
        static constexpr uint32_t MAX_PARTITIONS = 4;

    public:
        StreamPrefetcher(const g_string& _name, uint32_t _numBuffers, bool _partitionBuffers);
        ~StreamPrefetcher();
        void initStats(AggregateStat* parentStat);
        const char* getName() { return name.c_str();}
        void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network);
        void setChildren(const g_vector<BaseCache*>& children, Network* network);

        uint64_t access(MemReq& req);
        uint64_t invalidate(const InvReq& req);

        void simulatePrefetchResponse(PrefetchResponseEvent* ev, uint64_t cycle);
};

#endif  // PREFETCHER_H_
