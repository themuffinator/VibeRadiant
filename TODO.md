# Project Plan

## High Priority Fixes (Bugs)

| Index | Category | Task | Complexity | Priority |
| :--- | :--- | :--- | :--- | :--- |
| B1 | UI | MSI: installer bug with new folders? create custom dir, click New Folder icon, type "FOLDER\" - gets stuck | Low | High |
| B2 | UI | GTK2: gtk2 crashes when trying to use bitmap fonts such as MS Sans Serif | Medium | High |
| B3 | UI | GTK2: alt+tab while mouse button is held down issues | Low | High |
| B4 | UI | UI: changing resolution in floating-windows mode can screw up window positions. | Medium | High |
| B5 | Game Support | HalfLife: half-life maps saved in q1 map format are not supported (currently require Hammer conversion) | Medium | High |
| B6 | Core | Entity: creating a new entity with all the brushes of another entity selected results in the latter entity having no brushes. | High | High |
| B7 | UI | GUI: can't use arrow keys to navigate in camera view when capslock is enabled | Low | High |
| B8 | UI | GUI: screensaver causes assertion failure in `gdkgc-win32.c` | Low | High |

## Feature Tasks

| Index | Category | Task | Complexity | Priority |
| :--- | :--- | :--- | :--- | :--- |
| F1 | Tools | **Shader Browser & Creator** (idTech3) - multi-layer, animated, real-time visual editing. | High | High |
| F2 | Tools | **Packaging Tool** - automated release packaging and readme creation. | High | High |
| F3 | Tools | **Advanced Map Build System** - GUI-driven build presets and config. | High | High |
| F4 | UI | Paint-select or equivalent (e.g. area-selection with occlusion) | Medium | Medium |
| F5 | UI | Select-complete-tall or equivalent (e.g. subtract-from-selection modifier key) | Low | Medium |
| F6 | UI | Improve texture pane names legibility (overlap issues) | Low | Medium |
| F7 | Core | Entity: option to filter non-world entities (e.g. not func_group or func_static) | Low | Medium |
| F8 | Core | Rotate Tool: use parent-space rotation pivot if multiple objects with different local orientations selected | Medium | Medium |
| F9 | UI | Brush: MMB+ctrl to paint texture on whole brush/patch. | Low | Medium |
| F10 | UI | Camera: add alternative highlighting styles. | Low | Low |
| F11 | Game Support | Doom3: filter func_splinemovers | Low | Low |
| F12 | UI | Entity: draw arrowheads to show direction of connection-lines. | Low | Medium |
| F13 | UI | MMB to select a texture should also apply that texture to all selected faces. | Low | Medium |
| F14 | UI | Mouse: support 2-button mouse. | Low | Low |
| F15 | UI | Grid: background colour should be different when the smallest grid is invisible. | Low | Low |
| F16 | UI | Brush: option to disable dots on selected faces when not in face mode. | Low | Low |
| F17 | UI | Entity: draw direction arrow for func_door and func_button angle. | Low | Medium |
| F18 | Build | Build Menu: support for editing variables. | Medium | Medium |
| F19 | Rendering | Shaders: handle doom3 materials with multiple bumpmaps stage (use first, ignore later). | Medium | Low |
| F20 | Core | Brush: warn when a brush is dragged into a configuration with <0 volume | Low | Medium |
| F21 | UI | Textures: add option to give new brushes a specific texture instead of the last selected. | Low | Medium |
| F22 | UI | QE-tool: click anywhere on xy view to drag entity instead of requiring clicking directly on entity. | Medium | Low |
| F23 | Rendering | Textures: add anisotropic filtering. | Medium | Low |
| F24 | Core | Preferences: allow preference settings to be shared across games. | Medium | Low |
| F25 | Core | Preferences: add colour 'theme' files using prefs format. | Medium | Low |
| F26 | UI | Preferences: sensible default size for prefs window. | Low | Low |
| F27 | Game Support | Doom3: s_diversity light key. | Low | Low |
| F28 | Game Support | HalfLife: enable HL-mode on linux/osx. | Medium | Low |
| F29 | Rendering | Renderer: doom3 'parallel' and 'spot' light support. | High | Medium |
| F30 | UI | Entity: add mouse-editing for doom3 light_center key | Medium | Low |
| F31 | Rendering | Shaders: add support for texture transforms. | Medium | Medium |
| F32 | Rendering | Shaders: add support for 'addnormals' keyword | Medium | Low |
| F33 | Core | TGA Loader: check that true-colour images with palettes are properly handled. | Low | Low |
| F34 | Core | Module System: reinstate 'refresh' feature. | High | Medium |
| F35 | UI | Surface Inspector: add button for 'axial' projection for doom3. | Low | Low |
| F36 | Build | Build: fix hardcoded engine-launch commands - use similar system to build-menu command description. | Medium | Medium |
| F37 | Core | Filters: use q2/heretic2 content flags to filter brushes. | Low | Low |
| F38 | UI | Surface Inspector: allow material names not relative to 'textures/' for doom3 | Low | Low |
| F39 | Core | Module System: add versioning for module-system api. | Medium | Low |
| F40 | UI | Editing: add option to choose the default startup tool mode. | Low | Low |
| F41 | Rendering | Renderer: lighting for doom3 materials without bumpmaps | Medium | Low |
| F42 | Rendering | Renderer: realtime doom3 materials preview | High | Medium |
| F43 | Rendering | Renderer: realtime doom3 shadows preview | High | Medium |
| F44 | UI | Textures Window: add inner dark outline to distinguish 'is-shader' outline from white textures. | Low | Low |
| F45 | Game Support | HalfLife2: add HL2 map load/save. | High | Low |
| F46 | UI | Selection: add move-pivot mode to allow rotation/scale around a custom pivot-point. | Medium | Medium |
| F47 | UI | Selection: add rotate increment for rotate manipulator. | Low | Medium |
| F48 | UI | Selection: visibly distinguish between entity and brush selections | Low | Medium |
| F49 | UI | Selection: need 'add to selection' and 'subtract from selection' modifiers | Low | Medium |
| F50 | UI | Selection: Finish scale manipulator. | Medium | Medium |
| F51 | UI | FaceCopy/PasteTexture: Make face-copy/paste-texture shortcuts customisable. | Low | Low |
| F52 | Docs | Manual: add documentation about search paths for .ent/.def/.fgd, shaders etc for each game. | Low | Medium |
| F53 | Game Support | Halflife: add support for cstrike fgd. | Low | Low |
| F54 | Game Support | HalfLife: disable patches | Low | Low |
| F55 | Game Support | HalfLife: support fgd 'flags' attributes. | Low | Low |
| F56 | Game Support | Model: add support for doom3 md5anim format | Medium | Low |
| F57 | Game Support | Model: support doom3 ragdolls | High | Low |
| F58 | Core | VFS: add ability to browse VFS from file-open dialogs. | High | Medium |
| F59 | Core | Installer: enable q3 brush-primitives map support. | Medium | Low |
| F60 | Core | Installer: add editor manual to linux installer | Low | Low |
| F61 | Core | Map: add conversion between map formats | High | Medium |
| F62 | Core | Map: add conversion between entity definition formats | Medium | Low |
| F63 | Build | Build: add build-menu dmap support (doom3) | Medium | Low |
| F64 | UI | Entity: optionally draw target connection lines thicker than one pixel. | Low | Low |
| F65 | UI | Entity: add specialised attribute-entry in entity-inspector for integer/real/color attribute types. | Medium | Medium |
| F66 | UI | Patch: add cap-texture, fit-texture and natural-texture toolbar buttons | Low | Low |
| F67 | UI | Patch: draw patches in wireframe from the back, make patches selectable from the back | Low | Low |
| F68 | UI | Patch: add option for convert-selection-to-new-brush/patch | Low | Low |
| F69 | Core | Patch: fix bobtoolz merge-patches feature | Medium | Low |
| F70 | UI | Patch: fix insert/remove rows/cols indicated by current selected patch vertices. | Low | Low |
| F71 | Core | Autosave/Snapshots: Add support for multi-file maps. | Medium | Low |
| F72 | Game Support | Quake2: Q2 hint transparency support | Low | Low |
| F73 | UI | Shortcuts: convert shortcuts.ini to xml. | Low | Low |
| F74 | UI | Shortcuts: warn when duplicate shortcuts are registered | Low | Low |
| F75 | UI | Shortcuts: rename commands in order to group shortcuts list better. | Low | Low |
| F76 | UI | Doom3: lights should stay in place while resizing | Low | Low |

## Low Priority / Long Term

| Index | Category | Task |
| :--- | :--- | :--- |
| L1 | UI | Selection: Add shear manipulator? |
| L2 | UI | Textures Window: Improve texture-manipulation and texture-browsing tools. |
| L3 | Core | Undo: make selections undoable? |
| L4 | Installer | Win32 Installer: Automatically upgrade existing installation. |
| L5 | Core | General: refactor game-specific hacks to be parameterised by .game file |
| L6 | Core | Patch: Overlays, Bend Mode, Thicken. |
| L7 | Core | Brush: Add brush-specific plugin API. |
| L8 | UI | Entity: Draw light style numbers. |
| L9 | UI | Entity: Show models with model2 key. |
| L10 | Core | Entity: Interpret _remap* key (_MindLink_). |
| L11 | Core | Entity: Support _origin _angles _scale on groups. |
| L12 | UI | Selection: Add Primitive-mode shortcut key/button. |
| L13 | UI | Selection: Customisable manipulator size - +/- to change the size of the translate/rotate tool. |
| L14 | UI | Selection: Add optional screen-relative control for constrained rotations. |
| L15 | UI | Clipper: Change selection/manipulation to be consistent with other component editing. |
| L16 | UI | Filtering: Either deselect filtered nodes, or render filtered nodes that are selected. |
| L17 | UI | Filtering: Add customisable filter presets to set/unset multiple filters at once. |
| L18 | Core | Texdef: Make texdef formats abstract, add conversion between texdef formats. |
| L19 | UI | Textures Window: Precise display of texture size when selecting. (tooltip, possibly) |
| L20 | UI | Status: 'Size of brush' display on status bar. |
| L21 | UI | Colours: maya scheme default? |
| L22 | Game Support | Quake: add support for adjusting gamma on quake palette? |

## Detailed Task Outlines

### F1: Shader Browser & Creator (idTech3)
**Objective:** Create a comprehensive tool for browsing, editing, and creating shaders for idTech3 games.
*   **Shader Browser:**
    *   Show multi-layered shader images as they appear in-game (first frame).
    *   Animate on hover.
    *   View script and image side-by-side.
*   **Shader Creator/Editor:**
    *   Create new shader files and edit existing ones.
    *   Real-time visual depiction of the texture updates as code is edited.
    *   Draw shaders fully in the camera view (option to toggle animation).
    *   Support for standard idTech3 shader keywords and stages.

### F2: Packaging Tool
**Objective:** Simplify the release process for maps.
*   **Engine Consideration:** Adapt packaging rules based on the target game engine.
*   **Release Packaging:** Automate the creation of PK3/ZIP files with correct directory structures.
*   **Readme Generator:**
    *   Fill in details on an existing readme template.
    *   Allow manual editing of the generated readme.
    *   Include map details, credits, and installation instructions.

### F3: Advanced Map Build System
**Objective:** A fully GUI-driven interface for map compilation.
*   **Configuration Interface:**
    *   GUI for configuring build presets (BSP, VIS, LIGHT stages).
    *   Create and manage default build configs for "Development" (fast) and "Release" (high quality).
*   **Testing:**
    *   One-click "Build and Run" functionality.
    *   Load the map directly into the game engine after compilation.
*   **Documentation & Customization:**
    *   Document the XML format used for build menus.
    *   Support customizable variables in build commands (e.g., `[MapFile]`, `[RadiantPath]`).
    *   *Reference (Old TODO):*
        ```xml
        <pre>[q2map] -bsp "[MapFile]"</pre>
        becomes:
        <pre>"C:\Program Files\GtkRadiant 1.5.0\q2map" -fs_basepath "c:\quake2" -bsp "c:\quake2\baseq2\maps\blah.map"</pre>
        ```

### F52: Manual Documentation
*   Add documentation about search paths for `.ent`, `.def`, `.fgd`, shaders, etc., for each supported game.
*   Ensure documentation covers how VFS handles these paths.

### F58: VFS Integration
*   Implement `browse VFS` functionality in file-open dialogs.
*   Allow users to select resources directly from loaded PK3/WAD files without extracting them.
*   Ensure consistency with the existing VFS plugin architecture.
