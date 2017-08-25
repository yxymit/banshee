#include "mc.h"
#include "line_placement.h"
#include "page_placement.h"
#include "os_placement.h"
#include "mem_ctrls.h"
#include "dramsim_mem_ctrl.h"
#include "ddr_mem.h"
#include "zsim.h"

MemoryController::MemoryController(g_string& name, uint32_t frequency, uint32_t domain, Config& config)
	: _name (name)
{
	// Trace Related
	_collect_trace = config.get<bool>("sys.mem.enableTrace", false);
	if (_collect_trace && _name == "mem-0") {
		_cur_trace_len = 0;
		_max_trace_len = 10000;
		_trace_dir = config.get<const char *>("sys.mem.traceDir", "./");
		FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "wb");
		uint32_t num = 0;
		fwrite(&num, sizeof(uint32_t), 1, f);
		fclose(f);
	    futex_init(&_lock);
	}
	_sram_tag = config.get<bool>("sys.mem.sram_tag", false);
	_llc_latency = config.get<uint32_t>("sys.caches.l3.latency");
	double timing_scale = config.get<double>("sys.mem.dram_timing_scale", 1);
	g_string scheme = config.get<const char *>("sys.mem.cache_scheme", "NoCache");
	_ext_type = config.get<const char *>("sys.mem.ext_dram.type", "Simple");
	if (scheme != "NoCache") {
		_granularity = config.get<uint32_t>("sys.mem.mcdram.cache_granularity");	
		_num_ways = config.get<uint32_t>("sys.mem.mcdram.num_ways");	
		_mcdram_type = config.get<const char *>("sys.mem.mcdram.type", "Simple");
		_cache_size = config.get<uint32_t>("sys.mem.mcdram.size", 128) * 1024 * 1024;
	}
	if (scheme == "AlloyCache") {
		_scheme = AlloyCache;
		assert(_granularity == 64);
		assert(_num_ways == 1);
	}
	else if (scheme == "UnisonCache") {
		assert(_granularity == 4096);
		_scheme = UnisonCache;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	} else if (scheme == "HMA") {
		assert(_granularity == 4096);
		assert(_num_ways == _cache_size / _granularity);
		_scheme = HMA;
	} else if (scheme == "HybridCache") {
		// 4KB page or 2MB page
		assert(_granularity == 4096 || _granularity == 4096 * 512); 
		_scheme = HybridCache;
	} else if (scheme == "NoCache")
		_scheme = NoCache;
 	else if (scheme == "CacheOnly")
		_scheme = CacheOnly;
	else if (scheme == "Tagless") {
		_scheme = Tagless;
		_next_evict_idx = 0;
		_footprint_size = config.get<uint32_t>("sys.mem.mcdram.footprint_size");
	}
	else { 
		printf("scheme=%s\n", scheme.c_str());
		assert(false);
	}

	g_string placement_scheme = config.get<const char *>("sys.mem.mcdram.placementPolicy", "LRU");
	_bw_balance = config.get<bool>("sys.mem.bwBalance", false);
	_ds_index = 0;
	if (_bw_balance)
		assert(_scheme == AlloyCache || _scheme == HybridCache);

	// Configure the external Dram
	g_string ext_dram_name = _name + g_string("-ext");
	if (_ext_type == "Simple") {
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
        _ext_dram = (SimpleMemory *) gm_malloc(sizeof(SimpleMemory));
		new (_ext_dram)	SimpleMemory(latency, ext_dram_name, config);
	} else if (_ext_type == "DDR")
        _ext_dram = BuildDDRMemory(config, frequency, domain, ext_dram_name, "sys.mem.ext_dram.", 4, 1.0);
	else if (_ext_type == "MD1") {
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
        uint32_t bandwidth = config.get<uint32_t>("sys.mem.ext_dram.bandwidth", 6400);
        _ext_dram = (MD1Memory *) gm_malloc(sizeof(MD1Memory));
		new (_ext_dram) MD1Memory(64, frequency, bandwidth, latency, ext_dram_name);
    } else if (_ext_type == "DRAMSim") {
	    uint64_t cpuFreqHz = 1000000 * frequency;
        uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
        string dramTechIni = config.get<const char*>("sys.mem.techIni");
        string dramSystemIni = config.get<const char*>("sys.mem.systemIni");
        string outputDir = config.get<const char*>("sys.mem.outputDir");
        string traceName = config.get<const char*>("sys.mem.traceName", "dramsim");
		traceName += "_ext";
        _ext_dram = (DRAMSimMemory *) gm_malloc(sizeof(DRAMSimMemory));
    	uint32_t latency = config.get<uint32_t>("sys.mem.ext_dram.latency", 100);
		new (_ext_dram) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
	} else 
        panic("Invalid memory controller type %s", _ext_type.c_str());

	if (_scheme != NoCache) {		
		// Configure the MC-Dram (Timing Model)
		_mcdram_per_mc = config.get<uint32_t>("sys.mem.mcdram.mcdramPerMC", 4);
		//_mcdram = new MemObject * [_mcdram_per_mc];
		_mcdram = (MemObject **) gm_malloc(sizeof(MemObject *) * _mcdram_per_mc);
		for (uint32_t i = 0; i < _mcdram_per_mc; i++) {
			g_string mcdram_name = _name + g_string("-mc-") + g_string(to_string(i).c_str());
    	    //g_string mcdram_name(ss.str().c_str());
			if (_mcdram_type == "Simple") {
	    		uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				_mcdram[i] = (SimpleMemory *) gm_malloc(sizeof(SimpleMemory));
				new (_mcdram[i]) SimpleMemory(latency, mcdram_name, config);
	        	//_mcdram[i] = new SimpleMemory(latency, mcdram_name, config);
			} else if (_mcdram_type == "DDR") {
				// XXX HACK tBL for mcdram is 1, so for data access, should multiply by 2, for tad access, should multiply by 3. 
        		_mcdram[i] = BuildDDRMemory(config, frequency, domain, mcdram_name, "sys.mem.mcdram.", 4, timing_scale);
			} else if (_mcdram_type == "MD1") {
				uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
        		uint32_t bandwidth = config.get<uint32_t>("sys.mem.mcdram.bandwidth", 12800);
        		_mcdram[i] = (MD1Memory *) gm_malloc(sizeof(MD1Memory));
				new (_mcdram[i]) MD1Memory(64, frequency, bandwidth, latency, mcdram_name);
		    } else if (_mcdram_type == "DRAMSim") {
			    uint64_t cpuFreqHz = 1000000 * frequency;
		        uint32_t capacity = config.get<uint32_t>("sys.mem.capacityMB", 16384);
		        string dramTechIni = config.get<const char*>("sys.mem.techIni");
		        string dramSystemIni = config.get<const char*>("sys.mem.systemIni");
		        string outputDir = config.get<const char*>("sys.mem.outputDir");
		        string traceName = config.get<const char*>("sys.mem.traceName");
				traceName += "_mc";
				traceName += to_string(i);
		        _mcdram[i] = (DRAMSimMemory *) gm_malloc(sizeof(DRAMSimMemory));
    			uint32_t latency = config.get<uint32_t>("sys.mem.mcdram.latency", 50);
				new (_mcdram[i]) DRAMSimMemory(dramTechIni, dramSystemIni, outputDir, traceName, capacity, cpuFreqHz, latency, domain, name);
			} else 
    	     	panic("Invalid memory controller type %s", _mcdram_type.c_str());
		}
		// Configure MC-Dram Functional Model
		_num_sets = _cache_size / _num_ways / _granularity;
		if (_scheme == Tagless)
			assert(_num_sets == 1);
		_cache = (Set *) gm_malloc(sizeof(Set) * _num_sets);
		for (uint64_t i = 0; i < _num_sets; i ++) {
			_cache[i].ways = (Way *) gm_malloc(sizeof(Way) * _num_ways);
			_cache[i].num_ways = _num_ways;
			for (uint32_t j = 0; j < _num_ways; j++)
	   			_cache[i].ways[j].valid = false;
		}
		if (_scheme == AlloyCache) {
			_line_placement_policy = (LinePlacementPolicy *) gm_malloc(sizeof(LinePlacementPolicy));
			new (_line_placement_policy) LinePlacementPolicy();
   			_line_placement_policy->initialize(config);
		} else if (_scheme == HMA) {
			_os_placement_policy = (OSPlacementPolicy *) gm_malloc(sizeof(OSPlacementPolicy));
			new (_os_placement_policy) OSPlacementPolicy(this);
		} else if (_scheme == UnisonCache || _scheme == HybridCache ){
			_page_placement_policy = (PagePlacementPolicy *) gm_malloc(sizeof(PagePlacementPolicy));
			new (_page_placement_policy) PagePlacementPolicy(this);
		  		_page_placement_policy->initialize(config);
		}
	}
	if (_scheme == HybridCache) {
		_tag_buffer = (TagBuffer *) gm_malloc(sizeof(TagBuffer));	
		new (_tag_buffer) TagBuffer(config);
	}
 	// Stats
   _num_hit_per_step = 0;
   _num_miss_per_step = 0;
   _mc_bw_per_step = 0;
   _ext_bw_per_step = 0;
   for (uint32_t i = 0; i < MAX_STEPS; i++)
      _miss_rate_trace[i] = 0;
   _num_requests = 0;
}

uint64_t 
MemoryController::access(MemReq& req)
{
	switch (req.type) {
        case PUTS:
        case PUTX:
            *req.state = I;
            break;
        case GETS:
            *req.state = req.is(MemReq::NOEXCL)? S : E;
            break;
        case GETX:
            *req.state = M;
            break;
        default: panic("!?");
    }
	if (req.type == PUTS)
		return req.cycle;
	futex_lock(&_lock);
	// ignore clean LLC eviction
    if (_collect_trace && _name == "mem-0") {
        _address_trace[_cur_trace_len] = req.lineAddr;
        _type_trace[_cur_trace_len] = (req.type == PUTX)? 1 : 0;
        _cur_trace_len ++;
        assert(_cur_trace_len <= _max_trace_len);
        if (_cur_trace_len == _max_trace_len) {
            FILE * f = fopen((_trace_dir + g_string("/") + _name + g_string("trace.bin")).c_str(), "ab");
            fwrite(_address_trace, sizeof(Address), _max_trace_len, f);
            fwrite(_type_trace, sizeof(uint32_t), _max_trace_len, f);
            fclose(f);
            _cur_trace_len = 0;
        }
    }

	_num_requests ++;
	if (_scheme == NoCache) {
		///////   load from external dram
 		req.cycle = _ext_dram->access(req, 0, 4);
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
		////////////////////////////////////
	} 
	/////////////////////////////
	// TODO For UnisonCache
	// should correctly model way accesses
	/////////////////////////////
	
	ReqType type = (req.type == GETS || req.type == GETX)? LOAD : STORE;
	Address address = req.lineAddr;
	uint32_t mcdram_select = (address / 64) % _mcdram_per_mc;
	Address mc_address = (address / 64 / _mcdram_per_mc * 64) | (address % 64); 
	//printf("address=%ld, _mcdram_per_mc=%d, mc_address=%ld\n", address, _mcdram_per_mc, mc_address);
	Address tag = address / (_granularity / 64);
	uint64_t set_num = tag % _num_sets;
	uint32_t hit_way = _num_ways;
	//uint64_t orig_cycle = req.cycle;
	uint64_t data_ready_cycle = req.cycle;
    MESIState state;

	if (_scheme == CacheOnly) {
		///////   load from mcdram
		req.lineAddr = mc_address;
 		req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
		req.lineAddr = address;
		_numLoadHit.inc();
		futex_unlock(&_lock);
		return req.cycle;
		////////////////////////////////////
	}
	uint64_t step_length = _cache_size / 64 / 10; 

	// whether needs to probe tag for HybridCache.
	// need to do so for LLC dirty eviction and if the page is not in TB  
	bool hybrid_tag_probe = false; 
	if (_granularity >= 4096) {
		if (_tlb.find(tag) == _tlb.end())
         	_tlb[tag] = TLBEntry {tag, _num_ways, 0, 0, 0};
		if (_tlb[tag].way != _num_ways) {
			hit_way = _tlb[tag].way;
			assert(_cache[set_num].ways[hit_way].valid && _cache[set_num].ways[hit_way].tag == tag);
		} else if (_scheme != Tagless) {
			// for Tagless, this assertion takes too much time.
			for (uint32_t i = 0; i < _num_ways; i ++)
				assert(_cache[set_num].ways[i].tag != tag || !_cache[set_num].ways[i].valid);
		}

		if (_scheme == UnisonCache) {
			//// Tag and data access. For simplicity, use a single access.  
			if (type == LOAD) {
				req.lineAddr = mc_address; //transMCAddressPage(set_num, 0); //mc_address;
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
			} else {
				assert(type == STORE);
	            MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
			}
			///////////////////////////////
		}
		if (_scheme == HybridCache && type == STORE) {
			if (_tag_buffer->existInTB(tag) == _tag_buffer->getNumWays() && set_num >= _ds_index) {
				_numTBDirtyMiss.inc();
				if (!_sram_tag)
					hybrid_tag_probe = true;
			} else
				_numTBDirtyHit.inc();
		}
		if (_scheme == HybridCache && _sram_tag)
			req.cycle += _llc_latency;
 	}
   	else {
		assert(_scheme == AlloyCache);
		if (_cache[set_num].ways[0].valid && _cache[set_num].ways[0].tag == tag && set_num >= _ds_index) 
			hit_way = 0;
		if (type == LOAD && set_num >= _ds_index) { 
			///// mcdram TAD access
			// Modeling TAD as 2 cachelines
			if (_sram_tag) {
				req.cycle += _llc_latency; 
/*				if (hit_way == 0) {
					req.lineAddr = mc_address; 
					req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
					_mc_bw_per_step += 4;
					_numTagLoad.inc();
					req.lineAddr = address;
				}
*/
			} else { 
				req.lineAddr = mc_address; 
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 6);
				_mc_bw_per_step += 6;
				_numTagLoad.inc();
				req.lineAddr = address;
			}
			///////////////////////////////
		}
   	}
	bool cache_hit = hit_way != _num_ways;
	
	//orig_cycle = req.cycle; 
	// dram cache logic. Here, I'm assuming the 4 mcdram channels are 
	// organized centrally
	bool counter_access = false;
	// use the following state for requests, so that req.state is not changed
	if (!cache_hit)
	{
		uint64_t cur_cycle = req.cycle;
		_num_miss_per_step ++;
		if (type == LOAD)
			_numLoadMiss.inc();
		else
			_numStoreMiss.inc();
		
		uint32_t replace_way = _num_ways;
      	if (_scheme == AlloyCache) {
			bool place = false;
			if (set_num >= _ds_index)
	         	place = _line_placement_policy->handleCacheMiss(&_cache[set_num].ways[0]);
         	replace_way = place? 0 : 1;
      	} else if (_scheme == HMA)
         	_os_placement_policy->handleCacheAccess(tag, type);
      	else if (_scheme == Tagless) {
			replace_way = _next_evict_idx;
			_next_evict_idx = (_next_evict_idx + 1) % _num_ways;
		}
		else {
			if (set_num >= _ds_index)
	        	replace_way = _page_placement_policy->handleCacheMiss(tag, type, set_num, &_cache[set_num], counter_access);
		}

		/////// load from external dram
		if (_scheme == AlloyCache) {
			if (type == LOAD) {
				if (!_sram_tag && set_num >= _ds_index)
					req.cycle = _ext_dram->access(req, 1, 4);
				else 
					req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			} else if (type == STORE && replace_way >= _num_ways) {
				// no replacement
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			} else if (type == STORE) { // && replace_way < _num_ways)
	            MemReq load_req = {address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _ext_dram->access(load_req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		} else if (_scheme == HMA) { 
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		} else if (_scheme == UnisonCache) {
			if (type == LOAD) {
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			} else if (type == STORE && replace_way >= _num_ways) { 
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
			}
			data_ready_cycle = req.cycle;
		} else if (_scheme == HybridCache) {
			if (hybrid_tag_probe) {
		        MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				req.cycle = _ext_dram->access(req, 1, 4);
				_ext_bw_per_step += 4;
				_numTagLoad.inc();
				data_ready_cycle = req.cycle;
			} else {
				req.cycle = _ext_dram->access(req, 0, 4);
				_ext_bw_per_step += 4;
				data_ready_cycle = req.cycle;
			}
		} else if (_scheme == Tagless) {
			assert(_ext_dram);
			req.cycle = _ext_dram->access(req, 0, 4);
			_ext_bw_per_step += 4;
			data_ready_cycle = req.cycle;
		}
		////////////////////////////////////
      	
		if (replace_way < _num_ways)
      	{
			///// mcdram replacement 
			// TODO update the address 
			if (_scheme == AlloyCache) { 
	            MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				uint32_t size = _sram_tag? 4 : 6;
				_mcdram[mcdram_select]->access(insert_req, 2, size);
				_mc_bw_per_step += size;
				_numTagStore.inc();
			} else if (_scheme == UnisonCache || _scheme == HybridCache || _scheme == Tagless) {
				uint32_t access_size = (_scheme == UnisonCache || _scheme == Tagless)? _footprint_size : (_granularity / 64); 
				// load page from ext dram
		        MemReq load_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_ext_dram->access(load_req, 2, access_size*4);
				_ext_bw_per_step += access_size * 4;
				// store the page to mcdram
		        MemReq insert_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				_mcdram[mcdram_select]->access(insert_req, 2, access_size*4);
				_mc_bw_per_step += access_size * 4;
				if (_scheme == Tagless) {
		        	MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		        	MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
					_ext_dram->access(load_gipt_req, 2, 2); // update GIPT
					_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
					_ext_bw_per_step += 4;
				} else if (!_sram_tag) {
					_mcdram[mcdram_select]->access(insert_req, 2, 2); // store tag
					_mc_bw_per_step += 2;
				}
				_numTagStore.inc();
			}

			///////////////////////////////
			_numPlacement.inc();
         	if (_cache[set_num].ways[replace_way].valid)
			{
				Address replaced_tag = _cache[set_num].ways[replace_way].tag;
				// Note that tag_buffer is not updated if placed into an invalid entry.
				// this is like ignoring the initialization cost 
				if (_scheme == HybridCache) {
					// Update TagBuffer
					//if (!_tag_buffer->canInsert(tag, replaced_tag)) {
					//	printf("!!!!!!Occupancy = %f\n", _tag_buffer->getOccupancy());
					//	_tag_buffer->clearTagBuffer();
					//	_numTagBufferFlush.inc();
					//}
					//assert (_tag_buffer->canInsert(tag, replaced_tag));
					assert(_tag_buffer->canInsert(tag, replaced_tag));
					{
						_tag_buffer->insert(tag, true);
						_tag_buffer->insert(replaced_tag, true);
					}
					//else {
					//	goto end;
					//}
				}

           		_tlb[replaced_tag].way = _num_ways;
				// only used for UnisonCache
				uint32_t unison_dirty_lines = __builtin_popcountll(_tlb[replaced_tag].dirty_bitvec) * 4;
				uint32_t unison_touch_lines = __builtin_popcountll(_tlb[replaced_tag].touch_bitvec) * 4;
				if (_scheme == UnisonCache || _scheme == Tagless) {
					assert(unison_touch_lines > 0);
					assert(unison_touch_lines <= 64);
					assert(unison_dirty_lines <= 64);
					_numTouchedLines.inc(unison_touch_lines);
					_numEvictedLines.inc(unison_dirty_lines);
				}

				if (_cache[set_num].ways[replace_way].dirty) {
					_numDirtyEviction.inc();
					///////   store dirty line back to external dram
					// Store starts after TAD is loaded.
					// request not on critical path. 
					if (_scheme == AlloyCache) {
						if (type == STORE) {
							if (_sram_tag) {
			        	    	MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
								req.cycle = _mcdram[mcdram_select]->access(load_req, 2, 4);
								_mc_bw_per_step += 4;
								//_numTagLoad.inc();
							}
						}
		        	    MemReq wb_req = {_cache[set_num].ways[replace_way].tag, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, 4);
						_ext_bw_per_step += 4;
					} else if (_scheme == HybridCache) {
						// load page from mcdram
				        MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, (_granularity / 64)*4);
						_mc_bw_per_step += (_granularity / 64)*4;
						// store page to ext dram
						// TODO. this event should be appended under the one above. 
						// but they are parallel right now.
	        	    	MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, (_granularity / 64) * 4);
						_ext_bw_per_step += (_granularity / 64) * 4;
					} else if (_scheme == UnisonCache || _scheme == Tagless) {
						assert(unison_dirty_lines > 0);
						// load page from mcdram
						assert(unison_dirty_lines <= 64);
				        MemReq load_req = {mc_address, GETS, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_mcdram[mcdram_select]->access(load_req, 2, unison_dirty_lines*4);
						_mc_bw_per_step += unison_dirty_lines*4;
						// store page to ext dram
						// TODO. this event should be appended under the one above. 
						// but they are parallel right now.
	        	    	MemReq wb_req = {_cache[set_num].ways[replace_way].tag * 64, PUTX, req.childId, &state, cur_cycle, req.childLock, req.initialState, req.srcId, req.flags};
						_ext_dram->access(wb_req, 2, unison_dirty_lines*4);
						_ext_bw_per_step += unison_dirty_lines*4;
						if (_scheme == Tagless) {
				        	MemReq load_gipt_req = {tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				        	MemReq store_gipt_req = {tag * 64, PUTS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
							_ext_dram->access(load_gipt_req, 2, 2); // update GIPT
							_ext_dram->access(store_gipt_req, 2, 2); // update GIPT
							_ext_bw_per_step += 4;
						} 
					}

					/////////////////////////////
            	} else {
					_numCleanEviction.inc();
					 if (_scheme == UnisonCache || _scheme == Tagless) 
						assert(unison_dirty_lines == 0);
				}
         	}
         	_cache[set_num].ways[replace_way].valid = true;
			_cache[set_num].ways[replace_way].tag = tag;
         	_cache[set_num].ways[replace_way].dirty = (req.type == PUTX);
         	_tlb[tag].way = replace_way;
			if (_scheme == UnisonCache || _scheme == Tagless) {
				uint64_t bit = (address - tag * 64) / 4;
				assert(bit < 16 && bit >= 0);
				bit = ((uint64_t)1UL) << bit;
				_tlb[tag].touch_bitvec = 0;
				_tlb[tag].dirty_bitvec = 0;
				_tlb[tag].touch_bitvec |= bit;
				if (type == STORE)
					_tlb[tag].dirty_bitvec |= bit;
			}
      	} else {
			// Miss but no replacement 
			if (_scheme == HybridCache) 
				if (type == LOAD && _tag_buffer->canInsert(tag)) 
					_tag_buffer->insert(tag, false);
			assert(_scheme != Tagless)
		}
	} else { // cache_hit == true
		assert(set_num >= _ds_index);
		if (_scheme == AlloyCache) {
			if (type == LOAD && _sram_tag) {
		        MemReq read_req = {mc_address, GETX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(read_req, 0, 4);
				_mc_bw_per_step += 4;
			} 
			if (type == STORE) {
				// LLC dirty eviction hit
		        MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(write_req, 0, 4);
				_mc_bw_per_step += 4;
			}
		} else if (_scheme == UnisonCache && type == STORE)	{
			// LLC dirty eviction hit
	        MemReq write_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			req.cycle = _mcdram[mcdram_select]->access(write_req, 1, 4);
			_mc_bw_per_step += 4;
		}
		if (_scheme == AlloyCache || _scheme == UnisonCache)
			data_ready_cycle = req.cycle;
		_num_hit_per_step ++;
      	if (_scheme == HMA)
        	_os_placement_policy->handleCacheAccess(tag, type);
      	else if (_scheme == HybridCache || _scheme == UnisonCache) {
	       	_page_placement_policy->handleCacheHit(tag, type, set_num, &_cache[set_num], counter_access, hit_way);
		}


		if (req.type == PUTX) {
			_numStoreHit.inc();
			_cache[set_num].ways[hit_way].dirty = true;
		}
		else
			_numLoadHit.inc();
	
		if (_scheme == HybridCache) {
			if (!hybrid_tag_probe) {
				req.lineAddr = mc_address; 
				req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address;
				data_ready_cycle = req.cycle;
				if (type == LOAD && _tag_buffer->canInsert(tag)) 
					_tag_buffer->insert(tag, false);
			} else {
				assert(!_sram_tag);
	            MemReq tag_probe = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
				req.cycle = _mcdram[mcdram_select]->access(tag_probe, 0, 2);
				_mc_bw_per_step += 2;
				_numTagLoad.inc();
				req.lineAddr = mc_address; 
				req.cycle = _mcdram[mcdram_select]->access(req, 1, 4);
				_mc_bw_per_step += 4;
				req.lineAddr = address;
				data_ready_cycle = req.cycle;
			}
		}
		else if (_scheme == Tagless) {
			req.lineAddr = mc_address; 
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;
			
			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}

		//// data access  
		if (_scheme == HMA) {
			req.lineAddr = mc_address; //transMCAddressPage(set_num, hit_way); //mc_address;
			req.cycle = _mcdram[mcdram_select]->access(req, 0, 4);
			_mc_bw_per_step += 4;
			req.lineAddr = address;
			data_ready_cycle = req.cycle;
		}
		if (_scheme == UnisonCache) {
			// Update LRU information for UnisonCache
		    MemReq tag_update_req = {mc_address, PUTX, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
			_mcdram[mcdram_select]->access(tag_update_req, 2, 2);
			_mc_bw_per_step += 2;
			_numTagStore.inc();
			uint64_t bit = (address - tag * 64) / 4;
			assert(bit < 16 && bit >= 0);
			bit = ((uint64_t)1UL) << bit;
			_tlb[tag].touch_bitvec |= bit;
			if (type == STORE)
				_tlb[tag].dirty_bitvec |= bit;
		}
		///////////////////////////////
	}
//end:
	// TODO. make this part work again.
	if (counter_access && !_sram_tag) {
		// TODO may not need the counter load if we can store freq info inside TAD
		/////// model counter access in mcdram
		// One counter read and one coutner write
		assert(set_num >= _ds_index);
		_numCounterAccess.inc();
        MemReq counter_req = {mc_address, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		counter_req.type = PUTX;
		_mcdram[mcdram_select]->access(counter_req, 2, 2);
		_mc_bw_per_step += 4;
		//////////////////////////////////////
	}
	if (_scheme == HybridCache && _tag_buffer->getOccupancy() > 0.7) {
		printf("[Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
		_tag_buffer->clearTagBuffer();
		_tag_buffer->setClearTime(req.cycle);
		_numTagBufferFlush.inc();
	}

	// TODO. Make the timing info here correct.
	// TODO. should model system level stall 
	if (_scheme == HMA && _num_requests % _os_quantum == 0) {
      	uint64_t num_replace = _os_placement_policy->remapPages();
		_numPlacement.inc(num_replace * 2);
   	}

	if (_num_requests % step_length == 0)
	{
		_num_hit_per_step /= 2;	
		_num_miss_per_step /= 2;
		_mc_bw_per_step /= 2;
		_ext_bw_per_step /= 2;
		if (_bw_balance && _mc_bw_per_step + _ext_bw_per_step > 0) {
			// adjust _ds_index	based on mc vs. ext dram bandwidth.
			double ratio = 1.0 * _mc_bw_per_step / (_mc_bw_per_step + _ext_bw_per_step);
			double target_ratio = 0.8;  // because mc_bw = 4 * ext_bw
	
			// the larger the gap between ratios, the more _ds_index changes. 
			// _ds_index changes in the granualrity of 1/1000 dram cache capacity.
			// 1% in the ratio difference leads to 1/1000 _ds_index change. 		
			// 300 is arbitrarily chosen. 
			// XXX XXX XXX
			// 1000 is only used for graph500 and pagerank.
			//uint64_t index_step = _num_sets / 300; // in terms of the number of sets 
			uint64_t index_step = _num_sets / 1000; // in terms of the number of sets 
			int64_t delta_index = (ratio - target_ratio > -0.02 && ratio - target_ratio < 0.02)? 
					0 : index_step * (ratio - target_ratio) / 0.01;
			printf("ratio = %f\n", ratio);
			if (delta_index > 0) {
				// _ds_index will increase. All dirty data between _ds_index and _ds_index + delta_index
				// should be written back to external dram.
				// For Alloy cache, this is relatively easy. 
				// For Hybrid, we need to update tag buffer as well... 
				for (uint32_t mc = 0; mc < _mcdram_per_mc; mc ++) {
					for (uint64_t set = _ds_index; set < (uint64_t)(_ds_index + delta_index); set ++) {
						if (set >= _num_sets) break;
						for (uint32_t way = 0; way < _num_ways; way ++)	 {
							Way &meta = _cache[set].ways[way];
							if (meta.valid && meta.dirty) {
								// should write back to external dram. 					
						        MemReq load_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_mcdram[mc]->access(load_req, 2, (_granularity / 64)*4);
						        MemReq wb_req = {meta.tag * 64, GETS, req.childId, &state, req.cycle, req.childLock, req.initialState, req.srcId, req.flags};
								_ext_dram->access(wb_req, 2, (_granularity / 64)*4);
								_ext_bw_per_step += (_granularity / 64)*4;
								_mc_bw_per_step += (_granularity / 64)*4;
							}
							if (_scheme == HybridCache && meta.valid) {
				           		_tlb[meta.tag].way = _num_ways;
								// for Hybrid cache, should insert to tag buffer as well. 
								if (!_tag_buffer->canInsert(meta.tag)) {
									printf("Rebalance. [Tag Buffer FLUSH] occupancy = %f\n", _tag_buffer->getOccupancy());
									_tag_buffer->clearTagBuffer();
									_tag_buffer->setClearTime(req.cycle);
									_numTagBufferFlush.inc();
								}
								assert(_tag_buffer->canInsert(meta.tag));
								_tag_buffer->insert(meta.tag, true);
							}
							meta.valid = false;
							meta.dirty = false;
						}
						if (_scheme == HybridCache)
							_page_placement_policy->flushChunk(set);
					}
				}
			}
			_ds_index = ((int64_t)_ds_index + delta_index <= 0)? 0 : _ds_index + delta_index;
			printf("_ds_index = %ld/%ld\n", _ds_index, _num_sets);
		}
	}
 	futex_unlock(&_lock);
	//uint64_t latency = req.cycle - orig_cycle;
	//req.cycle = orig_cycle;
	return data_ready_cycle; //req.cycle + latency;
}

DDRMemory* 
MemoryController::BuildDDRMemory(Config& config, uint32_t frequency, 
								 uint32_t domain, g_string name, const string& prefix, uint32_t tBL, double timing_scale) 
{
    uint32_t ranksPerChannel = config.get<uint32_t>(prefix + "ranksPerChannel", 4);
    uint32_t banksPerRank = config.get<uint32_t>(prefix + "banksPerRank", 8);  // DDR3 std is 8
    uint32_t pageSize = config.get<uint32_t>(prefix + "pageSize", 8*1024);  // 1Kb cols, x4 devices
    const char* tech = config.get<const char*>(prefix + "tech", "DDR3-1333-CL10");  // see cpp file for other techs
    const char* addrMapping = config.get<const char*>(prefix + "addrMapping", "rank:col:bank");  // address splitter interleaves channels; row always on top

    // If set, writes are deferred and bursted out to reduce WTR overheads
    bool deferWrites = config.get<bool>(prefix + "deferWrites", true);
    bool closedPage = config.get<bool>(prefix + "closedPage", true);

    // Max row hits before we stop prioritizing further row hits to this bank.
    // Balances throughput and fairness; 0 -> FCFS / high (e.g., -1) -> pure FR-FCFS
    uint32_t maxRowHits = config.get<uint32_t>(prefix + "maxRowHits", 4);

    // Request queues
    uint32_t queueDepth = config.get<uint32_t>(prefix + "queueDepth", 16);
    uint32_t controllerLatency = config.get<uint32_t>(prefix + "controllerLatency", 10);  // in system cycles

    auto mem = (DDRMemory *) gm_malloc(sizeof(DDRMemory));
	new (mem) DDRMemory(zinfo->lineSize, pageSize, ranksPerChannel, banksPerRank, frequency, tech, addrMapping, controllerLatency, queueDepth, maxRowHits, deferWrites, closedPage, domain, name, tBL, timing_scale);
    return mem;
}

void 
MemoryController::initStats(AggregateStat* parentStat)
{
	AggregateStat* memStats = new AggregateStat();
	memStats->init(_name.c_str(), "Memory controller stats");

	_numPlacement.init("placement", "Number of Placement"); memStats->append(&_numPlacement);
	_numCleanEviction.init("cleanEvict", "Clean Eviction"); memStats->append(&_numCleanEviction);
	_numDirtyEviction.init("dirtyEvict", "Dirty Eviction"); memStats->append(&_numDirtyEviction);
	_numLoadHit.init("loadHit", "Load Hit"); memStats->append(&_numLoadHit);
	_numLoadMiss.init("loadMiss", "Load Miss"); memStats->append(&_numLoadMiss);
	_numStoreHit.init("storeHit", "Store Hit"); memStats->append(&_numStoreHit);
	_numStoreMiss.init("storeMiss", "Store Miss"); memStats->append(&_numStoreMiss);
	_numCounterAccess.init("counterAccess", "Counter Access"); memStats->append(&_numCounterAccess);
	
	_numTagLoad.init("tagLoad", "Number of tag loads"); memStats->append(&_numTagLoad);
	_numTagStore.init("tagStore", "Number of tag stores"); memStats->append(&_numTagStore);
	_numTagBufferFlush.init("tagBufferFlush", "Number of tag buffer flushes"); memStats->append(&_numTagBufferFlush);

	_numTBDirtyHit.init("TBDirtyHit", "Tag buffer hits (LLC dirty evict)"); memStats->append(&_numTBDirtyHit);
	_numTBDirtyMiss.init("TBDirtyMiss", "Tag buffer misses (LLC dirty evict)"); memStats->append(&_numTBDirtyMiss);
	
	_numTouchedLines.init("totalTouchLines", "total # of touched lines in UnisonCache"); memStats->append(&_numTouchedLines);
	_numEvictedLines.init("totalEvictLines", "total # of evicted lines in UnisonCache"); memStats->append(&_numEvictedLines);

	_ext_dram->initStats(memStats);
	for (uint32_t i = 0; i < _mcdram_per_mc; i++) 
		_mcdram[i]->initStats(memStats);

    parentStat->append(memStats);
}


Address 
MemoryController::transMCAddress(Address mc_addr)
{
	// 28 lines per DRAM row (2048 KB row)
	uint64_t num_lines_per_mc = 128*1024*1024 / 2048 * 28; 
	uint64_t set = mc_addr % num_lines_per_mc;
	return set / 28 * 32 + set % 28; 
}

Address 
MemoryController::transMCAddressPage(uint64_t set_num, uint32_t way_num)
{
	return (_num_ways * set_num + way_num) * _granularity;
}

TagBuffer::TagBuffer(Config & config)
{
	uint32_t tb_size = config.get<uint32_t>("sys.mem.mcdram.tag_buffer_size", 1024);
	_num_ways = 8; 
	_num_sets = tb_size / _num_ways;
	_entry_occupied = 0;
	_tag_buffer = (TagBufferEntry **) gm_malloc(sizeof(TagBufferEntry *) * _num_sets);
	//_tag_buffer = new TagBufferEntry * [_num_sets];
	for (uint32_t i = 0; i < _num_sets; i++) {
		_tag_buffer[i] = (TagBufferEntry *) gm_malloc(sizeof(TagBufferEntry) * _num_ways);
		//_tag_buffer[i] = new TagBufferEntry [_num_ways];
		for (uint32_t j = 0; j < _num_ways; j ++) {
			_tag_buffer[i][j].remap = false; 
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}

uint32_t 
TagBuffer::existInTB(Address tag) 
{
	uint32_t set_num = tag % _num_sets;
	for (uint32_t i = 0; i < _num_ways; i++)
		if (_tag_buffer[set_num][i].tag == tag) {
			//printf("existInTB\n");
			return i;
		}
	return _num_ways;
}

bool 
TagBuffer::canInsert(Address tag)
{
#if 1
	uint32_t num = 0;
	for (uint32_t i = 0; i < _num_sets; i++) 
		for (uint32_t j = 0; j < _num_ways; j++) 
			if (_tag_buffer[i][j].remap)
				num ++;
	assert(num == _entry_occupied);
#endif

	uint32_t set_num = tag % _num_sets;
	//printf("tag_buffer=%#lx, set_num=%d, tag_buffer[set_num]=%#lx, num_ways=%d\n", 
	//	(uint64_t)_tag_buffer, set_num, (uint64_t)_tag_buffer[set_num], _num_ways);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap || _tag_buffer[set_num][i].tag == tag)
			return true;
	return false;
}

bool 
TagBuffer::canInsert(Address tag1, Address tag2)
{
	uint32_t set_num1 = tag1 % _num_sets;
	uint32_t set_num2 = tag2 % _num_sets;
	if (set_num1 != set_num2)
		return canInsert(tag1) && canInsert(tag2);
	else {
		uint32_t num = 0;
		for (uint32_t i = 0; i < _num_ways; i++)
			if (!_tag_buffer[set_num1][i].remap 
				|| _tag_buffer[set_num1][i].tag == tag1 
				|| _tag_buffer[set_num1][i].tag == tag2)
				num ++;
		return num >= 2;
	}
}

void 
TagBuffer::insert(Address tag, bool remap)
{
	uint32_t set_num = tag % _num_sets;
	uint32_t exist_way = existInTB(tag);
#if 1
	for (uint32_t i = 0; i < _num_ways; i++) 
		for (uint32_t j = i+1; j < _num_ways; j++) { 
			//if (_tag_buffer[set_num][i].tag != 0 && _tag_buffer[set_num][i].tag == _tag_buffer[set_num][j].tag) {
			//	for (uint32_t k = 0; k < _num_ways; k++) 
			//		printf("_tag_buffer[%d][%d]: tag=%ld, remap=%d\n", 
			//			set_num, k, _tag_buffer[set_num][k].tag, _tag_buffer[set_num][k].remap);
			//}
			assert(_tag_buffer[set_num][i].tag != _tag_buffer[set_num][j].tag
				  || _tag_buffer[set_num][i].tag == 0);
		}
#endif
	if (exist_way < _num_ways) {
		// the tag already exists in the Tag Buffer
		assert(tag == _tag_buffer[set_num][exist_way].tag);
		if (remap) {
			if (!_tag_buffer[set_num][exist_way].remap)
				_entry_occupied ++;
			_tag_buffer[set_num][exist_way].remap = true;
		} else if (!_tag_buffer[set_num][exist_way].remap)
			updateLRU(set_num, exist_way);
		return;
	}

	uint32_t max_lru = 0;
	uint32_t replace_way = _num_ways;
	for (uint32_t i = 0; i < _num_ways; i++) {
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru >= max_lru) {
			max_lru = _tag_buffer[set_num][i].lru;
			replace_way = i;
		}
	}
	assert(replace_way != _num_ways);
	_tag_buffer[set_num][replace_way].tag = tag;
	_tag_buffer[set_num][replace_way].remap = remap;
	if (!remap) { 
		//printf("\tset=%d way=%d, insert. no remap\n", set_num, replace_way);
		updateLRU(set_num, replace_way);
	} else { 
		//printf("set=%d way=%d, insert\n", set_num, replace_way);
		_entry_occupied ++;
	}
}

void 
TagBuffer::updateLRU(uint32_t set_num, uint32_t way)
{
	assert(!_tag_buffer[set_num][way].remap);
	for (uint32_t i = 0; i < _num_ways; i++)
		if (!_tag_buffer[set_num][i].remap && _tag_buffer[set_num][i].lru < _tag_buffer[set_num][way].lru)
			_tag_buffer[set_num][i].lru ++;
	_tag_buffer[set_num][way].lru = 0;
}

void 
TagBuffer::clearTagBuffer() 
{
	_entry_occupied = 0;
	for (uint32_t i = 0; i < _num_sets; i++) {
		for (uint32_t j = 0; j < _num_ways; j ++) {
			_tag_buffer[i][j].remap = false; 
			_tag_buffer[i][j].tag = 0;
			_tag_buffer[i][j].lru = j;
		}
	}
}
