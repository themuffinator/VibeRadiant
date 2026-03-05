# Gamepacks Architecture

This document explains why the repository has `NRCPack`, `VibePack`, and `install/gamepacks`, and how they are now unified.

## Directory roles

- `games/VibePack/`
  - Canonical editable source pack for VibeRadiant.
  - Contains:
    - `games/*.game` descriptors.
    - Per-game payload folders (`*.game/`).

- `games/NRCPack/`
  - Legacy compatibility source pack inherited from NetRadiant-Custom workflows.
  - Kept for migration/reference, not the primary install source.

- `install/gamepacks/`
  - Staged runtime output layout consumed by the editor (`gamepacks/games/*.game` + payload folders).
  - Intended as install/runtime data, not the canonical authoring source.

## Why this was confusing

Historically, `install-gamepacks.sh` installed every `games/*Pack` source in glob order.  
When both `NRCPack` and `VibePack` existed, files were silently overridden by whichever pack ran later.

That created non-deterministic behavior and made it unclear which descriptor version was actually active at runtime.

## Unification policy

`install-gamepacks.sh` now selects exactly one source by default:

- `GAMEPACK_SOURCE=VibePack` (default, canonical)
- `GAMEPACK_SOURCE=NRCPack` (legacy fallback)
- `GAMEPACK_SOURCE=auto` (prefer VibePack, then NRCPack)

The legacy merge-everything mode (`GAMEPACK_SOURCE=all`) is intentionally
blocked to prevent silent per-game overrides.

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
- Gradually retire `NRCPack` once all required data is verified in `VibePack`.

## Audit command

Use the built-in audit helper:

```bash
python scripts/audit_gamepacks.py
```

This audits `games/VibePack` and reports drift against `games/NRCPack` by default.

To enforce FGD-only runtime data:

```bash
python scripts/audit_gamepacks.py --source install/gamepacks --no-compare --enforce-no-legacy-def
```
