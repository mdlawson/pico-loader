#pragma once
#include "sections.h"

DEFINE_SECTION_SYMBOLS(patch_census);

extern "C" void patch_census_tick_entry(void);
extern "C" void patch_census_abort_entry(void);

// variables the loader fills, as offsets into the census space (CensusPatchCode.s)
#define CENSUS_V_WANT       0x44
#define CENSUS_V_VECADDR    0x70
#define CENSUS_V_BSTACK     0x74
#define CENSUS_V_ARRAYS     0xF4
#define CENSUS_V_MAGIC      0x108
#define CENSUS_V_SLOTS      0x11C
