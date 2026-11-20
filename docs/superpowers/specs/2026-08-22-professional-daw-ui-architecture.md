# Professional DAW UI Architecture

## Status

This specification supersedes the UI scope and non-goals in the June 2026
hybrid arrangement and polish documents. Those files remain useful historical
context, but they must not prevent a full UI replacement.

## Product Standard

VeraCore is a production DAW, not a visual prototype. Every visible control
must operate real project or engine state. Every edit must support undo/redo,
project persistence, deterministic playback, and crash recovery where relevant.

The interface may be redesigned from scratch. Visual novelty never takes
priority over timing accuracy, discoverability, keyboard speed, stable muscle
memory, accessibility, or realtime performance.

## Audience And Direction

- Audience: musicians, producers, recording engineers, sound designers, and
  mixing engineers moving from Ableton Live, FL Studio, Cubase, REAPER, Logic
  Pro, Pro Tools, Studio One, or Bitwig Studio.
- Primary job: move from recording or composition to editing, arrangement,
  mixing, and export without changing applications.
- Tone: technical, restrained, dense, and instrument-like.
- Visual direction: Ableton clarity and routing, FL immediacy and piano-roll
  speed, Cubase expression editing, REAPER configurability, and Pro Tools
  signal-flow legibility.

Marketing-page patterns do not belong in the workstation. Avoid oversized
typography, decorative cards, excessive glass, gratuitous animation, hidden
controls, and whitespace that reduces useful timeline or mixer area.

## Stable Workspace Anatomy

The default workspace has five persistent anchors:

1. Global transport and project state at the top.
2. Browser and search at the left.
3. Track headers and arrangement in the center.
4. Inspector, clip editor, piano roll, automation, or device chain in the lower
   detail zone.
5. Mixer as a first-class switchable or dockable workspace.

Panels may dock, float, collapse, move to another monitor, or be stored in a
workspace preset. Commands, terminology, selection behavior, and project state
must remain identical across layouts.

Minimum workspace presets:

- Arrangement
- Session / performance
- Piano roll / MIDI editing
- Recording
- Mixing
- Mastering
- Dual-monitor
- User custom

## Cross-DAW Familiarity

VeraCore keeps one canonical command model and provides selectable keymap and
interaction profiles. Profiles translate muscle memory; they must not fork
project behavior.

| Source workflow | Familiar VeraCore behavior |
| --- | --- |
| Ableton Live | Arrangement-first layout, bottom device chain, rack/layer flow, linked track/send/return controls |
| FL Studio | Fast draw/paint piano roll, note properties, channel/layer colors, direct sample controls |
| Cubase | Expression maps, articulation lanes, inspector depth, advanced MIDI transforms |
| REAPER | Custom actions, dockable panels, routing visibility, configurable shortcuts |
| Logic Pro | Track inspector, smart controls, take/comp workflow, familiar editing terminology |
| Pro Tools | Edit/mix separation, recording states, inserts/sends/I/O clarity, sample/time displays |
| Studio One | Drag-and-drop devices, integrated browser, contextual inspector |
| Bitwig Studio | Modulation visibility, layered devices, hybrid clip/device workflow |

Profiles must cover keyboard shortcuts, mouse modifiers, zoom direction,
timeline navigation, selection semantics, and common tool switching. The active
profile is always visible in Preferences and exportable.

## Interaction Contract

- Single click selects. Double click opens the primary editor. Context click
  exposes actions relevant to the target.
- Drag previews the result continuously when safe. Escape cancels. Enter or
  pointer release commits.
- Every destructive or structural edit is undoable. Irreversible operations
  require explicit confirmation and recovery guidance.
- Selection is shared across arrangement, mixer, inspector, piano roll, and
  device chain unless the user pins a panel.
- Parameter changes show name, current value, unit, automation state, and fine
  adjustment modifier.
- No silent mode changes. Tool, snap, record, monitor, automation, loop, punch,
  and synchronization states must be visually persistent.
- Modal dialogs are reserved for decisions that cannot be made in context.
  Editing, routing, and parameter work stays non-modal.
- Focus never jumps because a background scan, autosave, waveform build, or
  plugin refresh finishes.

## Professional Signal Flow

Track, mixer, inspector, and device controls are different views of the same
parameter state. Volume, pan, mute, solo, arm, monitor, sends, returns, I/O,
inserts, latency, and automation must never diverge between views.

Required routing visibility:

- Hardware input and output
- Pre/post-fader sends
- Return buses
- Sidechain sources
- Group, bus, and folder routing
- Mono/stereo state and pan law
- Plugin and path latency
- PDC state
- Peak, RMS, and true-peak metering where applicable
- Clipping and headroom before hardware output limiting

## Editor Requirements

Arrangement, audio clip editor, piano roll, automation editor, and mixer share
the same transport, snap, time-signature map, tempo map, PPQ, loop, selection,
and undo history.

The piano roll must support FL-speed note editing and Cubase-style expression
maps. Articulations obtained from VST2, VST3, or CLAP are named and shown on
dedicated lanes and keys. Raw keyswitch notes remain available for plugins that
provide no metadata.

Audio clips expose source in/out, fades, gain, pan, normalize, polarity,
reverse, pitch, tempo, warp/stretch mode, detected tempo confidence, time
signature, transient markers, and make-unique state without hiding critical
values behind separate dialogs.

## Visual System

- Use semantic design tokens for surfaces, text, selection, focus, warning,
  clipping, recording, MIDI, audio, automation, and routing.
- Orange is an action/selection accent, not a full-page fill.
- Track colors identify content but never replace selection or warning states.
- Maintain a compact 4 px spacing grid and a documented control-height scale.
- Use one icon family and one geometry/radius system.
- Text and numeric readouts remain legible at 100%, 125%, 150%, and 200% DPI.
- Animation communicates state transitions only. Use transform/opacity, honor
  reduced motion, and keep timeline interaction immediate.
- Every interactive control has default, hover, focus, active, disabled,
  unavailable, automated, and error states as applicable.

## Architecture Boundaries

- Models own project state. Widgets render state and emit commands.
- A central command registry owns names, shortcuts, enablement, undo labels,
  menu entries, context actions, and command-palette entries.
- A shared selection service owns selected tracks, clips, notes, devices, and
  time ranges.
- A workspace manager owns docking, visibility, geometry, monitor placement,
  and preset migration.
- Audio-thread state is exposed through lock-free snapshots or bounded command
  queues. Widgets never call plugin or audio processing code directly.
- Waveform generation, sample analysis, tempo detection, stem separation,
  plugin scanning, and project compression run on bounded worker pools with
  cancellation and priority.
- UI redraw is dirty-region based. Timeline, meters, waveform tiles, and static
  chrome do not share one unrestricted repaint timer.

## Performance Budgets

- Audio callback: no allocation, deallocation, file I/O, blocking lock, UI
  invocation, logging, or unbounded work.
- Pointer-to-visual feedback: target below 8 ms.
- Common edit commit: target below 16 ms excluding explicitly asynchronous
  analysis.
- Meter and playhead painting: independent capped rates; audio processing never
  waits for paint.
- Timeline drawing: viewport culling, level-of-detail rendering, cached text,
  and waveform tiles.
- Browser search: incremental and cancellable; plugin/sample metadata is never
  rescanned on the UI thread.
- Large projects: work scales with visible or active content, not total project
  size per frame or per audio block.

## Reliability And Recovery

- Dirty projects receive atomic background recovery snapshots while transport
  is stopped and the UI is idle.
- A crash or unsafe shutdown offers the newest valid recovery snapshot at next
  launch without overwriting the original project.
- Plugin faults are isolated where the plugin format and host architecture
  permit it. A failed plugin is bypassed and identified instead of taking down
  the project when recovery is possible.
- Long operations report progress, allow cancellation, and leave the previous
  project state valid until commit.
- Project format changes are versioned and migrated forward without silently
  discarding unknown data.

## Acceptance Gates

The UI is not considered professional until static and runtime evidence proves:

- Every visible transport, track, mixer, clip, MIDI, routing, and export control
  changes the canonical model and survives save/load.
- Undo/redo restores both model and visible state.
- Arrangement, mixer, inspector, and plugin/device views remain synchronized.
- Keymap profiles cover the documented common actions without collisions.
- Keyboard-only operation is possible for transport, navigation, selection,
  editing, routing focus, and dialogs.
- Layout remains usable at 1280x720, 1920x1080, ultrawide, and high DPI.
- No clipping, hidden focus, accidental zero-size panel, or unreadable state.
- Large projects do not cause project-size work on every UI or audio frame.
- Autosave/recovery, plugin failure, missing media, and audio-device loss have
  explicit recoverable states.
- Playback, recording, MIDI, automation, PDC, render, and supported file formats
  pass end-to-end tests before release claims are made.

## Delivery Order

1. Stabilize audio/plugin callback ownership and remove realtime locks.
2. Establish canonical commands, shared selection, and parameter linking.
3. Build workspace manager and cross-DAW keymap profiles.
4. Replace main shell and design tokens if the current widgets cannot satisfy
   this specification.
5. Complete arrangement, piano roll/expression maps, audio editor, mixer, and
   routing workflows.
6. Verify accessibility, performance, recovery, project persistence, and
   end-to-end export.

