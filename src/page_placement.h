#pragma once

#include "config.h"
#include "mc.h"

class Way;
class Set; 
class DramCache;

class PagePlacementPolicy
{
public:
	enum RepScheme 
	{
		LRU = 0,
		FBR
	};

	PagePlacementPolicy(MemoryController * mc) : _mc(mc) {};
	void initialize(Config & config);
	uint32_t handleCacheMiss(Address tag, ReqType type, uint64_t set_num, Set * set, bool &counter_access);
	void handleCacheHit(Address tag, ReqType type, uint64_t set_num, Set * set, bool &counter_access, uint32_t hit_way);
	
	uint64_t getTraffic() { return _num_counter_read + _num_counter_write; };
	void flushChunk(uint32_t set);
	void clearStats(); 
	RepScheme get_placement_policy() { return _placement_policy; }
private:
	MemoryController * _mc;
	struct ChunkEntry 
	{
		bool valid;
		Address tag;
		uint32_t count;
	};
	struct ChunkInfo 
	{
		uint32_t access_count;
		ChunkEntry * entries;
		uint64_t num_hits;
		uint64_t num_misses;
	};

	uint32_t getChunkEntry(Address tag, ChunkInfo * chunk_info, bool allocate=true);
	bool sampleOrNot(double sample_rate, bool miss_rate_tune = true);
	bool compareCounter(ChunkEntry * entry1, ChunkEntry * entry2);
	uint32_t adjustEntryOrder(ChunkInfo * chunk_info, uint32_t idx);
	uint32_t pickVictimWay(ChunkInfo * chunk_info);
	void handleCounterOverflow(ChunkInfo * chunk_info, ChunkEntry * overflow_entry);
	void computeFreqDistr();
	void updateLRU(uint64_t set_num, uint32_t way_num);
	double getCurrSampleRate();

	RepScheme _placement_policy;
	drand48_data _buffer;
	Scheme _scheme;	
	uint32_t ** _lru_bits; // on per set

	uint32_t _granularity;
	// Frequency Base Replacement
	ChunkInfo * _chunks;
	
	// Parameters
	uint64_t _num_chunks;
	uint32_t _num_entries_per_chunk;
	//uint32_t _num_stable_entries;
	double _sample_rate;
	uint32_t _access_count_threshold;
	uint32_t _max_count_size;
	bool _enable_replace;

	// Stats
	uint64_t * _histogram;
	uint64_t _num_counter_read;
	uint64_t _num_counter_write;
	uint64_t _num_empty_replace;
};
