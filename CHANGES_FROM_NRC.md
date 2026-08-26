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

- Repository hygiene follow-up: removed 7.34 MiB of generated Visual Studio workspace databases/state from version control while retaining the existing local ignore, and narrowed the Make probe ignore from `/conftest*` to generated executables so the required tracked `conftest.cpp` source is no longer accidentally classified as ignored.
- Asset-browser startup/search performance follow-up: deferred hidden Surfaces, Entities, idTech4 AI, and Script population until each tab is first activated, including lazy entity-category/preview construction; coalesced scan-heavy Surfaces/AI/Script text filtering behind a 150 ms single-shot delay while keeping filter state, buttons, explicit refreshes, and tab activation immediate.
- Meson configure/provenance refinement: sourced the Meson project version directly from the tracked `VERSION` file (removing a configure-time shell process), let Meson's configured warning level own the baseline warning flags without redundant manual `-W`/`-Wall`, and recorded the recovered full JsonCpp vendor revision.
- Gamepack mod defaults: dedicated mod profiles can declare `defaultGameName`; startup applies that descriptor-provided default to `fs_game` before VFS initialization when no saved per-installation choice overrides it.
- Game setup/install detection: the startup installation manager now detects curated supported mods per selected game/install (`knowngame`/`knownmods`/`defaultGameName`), persists the chosen mod per installation, surfaces it on the installation page, requires an explicit supported-mod choice before continuing when multiple curated mods exist, and applies that choice to `fs_game` before VFS startup so selected installs no longer fall back to stale per-game mod preferences.
- File dialog performance/robustness: shared open/save dialogs now keep the native OS picker fast path instead of forcing Qt's slower non-native fallback when recent folders exist, skip synchronous stale-folder existence probes that could stall on removable/network paths, and disable custom directory-icon lookups in the shared chooser to reduce popup lag while browsing large trees.
- Startup performance pass: removed the splash-to-main-window dead-zone by keeping splash visible through main-frame construction (and through map/theme work when the loading screen is disabled), deferred heavy Sounds/Objects asset-browser tree scans until those tabs are first shown, and stopped eager startup-wide sound precaching so startup does less synchronous VFS/cache work on the critical path.
- idTech2 compiler toolchain integration: vendored the `VibeyMapTools` ericw-tools fork under `tools/quake2/ericw-tools` and wired it into both Meson and legacy Make workflows so builds stage `install/ericw/{qbsp,light,vis}` (with runtime dependencies) for Quake/Quake2 gamepack build-menu compatibility, including updated MSYS2 dependency/bootstrap tasks and build documentation.
- Asset browser redesign: expanded tabs to `Globals`, `Materials`, `Surfaces`, `Entities`, `Sounds`, `Objects` (renamed from Models), plus idTech4 `AI` and `Script`; added a new `Globals` panel with checklist + map stats/entity spawn/worldspawn breakdown views and quick-fix actions for missing checklist items; replaced placeholder Surfaces/AI/Script tabs with live list/filter panels; wired contextual workflow controls (View/Find-Replace/Flush-Reload where applicable), contextual find-replace entrypoints (`FindReplaceEntities`, `FindReplaceSounds`, `FindReplaceObjects`), and standardized filter bars with name filtering, global-scope toggle (globe), clear-filters state, persisted filter state, and `Used (count)` toggles across materials/surfaces/entities/sounds/objects/ai/script; and constrained Objects browsing to `models/mapobjects/*` roots.
- Build regression fixes: restored successful full Meson debug builds after the asset-browser expansion by correcting toolbar command overload usage and required icon/game API includes across asset/entity/model/sound browsers, fixing `CopiedString` filter reset + script-path collection type mismatches in asset panels, and updating bundled ericw-tools `jsoncpp` CMake settings to C++17 so `string_view` Json::Value symbols link consistently.
- Asset browser Globals stability/UX follow-up: fixed a Globals-tab refresh crash by replacing temporary-lambda toolbar callbacks with stable callbacks (also applied to related asset/entity/object reload actions), added editable worldspawn values directly in the Globals worldspawn table (`message`, `author`, `music/sounds` mapped to `music` key writes), and added guarded periodic auto-refresh while the Globals tab is visible (without interrupting active inline edits).
- Camera view orientation aids: camera viewport now draws a world-origin marker in 3D, and adds a new toggle (`View -> Show -> Show Camera Ortho Lines`) plus persisted preference (`ShowCameraOrthoLines`) to overlay X/Y/Z orthographic guide lines through the origin across the full camera scene.
- Compiler standard enforcement: hardened C++20 support across the active build workflows by forcing a C++20-capable compiler flag in Meson target arguments (including legacy build dirs where `cpp_std` may remain `none`) and by passing `-Dcpp_std=c++20` in VS Code MSYS2 configure/reconfigure tasks.
- Preferences/apply UX: preference apply now shows a small centered progress spinner overlay while settings save/window refresh work runs, reducing the "frozen dialog" feel during heavy apply paths.
- Console + asset loading workflow: persisted the console drawer collapsed/expanded state across sessions (`ConsoleCollapsed` preference), added passive asset caching controls in Texture Browser preferences (default on), and switched shader texture realization to deferred/background loading for simple materials so directory listing no longer forces eager texture uploads; visible/used assets request load progressively while unresolved surfaces render with placeholders until real textures are uploaded.
- Build system modernization (native Meson): replaced the legacy Make-wrapper entrypoint with a full native Meson target graph for tools/editor/modules/plugins/static libs (including optional bundled-Assimp build mode), rewired install-time data/runtime steps (`install-gamepacks.sh`, MSYS2 DLL bundling) into Meson install scripts, and updated VS Code MSYS2 tasks/settings to use `meson setup/compile/install` profiles directly.
- Build warning hygiene: scoped `libpicomodel` compile suppression for GCC `-Wstringop-overflow` (`-Wno-stringop-overflow`) to silence persistent `pm_lwo.c` warning noise without reducing warning coverage project-wide.
- Windows runtime launch fix: updated the default VS Code release/debug Meson profiles to install MSYS2 runtime DLLs (`-Dinstall_dlls=true`) so `install/radiant.exe` launches directly from Explorer/PowerShell without immediate `0xC0000135` DLL-load termination.
- Windows startup/runtime fix: default VS Code build-and-install tasks now bootstrap the externally maintained all-in-one VibePack once when it is absent, then install runtime data without redownloading it on normal incremental builds.
- FGD parser stability: fixed a startup-crashing parser gap in `eclass_fgd` by accepting explicit `vector3` key types (in addition to legacy aliases like `real3`/`origin`/`vector`), resolving `runtime error: unknown key type: "vector3"` after game-manager setup on packs that emit modern typed FGD fields.
- VS Code task workflow overhaul: replaced wipe-heavy task definitions with an incremental Meson task set centered on efficient release/debug configure+compile loops (no implicit clean rebuilds in normal tasks), added explicit `Build+Install` variants for syncing changed binaries to `install/`, and split full clean rebuild and one-time gamepack bootstrap into dedicated tasks.
- Meson runtime/install integration hardening: corrected `install_data` layout so `setup/data/tools/*` lands at runtime-expected root paths (for example `install/gl/*` instead of `install/tools/gl/*`), set the Windows `radiant.exe` target to GUI subsystem (`win_subsystem: windows`) to avoid spawning a console window on direct launch, and refreshed VS Code Meson tasks/launch defaults so normal incremental release/debug workflows install runtime data/DLLs while debugger launch no longer forces `externalConsole`.
- Native Meson Windows parity: embedded the existing application/tool icons into `radiant`, `q3map2`, `q2map`, `qdata3`, and `h2data` without relying on Make-generated source-tree resource files.
- Repository hygiene: stopped tracking interpreter-specific Python bytecode caches and ignored generated build probes, source-tree resource files, logs, and build directories.
- Meson cross-platform install reliability: isolated bundled ericw CMake state per Meson build directory, made incremental source checks cheap and `meson install --no-rebuild --tags runtime` truthful, excluded MinGW development import libraries from documented runtime installs, fixed source-tree stamp leakage and Unix `[ExecutableType]` naming, honored `DESTDIR` across scripted installs, exported gamepack options correctly, flattened the staged docs layout, disabled test-only googletest downloads for production tool builds, staged compiler/runtime licence notices, and made gamepack installs validate/normalize in a temporary tree before atomically replacing a working installation.
- Windows runtime staging: made the MSYS2 DLL dependency walk POSIX-shell-safe, private-temp-file based, path-with-spaces safe, recursive across editor/plugin/ericw binaries, and fail-fast when its executable or required tools are missing.
- Gamepack source policy simplification: resolved `NRCPack`/`VibePack`/`install` source ambiguity by enforcing the separately downloaded `games/VibePack` staging source in `install-gamepacks.sh`, narrowing Meson/compile docs to `-Dgamepack_source=VibePack`, and treating `NRCPack` as optional archival/reference data rather than an active install source.
- Game setup/install manager: replaced the standalone startup "Engine Path Configuration" prompt with a modern two-step game setup flow inspired by PakFu, including per-game card list entries (icon + game name + installation-count badge), per-game add/remove/auto-detect installation management, mandatory installation selection before startup can continue, persisted cross-game installation state (`game_installations.json`), and per-install engine executable assignment (including custom absolute paths) that is now used when launching the current map.
- Game setup/install manager follow-up: startup setup dialog now inherits the active saved theme (no hardcoded setup stylesheet), game list only shows titles that currently have configured installations, `Auto-detect` remains available when the list is empty, `Auto-detect` is always clickable even with no selection, and both Add/Auto-detect now show explicit guidance if no game profiles are loaded; `Cancel` now allows exiting startup cleanly instead of forcing users through installation setup.
- Game setup/install manager auto-detect: `Auto-detect` now scans all game profiles in one run (instead of prompting for a single game), adds every newly detected installation across games, refreshes existing installation engine overrides when better candidates are found, and auto-assigns a best-available source-port executable per installation using weighted executable-name heuristics (with fallback to per-game default engine executable names).
- Game setup/install naming: installation auto-naming now infers storefront/distribution labels (`Steam`, `GOG.com`, `Epic Games`, `itch.io`, `Microsoft Store`) from both detector source hints and install-path heuristics, includes those labels in generated names, and upgrades previously auto-generated legacy names to the richer format during auto-detect refreshes.
- Preferences UX: restored theme selection controls in Preferences (Unified Theme + density/accent options) and made all Preferences pages scrollable via per-page scroll containers; dialog sizing is now clamped to the active screen’s available geometry so the window does not exceed screen height/width on smaller displays.
- Theme/system integration + welcome layout: `System (OS)` theme detection now prefers native Windows dark-mode registry keys (`AppsUseLightTheme` / `SystemUsesLightTheme`) when Qt does not report an explicit light/dark scheme, and the startup welcome dialog now renders the splash image with preserved aspect ratio (no stretch), uses a narrower default width, and clamps sizing to available screen geometry.
- GenAI build fix: resolved a compile blocker in `radiant/genai.cpp` by removing a stray token in the timeout global declaration that prevented subsequent GenAI globals (including `g_genAIMaxOutputTokens`) from being parsed.
- Localization: audited and wired additional user-facing UI text through translation lookups (`i18n::tr`) across startup/update journey dialogs, setup/onboarding prompts, command list UI, map info/find dialogs, build-task dialog/tooltips, console context/toggles, UV workflow panel, MRU fallback labels, texture-browser action/filter UI, and entity-inspector typed-value/tooltips/action labels.
- Startup stability: moved setup/onboarding preflight execution ahead of loading-screen display and main-window construction (`Startup_PreMainWindowSetup`) so first-run setup dialogs are not blocked by the loading overlay during startup.
- Startup UX: refreshed splash-stage messaging to include startup metadata (`VibeRadiant`, version, build date, and live status text) and made the startup update phase explicitly report that it checks GitHub Releases while the splash is still active.
- Startup UX: expanded update prompts with richer release/package details, explicit `Install`/`Ignore This Version` actions, a persisted "Automatically install future updates" toggle, and automatic startup install flow when that preference is enabled.
- Startup onboarding: added startup setup detection for missing preferences or missing game-install path, with an onboarding wizard that offers auto-detection of valid game installations (with manual path setup fallback) and captures a persisted editor-style preference (currently `NetRadiant`).
- Startup UX: added a dedicated startup loading screen with progress reporting for late startup phases (main-window preparation, map load, theme apply), and deferred first editor-window show until loading completes.
- Startup UX: added a welcome dialog shown after startup load completion with a Blender-inspired two-section action layout, hero image + logo treatment, top-right version/date badge, quick actions for `Create New Map`, `Open Existing Map`, `Reopen Previous Map`, `Getting Started`, and `Donate`, and a persisted `Show this welcome screen on startup` toggle.
- Startup welcome dialog UX: made the welcome window fixed-size at its computed startup dimensions, explicitly centered it on the active screen, and corrected hero splash layout behavior so the image scales uniformly to the largest size that fits the top panel without changing the window/frame size.
- Startup compatibility: added preference-key compatibility aliases for startup/onboarding/welcome/update settings so older preference files continue to import/export after the startup-journey refactor.
- Startup robustness: initial map loading now validates command-line and "load last map" paths and gracefully falls back to creating a new map (with warning messaging) if those paths are invalid or unreadable.
- Startup diagnostics: added startup-stage timing instrumentation with log summaries for key milestones and a dedicated `-startup-diagnostics` CLI switch for explicit startup diagnostics runs.
- Startup diagnostics: added temporary debug fast-path flag `-startup-debug-skip-to-loading` that skips splash/update/setup-preflight phases and jumps directly into the loading-main-window path for crash isolation.
- Startup diagnostics: added a stricter crash-isolation debug run flag `-startup-debug-mainwindow-only` that fast-forwards to the loading/main-window path while skipping startup update checks, setup/welcome flows, initial map load, and startup theme apply.
- Startup stability: fixed a loading-journey crash by avoiding re-entrant event pumping during startup loading-dialog updates and by constructing `MainFrame` before showing the loading dialog (so main-window construction does not run under an active startup overlay event loop).
- Startup stability: fixed a Quake 4 main-window initialization/runtime assert where editor-only special renderstates (with no backing material shader) could be asked for shader flags during startup map/render setup; those states now safely report no material flags instead of aborting on `m_shader == null`.
- Startup rollout controls: added configurable startup-journey toggles (modern journey + loading screen) and rollout override CLI flags (`-startup-legacy-flow`, `-startup-no-loading-screen`, `-startup-no-welcome`) to permit controlled fallback to non-modern startup behavior.
- UI/theming: replaced the old GUI theme toggles with a tokenized modern theme engine (`System (OS)`, `Light`, `Dark`, `Darker`, `Blender`) that applies a unified Fusion palette+stylesheet across menus/toolbars/docks/tabs/forms/lists/splitters/scrollbars, defaults to OS dark/light detection (with Qt6 live color-scheme tracking), and maps every active GUI theme to its matching viewport theme preset.
- UI/theming: removed the split GUI/viewport sync toggle and separate viewport-preset switching from the theming menu so GUI and viewports always stay in one unified active theme (with a single "Reapply Active Theme" action for fast resync).
- UI/theming: viewport colour preset loading now parses the repository's existing themed JSON dialect (comments + trailing commas), ensuring unified GUI/viewport preset application works reliably across bundled themes.
- UI/theming: apply the initial GUI theme on the first event-loop tick (after main-window setup/show) and avoid first-time style/palette/QSS application from inside menu construction, fixing a post-game-selection main-window access violation (`0xC0000005`) introduced during the theming refactor.
- UI/theming: replaced static light/dark icon duplication with adaptive runtime icon themes generated from the single source SVG set; neutral SVG tones are recoloured from active theme tokens and emitted into hashed icon-theme directories, so icons follow the active theme without maintaining per-theme assets.
- UI/theming: added `Interface Density` presets (`Compact`, `Standard`, `Comfortable`) with global preference persistence (`GUIThemeDensity`), scaling typography and spacing in menus/toolbars/docks/lists/forms for more modern and consistent readability.
- UI/theming: added theme-aware accent customization (`Accent Color...` / `Use Theme Default Accent`) with persisted preferences (`GUIAccentOverrideEnabled`, `GUIAccentColor`) and WCAG contrast guardrails; the accent now drives both Qt selection/highlight UI and viewport interaction colours (selection/active grid/clipper split) for unified visual feedback.
- UI/theming: moved active-theme controls out of the menu bar and into Preferences (Interface), including unified theme selection, density selection, and persisted accent override controls, so theme management is centralized in one settings workflow.
- UI/theming: moved viewport-color customization into Preferences (Display -> Viewport Colors), including OpenGL font selection and all advanced per-color overrides, so viewport theming is configured in one preferences workflow instead of menu-only dialogs.
- UI/menus: split modern GUI theming from legacy viewport color overrides by renaming the Misc submenu to `Viewport Colors` (OpenGL font + advanced per-color overrides) and removing direct theme switching from menus.
- UI/menus: removed the standalone `Appearance` top-level menu now that theme/viewport customization lives in Preferences, reducing duplicated configuration entry points.
- Console UX: replaced the plain console pane with a modern collapsible console drawer featuring a standalone arrow toggle (no header bar), with regular layouts now using an overlay pull-up/drop-down console above orthographic views so collapsing/expanding no longer resizes editor panes.
- Console UX: status bar notification/warning/error badges are now interactive severity chips that hide at zero, show detailed tooltips, and open per-category message history dialogs while clearing unread counts for the clicked category.
- Console UX: fixed a regular-layout regression where the loose collapse/expand arrow could disappear by hosting the overlay drawer on a dedicated non-splitter container above the orthographic panel.
- UI/menus: modernized top-level menu structure by introducing dedicated `Window` and `Map` menus; panel toggles/workspace presets/focus/fullscreen now live under `Window`, and map utilities moved under `Map`.
- UI/menus: normalized action naming in the updated menu sections (`Toggle ...`, `Apply ...`, title-case consistency such as `Next Leak Spot`) to improve scanability and command discoverability.
- GenAI framework/UI: added an OpenAI integration framework (`radiant/genai.*`) with centralized request-preparation helpers and persisted OpenAI settings (base URL/path, API key, model, org/project headers, timeout, token cap, response-storage policy), plus a dedicated top-level `GenAI` menu (`Enable`, preferences/status/docs/API-key actions) and a new standalone `GenAI` preferences category.
- GenAI Prompt-to-Blockout: implemented a production-ready `Prompt-to-Blockout...` workflow in the GenAI menu that opens a generator dialog, supports OpenAI Responses API planning (with deterministic fallback when unavailable), and emits undo-safe blockout room/corridor brush geometry near the camera; added persisted defaults for prompt/shader/layout constraints and a one-click run action in GenAI preferences.
- GenAI API endpoint fix: corrected OpenAI endpoint construction to preserve base-path prefixes (for example `/v1`) when joining `Base URL` and `Responses API Path`, preventing accidental calls to `https://api.openai.com/responses` and restoring valid Responses API requests to `https://api.openai.com/v1/responses`.
- GenAI Prompt-to-Blockout overhaul: upgraded generation from simple solid room/corridor boxes to semantic layout synthesis with traversal-aware links (`door`, `stairs`, `ramp`, `func_plat`, `jumppad`, `teleporter`), hollow room/corridor shell construction with doorway openings, choke-point narrowing, platform/stair/ramp/liquid room features, defendable-item heuristics, and automatic spawn/item placement; the OpenAI planning prompt/schema now accepts typed connection semantics and choke metadata.
- Build fixes: resolved a GenAI compile break by restoring the missing `ToggleItem` declaration include (`gtkutil/widget.h`) required by the GenAI menu toggle registration path.
- Command workflow: added `CommandPalette` as the primary command-list entry (`Ctrl+Shift+P`) while keeping `Shortcuts` as a compatibility alias, surfaced command-palette access in both Edit and Window menus, and registered `Set2DBackgroundImage` as a first-class command for command-palette access.
- UX/layout: added one-click `Workspace Presets` (`Modeling`, `Texturing`, `Entity`, `Lighting`) in the View menu plus commands, with deterministic panel presentation (group pages, surface inspector, entity list) and lighting workspace camera-preview enablement.
- UX/layout: workspace preset switching now includes contextual focus cues and status messaging so the active working context is clearer while navigating between presets.
- UX/layout: added `Focus Mode` (View menu + command) to temporarily declutter the interface by hiding panels/toolbars/status bar while preserving/restoring each element's prior visibility state.
- Surface/content flags UI: unified flag-label resolution across Surface Inspector and Texture Browser so both use the same game-aware names, honoring per-game `.game` overrides (`surfN`/`contN`) and falling back to engine-specific defaults (idTech2/idTech3) with explicit bitmask labels when no named mapping exists.
- Startup stability: fixed a main-window startup crash caused by null default surface/content flag label entries dereferenced during shared flag-name cache rebuild; cache construction now null-checks optional labels before `string_empty` checks.
- Startup diagnostics: if `RADIANT_MAJOR` / `RADIANT_MINOR` marker files are missing in the install directory, startup now logs a warning and skips the strict setup-version guard (instead of aborting with a version-mismatch dialog), which is safer for source-tree/dev runs.
- Startup diagnostics: gamepack scanning now reports the exact missing directory (`gamepacks/games`) and the runtime error clearly points to installing runtime data/gamepacks (for example `make install-data`), replacing the previous ambiguous failure.
- Startup stability: selection-system scene hooks and undo-tracker attachment are deferred to the first event-loop tick, avoiding early module-init crashes during startup on some Windows runtime states.
- Windows packaging: `install-dlls-msys2-mingw.sh` now resolves dependency closure from built `install/modules/*.dll` and `install/plugins/*.dll` too (not just executables/Qt plugins), so runtime DLLs needed by modules like `assmodel.dll` are bundled correctly.
- Viewport themes: updated the Blender Dark viewport preset to Blender 4 default style values (including axis colours and viewport tone mapping) for closer Blender parity.
- Build fixes: fixed viewport preset menu enumeration to pass `const char*` paths into `Directory_forEach`, resolving a compile break introduced during the theming refactor.
- Entities: added a "Drop Entities to Floor" command to drop selected point entities onto brush geometry.
- Filters: idTech2 brush filtering now honors surface/content flags for clip, hint/skip, liquids, sky, translucent, and areaportals.
- Filters: idTech2 surface/content filter masks are now derived per-game from `.game` flag labels (with Q2 canonical-bit fallback), ensuring clip/liquid/hint-skip/areaportal/translucent/sky filtering stays correct across Quake II-derived gamepacks.
- Game detection: replaced stale Q2World probing with active idTech2 install detection (Quake II, Quake II: Rerelease, Heretic II, Kingpin) for automatic game configuration.
- Game install handling: engine-path configuration now supports multiple detected installs per game (for example Steam/GOG/local), auto-selects when exactly one valid install is found, and exposes a selectable "Detected Installations" list in path settings when multiple installs are available.
- Game detection/install management: replaced hardcoded per-game install probing with a dynamic game-description-driven system that works across all installed gamepacks, including alias-aware discovery, archive/content validation, Steam library scanning, GOG roots, Windows uninstall/registry probing, and environment/system root scanning.
- Startup `-gamedetect`: replaced the fixed game list with dynamic detection over all available `.game` descriptors in `gamepacks/games`, using gamepack attributes (`basegame`, engine executable, archive types, optional `detect_files`/aliases) plus legacy compatibility hints for classic packs.
- Game detection refinement (Quake Live vs Quake 3): tightened idTech3 install classification by adding Quake Live-specific `pak00.pk3` signatures and alias fallback hints to the legacy detector, and hardened required-file validation to still require a valid basegame directory so broad Quake 3 installs are no longer misclassified as Quake Live.
- Mod handling: project settings now support multiple curated mods per gamepack (`knownmods`/`knownmodnames` when present), keep legacy `knowngame` compatibility, and auto-discover installed mod directories from engine path content so `fs_game` selection is broader and more intuitive across games.
- BSP importing: made BSP decompile import version-aware for idTech2/idTech3 by sniffing BSP ident/version, trying matching `q3map2 -game` profiles (IBSP46/47, RBSP1, FBSP1), and adding an idTech2-specific bsputils-style path (prefer `bsputil --decompile`, fallback to Valve220-first `mbspc -bsp2map220`), including robust `bsputil` output-map detection (`.bsp.decompile.map` and extensionless variants) plus explicit diagnostics when conversion output is missing.
- Build/run: idTech2 engine launch arguments now key off `brushtypes="quake2"` instead of hardcoded game-type names, so all quake2-brush gamepacks follow the correct launch path.
- idTech2 UX parity: Curve/Patch menu and patch toolbar tools are now hidden for quake2-brush gamepacks (`brushtypes="quake2"`), while still honoring existing `no_patch` gamepack flags.
- Renderer: enabled idTech2 env skybox rendering from worldspawn `sky` for quake2-brush games, with shader-state refresh when `sky` is edited and skybox loading fallback for both `_ft` and `ft` cubemap suffix styles.
- Linked Duplicates: ported TrenchBroom-style linked group duplication and syncing (create/select/separate, transformation tracking, and linked group update propagation).
- Z-bar view: added the GtkRadiant-style vertical Z ruler alongside the 2D XY view.
- Z-bar view: completed GtkRadiant parity for mouse-driven selection/texture edits, Z-constrained drags, and resize minimum sizing.
- Build fixes: exported linked group module access for plugins, adjusted Qt mouse event handling for Qt5, and cleaned up a build warning in `libs/gtkutil/image.cpp`.
- Build fixes: updated asset-drop worldspawn handling to match the reference-return signature and resolve a build break.
- Build fixes: replaced texture hover shader clear with empty assignment to match CopiedString API and restore builds.
- Build fixes: restored the missing cuboid brush creation helper used by asset drop to resolve linker errors.
- Build fixes: restored Qt6/MSYS2 build compatibility by fixing typed-value `StringStream` conversions in entity inspector, removing duplicate in-class selection snapshot declarations, and storing texture-tag role data as `QByteArray` (plus correcting `StringOutputStream` construction) in the texture browser.
- Asset drop: model drag-and-drop now places created `misc_model` entities flush on top of the hit surface (accounts for entity origin and model bounds).
- Build fixes: aligned patch insert/remove declarations with their implementations to resolve compilation errors.
- Build fixes: declared Quake3 shader stage helpers before use so the shaders plugin builds cleanly.
- Build fixes: include qtexture_t definition for shader preview, clamp FloatFormat output length, use Qt6 checkbox signals, pull in stringio helpers for tools prefs, and quiet missing DLL probes in MSYS2 packaging.
- Build system: updated default Qt dependency to Qt6 (Core/Gui/Widgets/Svg/Network) and aligned MSYS2 packaging plus Qt6 input event handling.
- Build system: fixed a MinGW compile break in engine-path install labeling (`StringStream` -> `CopiedString`) and split warning suppression policy by C/C++ compiler paths so `make MAKEFILE_CONF=msys2-Makefile.conf` completes with zero warnings in this tree.
- Build fixes: restored missing includes for entity/sound browser builds, clarified shader highlighter depth handling, updated preview-lighting scene-change callback wiring, and resolved Qt updater parsing/formatting warnings.
- Build fixes: refreshed Qt6 mouse event handling in browser widgets and entity list hover handling, plus safer `.def` flag parsing to silence warnings.
- CI/nightly: ensured the nightly workflow fetches git tags during checkout so version/tag discovery works reliably in scheduled runs.
- Build fixes: resolved MinGW/GCC build breaks in the new preview lighting code caused by float/double `std::max` template deduction, and aligned CamWnd member initializer order to silence `-Wreorder`.
- Runtime stability: relaxed mapfile lookup in preview graphs so empty paths no longer trigger mapfile lookup crashes in browser views.
- Runtime stability: preview lighting now registers scene-change callbacks only when enabled and disables the preview shader pass if its GLSL shaders fail to load.
- Preview lighting: fixed shader-file tokenising so q3map2 directives (e.g. `q3map_surfacelight`, `q3map_skyLight`, `q3map_sunExt`) parse across newlines without console spam (and so skies/surfacelights contribute to the preview).
- Preview lighting: preserve lighting/shadow caches when hiding geometry or leaving Lighting mode, and keep change tagging active while the preview is off so re-enabling only rebuilds affected geometry.
- Preview lighting performance: defer scene rescans until Lighting mode is active, cache map bounds from rescans for directional shadow rays, and switch same-size lightmap texture refreshes from `glTexImage2D` to `glTexSubImage2D`.
- Preview lighting: keep the camera-view void background using the standard camera background colour (grey) while in Lighting mode.
- Preview lighting: reset GL state for the overlay pass (unbind program, disable stale client arrays, use `GL_REPLACE`) to avoid overly dark/tinted Lighting-mode output.
- Preview lighting: treat shaders with q3map2 sky lighting directives as sky portals even if `QER_SKY` isn't set, and keep them out of the shadow BVH so directional lighting isn't blocked.
- Preview lighting: fixed patch invalidation hashing to include tessellated vertex/normal/index data, and unified hidden/filtered participation checks so filtered or hidden brush/patch geometry no longer contributes to surfacelight/sky extraction or shadow BVH occlusion.
- Preview lighting: replaced the fast stencil-shadow-volume experiment with a `Fast Interaction (DarkRadiant-style)` model that renders direct lighting overlays without stencil volumes, and kept model selection in camera preferences.
- Preview lighting: fixed fast-interaction receiver filtering to match baked-overlay eligibility, so all eligible brush/patch surfaces participate consistently across both models.
- Startup stability: guarded OpenGL widget FBO setup in editor viewports and asset browser previews to prevent early paint crashes before valid sizing.
- Startup stability: defer text label texture allocation until a valid GL context exists and initialize Qt OpenGL functions to prevent early `glGenTextures` crashes.
- Startup stability: defer update-check network manager initialization and bind async callbacks to the update manager lifetime to avoid network-thread crashes on launch.
- Startup stability: guard status-bar brush count updates until the main window exists to prevent early asset browser/model load crashes.
- Startup stability: fixed an early main-window/asset-browser startup segfault by making entity/model/sound browser cell-layout font metrics null-safe before shared OpenGL font initialization, with fallback metrics and guarded pre-init text draws.
- Startup stability: fixed a follow-up startup crash caused by uninitialized/stale global OpenGL font pointers by explicitly initializing `OpenGLBinding` font/context fields, resetting font pointer on shared-context teardown, hardening `OpenGLBinding::drawString` against missing fonts, and requiring a valid shared GL context before asset-browser font metric dereferences.
- Crash diagnostics (Windows/MSYS2): added process-level crash reporting that installs unhandled exception/signal/terminate handlers, writes timestamped crash logs, and emits Windows minidumps (`.dmp`) to a crash directory (default `settings/.../crashes`, override via `VIBERADIANT_CRASH_DIR`; disable via `VIBERADIANT_DISABLE_CRASH_REPORTING`).
- Startup robustness: refactored main-window startup lifecycle to use a centralized runtime guard for splash/loading/pid/log/module cleanup across all exit paths, parse startup CLI flags in a single pass, make pid removal idempotent and error-aware, and harden `MainFrame` teardown/layout-style recovery for partial or corrupted startup state.
- Startup stability: hardened main-window creation and viewport/browser text initialization by adding explicit post-construction null-window checks, default-safe `MainFrame` style initialization, null-safe status-label redraws during early startup, and a shared safe OpenGL font-metrics/draw path used by asset/entity/model/sound/texture browsers plus XY/camera overlays so startup cannot dereference stale global font pointers.
- Runtime stability: flush entity browser preview instances before clearing the reference cache so map loads do not hit `destroying a referenced object` assertions.
- Runtime stability: initialize texture defaults and guard shader preview sizing to prevent crashes when shader textures are missing or unrealized.
- Runtime stability: validate texture image data before uploads and skybox resampling to avoid driver crashes on invalid dimensions.
- Runtime stability: swapped entity browser trigger previews to lightweight textured cubes to prevent post-cache crashes.
- Runtime stability: make Radiant shutdown idempotent to avoid double module release crashes on close.
- Runtime stability: make model resource realise/unrealise idempotent to prevent TraversableNode insert/erase assertions.
- Runtime stability: skip inserting null/failed model loads into traversables to prevent duplicate node set assertions.
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
- Texture browser: added Smart Tags support via `smarttags.txt` rule files (game + user), including combined manual/smart tag search, smart-tag checkbox indicators per selected shader, and a reload action in the Tags menu.
- Shader rendering: implemented Quake 3 multi-stage shader previews in the texture browser with hover animation, added a live shader editor preview, and added a 3D view animate/static shader toggle.
- Shader rendering: re-enabled Quake 3 multi-stage shader rendering by default while keeping the legacy single-texture fallback when stages are disabled.
- Shader rendering: guarded stage evaluation to fall back to the base shader texture when stage textures fail to realize, preventing material browser crashes on missing stage assets.
- Shader rendering: added safe texture fallback handling in texture browser and shader preview stage draws to avoid GL state crashes when stage textures are missing.
- Shader rendering: added safe fallback vertex-color arrays for Quake 3 stages to prevent GL crashes and matched `identityLighting` stage color scaling to Quake3e.
- Shader rendering: temporarily disabled Quake 3 shader stage rendering by default to avoid material browser crashes while the root cause is investigated.
- Gamepacks (VibePack): converted legacy `entities.def` files to generated `.fgd` definitions and switched each affected `.game` to `entityclasstype="fgd"` (preserving `xml` where present).
- Gamepack architecture: refactored installation flow to use a single canonical gamepack source (`GAMEPACK_SOURCE=VibePack` by default) instead of implicitly merging all `games/*Pack` directories, with explicit source selection modes (`NRCPack`, `auto`) and documented source/runtime layout (`docs/gamepacks-architecture.md`), plus a gamepack audit utility (`scripts/audit_gamepacks.py`).
- Gamepack descriptor normalization: unified divergent `.game` metadata for Quake/Quake II/Doom 3-family packs (`Q3`, `doom3`, `heretic2`, `kingpin`, `nexuiz`, `q1`, `q2`, `q2re`, `quakelive`, `warsow`) by keeping Vibe display fields while adding install/mod discovery fields (`install_aliases`, `knownmods`, `knownmodnames`, and `q2re` `detect_files`), then mirrored the canonical descriptors so `games/VibePack/games`, `games/NRCPack/games`, and `install/gamepacks/games` are in sync.
- Gamepack descriptor metadata completion: filled remaining `.game` descriptor metadata gaps across the canonical pack by ensuring all descriptors provide non-empty `install_aliases`, `knownmods`, and `knownmodnames` (using safe basegame/basegamename fallbacks where curated mod lists are unknown), then mirrored descriptors so all pack roots pass strict audit with zero warnings.
- Gamepack descriptor schema normalization: migrated `.game` descriptor attribute keys to camelCase across all packs (for example `basegame` -> `baseGame`, `entityclasstype` -> `entityClassType`, `enginepath_win32` -> `enginePathWin32`) and added compatibility key lookup in runtime descriptor parsing plus audit/tooling normalization so existing legacy key lookups continue to work while the stored descriptors remain consistently camelCase.
- Gamepack detection metadata expansion: added explicit `q3map2Type` values for additional idTech3-derived packs where mapping was implicit (`darkplaces`, `neverball`, `nexuiz`, `oa`, `osirion`, `q3rally`, `trem`, `zeq2lite`) and promoted existing hardcoded install-detection required files into descriptor metadata via `detectFiles` for `nexuiz` and `warsow` so install detection depends less on code-side legacy hints.
- Gamepack descriptor key completion (runtime/install quality): populated missing multiplayer engine keys (`mpEngineWin32`, `mpEngineLinux`, `mpEngineMacOS`) from per-platform engine defaults, standardized `forbiddenPaths` defaults for safer VFS scanning across all descriptors, and expanded curated `detectFiles` signatures for canonical packs (`q1`, `q2`, `q3`, `oa`, `wolf`, `et`, `jk2`, `ja`, `doom3`, `doom3-demo`, `darkplaces`, `heretic2`, `kingpin`, `aa_*`) while preserving existing explicit hints.
- Gamepack install hardening + entity-definition modernization: installation now enforces a single source pack (blocks legacy merge-all mode), cleans stale installed gamepack entries before copy, and runs a full legacy-definition normalization pass that converts all payload `*.def` files to `*.fgd`, upgrades descriptor `entityclasstype` from `def` to `fgd`, removes converted `*.def` files, and fails install if legacy `def` usage remains; also improved FGD support by mapping additional key types (`boolean`, `sound`, `texture`, `target`, `vector3`, etc.) to typed entity-inspector controls and expanded the conversion/refinement tool to emit richer typed FGD keys, canonicalize legacy type aliases, and fold safe numeric pseudo-option runs into proper `choices` controls across all entity definitions.
- Gamepacks: renamed NRCPack references to VibePack in tooling and downloads.
- Preferences: added game-default brush texture scale (idTech2=1.0, idTech3/4=0.5) and texture thumbnail scale (idTech2=200%, idTech3/4=100%).
- Preferences UI: refactored the main preferences dialog into a cleaner split navigation/content layout with a top "smart search" bar that updates results on every keystroke, lists all matching settings as direct navigation targets, and restores the normal tree view when cleared via the built-in clear (`x`) action.
- Build/launch workflow: introduced a task-oriented build UX with VSCode-style commands and hotkeys (`Ctrl+Shift+B` build task, `F5` launch task, `Ctrl+F5` build+launch task), modernized Build menu/task dialog wording, and added build preferences source-port presets (game-aware common ports with one-click apply to engine executable/arguments while preserving manual custom mode).
- Build/status + drag/drop: added a live build/launch status badge in the status bar (building/launching/success/failure states) and corrected asset/entity drag-drop placement by aligning dropped entities/models using full bounds-based XY+Z offsets at the drop point, so placement matches the intended surface/location more reliably.
- Build preferences crash fix: resolved startup/load crash in the Build Preferences page caused by `Source Port Preset` combo callbacks capturing stack lambdas (dangling callback environment). Preset import/export now uses stable free-function callbacks, preventing invalid callback execution during dialog data import/export.
- Selection/tools: added a default startup tool mode preference, a primitive-mode toggle/button (Ctrl+Space), and adjustable manipulator size with +/- shortcuts.
- UI: preferences dialog is now resizable with a larger default size, and the status bar shows selection size.
- UI: added an Issue Browser panel with map diagnostics (missing classname, duplicate `targetname`, broken `target` references), selection helpers, and one-click fixes.
- UV workflow: added a UV View panel with direct controls for UV tool mode switching, fit/projection actions, UV shift/scale nudges, and live selection-mode/status readout.
- Entity Inspector: key/value editing now includes typed smart controls for boolean/list/color/model/sound keys, plus spawnflags guidance in-line with the key/value editor.
- Selection undo/redo: selection-state changes are now tracked in the undo system (including command-driven selection actions and mouse selection workflows), with redo parity.
- Command workflow: added macro recording/playback commands (`MacroRecordStart`, `MacroRecordStop`, `MacroPlay`, `MacroClear`) with an Edit > Macros menu and shortcut support.
- View defaults: added a coarse grid background color for extreme zoom levels and switched default theme colors to a Maya-style palette.
- Filters: added a Doom3 filter for `func_splinemover`.
- Targeting: added a toggle/preference for thicker target connection lines.
- Shortcuts: warn on duplicate shortcut bindings at startup.
- Camera view: added a real-time lighting preview mode using point lights, surface lights, and sky/worldspawn sun keys to approximate map lighting.
- Camera view: added a menu toggle for lighting preview.
- Camera view: overhauled the lighting preview to render a fullbright base pass with an on-the-fly, shadowed lightmap overlay (time-sliced rebuilds and per-brush dirty updates) for a closer baked-lighting look.
- Camera view: lighting preview now includes `q3map_surfacelight` emitters even when the shader is `nodraw`.
- Camera view: lighting preview now supports q3map2 sky directives `q3map_skyLight` (sampled skylight) and `q3map_sunExt` (deviance/samples) for closer shader-based outdoor lighting.
- Documentation: added `docs/lighting-preview.md` describing lighting preview features, supported q3map2 directives, and implementation details.
- Input: camera key-move handling now ignores Caps Lock state so arrow/WASD navigation works with Caps Lock enabled.
- Gamepack model types: added `md5mesh` and `iqm` to explicit `modeltypes` lists so MD5/IQM models are available in the editor for non-wildcard game configs.
- idTech4 support: added The Dark Mod standalone gamepack profiles (`darkmod.game`) for both VibePack and NRCPack, including Dark Mod `dmap` build presets and shader list scaffolding.
- DarkRadiant gamepack parity: ported missing DarkRadiant idTech4 descriptors/packs (`doom3-demo.game`, `xreal.game`) into both `games/VibePack` and `games/NRCPack`, mirrored them into `install/gamepacks`, and backfilled `darkmod.game` into VibePack so both gamepack sources now cover the DarkRadiant idTech4 set.
- Game/install manager integration: extended startup auto-detect/install alias hints for the new DarkRadiant-derived packs (`doom3-demo`, `xreal`) in both detection code paths (`radiant/environment.cpp` and `radiant/mainframe.cpp`) so game selection and installation onboarding recognize them consistently.
- Build menu compatibility: extended variable expansion to support `${VAR}` syntax (alongside `[VAR]`) and added DarkRadiant-style build variables (`EXEC_ENGINE`, `MAP_NAME`, `REF_MAP`, `REF_ABSMAP`) for idTech4 build-menu parity.
- Releases/updates: added a `VERSION` file, release packaging workflow with update manifest generation, and an in-app auto-updater that checks GitHub releases and installs updates (Windows zip, Linux AppImage).
- Releases/updates: ported the PakFu updater/startup release flow by resolving `update.json` via GitHub Releases API (stable + prerelease channels), running automatic update checks during the splash startup path, and adding a scheduled nightly prerelease pipeline (`.github/workflows/nightly.yml`) with scripted nightly versioning/release notes.
- Releases/updates: switched CI/release workflows to Qt6 dependencies, added macOS arm64 build artifacts to build/release/nightly pipelines, and extended the updater/manifest flow to support macOS tar.gz packages (including installation/relaunch on macOS).
- Documentation: added `RELEASING.md` for versioning/packaging/release details and `docs/auto-updater.md` for user-facing update instructions.
- Documentation: added `docs/language-packs.md` to describe language packs and supported languages.
- Clipper tool: added a visual style option (GTK/NRC/VIBE) for clipped volume previewing, including a VIBE mode with a red dashed cut line and striped fill.
- Clipper tool: apply the selected clipper fill/stipple style in orthographic views for VIBE/GTK parity.
- Debugging: added a clipper ortho debug overlay (View > Show) that exercises line/polygon stipple and point rendering while logging GL state for diagnosing missing clipper indicators.
- Asset browser: added entity and sound browser tabs alongside textures, with drag-and-drop into 2D/3D views to create entities or assign `noise`/`target_speaker` sounds.
- Asset browser: re-enabled texture/entity/sound tabs and labeled the combined view as the Asset Browser.
- Asset browser: merged the model browser into the asset browser and enabled model drag-drop to create `misc_model` entities with the model key set.
- Asset browser: added hover scale transitions and yellow outlines for asset tiles.
- Asset browser: sound tiles now preview on double-click with a stop icon and single-click stop; drag-and-drop uses transparent tile snapshots and drops into 2D/3D views.
- Asset browser: drag-and-drop no longer grabs the pointer on left-drag; model rotation is available via Alt+drag.
- Asset browser: fixed entity/sound hover hit-testing and drag start, and scale entity/model tiles proportionally using per-browser max extents.
- Asset browser: hovered entity/model tiles now ease into a continuous Z-axis rotation and snap back to the default 45/0/45 orientation when not hovered.
- Asset browser: brush-entity drops create a notex 64^3 cube when no world brush is under the drop point.
- Asset browser: brush-entity drops now place the created cube flush on top of the hit surface (instead of centered/sinking into the floor).
- Asset browser: cube entity tiles use a dedicated directional light pass, and tile scaling now accounts for 45/0/45 rotated extents so angled previews fit their frames.
- Asset browser: entity/model tiles now default to 45/0/45 rotation, fixedsize entity tiles render colored cubes, and triggers render as double-sized trigger-textured cubes.
- Asset browser: brush-model entities render as notex-textured cubes in entity tiles.
- Asset browser: model tiles now default to `models/mapobjects/`, fixedsize entity tiles draw a solid cone direction arrow with a slimmer shaft, and the default preview angles are configurable.
- Asset browser: fixedsize entity arrows now use an inverted/closed cone with a connecting cylinder shaft for clearer direction cues.
- Entity rendering: `model2` now renders as a secondary model alongside the primary model for misc_model and eclass model entities (e.g., Q3 powerups/health bubbles).
- Sound browser: root moved to `sound/world/`, added precache of world sounds after textures/models, and added a reload sounds button.
- File dialogs: remember recent folders and expose them in open/save dialogs for faster path reuse.
- Brush tools: added the "Silly Sausage Tool" to build a tapered, bowed sausage of brushes from a selected brush and group it under `func_group`.
- Brush tools: fixed the Silly Sausage Tool to accept component-selected faces and handle tapered end tips without degenerating.
- Brush tools: Silly Sausage Tool now keeps divisions on the body only and adds separate hemispherical cap brushes at each end.
- Asset browser: renamed the Textures tab to Materials, removed the standalone model browser window, and moved Refresh Models into the model browser toolbar.
- Asset browser follow-up: Globals checklist now auto-updates in place (without rebuilding the widget), worldspawn now lists all set keys with inline editing, and music/sounds checks are split with engine-aware expectations (`sounds` for classic idTech2, `music` for idTech2 rerelease/idTech3/idTech4).
- Materials browser: Surface/Content flag filter controls are now shown only for idTech2 (`quake2`) games.
- Entities browser: added icon/list view toggle, switched preview-node loading to visible-range on-demand loading, and added fallback preview cubes for misc_model-style classes that have no default model path so entries remain visible.
- Asset tile legibility: model/entity/sound/texture tile labels now render over dark backplates with heavier text treatment for better readability.
- Entity Inspector (`N`): introduced `Prefab Keys` and `Legacy Keys` tabs and cleaned up the legacy key/value presentation (headers + clearer entry fields).
- Branding: replaced the splash screen artwork with a new 1536x1024 `splash.png`.
- Branding: updated the main window title to use the "VibeRadiant" name without a space.
- Documentation/branding: refreshed `README.md` with a new `docs/viberadiant-banner.png` social banner and updated project overview/links.
- Documentation: modernized the `TODO` backlog into a structured table with indexed details.
- Documentation: expanded linked duplicates documentation with link ids, transform keys, and synchronization behavior.
- Documentation: added `docs/ai-level-design-tools.md` outlining an OpenAI-powered level design tool suite, integration architecture, and phased rollout plan.
- GenAI Prompt-to-Blockout: upgraded corridor/room connectivity to route boundary-to-boundary with aligned wall openings (removing blocked transitions), added per-surface shader controls (floor/wall/ceiling/sky) with game-aware defaults, added sky-ceiling toggle, introduced mitred corridor side-wall joins, and added an idTech3-only caulk option for utility/detail brushes in both dialog and preferences.
- GenAI Prompt-to-Blockout (Quake design pass): added playstyle inference (`Deathmatch`, `Campaign`, `Hybrid`) and style-aware planning/generation heuristics inspired by Quake map design patterns: DM now emphasizes looped circulation, fewer dead ends, contested control spaces, and skill-route traversal; Campaign now emphasizes readable progression paths, paced encounter transitions, optional detours, and resource cadence. Added long-corridor line-of-sight breakers, style-aware spawn placement, and style-aware item distribution; OpenAI planner prompts now include DM/campaign-specific layout guidance and generated summaries report selected playstyle.
- GenAI Prompt-to-Blockout (Quake refinement pass): added graph-derived main progression pathing (instead of pure axis sort) for campaign semantics, planner role support (`hub|arena|connector|secret|start|goal`) with downstream room-hint application, DM hub/control-loop reinforcement with additional ingress links, room-feature refinements for hub/combat cover geometry, spawn-spacing safeguards for deathmatch fairness, and a cleaner risk/reward item economy (spaced major-item placement + budgeted minor pickups by room role/progression).

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
