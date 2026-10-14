# DayZ 0.62 inventory rotation

This repository contains the rotation feature changes prepared for the specific x64 builds below. It packages source patches and the native bridge without redistributing the original game's executables, PBOs, or full script sources.

## Supported builds

| Component | Build | SHA-256 |
| --- | --- | --- |
| Client | DayZ 0.62.144063 | `423212bad091f47f05bd7621f6ba4040d4f3db4c93752c3f19ee0fa7a62bdb9d` |
| Server | DayZServer 0.62.140099 | `531330410a8894b81b10e9e2f3d562b4f8a4c35c925d7504aff5717a17ccccb6` |

The generated direct-launch copies have these hashes:

| Patched output | SHA-256 |
| --- | --- |
| `DayZ_x64_rotation.exe` | `d2e1d5fc10b5cd58ce0867a65c5c762ba084c1c2a01f0257b657d8d668ddb07d` |
| `DayZServerP69_x64_rotation.exe` | `ebd19cc0614736d1f2bd2a3b0f20db11c5de3d1751482ed9207fe6289c15b24e` |

The bridge in `bin/` is build-specific. Do not use it with a different executable build. `manifest.json` records the original and patched executable hashes. The v12 runtime regression check is still pending; the last confirmed manual test covered rotation and movement in cargo, including persistence after reconnect. The remaining hands/equipped transition should be retested on v12.

## Contents

- `patches/client/` and `patches/server/`: unified patches for the scripts changed by the feature. These are patches against extracted vanilla scripts, not replacement game files.
- `native/src/` and `native/include/`: bridge, rotation validation, instrumentation, and controlled launcher source.
- `bin/`: build-specific bridge DLL and launcher.
- `tools/patch_dayz_exe.py`: creates a patched copy of either supported executable, without overwriting the input.
- `diagnostics/`: optional read-only debug consoles.
- `manifest.json`: supported executable hashes, changed files, and package integrity hashes.

## Apply script patches

Back up the relevant script PBOs and extract them using the same 0.62 toolchain used for this build. Verify that each source file matches the corresponding `base_sha256` in `manifest.json`. From the extracted directory that contains the `scripts` folder, apply the client patches to the client scripts and the server patches to the server scripts:

```powershell
Get-ChildItem 'path\to\this-repo\patches\client\*.patch' | ForEach-Object { git apply --directory=. $_.FullName }
```

Repeat with `patches\server\*.patch` from the server scripts directory. Repack/compile the script PBOs with the matching DayZ 0.62 workflow. A patch failing to apply or a source hash mismatch means the base scripts differ; stop and reverse-engineer that build instead of forcing the patch.

## Native bridge

`bin/dayz_rotation_move_v12.dll` and `bin/dayz_rotation_loader.exe` are provided for the exact hashes above. The loader verifies the executable build before resuming its own suspended child process. Use it only with a private, controlled test installation and the matching bridge. Its command format is `dayz_rotation_loader.exe <supported-game.exe> <matching-bridge.dll> [original game arguments...]`. For source rebuilds, open `native/build.ps1` and set `VsDevCmd` to the installed Visual Studio C++ tools path; the source build script produces the bridge and loader.

### Open the patched `.exe` directly

For the supported builds, the import-table patcher adds the bridge DLL and a small entry-point bootstrap that calls `DZ62_InstallDebugBinding` before the original game entry point. It writes a new executable beside the selected output path and never replaces the source. Example:

```powershell
python .\tools\patch_dayz_exe.py 'D:\DayZ_0.62.144063_Client\DayZ_0.62.144063_Client\DayZ_x64.exe' 'D:\DayZ_0.62.144063_Client\DayZ_0.62.144063_Client\DayZ_x64_rotation.exe'
```

Repeat for the server executable. Place the matching `bin/dayz_rotation_move_v12.dll` beside each patched executable, then open the `*_rotation.exe` directly. The original executables remain unchanged. The client build has an Authenticode certificate; modifying its copy invalidates that signature. Steam file verification or a game update may replace the original files, so keep the patched copies separate and only use the exact supported hashes.

## Debug consoles

The two debug consoles are optional diagnostics, not required for rotation. They read logs and observe loaded modules for the supplied executable paths. Set `RuntimeRoot` to a folder containing `client1` and `server` subfolders with each process's `trial_profile` logs, and pass the actual client/server executable paths:

```powershell
.\diagnostics\start_debug_consoles.ps1 -RuntimeRoot 'D:\DayZTest' -ClientExe 'D:\DayZTest\client1\DayZ_x64.exe' -ServerExe 'D:\DayZTest\server\DayZServerP69_x64.exe' -LoaderLogDir 'D:\DayZTest\logs'
```

The consoles are read-only diagnostics; they do not decode arbitrary executable memory or provide a complete binary execution trace.

## Validation record

The source package was built and checked before export. The recorded checks include 128 dispatch fixtures, 24 virtual-thunk fixtures, 2,602 original native rectangle fixtures, 58 rotation-core fixtures, and loader startup/rejection fixtures. These are source/build checks, not a substitute for the pending v12 in-game hands/equipment regression check.
