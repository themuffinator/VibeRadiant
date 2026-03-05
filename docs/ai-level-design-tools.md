# AI-Powered Level Design Tools (OpenAI API Integration)

## Purpose
This document outlines a practical tool suite for integrating AI into VibeRadiant map production using the OpenAI API. The focus is production speed, consistent quality, and safe editor-side automation.

## Goals
- Cut blockout, encounter, and polish iteration time.
- Reduce build-break and logic-break regressions.
- Keep AI changes reviewable, reversible, and deterministic.
- Fit naturally into existing VibeRadiant command, selection, and undo workflows.

## Proposed Tool Suite

### 1) Prompt-to-Blockout Generator
- Converts a design brief into a playable blockout plan.
- Produces room and route graph proposals, then brush or patch actions as a preview diff.
- Applies only after user confirmation.

### 2) Encounter Composer
- Populates selected areas with enemy, item, cover, and objective placements.
- Optimizes for pacing constraints such as pressure, travel timing, and sightline balance.
- Emits placement rationale and quick variant options.

### 3) Entity Wiring Assistant
- Builds and validates trigger logic chains (`target`, `targetname`, relay patterns, objectives).
- Auto-fixes missing keyvalues, broken references, and naming conflicts.
- Integrates with issue-style diagnostics for one-click repair.

### 4) Texture and Shader Pass Assistant
- Applies semantic material suggestions to selected surfaces or zones.
- Normalizes scale, rotation, and alignment to style rules.
- Flags shader misuse and proposes safe replacements.

### 5) Lighting Pass Copilot
- Proposes light entities and keyvalues from gameplay intent (readability, mood, path guidance).
- Iterates from compile logs and viewport feedback.
- Supports fast pass and quality pass profiles.

### 6) Compile and Error Triage Agent
- Parses compile output and maps errors to likely geometry or entity causes.
- Suggests minimal, ranked fix actions.
- Can batch non-destructive fixes in a single undo transaction.

### 7) Playtest Analytics Coach
- Ingests telemetry (heatmaps, death locations, route frequency, objective failure points).
- Ranks map edits by expected impact.
- Generates targeted follow-up tasks for the next iteration.

## Integration Architecture

### Editor-Side Components
- `AI Tools` panel for prompts, previews, and apply flow.
- Command entries for each tool action.
- Selection-aware context capture (active selection, layer, map mode, gamepack metadata).
- Undo-safe operation batching for all applied edits.

### AI Service Layer
- Use OpenAI Responses API as the orchestration core.
- Use function calling with strict structured outputs for edit operations.
- Disable parallel mutating calls during map edits to avoid conflicting changes.
- Use vector store retrieval for local design conventions, mapping standards, and gamepack rules.
- Use background mode plus webhooks for long-running map-wide analysis.
- Use batch processing for nightly audits or style conformance scoring.

### Suggested Tool Functions (Examples)
- `create_brush_prism`
- `create_patch_grid`
- `move_selection`
- `set_entity_keyvalue`
- `connect_entity_targets`
- `apply_shader_to_faces`
- `run_validation_checks`
- `propose_fix_batch`

Each function should return typed, machine-validated payloads so every operation is deterministic and replayable.

## Trust and Safety Guardrails
- Preview-first workflow: AI proposes, user confirms.
- Scope boundaries: selection, layer, or named zone only.
- Hard limits: operation count, spatial bounds, entity class allowlists.
- Auto-rollback: every apply is wrapped in one undoable command.
- Audit trail: store prompt, context hash, tool calls, and applied diff summary.
- Protected modes: disallow destructive global actions unless explicitly enabled.

## Rollout Plan

### Phase 1 (Highest ROI)
- Compile and Error Triage Agent.
- Entity Wiring Assistant.
- Shared AI command pipeline with strict function outputs and preview UI.

### Phase 2
- Prompt-to-Blockout Generator.
- Encounter Composer.
- Selection and zone-aware apply constraints.

### Phase 3
- Texture and Shader Pass Assistant.
- Lighting Pass Copilot.
- Rule packs per game type and style profile.

### Phase 4
- Playtest Analytics Coach.
- Continuous optimization loop from telemetry to prioritized map edits.

## Success Metrics
- Time-to-first-playable blockout.
- Compile error count per iteration.
- Manual fix count after AI-assisted apply.
- Encounter balance iteration count to target metrics.
- User acceptance rate of AI proposals.
- Revert rate of AI-applied changes.

## Cost and Performance Notes
- Use smaller, faster models for interactive proposal drafts.
- Reserve larger reasoning models for map-wide planning and triage.
- Cache static context (style guides, schemas, conventions) to reduce token usage.
- Run heavy analysis asynchronously and notify on completion.

## Implementation Notes for VibeRadiant
- Primary integration points are command registration, plugin dispatch, selection APIs, and undo transaction boundaries.
- Keep initial implementation as a non-invasive plugin plus service bridge.
- Promote stable operations into core editor commands only after reliability targets are met.

## Summary
This tool suite is designed to accelerate production without sacrificing editor control. The core principle is constrained automation: AI handles high-volume planning and repetitive setup, while designers remain in control of final intent and quality.
