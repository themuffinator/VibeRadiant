# Gamepacks Architecture

This document explains the downloaded `games/VibePack` source tree, the staged `install/gamepacks` runtime tree, and how legacy `NRCPack` data fits in.

## Directory roles

- `games/VibePack/`
  - Canonical staging source for VibeRadiant, downloaded and maintained separately from this repository.
  - Run `download-gamepacks.sh` before a no-download install, or configure Meson with `-Ddownload_gamepacks=allinone` for a fresh checkout.
  - Contains:
    - `games/*.game` descriptors.
    - Per-game payload folders (`*.game/`).

- `games/NRCPack/`
  - Legacy compatibility data inherited from NetRadiant-Custom workflows.
  - Archived/reference-only and no longer used by install tooling.

- `install/gamepacks/`
  - Staged runtime output layout consumed by the editor (`gamepacks/games/*.game` + payload folders).
  - Intended as install/runtime data, not the canonical authoring source.

## Why this was confusing

Historically, `install-gamepacks.sh` installed every `games/*Pack` source in glob order.  
When both `NRCPack` and `VibePack` existed, files were silently overridden by whichever pack ran later.

That created non-deterministic behavior and made it unclear which descriptor version was actually active at runtime.

## Unification policy

`install-gamepacks.sh` now installs from exactly one canonical source:

- `GAMEPACK_SOURCE=VibePack` (required)

Legacy multi-source modes are intentionally blocked to prevent silent per-game overrides and source ambiguity.

`Makefile` now forwards `GAMEPACK_SOURCE` during `install-data`.

`install-gamepacks.sh` also performs deterministic normalization:

- Cleans stale installed `gamepacks/games/*.game` descriptors and payload dirs.
- Installs from one source pack only.
- Converts all legacy payload `*.def` files to sibling `*.fgd`.
- Rewrites descriptor `entityclasstype` tokens from `def` to `fgd`.
- Refines generated/existing `*.fgd` keys with typed metadata heuristics for better editor controls.
- Folds legacy numeric pseudo-option key runs (`0`, `1`, `2`, ...) into proper `choices` controls where safe.
- Removes converted `*.def` files and fails if any legacy `def` remains.

## Game descriptor format guidelines

Descriptor location and payload coupling:

- Descriptor: `games/<PackName>/games/<game>.game`
- Payload dir: `games/<PackName>/<game>.game/`

Recommended descriptor keys for install/game detection quality:

- Core: `type`, `name`, `baseGame`, `enginePath*`, `engine*`
- Launch variants: `mpEngineWin32`, `mpEngineLinux`, `mpEngineMacOS`
- Archive/content: `archiveTypes`, `mapTypes`, `shaders`, `entityClass`, `entities`
- UX/detection: `baseGameName`, `unknownGameName`, `installAliases`
- Mods: `knownMods`, `knownModNames`
- Optional mod defaulting: `defaultGameName`
- Optional install hints: `detectFile1`, `detectFile2`, `detectFiles`
- VFS hygiene: `forbiddenPaths`

Notes:

- Prefer descriptor-driven detection hints (`detectFiles`) over hardcoded game-name special-cases where reliable files are known.
- Prefer explicit `q3map2Type` for idTech3-family packs even when engine-type fallback mapping would work, to keep build/decompile behavior deterministic and self-describing.

Descriptor key style:

- Use camelCase keys in `.game` descriptors (for example `baseGame`, `entityClassType`, `enginePathWin32`).
- Runtime/tooling keeps backward-compatible lookup for legacy snake_case/lowercase aliases, but canonical source descriptors should stay camelCase.

## Refactoring opportunities

- Add a validation script that checks:
  - every `games/*.game` has a matching payload directory.
  - required attributes exist.
  - descriptor names are unique and consistently cased.
- Normalize descriptor key ordering for easier diff/review.
- Keep `NRCPack` read-only as historical reference only.

## Audit command

Use the built-in audit helper:

```bash
python scripts/audit_gamepacks.py
```

This audits `games/VibePack`.
If you still have a local `games/NRCPack` tree, you can compare explicitly with:

```bash
python scripts/audit_gamepacks.py --compare-with games/NRCPack
```

To enforce FGD-only runtime data:

```bash
python scripts/audit_gamepacks.py --source install/gamepacks --no-compare --enforce-no-legacy-def
```
