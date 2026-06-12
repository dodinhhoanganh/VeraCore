# Hybrid Arrangement UI Design

## Goal

VeraCore should feel fast to control like FL Studio while keeping Ableton-style arrangement logic. The main workspace is arrangement-first: tracks live in a linear timeline, MIDI is created directly on MIDI lanes, and there is no separate pattern/channel-rack workflow.

## Approved Direction

Use an Ableton Arrangement core with FL-speed track headers.

- Keep the main layout as `Browser | Track Headers + Arrangement Timeline | Bottom Inspector`.
- Keep the browser fast and searchable for plugins and samples.
- Use the track-header column as the fast channel-control surface.
- Create and edit MIDI directly in the arrangement, not through a pattern rack.
- Use the bottom inspector as the selected track/device/clip detail area.

## Workspace Layout

The current Qt splitter structure remains the right foundation:

- `TransportWidget` stays as a fixed top bar.
- `browserPanel` stays on the left for plugin/sample discovery.
- `trackHeadersScroll` stays left of `PlaylistView` and remains vertically synced with the arrangement.
- `PlaylistView` stays the central timeline with a fixed top ruler and stacked track lanes.
- `InspectorWidget` stays below the arrangement as an Ableton-style device/clip strip.

The design should avoid adding another primary panel for a channel rack. Fast channel operations belong inside the track headers and inspector.

## Track Header Design

Track headers are the main "fast channel" surface. Each header should stay compact and lane-aligned, but expose the controls users need constantly.

Required controls:

- Track type color strip: MIDI teal, audio orange.
- Track name.
- Mute, solo, record-arm, and monitor buttons.
- Volume control.
- Pan control.
- MIDI tracks: quick instrument slot/open action.
- All tracks: quick FX/device open action.
- Selected-state highlight that matches the currently focused track/clip.

The existing `TrackWidget` lane height is coupled to `PlaylistView::m_trackHeight`. The first implementation should keep the current 60 px lane height unless a coordinated resize updates both the headers and arrangement grid.

## Arrangement And MIDI Creation

MIDI is created like Ableton, directly in the arrangement:

- Double-click an empty MIDI lane to create a default-length MIDI clip.
- Drag across an empty MIDI lane to create a MIDI clip with the dragged length.
- Double-click an existing MIDI clip to open MIDI editing.
- Selecting a MIDI clip changes the bottom inspector from track devices to clip/MIDI editing.
- Audio lanes do not create MIDI clips; audio clip creation is outside this design pass.

The first pass can use simple visual MIDI clips without full piano-roll note editing, as long as the interaction model and data boundaries are ready for MIDI editor integration.

## Component Boundaries

`VeraCoreQT` should continue to own top-level composition: splitters, track list, selection routing, and browser population.

`TrackWidget` should own track-header presentation and fast controls. It should emit focused signals for selection and fast-control changes rather than letting the main window inspect child widgets.

`PlaylistView` should own arrangement drawing, hit testing, clip visuals, lane mapping, and clip creation gestures.

`InspectorWidget` should own the bottom detail surface. It should support two selection modes:

- Track mode: track tile, instrument slot for MIDI tracks, FX chain, output.
- MIDI clip mode: clip name/length and MIDI-editor placeholder controls.

The `Track` model can stay simple for this design pass, but clip data should be introduced as a separate arrangement model rather than stuffed into widget state long term.

## Data Flow

Selection flows from track headers or arrangement items into `VeraCoreQT`, then into `InspectorWidget`.

Track control changes flow from `TrackWidget` to the underlying `Track` object and then refresh the widget state.

MIDI clip creation starts in `PlaylistView` through lane hit testing. The view should create or request creation of a clip associated with the target MIDI track and beat range.

Zoom and scrolling remain owned by `PlaylistView`; vertical scrolling remains synced with the track-header scroll area.

## Error Handling And Guardrails

- Ignore MIDI-creation gestures on audio tracks.
- Clamp created clip start/end to non-negative beat positions.
- Enforce a minimum MIDI clip length when double-clicking or dragging a tiny region.
- Keep track-header and lane heights synchronized.
- Preserve existing plugin scanning and browser behavior.
- Avoid raw Unicode glyphs in C++ string literals; use UTF-8 hex escapes when symbols are needed.

## Testing And Verification

Manual verification is expected because the repo currently has no automated tests.

Verify:

- Solution builds in Debug x64 with MSBuild.
- Main window opens maximized.
- Browser search still filters plugin tree entries.
- Track headers remain aligned with playlist lanes while scrolling.
- Selecting a track updates the inspector.
- Track mute/solo/arm/monitor controls are visually distinct and do not resize the lane.
- Double-clicking or dragging on MIDI lanes creates visible MIDI clips.
- The same actions on audio lanes do not create MIDI clips.
- Double-clicking a MIDI clip switches the inspector into clip/MIDI editing mode.

## Non-Goals

- No FL-style pattern/channel-rack workflow.
- No full piano-roll editor in the first UI pass.
- No audio clip recording/editing in this design pass.
- No plugin hosting changes.
- No transport/audio-engine behavior changes.
