#include "FaultAllocation.h"
// The actual inflater owns its two malloc blocks; inject both independently.
#define malloc coverTestMalloc
#include "../../../lib/miniz/src/InflateStream.cpp"
#undef malloc
