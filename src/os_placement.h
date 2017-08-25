#pragma once
#include "memory_hierarchy.h"
#include "mc.h"

class DramCache;

class OSPlacementPolicy
{
public:
	OSPlacementPolicy(MemoryController * mc) : _mc(mc) {};
	void handleCacheAccess(Address tag, ReqType type);
	uint64_t remapPages(); 
	
	void clearStats(); 
	//void printInfo();
	 
private:
	
	MemoryController * _mc;
};
