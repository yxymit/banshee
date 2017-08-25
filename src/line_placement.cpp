#include "line_placement.h"
#include "mc.h"
#include <stdlib.h>

void
LinePlacementPolicy::initialize(Config & config)
{
   srand48_r(rand(), &_buffer);
   _sample_rate = config.get<double>("sys.mem.mcdram.sampleRate");
   _enable_replace = config.get<bool>("sys.mem.mcdram.enableReplace", true); 
}

bool 
LinePlacementPolicy::handleCacheMiss(Way * current_tad)
{
	if (!current_tad->valid)
		return true;
	if (!_enable_replace)
		return false;
	double f;
    drand48_r(&_buffer, &f);
    return f < _sample_rate;
}
