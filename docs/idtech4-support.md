# idTech4 Support Gap Analysis

This note captures the idTech4 parity check against DarkRadiant and the resulting VibeRadiant changes.

## DarkRadiant baseline reviewed

Primary references:
- DarkRadiant project/about docs and Game Manager docs.
- DarkRadiant game descriptors in `install/games/`:
  - `darkmod.game`
  - `doom3-demo.game`
  - `doom3.game`
  - `prey.game`
  - `quake3.game`
  - `quake4.game`
  - `xreal.game`

Key idTech4 expectations from that baseline:
- Dedicated standalone The Dark Mod game profile.
- Doom 3 map-format editing (`mapdoom3`, `brushDef3`, `patchDef2/3`).
- Doom 3-style material/entity parsing (`.mtr`, `.def`).
- idTech4-usable build menu presets for `dmap` workflows.

## VibeRadiant status before this change

Already present:
- Doom 3 / Quake 4 / Prey map format support in code and game descriptors.
- Doom 3 and Quake 4 shader language support.
- Doom 3 entity-class and light handling.

Missing:
- No dedicated `darkmod.game` profile in VibePack.
- No `doom3-demo.game` profile.
- No `xreal.game` profile.
- Build menu syntax compatibility gap for DarkRadiant-style `${VAR}` variables.
- No DarkRadiant-compatible idTech4 `dmap` build preset scaffold.

## Implemented now

- Added standalone The Dark Mod game descriptor and pack folders to both pack sources:
  - `games/VibePack/games/darkmod.game`
  - `games/NRCPack/games/darkmod.game`
  - `games/VibePack/darkmod.game/*`
  - `games/NRCPack/darkmod.game/*`
- Added missing DarkRadiant game descriptors and pack folders:
  - `games/VibePack/games/doom3-demo.game`
  - `games/NRCPack/games/doom3-demo.game`
  - `games/VibePack/doom3-demo.game/*`
  - `games/NRCPack/doom3-demo.game/*`
  - `games/VibePack/games/xreal.game`
  - `games/NRCPack/games/xreal.game`
  - `games/VibePack/xreal.game/*`
  - `games/NRCPack/xreal.game/*`
- Mirrored the above into runtime install data under `install/gamepacks/`.
- Added default Dark Mod build presets (`dmap` variants) and shader list scaffold.
- Extended build variable expansion to support `${VAR}` in addition to existing `[VAR]`.
- Added DarkRadiant-style build variables:
  - `EXEC_ENGINE`
  - `MAP_NAME`
  - `REF_MAP`
  - `REF_ABSMAP`
- Extended installation-manager alias hints for the newly added packs (`doom3-demo`, `xreal`) in startup/game auto-detection paths.

## Follow-up candidates

- Add validated Doom 3 / Quake 4 / Prey default `dmap` presets (currently still minimal/empty).
- Add optional packaged idTech4 helper assets where licensing and redistribution allow.
