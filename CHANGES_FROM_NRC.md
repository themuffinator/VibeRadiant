# CHANGES_FROM_NRC.md

This document captures significant, *documented* changes in VibeRadiant relative to the
NetRadiant-Custom (NRC) baseline. It is intentionally scoped to items that have explicit
documentation in this repository. If you need a full, audited diff against NRC, see the
"Open items" section at the end.

Sources used:
- `README.md`
- `RELEASING.md`
- `docs/changelog-custom.txt`
- `docs/Additional_map_editor_features.htm`
- `docs/Additional_map_compiler_features.htm`
- `docs/auto-updater.md`
- `docs/fsr_readme.txt`

## Local changes (this repo)

- Entities: added a "Drop Entities to Floor" command to drop selected point entities onto brush geometry.
- Filters: idTech2 brush filtering now honors surface/content flags for clip, hint/skip, liquids, sky, translucent, and areaportals.
- Linked Duplicates: ported TrenchBroom-style linked group duplication and syncing (create/select/separate, transformation tracking, and linked group update propagation).
- Z-bar view: added the GtkRadiant-style vertical Z ruler alongside the 2D XY view.
- Z-bar view: completed GtkRadiant parity for mouse-driven selection/texture edits, Z-constrained drags, and resize minimum sizing.
- Build fixes: exported linked group module access for plugins, adjusted Qt mouse event handling for Qt5, and cleaned up a build warning in `libs/gtkutil/image.cpp`.
- Build fixes: updated asset-drop worldspawn handling to match the reference-return signature and resolve a build break.
- Build fixes: replaced texture hover shader clear with empty assignment to match CopiedString API and restore builds.
- Build fixes: restored the missing cuboid brush creation helper used by asset drop to resolve linker errors.
- Build fixes: aligned patch insert/remove declarations with their implementations to resolve compilation errors.
- Build fixes: declared Quake3 shader stage helpers before use so the shaders plugin builds cleanly.
- Build fixes: include qtexture_t definition for shader preview, clamp FloatFormat output length, use Qt6 checkbox signals, pull in stringio helpers for tools prefs, and quiet missing DLL probes in MSYS2 packaging.
- Build system: updated default Qt dependency to Qt6 (Core/Gui/Widgets/Svg/Network) and aligned MSYS2 packaging plus Qt6 input event handling.
- Build fixes: restored missing includes for entity/sound browser builds, clarified shader highlighter depth handling, updated preview-lighting scene-change callback wiring, and resolved Qt updater parsing/formatting warnings.
- Build fixes: refreshed Qt6 mouse event handling in browser widgets and entity list hover handling, plus safer `.def` flag parsing to silence warnings.
- Runtime stability: relaxed mapfile lookup in preview graphs so empty paths no longer trigger mapfile lookup crashes in browser views.
- Runtime stability: preview lighting now registers scene-change callbacks only when enabled and disables the preview shader pass if its GLSL shaders fail to load.
- Startup stability: guarded OpenGL widget FBO setup in editor viewports and asset browser previews to prevent early paint crashes before valid sizing.
- Startup stability: defer text label texture allocation until a valid GL context exists and initialize Qt OpenGL functions to prevent early `glGenTextures` crashes.
- Startup stability: defer update-check network manager initialization and bind async callbacks to the update manager lifetime to avoid network-thread crashes on launch.
- Startup stability: guard status-bar brush count updates until the main window exists to prevent early asset browser/model load crashes.
- Runtime stability: flush entity browser preview instances before clearing the reference cache so map loads do not hit `destroying a referenced object` assertions.
- Runtime stability: initialize texture defaults and guard shader preview sizing to prevent crashes when shader textures are missing or unrealized.
- Runtime stability: validate texture image data before uploads and skybox resampling to avoid driver crashes on invalid dimensions.
- Runtime stability: swapped entity browser trigger previews to lightweight textured cubes to prevent post-cache crashes.
- UI stability: clamp restored floating-window geometry to the available screen bounds so resolution changes do not strand windows off-screen.
- Entity creation: creating new brush entities now only re-parents worldspawn brushes, keeping other entities intact.
- Patch editing: insert/remove rows and columns now respects the selected patch vertices.
- Doom3 lights: drag-resize keeps the light origin fixed while updating light radii.
- Image loading: TGA loader skips palette blocks for true-color images to avoid corrupt decoding.
- bobtoolz: merge-patches now wraps edge rotations correctly and rejects width mismatches before combining.
- bobtoolz: initialize portal normals when loading `.prt` data to avoid uninitialized comparisons in `DEntity::LoadFromPrt`.
- Windows packaging: include Qt TLS plugins and their OpenSSL runtime dependencies so in-app update checks can complete.
- Windows packaging: bundle Qt multimedia plugins and point Qt to the local plugin prefix so QMediaPlayer backends load from the install tree.
- Debugging: added `VIBERADIANT_DISABLE_OPENGL`/`RADIANT_DISABLE_OPENGL` to disable OpenGL widgets and substitute placeholder views for crash isolation.
- Texture browser: initialize scroll/size state and guard scroll updates until widgets exist to avoid early null dereferences.
- Texture browser: refreshed layout with a unified filter bar, name search, and surface/content flag filtering.
- Shader rendering: implemented Quake 3 multi-stage shader previews in the texture browser with hover animation, added a live shader editor preview, and added a 3D view animate/static shader toggle.
- Shader rendering: defaulted to legacy single-texture rendering and legacy Quake3 shader parsing when multi-stage rendering is disabled for stability testing.
- Preferences: added game-default brush texture scale (idTech2=1.0, idTech3/4=0.5) and texture thumbnail scale (idTech2=200%, idTech3/4=100%).
- Selection/tools: added a default startup tool mode preference, a primitive-mode toggle/button (Ctrl+Space), and adjustable manipulator size with +/- shortcuts.
- UI: preferences dialog is now resizable with a larger default size, and the status bar shows selection size.
- View defaults: added a coarse grid background color for extreme zoom levels and switched default theme colors to a Maya-style palette.
- Filters: added a Doom3 filter for `func_splinemover`.
- Targeting: added a toggle/preference for thicker target connection lines.
- Shortcuts: warn on duplicate shortcut bindings at startup.
- Camera view: added a real-time lighting preview mode using point lights, surface lights, and sky/worldspawn sun keys to approximate map lighting.
- Camera view: added a menu toggle for lighting preview.
- Input: camera key-move handling now ignores Caps Lock state so arrow/WASD navigation works with Caps Lock enabled.
- Gamepack model types: added `md5mesh` and `iqm` to explicit `modeltypes` lists so MD5/IQM models are available in the editor for non-wildcard game configs.
- Releases/updates: added a `VERSION` file, release packaging workflow with update manifest generation, and an in-app auto-updater that checks GitHub releases and installs updates (Windows zip, Linux AppImage).
- Documentation: added `RELEASING.md` for versioning/packaging/release details and `docs/auto-updater.md` for user-facing update instructions.
- Documentation: added `docs/language-packs.md` to describe language packs and supported languages.
- Clipper tool: added a visual style option (GTK/NRC/VIBE) for clipped volume previewing, including a VIBE mode with a red dashed cut line and striped fill.
- Clipper tool: apply the selected clipper fill/stipple style in orthographic views for VIBE/GTK parity.
- Asset browser: added entity and sound browser tabs alongside textures, with drag-and-drop into 2D/3D views to create entities or assign `noise`/`target_speaker` sounds.
- Asset browser: re-enabled texture/entity/sound tabs and labeled the combined view as the Asset Browser.
- Asset browser: merged the model browser into the asset browser and enabled model drag-drop to create `misc_model` entities with the model key set.
- Asset browser: added hover scale transitions and yellow outlines for asset tiles.
- Asset browser: sound tiles now preview on double-click with a stop icon and single-click stop; drag-and-drop uses transparent tile snapshots and drops into 2D/3D views.
- Asset browser: drag-and-drop no longer grabs the pointer on left-drag; model rotation is available via Alt+drag.
- Asset browser: fixed entity/sound hover hit-testing and drag start, and scale entity/model tiles proportionally using per-browser max extents.
- Asset browser: brush-entity drops create a notex 64^3 cube when no world brush is under the drop point.
- Asset browser: cube entity tiles use a dedicated directional light pass, and tile scaling now accounts for 45/0/45 rotated extents so angled previews fit their frames.
- Asset browser: entity/model tiles now default to 45/0/45 rotation, fixedsize entity tiles render colored cubes, and triggers render as double-sized trigger-textured cubes.
- Asset browser: brush-model entities render as notex-textured cubes in entity tiles.
- Asset browser: renamed the Textures tab to Materials, removed the standalone model browser window, and moved Refresh Models into the model browser toolbar.
- Branding: replaced the splash screen artwork with a new 1536x1024 `splash.png`.
- Branding: updated the main window title to use the "VibeRadiant" name without a space.
- Documentation/branding: refreshed `README.md` with a new `docs/viberadiant-banner.png` social banner and updated project overview/links.
- Documentation: modernized the `TODO` backlog into a structured table with indexed details.
- Documentation: expanded linked duplicates documentation with link ids, transform keys, and synchronization behavior.

## Map editor changes (documented)

From `docs/Additional_map_editor_features.htm`:
- OBJ model support with `.mtl` texture association.
- Ctrl-Alt-E expands selection to whole entities.
- Multi-vertex selection in component mode (Shift to add/remove and box-select).
- Parent selection (select brushes, then the entity, then Edit -> Parent).
- Targeting lines support `target2`, `target3`, `target4`, `killtarget`.
- Rotate/Scale dialogs are non-modal.
- Four-pane view: Ctrl-Tab centers all 2D views to selection.
- Configurable strafe mode behavior.
- Regrouping entities (move brushes in/out of entities without retyping keys).
- Clone selection no longer rewrites `targetname`/`target` (Shift+Space keeps old behavior).
- Linked duplicates for synchronized group copies (create/select/separate).
- Clip tool shows a direction line indicating which half is deleted.
- Automatic game configuration when launched inside known game installs.
- Keyboard shortcuts editor (editable bindings).
- Build menu XML format and variable substitution (Build > Customize).
- Portable mode by creating a `settings/` directory in the install.

From `docs/changelog-custom.txt`:
- `.webp` image format support.
- Rotate dialog non-modal (explicitly called out).
- `func_static` included in world filter (not hidden by entity filter).
- Texture browser text legibility improvements.
- `misc_model` supports `model2` key.
- Light style number display.
- Group entities: force arrow drawing for `func_door` and `func_button`.
- "Select Touching Tall" command (2D-touching select ignoring height).
- Option to disable dots on selected faces outside face mode.

From `README.md` (high-level editor highlights):
- WASD camera binds and 3D view editing workflow improvements.
- UV Tool, autocaulk, texture painting by drag, and texture lock support.
- MeshTex plugin, patch thicken, patch prefab alignment to active projection.
- Expanded selection/transform tools (skew, affine bbox manipulator, custom pivot).
- Extended filters toolbar and viewport navigation tweaks.
- Texture browser search improvements and transparency view option.

## Map compiler / q3map2 changes (documented)

From `docs/Additional_map_compiler_features.htm`:
- Floodlighting via `_floodlight` worldspawn key.
- `-exposure` light compile parameter.
- `q3map_alphagen dotProductScale` and `dotProduct2scale`.
- BSP stage `-minsamplesize`.
- `-convert -format ase -shadersasbitmap` for ASE prefabs.
- `-celshader` support.
- Minimap generator (Nexuiz-style).

From `docs/fsr_readme.txt` (FS-R Q3Map2 modifications):
- `-gridscale` / `-gridambientscale` for grid lighting (R5).
- Light spawnflags `unnormalized` (32) and `distance_falloff` (64) (R4).
- `_deviance` implies `_samples` (R4).
- Deluxemap fixes and `-keeplights` behavior (R3).
- Floodlight behavior adjustments and new `q3map_floodlight` parameters (R3).
- Entity normal smoothing keys `_smoothnormals`/`_sn`/`_smooth` (R2).
- `-deluxemode` and per-game defaults (R2).
- `q3map_deprecateShader` keyword (R1).
- Entity `_patchMeta`, `_patchQuality`, `_patchSubdivide` (R1).
- `MAX_TW_VERTS` increase for complex curves (R1).
- Game-type defaults and negation switches for `-deluxe`, `-subdivisions`, `-nostyles`, `-patchshadows` (R1).
- `-samplesize` global lightmap sample scaling (R1).
- `_ls` short key for `_lightmapscale` (R1).

From `README.md` (Q3Map2 feature summary):
- Shader remap improvements, lightmap brightness/contrast/saturation controls.
- `-nolm`, `-novertex`, `-vertexscale`, `-extlmhacksize`.
- Area light “backsplash” and other light pipeline updates.
- Valve220 mapformat detection and support.
- Assimp-based model loading (40+ formats).
- `-json` BSP export/import, `-mergebsp`, and shader discovery without `shaderlist.txt`.

## Open items / needs verification

This repository does not contain a direct NRC version pin or an authoritative
"baseline diff" list. To make this document exhaustive, the following inputs are needed:
- The exact NRC commit/tag this fork was based on.
- A curated list of VibeRadiant-specific commits after the fork point.

If you can provide the NRC base reference (tag/commit), I can expand this file with a
verified, diff-driven change list.
