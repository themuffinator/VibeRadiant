# Lighting Preview (Camera View)

This document describes VibeRadiant’s real-time lighting preview: what it supports, how to use it, and how it works internally.

The goal of the lighting preview is **fast, interactive “baked-ish” feedback** (shadows + approximate light distribution) while editing, without running a map compile.


## Quick start

- Toggle via `View > Camera > Toggle Lighting Preview` (`F3`).
- Choose the preview model in `Preferences > Camera > Lighting Preview Model`:
  - `Baked Overlay (Quality)` (default)
  - `Fast Interaction (DarkRadiant-style)`
- `Baked Overlay (Quality)` renders a **fullbright base pass** plus a **multiplicative lighting overlay**.
- `Fast Interaction (DarkRadiant-style)` renders a **fullbright base pass** plus a **dynamic direct-light interaction overlay** (no shadow volumes).
- `Baked Overlay (Quality)` updates are **incremental** and **time-sliced**; `Fast Interaction` updates immediately each frame.


## What gets previewed

The preview builds a temporary lighting model from three main sources:

### 1) Light entities (point + directional)

Entities whose classname begins with `light` are considered light sources.

- **Point lights** (most `light*` entities)
  - Position: `origin` (falls back to instance `worldAABB().origin`).
  - Color: `_color` (defaults to white).
  - Intensity: `_light` / `light` / `_light` vector formats (see `parse_light_intensity()`).
  - Radius: `light_radius` (vector don’t-care axis: the max component is used); otherwise derived from intensity.
  - Falloff type: inferred from `spawnflags` (see `spawnflags_linear()`).
  - Optional scale: `scale` multiplies intensity.

- **Directional lights** (treated as “sun” lights)
  - `light_environment` or any `light*` with `_sun` truthy becomes a directional light.
  - Direction: from `target` (origin → target entity origin), otherwise from entity angles.

### 2) Surface lights (emissive shaders)

For idTech3 games, shaders can emit light via q3map/q3map2 directives:

- `q3map_surfacelight` / `q3map_surfaceLight` create an emitter.
- `q3map_lightRGB` sets the emitted color.
- If no `q3map_lightRGB` is present, the preview falls back to the shader texture’s average color as an approximation.

Emitters are generated for:

- **Brush faces** using the shader (light placed at face centroid, intensity scaled by face area).
- **Patches** using the shader (light placed at patch centroid, intensity scaled by patch area).

Note: surfacelights are considered even on `nodraw` shaders (useful for “light-only” materials).

### 3) Sky / sun lighting (worldspawn and sky shaders)

The preview supports common “sun” sources and q3map2 sky directives:

- **Worldspawn sun keys**
  - `_sun` / `sun` (full 6-float form: rgb intensity degrees elevation)
  - `_sunlight` / `sunlight` / `_sun_light` / `sun_light` plus optional `_sunlight_color`/`sunlight_color`, etc.
  - Direction keys supported include vector and mangle/angle variants plus `*_sun_target` targeting.

- **Shader-defined suns** (sky shaders)
  - `sun`, `q3map_sun` (basic sun)
  - `q3map_sunExt` (adds deviance + samples; used to approximate softer shadows)

- **Shader-defined skylight** (sky shaders)
  - `q3map_skyLight` / `q3map_skylight <value> <iterations> [horizon_min horizon_max sample_color]`
  - The preview turns this into a set of sampled directional lights spread over the sky dome.
  - `sample_color` is parsed but currently does not sample sky textures; the preview uses a constant color approximation.


## How it renders (two-pass approach)

Lighting preview mode is implemented as:

1. **Base pass (fullbright)**
   - The scene is drawn without traditional per-vertex/per-pixel lighting.
   - This keeps material appearance readable while lighting is previewed as a modulation overlay.

2. **Overlay pass (multiplicative)**
   - The preview draws lighting onto the already-rendered scene using multiplicative blending:
     - `dst = dst * src` (OpenGL: `glBlendFunc(GL_ZERO, GL_SRC_COLOR)`).
   - Brush faces are overlaid with per-face lightmap textures.
   - Patches are overlaid using per-vertex colors.

This approach approximates baked lighting well (including shadows) while remaining fast and avoiding complex per-material lighting pipelines.


## Lighting math (what a luxel/vertex computes)

For each sample point (a brush “luxel” or patch vertex), the preview computes:

- A small **ambient term** (constant).
- For each affecting light:
  - Lambert diffuse: `max(0, dot(N, L))`
  - Point attenuation based on a radius, with either linear or quadratic falloff.
  - A **shadow ray test** against scene geometry.

The accumulated RGB is clamped to `[0, 1]` for preview output.

Important implementation constants (in `radiant/previewlighting.cpp`):

- `kAmbient`: constant ambient fill.
- `kLuxelSize`: world units per brush luxel (controls quality vs speed).
- `kShadowBias`: normal-offset bias to reduce self-shadowing artifacts.
- `kLightCutoff`: small cutoff to skip negligible contributions.


## Shadows: BVH-accelerated ray casting

To make per-sample shadow tests practical, the preview builds a triangle BVH (bounding volume hierarchy) from visible scene geometry:

- **Brushes:** each face winding is triangulated into a fan in world space.
- **Patches:** patch tessellation is triangulated from its quad strips.

Shadow testing then becomes:

- Ray vs BVH node AABBs to quickly reject large regions.
- Ray vs triangles for actual occlusion checks.

Directional lights cast rays up to a computed “directionalDistance” derived from map bounds (large enough to reach across typical maps).


## Incremental updates (only rebuild what changed)

The preview is designed to stay responsive during editing. It avoids full rebuilds by:

### Scene rescan + hashing

When the scene changes, the system:

- Re-scans visible brushes and patches.
- Defers scene rescans while Lighting mode is off; dirty changes are accumulated and applied on re-enable.
- Computes hashes for:
  - Brush geometry (transform + face planes + shader names/flags + filters).
  - Patch geometry (transform + tessellation topology + shader/flags).
  - Lights (type + parameters + influence AABB).
- Reuses cached lightmaps/vertex colors when hashes match.

### Dirty marking

Only receivers impacted by edits are queued:

- If a light changes, any brush/patch whose AABB intersects the light’s influence is marked dirty.
- If geometry changes, all receivers inside influences of lights overlapping that geometry are marked dirty.

### Time-sliced rebuilds

Lightmap/vertex-color generation runs under a per-frame time budget (to avoid UI hitches):

- Brushes are processed from a dirty queue: rebuild per-face lightmaps.
- Patches are processed from a dirty queue: rebuild per-vertex colors.

This means lighting may “fill in” over multiple frames after large edits, but the editor remains interactive.


## Brush lightmaps: what gets generated

Each brush face gets a small CPU-generated RGB texture (preview lightmap):

- A local 2D basis (`uAxis`, `vAxis`) is constructed per face.
- Face bounds are projected onto that basis to choose a resolution.
- Each luxel is sampled at its world position with a world normal.
- The resulting texture is uploaded to OpenGL and applied using object-linear texgen planes per face.


## Patch lighting: per-vertex colors

Patches do not use textures for preview lighting. Instead:

- Each tessellated patch vertex is lit in world space.
- The result is stored as an RGBA array matching the tessellation vertex list.
- During the overlay pass, the patch draws with that color array enabled.


## Sky lighting details (q3map2 approximation)

### `q3map_skyLight`

The preview turns `q3map_skyLight` into multiple directional lights distributed over the hemisphere using the same “iterations → angular steps” approach as q3map2.

For performance, the preview caps the number of skylight samples (currently 64) and compensates intensity so total energy remains similar.

### `q3map_sunExt`

`q3map_sunExt` provides:

- A base sun direction (degrees + elevation).
- Optional “deviance” (angular blur) and “samples”.

The preview approximates this by splitting the sun into multiple sampled directional lights:

- Each sample uses a low-discrepancy pattern to jitter the sun direction within the deviance radius.
- Intensity is divided by the number of samples so total brightness stays stable.


## Limitations / differences vs a real q3map2 bake

This is a preview system, not a replacement for compiling:

- No radiosity/bounce lighting.
- No lightstyles and only limited entity/shader nuance.
- Surface lights are approximated as centroid point lights (not full area emitters).
- `q3map_skyLight` “sample_color” does not currently sample sky textures.
- Lightmap resolution is intentionally coarse and controlled by constants to keep it real-time.


## Fast mode: interaction lighting (DarkRadiant-style)

When `Lighting Preview Model` is set to `Fast Interaction (DarkRadiant-style)`, the preview skips CPU lightmap/patch-colour baking and evaluates direct lighting at draw time.

- Designed for responsiveness on large scenes and frequent edits.
- Uses the current temporary light set and draws lit overlays directly on brush/patch geometry.
- Does **not** use stencil shadow volumes and does not cast dynamic preview shadows in this mode.

Trade-offs versus `Baked Overlay (Quality)`:

- Much faster convergence (no per-luxel/per-vertex rebuild queues).
- Less physically representative than the baked-ish overlay.
- No occluder-based shadowing in fast mode.
- Receiver eligibility matches baked-overlay surface rules so the same brush/patch surfaces are affected in both models.


## Where the code lives

- Core: `radiant/previewlighting.cpp`
  - Public entry points: `PreviewLighting_Enable`, `PreviewLighting_UpdateIfNeeded`, `PreviewLighting_RenderOverlay`
  - Core pipeline: `rescan_scene()`, `apply_rescan()`, `update()`, `render_overlay()`
  - Lighting work: `build_brush_lightmaps()`, `build_patch_colours()`, `compute_lighting()`
  - Shadows: `rebuild_bvh_from_scene()`, BVH traversal + triangle tests

- Integration: `radiant/camwindow.cpp`
  - Calls `PreviewLighting_UpdateIfNeeded()` every camera draw (also allows safe GL cleanup when turning preview off).
  - In lighting mode, draws fullbright base and then `PreviewLighting_RenderOverlay()`.


## Tuning (developer knobs)

If you need to adjust quality/performance for your use-case, start by reviewing:

- `kLuxelSize`: smaller = sharper shadows/detail, slower rebuilds.
- `kWorkBudgetMs`: larger = faster convergence after edits, more CPU per frame.
- `kMaxLightmapRes`: upper bound for per-face preview textures.
- `kAmbient`: base ambient floor used by both preview models.
- `kLightCutoff`: tiny-contribution cutoff used to skip negligible point-light work.
- Skylight max-samples cap and sun sample cap (both currently limited for performance).

These are currently compile-time constants in `radiant/previewlighting.cpp`.
