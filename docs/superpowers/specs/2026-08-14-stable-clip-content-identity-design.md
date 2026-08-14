# Stable Clip Content Identity Design

## Goal

Make duplicated MIDI and audio clips share editable content until the user chooses Make Unique. Timeline placement must never be used as content identity.

## Model

- Every playlist clip instance has a stable `clipId` and references a stable `contentId`.
- Instance-owned state: track, timeline start, trim offset, displayed duration, selection.
- MIDI content owns notes, performance events, pattern length, musical metadata, and display name.
- Audio content owns source path, detected source tempo, sync mode, target bars, pitch shift, and source duration.
- Reference counts are derived from playlist instances, not persisted separately.

## Operations

- Duplicate creates a new `clipId` and preserves `contentId`.
- Editing MIDI notes or shared audio properties updates content and therefore every linked instance.
- Moving or resizing an instance changes only placement/trim state.
- Make Unique clones referenced content, assigns a new `contentId`, and keeps instance placement unchanged.
- If content has one or zero references, Make Unique asks for confirmation before cloning.
- Content with no remaining instances is removed.

## Persistence

- Project format advances to version 2.
- Version 2 stores separate `midiContents`, `audioContents`, `midiClips`, and `audioClips` collections.
- Clip records persist `clipId` and `contentId`.
- Version 1 remains readable through transactional migration.
- MIDI v1 records group only when track/name and serialized document payload agree. A missing duplicate document may reuse the matching non-empty document.
- Audio v1 records group only when source path and all shared audio properties agree.
- Records marked unique always receive independent content.
- Missing media paths remain represented and do not abort project load.

## Invariants

- Every clip references exactly one existing content object after validation.
- IDs are non-empty and unique within their collection.
- Placement changes cannot orphan or silently replace content.
- Make Unique cannot mutate sibling instances.
- Save/load preserves linked-instance relationships.
- Migration never merges conflicting payloads.

## Error Handling

- Reject malformed, duplicate, dangling, or oversized IDs/content records before resetting the current session.
- Generate collision-resistant IDs for newly created and migrated objects.
- Failed audio source cloning leaves the selected instance and old content unchanged.
- Failed project save remains atomic through `QSaveFile`.

## Verification

Static checks in this pass:

- Duplicate MIDI instances resolve to the same note document.
- Make Unique creates a distinct content object with equal initial payload.
- Audio shared-property edits resolve through `contentId`.
- v1 and v2 validation paths reject dangling references.
- Project serialization no longer derives identity from name, start, or length.

Runtime/build verification is excluded because the user explicitly requested no build or run.

## Non-Goals

- Audio recording, FX DSP, realtime lock-free refactoring, and UI redesign.
- Changing plugin layer identity or plugin state ownership.
- Adding loop-region or seek behavior.
