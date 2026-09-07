#pragma once
#include "FaultAllocation.h"
// Intercept only the project's arena boundary; retain its real implementation.
#define malloc coverTestMalloc
#include "../../../lib/Memory/Arena.h"
#undef malloc
