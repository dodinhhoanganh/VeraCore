# Piano Roll MIDI Editor Design

## Goal

VeraCore should support a dedicated MIDI editor window in the style of FL Studio's piano roll, with Cubase-style keyswitch handling. The editor must be opened from a MIDI clip, live as its own window, and support fast note entry and editing without turning the main arrangement into a dense editor surface.

## Approved Direction

Use a dedicated floating piano-roll window.

- Open the editor from a MIDI clip in the arrangement.
- Keep it separate from the main inspector and arrangement views.
- Make the editor the primary place for note entry, note deletion, keyswitches, and pitch transposition.

## Editor Surface

The piano roll window should contain:

- A top bar with the active clip name, track name, snap value, and basic editor actions.
- A left piano keyboard strip with note names.
- A central note grid with beats on X and pitch on Y.
- A bottom keyswitch lane for articulations and expression switching.
- Optional future lanes such as velocity or controllers, but not in the first pass.

The note grid should reuse the same beat/grid logic as the arrangement so clip timing feels consistent across both editors.

## Note Editing Behavior

The following interactions are required:

- Left click on empty grid: create a note.
- Right click on a note: delete that note.
- Drag an existing note: move it.
- Drag a note edge: resize it.
- Double-click an existing note: edit or open note properties later, if needed.

The editor must remember the last note length used during the current editing session. When the user creates a new note, the editor should default to the previous note length unless the user actively changes it.

## Transpose And Nudge Rules

Keyboard shortcuts should behave like fast MIDI editing shortcuts:

- `Ctrl+Up` and `Ctrl+Down`: transpose the selected note(s) by 1 octave.
- `Shift+Up` and `Shift+Down`: transpose the selected note(s) by 1 semitone.

These shortcuts apply only when the piano-roll window is focused.

Transpose shortcuts apply to the current note selection. If nothing is selected, they apply to the last note created in the current editor session.

## Keyswitch Lane

The bottom lane acts as a Cubase-style keyswitch lane.

- Keyswitches are displayed as special low-pitch note markers or lane events.
- Keyswitch notes should be visually distinct from normal musical notes.
- The lane should support click-to-create and click-to-delete behavior consistent with the main note grid.
- Keyswitch notes must not be treated as normal pitched musical notes when the editor later exports or routes the data.

The first pass may treat keyswitches as a separate note subtype in the editor model, even if full articulation playback is not yet wired end to end.

## Data Model

The editor should not store notes directly in widget state.

Recommended model shape:

- `MidiClip` owns a note collection and clip timing.
- Each note stores pitch, start beat, length, velocity, selection state, and whether it is a keyswitch.
- The piano-roll window edits a single active clip at a time.

This keeps the editor window independent from the arrangement surface and makes later MIDI export, copy/paste, and quantization easier to add.

## Integration With Arrangement

The arrangement should open the piano-roll window from an existing MIDI clip.

- Double-clicking a MIDI clip opens the editor.
- The arrangement should keep the clip selected while the editor is open.
- Closing the editor should not destroy clip data.
- The editor should update the clip model so the arrangement can reflect note count or clip state later if needed.

The arrangement itself should stay clip-oriented and not gain full note-editing behavior.

## Error Handling And Guardrails

- Ignore note creation on non-MIDI clips.
- Clamp notes to valid pitch and beat ranges.
- Prevent zero-length notes.
- Keep keyswitch notes inside the dedicated lane range.
- Preserve existing track selection, browser filtering, and transport behavior.
- Keep shortcut handling scoped to the piano-roll window so the main window does not intercept editor-specific note commands.

## Testing And Verification

Manual verification is expected because the repo currently has no automated UI tests.

Verify:

- Double-clicking a MIDI clip opens the dedicated piano-roll window.
- Left click draws a note in the editor.
- Right click deletes a note.
- Newly drawn notes reuse the previous note length.
- `Ctrl+Up` and `Ctrl+Down` transpose by an octave.
- `Shift+Up` and `Shift+Down` transpose by a semitone.
- Keyswitch notes can be created and removed in the dedicated lane.
- The main arrangement still behaves as a clip timeline, not a note editor.

## Non-Goals

- No audio-region editor.
- No full Cubase articulation map management in the first pass.
- No advanced piano-roll tools such as chord brushes, step sequencer mode, scale highlighting, or per-note automation.
- No change to track routing or plugin hosting behavior.
