#include "os_placement.h"
#include "cache.h"
#include <queue>
#include <vector>
#include <functional>

void
OSPlacementPolicy::handleCacheAccess(Address tag, ReqType type)
{
   //map<Address, TLBEntry> &tlb = *_mc->getTLB();
   //assert(tlb.find(tag) != tlb.end());
   //tlb[tag].count ++; 
}

uint64_t 
OSPlacementPolicy::remapPages() 
{
   /*
   map<Address, TLBEntry> &tlb = *_mc->getTLB();
   // sort the pages based on frequency. 
   // if they have the same frequency, prioritize the page already cached.
   auto cmp = [](TLBEntry * left, TLBEntry * right) { return (left->count < right->count); };
   priority_queue<TLBEntry *, vector<TLBEntry *>, decltype(cmp)> queue(cmp);
   //priority_queue<TLBEntry *> queue(cmp);
   //printf("size = %ld\n", tlb.size());
   for(map<Address, TLBEntry>::iterator it = tlb.begin(); it != tlb.end(); ++it) {
      // move the page out of dram cache.
      it->second.way = _mc->getNumWays();
      queue.push(&it->second);
   }
   
   uint64_t cur_way = 0;
   assert(_mc->getNumSets() == 1);
   uint64_t num_replace = 0;
   for (uint64_t i = 0; i < _mc->getNumWays(); i++) {
      if (i == tlb.size())
         break;
      TLBEntry * top = queue.top();
	  if (top->way == _mc->getNumWays())
		  num_replace ++;
      top->way = cur_way;
      queue.pop();

      Set * cache = _mc->getSets();
      cache[0].ways[cur_way].valid = true;
      cache[0].ways[cur_way].tag = top->tag;
      cache[0].ways[cur_way].dirty = false; 
      cur_way ++;
   }
   // Clear all the counters in TLB
   for(map<Address, TLBEntry>::iterator it = tlb.begin(); it != tlb.end(); ++it)
      it->second.count /= 2;
   return num_replace;
   */
   return 0;
}
