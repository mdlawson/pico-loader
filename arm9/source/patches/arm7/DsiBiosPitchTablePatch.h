#pragma once
#include "../Patch.h"
#include "moduleParams.h"
#include "ndsHeader.h"

/// @brief The arm7's module params (through crt0's literal, else the magic), or null.
const module_params_ntr_t* findArm7ModuleParams(const nds_header_ntr_t* romHeader);

/// @brief Arm7 patch for games that run on the bios of the DSi, whose
///        GetPitchTable reads the wrong table. Every call to the sdk thunk is
///        pointed at a stub that corrects the index before the bios call, so
///        the sound driver gets the pitch values it expects.
///        Optional: silently not applied when the thunk is not found.
class DsiBiosPitchTablePatch : public Patch
{
public:
    bool FindPatchTarget(PatchContext& patchContext) override;
    void ApplyPatch(PatchContext& patchContext) override;

private:
    // 32-bit fields only: patches live in vram, where byte writes are lost on DS and DS Lite.
    u32 _moduleParams = 0;
    u32 _thunkAddress = 0; // runtime thumb address of the thunk, bit 0 set
};
