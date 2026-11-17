# HeavyMP telemetry source

Complete C++ and assembly sources used to build `heavymp-telemetry.dll` for
the HeavyMP Windows client. This snapshot targets ETS2 **1.62.3.3s**, with
the executable fingerprint recorded in `native/heavymp_game_build.h`.
The native engine interfaces depend on this game build.

## Build on Windows

Install Visual Studio 2022 or newer, or Visual Studio Build Tools, with
**Desktop development with C++**, the MSVC x64 compiler and the Windows SDK.
From a PowerShell window in this directory, run

```powershell
.\build.ps1 -Tests
```

The result is `build/heavymp-telemetry.dll`. All required SDK headers and
MinHook source files are included. This build uses the local Windows tools.
The optional `-Tests` switch runs 17 native checks, including content
verification, notifications, traffic state, collisions and packet handling.

The DLL works with the HeavyMP launcher, its network client and its
multiplayer UI DLL. The launcher publishes process-bound requests before
starting a multiplayer session. The telemetry transport uses loopback UDP.
This source package builds the telemetry DLL. The separate launcher and
multiplayer UI have their own build.

## Source layout

| File or directory | Purpose |
| --- | --- |
| `native/heavymp_telemetry.cpp` | SDK initialization, channel registration and outgoing telemetry |
| `native/heavymp_session.inl` | Launcher request validation and multiplayer UI activation |
| `native/heavymp_content.cpp` | Server addon verification, mounting and missing-content recovery |
| `native/heavymp_native.cpp` | Game engine bridge, vehicles, traffic, world and collisions |
| `native/heavymp_vehicles.cpp` | Incoming state, vehicle presentation and render integration |
| `native/heavymp_*.h`, `*.inl` and `*.inc` | Shared data structures and native implementations |
| `native/heavymp_clock_abi.asm` | Register-preserving clock bridge |
| `native/*_test.cpp` | Isolated native checks |
| `native/sdk/` | SCS Telemetry SDK headers and licence |
| `native/vendor/minhook/` | MinHook sources and licence |
| `native/recovery/` | Fallback vehicle and trailer definitions |

The game supplies position, orientation, speed, vehicle configuration,
wheel movement, lights, horns, controls and job events. The bridge also
handles coordinated native AI traffic and incoming player states.
The content loader verifies the game executable and addon hashes before
mounting a multiplayer session.

The stock ETS2 menu cleanup is included in this snapshot. The embedded
resource archive contains the fallback vehicle definitions.

## Editing

The C++ files use four-space indentation and the supplied `.clang-format`.
Keep include order intact because Windows socket headers must precede
`windows.h`. Generated recovery data and third-party headers retain their
original formatting.

The recovery header is already included for compilation. If you change its
definitions, regenerate it with Python 3

```powershell
python scripts/generate-recovery.py
```

The `native/heavymp_recovery_archive.h` file is generated from the readable
definitions in `native/recovery/`.

## Snapshot validation

On 17 November 2026, this source snapshot compiled with MSVC x64 and all
17 bundled native checks passed. The recovery generator reproduced the
embedded data used for that build. These are compilation and isolated
checks. Driving and two-player behavior depend on the matching launcher,
UI and ETS2 build.

## Licensing

HeavyMP-authored source is supplied under the MIT licence in `LICENSE`.
Third-party notices and the origin of the recovery definitions are described
in `THIRD_PARTY.md`. The game executable and its archives are required at
runtime.
