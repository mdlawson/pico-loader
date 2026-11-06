#pragma once
#include "../Patch.h"
#include "OSResetSystemPatch.h"

/// @brief Arm9 patch that hooks the SDK IRQ dispatcher to react to the in-game
///        exit combo the arm7 patch detects, and reboot into the launcher through
///        the OS_ResetSystem reboot code, which it emits itself when the game has
///        no OS_ResetSystem. Optional: silently not applied when any prerequisite
///        is missing, or when its pieces do not all fit what the patch heap has left.
class ExitComboPatch : public Patch
{
public:
    explicit ExitComboPatch(OSResetSystemPatch* osResetSystemPatch)
        : _osResetSystemPatch(osResetSystemPatch) { }

    bool FindPatchTarget(PatchContext& patchContext) override;
    void ApplyPatch(PatchContext& patchContext) override;

    bool IsInstalled() const { return _installed != 0; }

    /// @brief Takes the hook back out and puts back what the game had where the
    ///        pieces went. For when the arm7 half could not be installed, so the
    ///        exit could never fire. Must run before the game boots.
    void Deactivate();

    /// @brief Puts the register-write census into space (CENSUS_SPACE_SIZE, in
    ///        main memory the arm9 can reach) and has the hook call it on every
    ///        vblank. Only with the hook in, and not for twl sdk games.
    bool InstallCensus(void* space, u32 runAddress, u32 spaceSize);

    /// @brief Has the vblank hook call address (keeping r4-r11), in its census slot.
    void SetVblankCall(u32 address)
    {
        if (_installed && _censusLiteral)
            *_censusLiteral = address;
    }

private:
    // 32-bit fields only: patches live in vram, where byte writes are lost on DS and DS Lite.
    OSResetSystemPatch* _osResetSystemPatch;
    u32* _dispatchTail = nullptr;
    // the twl sdk's dispatcher (clz form): its ldrne lr, =table end, nullptr if not
    u32* _clzDispatch = nullptr;
    u32* _patchedAt = nullptr; // the two words the hook took
    u32 _savedTail[2];
    u32 _hookEntry = 0;
    u32* _savedGameWords = nullptr;
    u32 _savedRegionAddress[8]; // loader_info_t, both reboot code sections, the four pieces, the clz entry
    u32 _savedRegionSize[8];
    u32 _savedRegionCount = 0;
    u32 _installed = 0;
    u32* _censusLiteral = nullptr;
    u32* _censusGateTarget = nullptr; // in the optional gate piece, nullptr without one
    u32 _censusGateEntry = 0;
    u32 _censusGateAddress = 0;
    u32 _censusGateSaved[16]; // what the game had where the gate went
    u32 _blockAddress = 0;
    u32 _twlSdk = 0;
};
