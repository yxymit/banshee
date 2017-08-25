#include "page_placement.h"
#include "mc.h"
#include <stdlib.h>
#include <iostream>

void
PagePlacementPolicy::initialize(Config & config)
{
	_num_chunks = _mc->getNumSets();
	_chunks = (ChunkInfo *) gm_malloc(sizeof(ChunkInfo) *  _num_chunks);
	
	_scheme = _mc->getScheme();
	_sample_rate = config.get<double>("sys.mem.mcdram.sampleRate");
    _enable_replace = config.get<bool>("sys.mem.mcdram.enableReplace", true); 
	if (_sample_rate < 1) {
		_max_count_size = 31; //g_max_count_size;
		if (_mc->getGranularity() > 4096) // large page
			_max_count_size = 255; 
	} else 
		_max_count_size = 255;

	_num_entries_per_chunk = 9; //g_num_entries_per_chunk;
	//_num_stable_entries = _num_entries_per_chunk / 2;
	assert(_num_entries_per_chunk > _mc->getNumWays());
	for (uint64_t i = 0; i < _num_chunks; i++)
	{
		_chunks[i].num_hits = 0;
		_chunks[i].num_misses = 0;
		_chunks[i].entries = (ChunkEntry *) gm_malloc(sizeof(ChunkEntry) * _num_entries_per_chunk);
		for (uint32_t j = 0; j < _num_entries_per_chunk; j++)
			_chunks[i].entries[j].valid = false;
	}
	_histogram = NULL;
	srand48_r(rand(), &_buffer);
	clearStats();

	g_string scheme = config.get<const char *>("sys.mem.mcdram.placementPolicy");
	_lru_bits = (uint32_t **) gm_malloc(sizeof(uint32_t *) * _mc->getNumSets());
	for (uint64_t i = 0; i < _mc->getNumSets(); i++) {
		_lru_bits[i] = (uint32_t *) gm_malloc(sizeof(uint32_t) * _mc->getNumWays()); 
		for (uint32_t j = 0; j < _mc->getNumWays(); j++)
			_lru_bits[i][j] = j;
	}
	// hyrbid
	if (scheme == "LRU")
		_placement_policy = LRU;
	else if (scheme == "FBR")
		_placement_policy = FBR;
	else 
		assert(false);
}

uint32_t 
PagePlacementPolicy::handleCacheMiss(Address tag, ReqType type, uint64_t set_num, Set * set, bool &counter_access)
{
	uint64_t chunk_num = set_num;
	//ChunkInfo * chunk = &_chunks[chunk_num];
	_chunks[chunk_num].num_misses ++;
	
	if (_placement_policy == LRU)
	{
		if (set->hasEmptyWay()) {
			updateLRU(set_num, set->getEmptyWay());
			return set->getEmptyWay();
	 	}
		if (!_enable_replace)
			return _mc->getNumWays();
	  	double f;
	  	int64_t way;
		drand48_r(&_buffer, &f);
	  	lrand48_r(&_buffer, &way);
		if (f < _sample_rate) {
			//if (_scheme == UnisonCache) {
				for (uint32_t i = 0; i < _mc->getNumWays(); i++)
					if (_lru_bits[set_num][i] == _mc->getNumWays() - 1) {
						Address victim_tag = set->ways[i].tag;
						if (_scheme == HybridCache) {
							if (_mc->getTagBuffer()->canInsert(tag, victim_tag)) {
								updateLRU(set_num, i);
								return i;
							} else 
								return _mc->getNumWays();
						} else { 
							updateLRU(set_num, i);
							return i;
						}
					}
			//} else 
			//	return way % _mc->getNumWays();
		} else 
			return _mc->getNumWays();
	}
	assert(_placement_policy == FBR);
	assert(_enable_replace);

#if 1
	ChunkInfo * chunk = &_chunks[chunk_num];
	for (uint32_t way = 0; way < _mc->getNumWays(); way++)
		if (set->ways[way].valid) {
			if (set->ways[way].tag != _chunks[chunk_num].entries[way].tag)
			{
				for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
					printf("ID=%d, tag=%ld, valid=%d, count=%d\n", 
						i, chunk->entries[i].tag, chunk->entries[i].valid, chunk->entries[i].count);
				for (uint32_t i = 0; i < _mc->getNumWays(); i++)
					printf("ID=%d, tag=%ld\n", i, set->ways[i].tag);
			}
			assert(set->ways[way].tag == _chunks[chunk_num].entries[way].tag);
		}
#endif 

	// for HybridCache, never replace for store (LLC dirty evict) 
	if (type == STORE)
		return _mc->getNumWays();

	double sample_rate = _sample_rate;
	bool miss_rate_tune = true; //false;
	if (sample_rate == 1)
		miss_rate_tune = false;
	if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
		sample_rate = 1;

	// the set uses FBR replacement policy
	bool updateFBR = set->hasEmptyWay() ||  sampleOrNot(sample_rate, miss_rate_tune);
	if (updateFBR)
	{
		uint32_t empty_way = set->getEmptyWay();
		counter_access = true;
		_num_counter_read ++;
		_num_counter_write ++;
		uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num]);
		if (idx == _num_entries_per_chunk)
			return _mc->getNumWays();
		ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
		chunk_entry->count ++;
		if (chunk_entry->count >= _max_count_size) 
			handleCounterOverflow(&_chunks[chunk_num], chunk_entry);
		
		//idx = adjustEntryOrder(&_chunks[chunk_num], idx);
		//chunk_entry = &_chunks[chunk_num].entries[idx];
		
		// empty slots left in dram cache
		if (empty_way < _mc->getNumWays()) {
			assert(idx == empty_way);
			_num_empty_replace ++;
			return empty_way;
		}
		else // figure if we can replace an entry. 
		{
			assert(idx >= _mc->getNumWays());
			uint32_t victim_way = pickVictimWay(&_chunks[chunk_num]);
			assert(victim_way < _mc->getNumWays());
/*			if (compareCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way]) && !_mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].tag)) 
			{
				printf("!!!!!!Occupancy = %f\n", _mc->getTagBuffer()->getOccupancy());
				static int n = 0;
				printf("cannot insert (%d)   occupancy=%f.  set1=%ld, set2=%ld\n", 
						n++, _mc->getTagBuffer()->getOccupancy(), (tag % 128), _chunks[chunk_num].entries[victim_way].tag % 128);
			}
*/			
			if (compareCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way])
				&& _mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].tag))
			{
				//assert(idx < _num_stable_entries);
				// swap current way with victim way.
				ChunkEntry tmp = _chunks[chunk_num].entries[idx];
				_chunks[chunk_num].entries[idx] = _chunks[chunk_num].entries[victim_way];
				_chunks[chunk_num].entries[victim_way] = tmp;
				//assert(idx >= _mc->getNumWays() && idx < _num_stable_entries);
				return victim_way;
			} 
			else {
				return _mc->getNumWays();
			}
		}
	}
	return _mc->getNumWays();
}

void 
PagePlacementPolicy::handleCacheHit(Address tag, ReqType type, uint64_t set_num, Set * set, bool &counter_access, uint32_t hit_way)
{
	assert(tag == set->ways[hit_way].tag);
	if (_placement_policy == LRU) {
		//if (_scheme == UnisonCache)
			updateLRU(set_num, hit_way);
		return;
	}
	uint64_t chunk_num = set_num;
	ChunkInfo * chunk = &_chunks[chunk_num];
#if 1
	// for DEBUG
	// the first few entries in chunk->entries must be in dram cache
	for (uint32_t way = 0; way < _mc->getNumWays(); way++)
		if (set->ways[way].valid) {
			if (set->ways[way].tag != _chunks[chunk_num].entries[way].tag)
			{
				for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
					printf("ID=%d, tag=%ld, valid=%d, count=%d\n", 
						i, chunk->entries[i].tag, chunk->entries[i].valid, chunk->entries[i].count);
				for (uint32_t i = 0; i < _mc->getNumWays(); i++)
					printf("ID=%dm tag=%ld\n", i, set->ways[i].tag);
			}
			assert(set->ways[way].tag == _chunks[chunk_num].entries[way].tag);
		}
	// chunk->entries are properly ordered.
	/*uint32_t min_count = 10000;
	for (uint32_t way = 0; way < _num_stable_entries; way++)
		if (chunk->entries[way].valid && chunk->entries[way].count < min_count)
			min_count = chunk->entries[way].count;
	for (uint32_t way = _num_stable_entries; way < _num_entries_per_chunk; way++)
		if (chunk->entries[way].valid) {
			if (chunk->entries[way].count > min_count)
			{
				for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
					cout <<  i << " " << chunk->entries[i].valid << " " << chunk->entries[i].count << endl;
			}
			assert(chunk->entries[way].count <= min_count);
		}*/
#endif 

	double sample_rate = _sample_rate;
	bool miss_rate_tune = true; //false; 
	if (sample_rate == 1)
		miss_rate_tune = false;
	if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
	 	sample_rate = 1;
	if (sampleOrNot(sample_rate, miss_rate_tune))
	{
		counter_access = true;
		_num_counter_read ++;
		_num_counter_write ++;
		uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num]);
		ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
		assert(idx < _mc->getNumWays()); 
		chunk_entry->count ++;
		//assert( idx == adjustEntryOrder(&_chunks[chunk_num], idx ));
		if (chunk_entry->count >= _max_count_size) 
			handleCounterOverflow(&_chunks[chunk_num], chunk_entry);
	}
}

uint32_t
PagePlacementPolicy::getChunkEntry(Address tag, ChunkInfo * chunk_info, bool allocate)
{
	uint32_t idx = _num_entries_per_chunk; 
	for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
	{
		if (chunk_info->entries[i].valid && chunk_info->entries[i].tag == tag)
			return i; //&chunk_info->entries[i];
		else if (!chunk_info->entries[i].valid && idx == _num_entries_per_chunk) 
			idx = i; 
	}
	if (idx == _num_entries_per_chunk && allocate) 
	{
	  	int64_t rand;
		double f;
	  	lrand48_r(&_buffer, &rand);
		drand48_r(&_buffer, &f);
		// randomly pick a victim entry
		idx = _mc->getNumWays() + rand % (_num_entries_per_chunk - _mc->getNumWays());
		assert(idx >= _mc->getNumWays());
		// replace the entry with certain probability.
		// high count value reduces the probability
		if (chunk_info->entries[idx].count > 0 && f > 1.0 / chunk_info->entries[idx].count)
			idx = _num_entries_per_chunk;
	}
	if (idx < _num_entries_per_chunk) {
		chunk_info->entries[idx].valid = true; 
		chunk_info->entries[idx].tag = tag; 
		chunk_info->entries[idx].count = 0;
	}
	return idx;
}

bool 
PagePlacementPolicy::sampleOrNot(double sample_rate, bool miss_rate_tune)
{
	double miss_rate = _mc->getRecentMissRate();
	double f;
	drand48_r(&_buffer, &f);
	if (miss_rate_tune)
		return f < sample_rate * miss_rate;
	else 
		return f < sample_rate;
}

bool
PagePlacementPolicy::compareCounter(ChunkEntry * entry1, ChunkEntry * entry2)
{
	if (!entry1)
		return false;
	else if (!entry2)
		return entry1->count > 0;
	else
		//return entry1->count >= entry2->count + 32 * _sample_rate; 
		return entry1->count >= entry2->count + (_mc->getGranularity() / 64 / 2) * _sample_rate; 
		//getCurrSampleRate());
		//return (entry1->count - 1 > 1.1 * entry2->count); 
}

uint32_t
PagePlacementPolicy::adjustEntryOrder(ChunkInfo * chunk_info, uint32_t idx)
{
	assert(false);
/*
	// the modified entry was a stable entry, return 
	if (idx < _num_stable_entries)
		return idx;
	// the modified entry may be promoted.
	uint32_t min_count = 10000;
	uint32_t min_idx = 100;
	for (uint32_t i = _mc->getNumWays(); i < _num_stable_entries; i++)
	{
		assert(chunk_info->entries[i].valid);
		if (chunk_info->entries[i].count < min_count)
		{
			min_count = chunk_info->entries[i].count;
			min_idx = i;
		}
	}
	if (min_count <= chunk_info->entries[idx].count)
	{
		// swap the two entries
		ChunkEntry tmp = chunk_info->entries[idx];
		chunk_info->entries[idx] = chunk_info->entries[min_idx];
		chunk_info->entries[min_idx] = tmp;
		return min_idx;
	}
	return idx;
*/
}

uint32_t 
PagePlacementPolicy::pickVictimWay(ChunkInfo * chunk_info)
{
	uint32_t min_count = 10000;
	uint32_t min_idx = _mc->getNumWays();
	for (uint32_t i = 0; i < _mc->getNumWays(); i++)
	{
		assert(chunk_info->entries[i].valid);
		if (chunk_info->entries[i].count < min_count)
		{
			min_count = chunk_info->entries[i].count;
			min_idx = i;
		}
	}
	return min_idx;
}

void 
PagePlacementPolicy::handleCounterOverflow(ChunkInfo * chunk_info, ChunkEntry * overflow_entry)
{
	for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
	{
		if (overflow_entry == &chunk_info->entries[i])
			++(overflow_entry->count) /= 2; 
		else 
			chunk_info->entries[i].count /= 2;
	}
}

void 
PagePlacementPolicy::clearStats()
{
	_num_counter_read = 0;
	_num_counter_write = 0;
	
	_num_empty_replace = 0;
}

void 
PagePlacementPolicy::computeFreqDistr()
{
	assert(!_histogram);
	//_histogram = new uint64_t [_num_entries_per_chunk];
	for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		_histogram[i] = 0;
	for (uint64_t chunk_id = 0; chunk_id < _num_chunks; chunk_id ++)
	{
		// pick the most frequent chunk, then the second most frequent, etc. 
		for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		{
			uint32_t idx = _num_entries_per_chunk;
			uint32_t max_count = 0; 
			for (uint32_t j = 0; j < _num_entries_per_chunk; j++)
			{
				if (_chunks[chunk_id].entries[j].valid)
				{
					if (_chunks[chunk_id].entries[j].count > max_count)
					{
						max_count = _chunks[chunk_id].entries[j].count;
						idx = j;
					}
				}
				else 
					break;
			}
			if (idx != _num_entries_per_chunk)
			{
				_histogram[i] += max_count;
				_chunks[chunk_id].entries[idx].count = 0;
			}
			else 
				break;
		}
	}
}

void 
PagePlacementPolicy::updateLRU(uint64_t set_num, uint32_t way_num)
{
	for (uint32_t i = 0; i < _mc->getNumWays(); i++)
		if (_lru_bits[set_num][i] < _lru_bits[set_num][way_num])
			_lru_bits[set_num][i] ++;
	_lru_bits[set_num][way_num] = 0;
}

void 
PagePlacementPolicy::flushChunk(uint32_t set)
{
	for (uint32_t i = 0; i < _num_entries_per_chunk; i ++) {
		_chunks[set].entries[i].valid = false; 
		_chunks[set].entries[i].tag = 0; 
		_chunks[set].entries[i].count = 0; 
	}	
}

