# melonDS patches for the RTS harness

The harness (`../host/dshost`) links against melonDS's core library with these three patches applied.
Base commit: upstream melonDS **1e65444** (https://github.com/melonDS-emu/melonDS).

| Patch | What it adds |
|---|---|
| 0001 | The DSpico's SD commands in the homebrew cart, so Pico Loader's DSPICO platform build runs unchanged with the `dspicoDldi.bin` driver. |
| 0002 | SD latency for the DSpico cart (`--dspico-delay`), and a trace of data-permission changes (`MELON_TRACE_DPERM`). |
| 0003 | A memory write watch (`MELON_TRACE_MEM=start[:end]`), ARM7/ARM9 PC history rings (`PC7_RING`/`PC9_RING`), PU region map sets, and hooks the harness uses for its IO profiling and the register shadow logs. |

## Build

```bash
git clone https://github.com/melonDS-emu/melonDS ~/emu/melonDS
```

```bash
cd ~/emu/melonDS && git checkout 1e65444 && git am /path/to/pico-loader/tools/rts-harness/melonds-patches/*.patch
```

```bash
MELONDS=~/emu/melonDS /path/to/pico-loader/tools/rts-harness/host/build_dshost.sh
```

`build_dshost.sh` configures with `-DBUILD_QT_SDL=OFF -DCMAKE_BUILD_TYPE=Release`, builds only the
`core` target (libcore.a plus libteakra.a), then links `host/dshost`. It needs cmake, ninja or make,
and a C++20 g++. The melonDS checkout itself stays local; these patches aren't meant for upstream.
