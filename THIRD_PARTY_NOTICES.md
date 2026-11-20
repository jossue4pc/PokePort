# Third-party notices

This repository is a fork of **WiiCompiled** adapted to PokéPark Wii. The full notices of the upstream tree (aurora,
Dolphin data files, pugixml, Crypto++, toml11, YamlDotNet, libco, and the libraries fetched at build time) are in
[`src/THIRD-PARTY-NOTICES.md`](src/THIRD-PARTY-NOTICES.md) and apply unchanged.

## Included here
| Component | Where | License |
|---|---|---|
| WiiCompiled (translator, runtime, build scripts), modified for PokéPark | `src/` | GPL-3.0 (`LICENSE`) |
| aurora, modified (frame interpolation guest rate) | `src/aurora-main/` | MIT (`src/aurora-main/LICENSE`) |
| Dolphin free replacement IPL fonts (`font_western.bin`, `font_japanese.bin`) | `fonts/` | GPL-2.0-or-later (Dolphin) |
| PokéPark Wii decompilation symbol map (function addresses and names) | `src/projects/pokepark/MAP*.txt` | from the SephDB PokéPark decompilation project |
| Launcher, build pipeline, icon (original work) | `launcher/`, `PokeParkLauncher.bat` | GPL-3.0 |

## Downloaded on the player's machine during installation (not redistributed)
| Tool | Source |
|---|---|
| LLVM-MinGW (compiler) | https://github.com/mstorsjo/llvm-mingw |
| CMake | https://github.com/Kitware/CMake |
| Ninja | https://github.com/ninja-build/ninja |
| .NET 8 SDK | https://dot.net (official install script) |
| nodtool (disc image reader) | https://github.com/encounter/nod |
| SDL, Dawn, abseil, fmt, freetype, imgui, libusb, libpng, sqlite, tracy, xxhash, zlib, zstd | pinned in `src/Launcher/Prepare-Dependencies.ps1` |

Each of these is governed by its own license, available from its source.

## Game data
No code, data or assets from *PokéPark Wii: Pikachu's Adventure* are included. The game is read from the player's own
disc image on their machine, and everything derived from it (`game/`, `src/generated/`, `.build/`, `PokePark/`) is
excluded by `.gitignore`.
