#pragma once

#include "config.h"
#include "memory_hierarchy.h"

using namespace std;

class MemoryController;
class Way;

class LinePlacementPolicy
{
public:
   LinePlacementPolicy() {}; 
   void initialize(Config & config);
   bool handleCacheMiss(Way * current_tad);
   
private:
   drand48_data _buffer;
   double _sample_rate;
   bool _enable_replace;
};
