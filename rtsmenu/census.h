#pragma once
// The arm9 register-write census, from the menu's side (census.c).

int censusPresent(void);
int censusWanted(void);
/// @brief 0 off, 1 trapping, 2 no region it can use, 3 stopped (the report says why).
int censusState(void);
// Traps off while the menu runs, back on as it resumes the game.
void censusPause(void);
void censusResume(void);
// On to off, or off (or stopped) to on; the game's next vblank does it.
void censusToggle(void);
// The report into out; the menu's two status lines into line1 and line2 (33 bytes).
u32 censusReport(char* out, u32 size, char* line1, char* line2);
// Counts back to 0, the calibration kept.
void censusRestart(void);
