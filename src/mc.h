#ifndef _MC_H_
#define _MC_H_

#include "config.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include <string>
#include "stats.h"
#include "g_std/g_unordered_map.h"

#define MAX_STEPS 10000

enum ReqType
{
	LOAD = 0,
	STORE
};

enum Scheme
{
   AlloyCache,
   UnisonCache,
   HMA,
   HybridCache,
   NoCache,
   CacheOnly,
   Tagless
};

class Way
{
public:
   Address tag;
   bool valid;
   bool dirty;
};

class Set
{
public:
   Way * ways;
   uint32_t num_ways;

   uint32_t getEmptyWay()
   {
      for (uint32_t i = 0; i < num_ways; i++)
         if (!ways[i].valid)
            return i;
      return num_ways;
   };
   bool hasEmptyWay() { return getEmptyWay() < num_ways; };
};

// Not modeling all details of the tag buffer. 
class TagBufferEntry
{
public:
	Address tag;
	bool remap;
	uint32_t lru;
};

class TagBuffer : public GlobAlloc {
public:
	TagBuffer(Config &config);
	// return: exists in tag buffer or not.
	uint32_t existInTB(Address tag);
	uint32_t getNumWays() { return _num_ways; };

	// return: if the address can be inserted to tag buffer or not.
	bool canInsert(Address tag);
	bool canInsert(Address tag1, Address tag2);
	void insert(Address tag, bool remap);
	double getOccupancy() { return 1.0 * _entry_occupied / _num_ways / _num_sets; };
	void clearTagBuffer();
	void setClearTime(uint64_t time) { _last_clear_time = time; };
	uint64_t getClearTime() { return _last_clear_time; };
private:
	void updateLRU(uint32_t set_num, uint32_t way);
	TagBufferEntry ** _tag_buffer;
	uint32_t _num_ways;
	uint32_t _num_sets;
	uint32_t _entry_occupied;
	uint64_t _last_clear_time;
};

class TLBEntry
{
public:
   uint64_t tag;
   uint64_t way;
   uint64_t count; // for OS based placement policy

   // the following two are only for UnisonCache
   // due to space cosntraint, it is not feasible to keep one bit for each line, 
   // so we use 1 bit for 4 lines.
   uint64_t touch_bitvec; // whether a line is touched in a page
   uint64_t dirty_bitvec; // whether a line is dirty in page
};

class LinePlacementPolicy;
class PagePlacementPolicy;
class OSPlacementPolicy;

//class PlacementPolicy;
class DDRMemory;

class MemoryController : public MemObject {
private:
	DDRMemory * BuildDDRMemory(Config& config, uint32_t frequency, uint32_t domain, g_string name, const std::string& prefix, uint32_t tBL, double timing_scale);
	
	g_string _name;

	// Trace related code
	lock_t _lock;
	bool _collect_trace;
	g_string _trace_dir;
	Address _address_trace[10000];
	uint32_t _type_trace[10000];
	uint32_t _cur_trace_len;
	uint32_t _max_trace_len;

	// External Dram Configuration
	MemObject *	_ext_dram;
	g_string _ext_type; 
public:	
	// MC-Dram Configuration
	MemObject ** _mcdram;
	uint32_t _mcdram_per_mc;
	g_string _mcdram_type;
	
	uint64_t getNumRequests() { return _num_requests; };
   	uint64_t getNumSets()     { return _num_sets; };
   	uint32_t getNumWays()     { return _num_ways; };
   	double getRecentMissRate(){ return (double) _num_miss_per_step / (_num_miss_per_step + _num_hit_per_step); };
   	Scheme getScheme()      { return _scheme; };
   	Set * getSets()         { return _cache; };
   	g_unordered_map<Address, TLBEntry> * getTLB() { return &_tlb; };
	TagBuffer * getTagBuffer() { return _tag_buffer; };

	uint64_t getGranularity() { return _granularity; };

private:
	// For Alloy Cache.
	Address transMCAddress(Address mc_addr);
	// For Page Granularity Cache
	Address transMCAddressPage(uint64_t set_num, uint32_t way_num); 

	// For Tagless.
	// For Tagless, we don't use "Set * _cache;" as other schemes. Instead, we use the following 
	// structure to model a fully associative cache with FIFO replacement 
	//vector<Address> _idx_to_address;
	uint64_t _next_evict_idx;
	//map<uint64_t, uint64_t> _address_to_idx;

	// Cache structure
	uint64_t _granularity;
	uint64_t _num_ways;
	uint64_t _cache_size;  // in Bytes
	uint64_t _num_sets;
	Set * _cache;
	LinePlacementPolicy * _line_placement_policy;
	PagePlacementPolicy * _page_placement_policy;
	OSPlacementPolicy * _os_placement_policy;
	uint64_t _num_requests;
	Scheme _scheme; 
	TagBuffer * _tag_buffer;
	
	// For HybridCache
	uint32_t _footprint_size; 

	// Balance in- and off-package DRAM bandwidth. 
	// From "BATMAN: Maximizing Bandwidth Utilization of Hybrid Memory Systems"
	bool _bw_balance; 
	uint64_t _ds_index;

	// TLB Hack
	g_unordered_map <Address, TLBEntry> _tlb;
	uint64_t _os_quantum;

    // Stats
	Counter _numPlacement;
  	Counter _numCleanEviction;
	Counter _numDirtyEviction;
	Counter _numLoadHit;
	Counter _numLoadMiss;
	Counter _numStoreHit;
	Counter _numStoreMiss;
	Counter _numCounterAccess; // for FBR placement policy  

	Counter _numTagLoad;
	Counter _numTagStore;
	// For HybridCache	
	Counter _numTagBufferFlush;
	Counter _numTBDirtyHit;
	Counter _numTBDirtyMiss;
	// For UnisonCache
	Counter _numTouchedLines;
	Counter _numEvictedLines;

	uint64_t _num_hit_per_step;
   	uint64_t _num_miss_per_step;
	uint64_t _mc_bw_per_step;
	uint64_t _ext_bw_per_step;
   	double _miss_rate_trace[MAX_STEPS];

   	uint32_t _num_steps;

	// to model the SRAM tag
	bool 	_sram_tag;
	uint32_t _llc_latency;
public:
	MemoryController(g_string& name, uint32_t frequency, uint32_t domain, Config& config);
	uint64_t access(MemReq& req);
	const char * getName() { return _name.c_str(); };
	void initStats(AggregateStat* parentStat); 
	// Use glob mem
	//using GlobAlloc::operator new;
	//using GlobAlloc::operator delete;
};

#endif
