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

#include "prefetcher.h"
#include "bithacks.h"
//#include "jigsaw_runtime.h"
#include "network.h"

#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

#define DBG(args...) //info(args)

class PrefetchResponseEvent : public TimingEvent {
    private:
        StreamPrefetcher* pf;
    public:
        const uint32_t idx;
        const uint32_t prefetchPos;

    public:
        PrefetchResponseEvent(StreamPrefetcher* _pf, uint32_t _idx, uint32_t _prefetchPos, int32_t domain) :
            TimingEvent(0, 0, domain), pf(_pf), idx(_idx), prefetchPos(_prefetchPos) {}

        void simulate(uint64_t startCycle) {
            pf->simulatePrefetchResponse(this, startCycle);
            done(startCycle);
        }
};

StreamPrefetcher::StreamPrefetcher(const g_string& _name, uint32_t _numBuffers, bool _partitionBuffers)
    : timestamp(0), name(_name), numBuffers(_numBuffers), partitionBuffers(_partitionBuffers), partitions(1)
{
    tag = gm_calloc<Address>(numBuffers);
    array = gm_calloc<Entry>(numBuffers);
}

StreamPrefetcher::~StreamPrefetcher() {
    gm_free(tag);
    gm_free(array);
}

void StreamPrefetcher::setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
    childId = _childId;
    if (parents.size() != 1) panic("Must have one parent, %ld passed", parents.size());
    parent = parents[0];
    if (network && network->getRTT(name.c_str(), parent->getName())) panic("Network not handled (non-zero delay with parent)");
}

void StreamPrefetcher::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    if (children.size() != 1) panic("Must have one child, %ld passed", children.size());
    child = children[0];
    if (network && network->getRTT(name.c_str(), child->getName())) panic("Network not handled (non-zero delay with child)");
}

void StreamPrefetcher::initStats(AggregateStat* parentStat) {
    AggregateStat* s = new AggregateStat();
    s->init(name.c_str(), "Prefetcher stats");
    profAccesses.init("acc", "Accesses"); s->append(&profAccesses);
    profPrefetches.init("pf", "Issued prefetches"); s->append(&profPrefetches);
    profDoublePrefetches.init("dpf", "Issued double prefetches"); s->append(&profDoublePrefetches);
    profPageHits.init("pghit", "Page/entry hit"); s->append(&profPageHits);
    profHits.init("hit", "Prefetch buffer hits, short and full"); s->append(&profHits);
    profShortHits.init("shortHit", "Prefetch buffer short hits"); s->append(&profShortHits);
    profStrideSwitches.init("strideSwitches", "Predicted stride switches"); s->append(&profStrideSwitches);
    profLowConfAccs.init("lcAccs", "Low-confidence accesses with no prefetches"); s->append(&profLowConfAccs);
    parentStat->append(s);
}

uint64_t StreamPrefetcher::access(MemReq& req) {
    uint32_t origChildId = req.childId;
    req.childId = childId;

    if (req.type != GETS) return parent->access(req); //other reqs ignored, including stores

    profAccesses.inc();

    uint64_t reqCycle = req.cycle;
    uint64_t respCycle = parent->access(req);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    TimingRecord acc;
    acc.clear();
    if (unlikely(evRec && evRec->hasRecord())) {
        acc = evRec->popRecord();
    }

    // For performance reasons, we don't want to initialize acc and push a
    // TimingRecord down to the core every access.  Many accesses don't result
    // in prefetches or prefetch-hits, and don't generate timing events. This
    // lambda is called only when acc is needed, and can be safely called
    // multiple times.
    auto initAcc = [&]() {
        // Massage acc TimingRecord, which may or may not be valid
        if (acc.isValid()) {  // Due to the demand access or a previous prefetch
            if (acc.reqCycle > reqCycle) {
                DelayEvent* dUpEv = new (evRec) DelayEvent(acc.reqCycle - reqCycle);
                dUpEv->setMinStartCycle(reqCycle);
                dUpEv->addChild(acc.startEvent, evRec);
                acc.reqCycle = reqCycle;
                acc.startEvent = dUpEv;
            }
        } else {
            // Create fixed-delay start and end events, so that we can pass a TimingRecord
            // to the core (we always want to pass a TimingRecord if we issue prefetch accesses,
            // so that we account for latencies correctly even with skews)
            DelayEvent* startEv = new (evRec) DelayEvent(respCycle - reqCycle);
            startEv->setMinStartCycle(reqCycle);
            DelayEvent* endEv = new (evRec) DelayEvent(0);
            endEv->setMinStartCycle(respCycle);
            startEv->addChild(endEv, evRec);
            acc = {req.lineAddr, reqCycle, respCycle, req.type, startEv, endEv};
            assert(acc.isValid());
        }
        // Now acc is valid & startEvent is always at reqCycle
    };

    Address pageAddr = req.lineAddr >> 6;
    uint32_t startEntry, endEntry;

    if (likely(!partitionBuffers)) {
        startEntry = 0;
        endEntry = numBuffers;
    } else {
		assert(false);
/*        //assert_msg(zinfo->jsr != nullptr, "need jigsaw runtime for page to share mappings");

        uint32_t share = zinfo->jsr->getPageToShare(pageAddr);
        if (share == (uint32_t)-1) share = 0;

        assert(share < MAX_PARTITIONS); // FIXME we support max four partitions for now
        if (share + 1 > partitions) {
            info("updated partitions %u", partitions);
            partitions = share + 1;
        }

        // Divide the stream buffers equally among partitions 
        startEntry = numBuffers/partitions*(share);
        endEntry = numBuffers/partitions*(share + 1);
*/
    }

    uint32_t pos = req.lineAddr & (64-1);
    uint32_t idx = numBuffers;

    // This loop gets unrolled and there are no control dependences. Way faster than a break (but should watch for the avoidable loop-carried dep)
    for (uint32_t i = startEntry; i < endEntry; i++) {
        bool match = (pageAddr == tag[i]);
        idx = match?  i : idx;  // ccmov, no branch
    }

    DBG("%s: 0x%lx page %lx pos %d", name.c_str(), req.lineAddr, pageAddr, pos);

    if (idx == numBuffers) {  // entry miss
        uint32_t cand = numBuffers;
        uint64_t candScore = -1;
        //uint64_t candScore = 0;
        for (uint32_t i = 0; i < 16; i++) {
            if (array[i].lastCycle > reqCycle + 5000) continue;  // warm prefetches, not even a candidate
            /*uint64_t score = (reqCycle - array[i].lastCycle)*(3 - array[i].conf.counter());
            if (score > candScore) {
                cand = i;
                candScore = score;
            }*/
            if (array[i].ts < candScore) {  // just LRU
                cand = i;
                candScore = array[i].ts;
            }
        }

        if (cand < numBuffers) {
            idx = cand;
            array[idx].alloc(reqCycle);
            array[idx].lastPos = pos;
            array[idx].ts = timestamp++;
            tag[idx] = pageAddr;
        }
        DBG("%s: MISS alloc idx %d", name.c_str(), idx);
    } else {  // entry hit
        profPageHits.inc();
        Entry& e = array[idx];
        array[idx].ts = timestamp++;
        DBG("%s: PAGE HIT idx %d", name.c_str(), idx);

        // 1. Did we prefetch-hit?
        bool shortPrefetch = false;
        if (e.valid[pos]) {
            uint64_t pfRespCycle = e.times[pos].respCycle;
            shortPrefetch = pfRespCycle > respCycle;
            e.valid[pos] = false;  // close, will help with long-lived transactions
            respCycle = MAX(pfRespCycle, respCycle);
            e.lastCycle = MAX(respCycle, e.lastCycle);
            profHits.inc();
            if (shortPrefetch) profShortHits.inc();
            DBG("%s: pos %d prefetched on %ld, pf resp %ld, demand resp %ld, short %d", name.c_str(), pos, e.times[pos].startCycle, pfRespCycle, respCycle, shortPrefetch);

            if (evRec && e.respEvents[pos]) {
                initAcc();
                // Link resp with PrefetchResponseEvent
                assert(acc.respCycle <= respCycle);
                assert(acc.endEvent);
                DelayEvent* dDownEv = new (evRec) DelayEvent(respCycle - acc.respCycle);
                dDownEv->setMinStartCycle(acc.respCycle);
                DelayEvent* dEndEv = new (evRec) DelayEvent(0);
                dEndEv->setMinStartCycle(respCycle);

                acc.endEvent->addChild(dDownEv, evRec)->addChild(dEndEv, evRec);
                e.respEvents[pos]->addChild(dEndEv, evRec);

                acc.respCycle = respCycle;
                acc.endEvent = dEndEv;
            }
        }

        // 2. Update predictors, issue prefetches
        int32_t stride = pos - e.lastPos;
        DBG("%s: pos %d lastPos %d lastLastPost %d e.stride %d", name.c_str(), pos, e.lastPos, e.lastLastPos, e.stride);
        if (e.stride == stride) {
            e.conf.inc();
            if (e.conf.pred()) {  // do prefetches
                int32_t fetchDepth = (e.lastPrefetchPos - e.lastPos)/stride;
                uint32_t prefetchPos = e.lastPrefetchPos + stride;
                if (fetchDepth < 1) {
                    prefetchPos = pos + stride;
                    fetchDepth = 1;
                }
                DBG("%s: pos %d stride %d conf %d lastPrefetchPos %d prefetchPos %d fetchDepth %d", name.c_str(), pos, stride, e.conf.counter(), e.lastPrefetchPos, prefetchPos, fetchDepth);

                auto issuePrefetch = [&](uint32_t prefetchPos) {
                    DBG("issuing prefetch");
                    MESIState state = I;
                    MemReq pfReq = {req.lineAddr + prefetchPos - pos, GETS, req.childId, &state, reqCycle, req.childLock, state, req.srcId, MemReq::PREFETCH};
                    uint64_t pfRespCycle = parent->access(pfReq);
                    assert(state == I);  // prefetch access should not give us any permissions

                    e.valid[prefetchPos] = true;
                    e.times[prefetchPos].fill(reqCycle, pfRespCycle);

                    if (evRec) {  // create & connect weave-phase events
                        DelayEvent* pfStartEv;
                        PrefetchResponseEvent* pfEndEv = new (evRec) PrefetchResponseEvent(this, idx, prefetchPos, 0 /*FIXME: assign domain @ init*/);
                        pfEndEv->setMinStartCycle(pfRespCycle);

                        if (evRec->hasRecord()) {
                            TimingRecord pfAcc = evRec->popRecord();
                            assert(pfAcc.isValid());
                            assert(pfAcc.reqCycle >= reqCycle);
                            assert(pfAcc.respCycle <= pfRespCycle);
                            pfStartEv = new (evRec) DelayEvent(pfAcc.reqCycle - reqCycle);
                            pfStartEv->setMinStartCycle(reqCycle);
                            pfStartEv->addChild(pfAcc.startEvent, evRec);

                            DelayEvent* pfDownEv = new (evRec) DelayEvent(pfRespCycle - pfAcc.respCycle);
                            pfDownEv->setMinStartCycle(pfAcc.respCycle);
                            pfAcc.endEvent->addChild(pfDownEv, evRec)->addChild(pfEndEv, evRec);
                        } else {
                            pfStartEv = new (evRec) DelayEvent(pfRespCycle - reqCycle);
                            pfStartEv->setMinStartCycle(reqCycle);
                            pfStartEv->addChild(pfEndEv, evRec);
                        }

                        initAcc();
                        assert(acc.isValid() && acc.reqCycle == reqCycle);

                        // Connect prefetch to start event
                        DelayEvent* startEv = new (evRec) DelayEvent(0);
                        startEv->setMinStartCycle(reqCycle);
                        startEv->addChild(acc.startEvent, evRec);
                        startEv->addChild(pfStartEv, evRec);
                        acc.startEvent = startEv;
                        assert(acc.reqCycle == reqCycle);

                        // Record the PrefetchEndEvent so that we can later connect it with prefetch-hit requests
                        e.respEvents[prefetchPos] = pfEndEv;
                    }
                };

                if (prefetchPos < 64 && !e.valid[prefetchPos]) {
                    issuePrefetch(prefetchPos);
                    profPrefetches.inc();

                    if (shortPrefetch && fetchDepth < 8 && prefetchPos + stride < 64 && !e.valid[prefetchPos + stride]) {
                        prefetchPos += stride;
                        issuePrefetch(prefetchPos);
                        profPrefetches.inc();
                        profDoublePrefetches.inc();
                    }
                    e.lastPrefetchPos = prefetchPos;
                }
            } else {
                profLowConfAccs.inc();
            }
        } else {
            e.conf.dec();
            // See if we need to switch strides
            if (!e.conf.pred()) {
                int32_t lastStride = e.lastPos - e.lastLastPos;

                if (stride && stride != e.stride && stride == lastStride) {
                    e.conf.reset();
                    e.stride = stride;
                    profStrideSwitches.inc();
                }
            }
            e.lastPrefetchPos = pos;
        }

        e.lastLastPos = e.lastPos;
        e.lastPos = pos;
    }

    if (acc.isValid()) evRec->pushRecord(acc);

    req.childId = origChildId;
    return respCycle;
}

// nop for now; do we need to invalidate our own state?
uint64_t StreamPrefetcher::invalidate(const InvReq& req) {
    return child->invalidate(req);
}

void StreamPrefetcher::simulatePrefetchResponse(PrefetchResponseEvent* ev, uint64_t cycle) {
    // Self-clean so future requests don't get linked to a stale event
    DBG("[%s] PrefetchResponse %d/%d cycle %ld min %ld", name.c_str(), ev->idx, ev->prefetchPos, cycle, ev->getMinStartCycle());
    assert(ev->idx < 16 && ev-> prefetchPos < 64);
    auto& evPtr = array[ev->idx].respEvents[ev->prefetchPos];
    // Guard avoids nullifying a pointer that changed before resp arrival (e.g., if entry was reused); this should be rare
    if (evPtr == ev) {
        evPtr = nullptr;
    } else {
        DBG("[%s] PrefetchResponse already changed (%p)", name.c_str(), evPtr);
    }
}
