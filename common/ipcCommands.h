#pragma once

#define IPC_COMMAND_ARM9_WRAM_CONFIG                1
#define IPC_COMMAND_ARM9_CLEAR_MAIN_MEM             2
#define IPC_COMMAND_ARM9_APPLY_PATCHES              3
#define IPC_COMMAND_ARM9_APPLY_ARM7_PATCHES         4
#define IPC_COMMAND_ARM9_SET_AP_INFO                5
#define IPC_COMMAND_ARM9_SET_ROM_FILE_INFO          6
#define IPC_COMMAND_ARM9_INITIALIZE_SD_CARD         8
#define IPC_COMMAND_ARM9_INITIALIZE_LOADER_INFO     9
#define IPC_COMMAND_ARM9_GET_SD_FUNCTIONS           0xA
#define IPC_COMMAND_ARM9_DISPLAY_ERROR              0xB
#define IPC_COMMAND_ARM9_SWITCH_TO_DS_MODE          0xD
#define IPC_COMMAND_ARM9_BOOT                       0xF
#define IPC_COMMAND_ARM9_SETUP_HOMEBREW_BOOTSTUB    0x10
#define IPC_COMMAND_ARM9_RESTORE_TWL_MODE           0x11
#define IPC_COMMAND_ARM9_GET_BUILT_IN_DLDI_DRIVER   0x12
#define IPC_COMMAND_ARM9_GET_BUILT_IN_DLDI_HEADER   0x13
#define IPC_COMMAND_ARM9_SHADOW_OVERLAYS            0x14 // the rts register shadow: does it want the overlays?
#define IPC_COMMAND_ARM9_SHADOW_OVERLAY             0x15 // one overlay, read into place: table entry, file size
#define IPC_COMMAND_ARM9_SHADOW_CACHE_KEY           0x16 // the overlay scan cache: the arm9's part of its key
#define IPC_COMMAND_ARM9_SHADOW_CACHE_LOAD          0x17 // word count, the words; answers 1 when taken
#define IPC_COMMAND_ARM9_SHADOW_CACHE_SAVE          0x18 // answers the word count, then the words
