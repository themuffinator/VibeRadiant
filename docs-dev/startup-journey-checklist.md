# Startup Journey Implementation Checklist

## Scope
Implement the startup journey in staged increments and keep this checklist as the source of truth.

## Stage 1 - Splash + GitHub Release Check
- [x] Show splash screen at startup before editor window creation.
- [x] Splash includes VibeRadiant version info.
- [x] Splash includes build date info.
- [x] Startup status explicitly indicates update check against GitHub releases.
- [x] Automatic update check runs while splash is visible.
- [x] Main window remains hidden until startup sequence completes.
- [x] Stage 1 implementation landed in code (`radiant/main.cpp`, `radiant/mainframe.cpp`).

## Stage 2 - Update Available Decision Dialog
- [x] If update exists, show update details in a polished dialog.
- [x] Provide install and ignore actions.
- [x] Add checkbox to auto-install future updates.
- [x] Stage 2 implementation landed in code (`radiant/update.cpp`).

## Stage 3 - Setup Detection + Onboarding
- [x] Detect whether app setup is incomplete (missing prefs or missing game installation path).
- [x] Provide setup flow with auto-detect option for game installation.
- [x] Ask preferred editor style (currently NetRadiant only).
- [x] Stage 3 implementation landed in code (`radiant/mainframe.cpp`).

## Stage 4 - Dedicated Loading Screen
- [x] Show modern minimal loading screen with progress bar.
- [x] Keep editor window hidden until loading reaches completion.
- [x] Stage 4 implementation landed in code (`radiant/main.cpp`, `radiant/mainframe.cpp`).

## Stage 5 - Main Window Reveal + Welcome Popup
- [x] Show editor window only after loading completes.
- [x] Open welcome popup on startup after main-window reveal.
- [x] Stage 5 implementation landed in code (`radiant/main.cpp`).

## Stage 6 - Welcome Popup Content/Behavior
- [x] Show Blender-inspired welcome layout with distinct action sections.
- [x] Welcome actions: New Map, Open Existing, Reopen Previous, Getting Started, Donate.
- [x] Welcome top area includes VibeRadiant graphic/logo treatment, plus version and date in the top-right.
- [x] Persist startup behavior with a `Show this welcome screen on startup` toggle.
- [x] Stage 6 implementation landed in code (`radiant/main.cpp`, `radiant/preferences.cpp`).

## Stage 7 - Startup Preferences Compatibility
- [x] Add compatibility aliases for newly introduced startup/update preference keys.
- [x] Keep old preference names readable/writable to avoid breaking existing preference files.
- [x] Stage 7 implementation landed in code (`radiant/update.cpp`, `radiant/mainframe.cpp`, `radiant/preferences.cpp`).

## Stage 8 - Startup Fallback Hardening
- [x] Harden startup map loading against invalid command-line map paths.
- [x] Harden startup map loading against missing previous-map paths and recover to `Map_New`.
- [x] Keep welcome-screen reopen behavior defensive when previous map is unavailable.
- [x] Stage 8 implementation landed in code (`radiant/main.cpp`).

## Stage 9 - Startup Diagnostics and Timing
- [x] Capture startup stage timings for key startup milestones.
- [x] Emit startup timing summaries to log output for diagnostics.
- [x] Add startup diagnostics CLI support (`-startup-diagnostics`) for explicit diagnostic runs.
- [x] Stage 9 implementation landed in code (`radiant/main.cpp`).

## Stage 10 - Rollout Controls and Legacy Fallback
- [x] Add startup rollout preference toggles for modern journey + loading screen behavior.
- [x] Add CLI override flags for rollout/operations (`-startup-legacy-flow`, `-startup-no-loading-screen`, `-startup-no-welcome`).
- [x] Provide startup-journey fallback behavior when modern journey is disabled.
- [x] Stage 10 implementation landed in code (`radiant/main.cpp`, `radiant/preferences.cpp`).

## Stage 11 - Crash Debug Fast-Forward Run
- [x] Add dedicated debug run flag to fast-forward to the loading-main-window path (`-startup-debug-mainwindow-only`).
- [x] Skip non-essential startup work in this debug run (startup update check, setup/welcome, initial map load, startup theme apply).
- [x] Keep loading-screen/main-window construction path active for targeted crash isolation.
- [x] Stage 11 implementation landed in code (`radiant/main.cpp`).

## Implementation Notes
- Keep `CHANGES_FROM_NRC.md` updated for each significant stage completion.
- Stage completions should be marked in this checklist when merged.
