# Qt Ableton/FL Polish Design

## Goal

Make VeraCore's Qt main window feel more like a focused DAW surface: Ableton-style arrangement and device strip, with FL Studio-style immediacy in the transport, browser, and track headers. This pass improves layout polish, margins, visual hierarchy, and panel show/hide behavior without introducing a separate channel rack or mixer workflow.

## Approved Direction

Keep the current `Browser | Arrangement | Bottom Inspector` structure.

The implementation should refine what already exists:

- `TransportWidget` remains a fixed top transport bar.
- `browserPanel` remains the searchable left browser.
- `trackHeadersScroll` remains lane-aligned with `PlaylistView`.
- `PlaylistView` remains the central arrangement timeline.
- `InspectorWidget` remains the bottom Ableton-style device/clip strip.

Do not add a new FL-style channel rack or mixer in this pass.

## Main Window Layout

The existing splitter structure stays in place, but should behave more predictably:

- Set stable initial splitter sizes for browser, track headers, arrangement, and inspector.
- Prevent accidental zero-width or zero-height panels where that leaves unusable empty layout.
- Preserve useful panel sizes when users hide and show Browser or Inspector.
- Keep track headers and playlist vertically aligned after resize, scroll, and panel toggles.
- Keep all margins tight enough for a DAW workspace, with clear separators instead of wide padding.

The View menu should control real visibility:

- Browser toggles the left browser panel.
- Playlist toggles the arrangement/track-header area, but should not break the inspector.
- Inspector should be exposed through a View action and toggle the bottom device strip.
- MIDI Editor should show and raise the floating editor when an editor exists; otherwise it should leave the menu unchecked and report that no MIDI clip is open.

## Visual Style

The target visual language is a restrained hybrid:

- Ableton influence: flat dark surfaces, crisp grid, compact panels, clear device strip.
- FL Studio influence: orange active states, immediate transport controls, vivid track identity colors.

Use a darker neutral base and avoid making the whole app orange. Orange is reserved for active, selected, hovered, record, and focus states. MIDI remains teal and audio remains orange/brown as track identity colors.

Widget styling should be consistent across global QSS and local widget styles:

- Reduce inconsistent border radii and padding.
- Normalize button heights and label sizes.
- Keep menu, browser, track, and inspector colors in the same palette.
- Make scrollbars and splitter handles visible but compact.

## Transport

The transport should read as a professional DAW control strip:

- Keep the current top placement and 56 px maximum height unless a coordinated layout update requires a small adjustment.
- Center the transport cluster, time display, tempo, signature, metronome, and key controls cleanly.
- Use fixed control sizes so resizing the window does not distort the layout.
- Keep UTF-8 symbols in C++ as escaped byte strings, not raw glyph literals.
- Ensure text does not overflow at common desktop widths.

## Track Headers

Track headers remain the main fast channel surface and must stay coupled to the playlist lane height.

Required behavior:

- Keep 60 px header height unless `PlaylistView::m_trackHeight` is changed at the same time.
- Keep the 30 px top offset matching `PlaylistView::m_timelineHeight`.
- Preserve vertical scrollbar sync between headers and arrangement.
- Selecting a header updates the inspector and selected visual state.

Required presentation:

- Clear left color strip for MIDI vs audio.
- Readable track name with truncation instead of layout expansion.
- Compact mute, solo, record-arm, monitor, and device controls.
- Volume and pan controls that fit inside the lane without causing clipping.
- Strong selected state using border/fill contrast and the track identity color.

## Playlist

The arrangement should feel crisp and legible:

- Darken lane alternation slightly, but keep enough contrast to scan rows.
- Keep measure lines stronger than beat lines.
- Keep MIDI clips visible with selected-state contrast.
- Keep clip creation gestures unchanged: double-click creates a default MIDI clip, drag creates a length-based MIDI clip, and audio tracks ignore MIDI creation.
- Clamp created clips to non-negative beat positions and minimum one beat length.

## Inspector

The inspector remains a bottom device/clip strip.

Track mode:

- Show selected track tile.
- Show instrument section only for MIDI tracks.
- Show FX and output sections for all selected tracks.
- Keep empty states compact and clear.

MIDI clip mode:

- Show clip name, start, and length.
- Hide instrument, FX, and output sections while clip details are active.
- Provide an Open MIDI Editor action that opens or raises the MIDI editor for the selected clip.
- Avoid hidden groups leaving large empty gaps.

No-track mode:

- Show the track tile as "No Track Selected".
- Hide detail groups.
- Do not leave awkward empty group boxes visible.

## Browser

The browser should stay fast and searchable:

- Search should keep parent nodes visible when descendants match.
- Clearing search should restore visibility for all nodes.
- Matching branches should expand while searching.
- Top-level categories should remain readable with tighter margins and consistent item spacing.
- Plugin cache population behavior must not change.

## Behavior And Guardrails

- Do not change audio engine behavior.
- Do not change plugin scanning, loading, or cache semantics.
- Do not rename `.ui` object names referenced by `VeraCoreQT.cpp`.
- Do not change the Qt VS Tools installation name in the project file.
- Do not touch generated build artifacts as part of this UI pass.
- Preserve existing user or generated changes already in the dirty worktree.

## Testing And Verification

Manual verification is required because this repo has no automated tests.

Verify:

- The solution builds in Debug x64 using `D:\VS2026\MSBuild\Current\Bin\MSBuild.exe VeraCore.sln -p:Configuration=Debug -p:Platform=x64 -m`.
- The app opens maximized.
- Browser search filters and restores nodes correctly.
- Browser, Playlist, Inspector, and MIDI Editor View actions behave predictably.
- Track headers remain aligned with playlist lanes after scrolling and resizing.
- Selecting tracks and MIDI clips updates selected states and inspector content.
- MIDI clip double-click opens or raises the MIDI editor.
- Hidden inspector/browser panels show again with usable sizes.
- No UI text clips or overlaps in the main window at 1280 x 760 and maximized desktop size.

## Non-Goals

- No new FL-style channel rack.
- No full mixer view.
- No plugin hosting changes.
- No audio recording or audio clip editing changes.
- No full piano-roll redesign beyond opening/raising the existing MIDI editor.
