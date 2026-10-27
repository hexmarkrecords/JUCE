# Internal Audio Player

Add **Audio Player** from AudioPluginHost's existing internal-plugin menu, connect
its audio output to the destination under test, and connect a MIDI source to its
MIDI input. Use **Load New Sample** or drop one supported audio file onto the editor.
The initial state is stopped. A note-on starts the full file from frame zero;
the same note number toggles it off, and the next note-on restarts from zero.
Note numbers identify toggles across all MIDI channels. Other note numbers are
ignored while playing. Note-off (including velocity-zero note-on), sustain,
pressure, pitch bend and velocity do not change playback. Loading another file,
resetting or releasing the processor stops it.

Save the audio graph after choosing a file. The plugin saves its absolute file
path in its graph state and reloads that file on recall, even if its editor is
closed. Recall starts stopped; send a note-on to begin playback. The file itself
is not embedded, so it must remain at the saved path. An unavailable file leaves
the player silent and displays its path with an "Unable to load" label. The path
is retained when saving again, so restoring the file later allows another recall
to succeed. Choose another file with the existing load button to replace it.

This is a file reference in plugin state, not a numeric automation parameter.
AudioPluginHost calls state save/restore on the message thread. Restoration uses
the same complete-file loading and atomic handoff as manual selection; it adds
no decoding or filesystem access to the audio callback. A versioned XML payload
stores `filePath`; legacy empty state clears the selection, and malformed or
unknown-version state is ignored. An open editor receives change notifications
so its waveform and file-path label reflect the restored selection.

## Architecture and implementation

The existing implementation is `examples/Plugins/SamplerPluginDemo.h`:

- `SamplerAudioProcessor` creates up to 20 `MPESamplerVoice` instances in an
  `MPESynthesiser`. MIDI note handling is delegated to JUCE's MPE synthesiser.
- `MPESamplerVoice::noteStarted` sets velocity, frequency and position.
  `noteStopped` enables an exponential tail (`tailOff *= 0.9999`), which ends
  below 0.005. Its renderer interpolates samples and advances according to
  note frequency / sample centre frequency; its EOF path clears the voice.
- `Sample` decodes mono/stereo audio into memory, with a caller-supplied duration
  cap. The original processor supplies 10 seconds both initially and on load.
- `MainSamplerView` owns the asynchronous chooser. `FileAudioFormatReaderFactory`
  creates readers through an `AudioFormatManager`; `DataModel` changes reach
  `SamplerAudioProcessorEditor::sampleReaderChanged` and `setSample`.
- The Sampler constructs resources on the message thread, transfers commands
  through `CommandFifo`, and uses a try-lock to coordinate editor snapshots.
  `WaveformEditor` provides the thumbnail, ruler and position overlay.
- `InternalPluginFormat::InternalPluginFactory` in
  `Source/Plugins/InternalPlugins.cpp` constructs internal processors through
  `InternalPlugin`, which delegates host processor calls and descriptions.

`Source/Plugins/AudioPlayer.h` adds `AudioPlayerProcessor` and its small `Editor`.
It directly reuses `Sample`, the reader factory, `DataModel`, and `WaveformEditor`.
The chooser and drop handling follow the Sampler editor. The UI retains its
waveform and Load New Sample button, with a short instruction label replacing
irrelevant pitch/MPE/loop-region controls. No Sampler code was changed or
refactored. Only one factory entry and include were added to `InternalPlugins.cpp`.

Strict monophony is structural: there is one current file, one fractional cursor
and one `activeNote` integer (`-1` means stopped). No synthesiser, voices or
sampler envelope are instantiated. MIDI events split each block at their sample
positions, so start/stop timing is sample accurate. EOF uses modulo file length,
preserving fractional overshoot; interpolation wraps its second input frame to
zero too. The increment is file sample rate / host sample rate. MIDI pitch does
not transpose the file. Mono input duplicates to stereo, and stereo averages
when the output bus is mono. Float and double processing are supported.

Loading allocates and decodes on the message thread. A bounded atomic pointer
handoff publishes the newest load and returns the previous file to a message
thread timer for deletion. Rapid pending loads replace each other. Adoption
waits if a retired file has not yet been reclaimed. This avoids retaining the
Sampler command FIFO's many previous large files. The audio callback performs
no file reads, allocation, resource destruction, locks or logging. SysEx is
skipped before constructing a potentially allocating `MidiMessage`. Only the
playback-position atomic is read by the waveform UI; reader snapshots belong
to the message thread.

## Validation

The standalone `AudioPlayerTests.cpp` and `CMakeLists.txt` build with:

```sh
cmake -S extras/AudioPluginHost/Tests -B /tmp/juce-audio-player-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/juce-audio-player-tests --target AudioPlayerTests -j 4
ctest --test-dir /tmp/juce-audio-player-tests --output-on-failure
```

With this older JUCE checkout and Xcode's macOS 26 SDK, configuration also used:

```sh
-DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 -DCMAKE_CXX_FLAGS=-mmacosx-version-min=10.15
```

The compiler flag also reaches the recursively built juceaide helper, allowing
its existing deprecated macOS screenshot API to compile. No JUCE platform code
or existing build cache was changed. The complete AudioPluginHost Release target
also built successfully in `/tmp/juce-audio-player-host` with these flags.

Automated tests compare actual output with a generated 15.003-second stereo WAV:
initial silence, timed start, full duration beyond 10 seconds, EOF, 470+ loops
(over two hours of accelerated programme time), same-note stop, restart, rapid
toggles, other-note monophony, note-off and pitch-bend immunity, velocity-independent
level, channel-independent toggling, up/down sample-rate conversion, mono output,
double precision, invalid-file rejection, editor reopening, rapid file replacement,
and unload. The original Sampler is instantiated with its default cello asset,
then checked for audible start and its existing note-off tail ending in silence.
Additional state tests cover file-path round trips while playing, recall with no
editor, initial silence followed by exact playback from the beginning, restore
with an open editor, missing files remaining silent and retaining their saved
reference, malformed state, and legacy empty state. These tests and the full host
build passed.

## Limits and remaining manual validation

A real-time, long-running HexCast/network soak and interactive chooser/drop
exercise were not performed. The accelerated test verifies playback state and
samples, not network behaviour or device deadlines.

The complete decoded file resides in RAM. The shared Sample supports only its
first two channels and an integer frame count; empty files, invalid rates and
files exceeding `INT_MAX - 4` frames are rejected. Available memory limits
practical file size; loading is synchronous on the GUI thread, as in the Sampler.
File selections persist via graph state; playback position and active MIDI toggles
are not serialized. The original Sampler retains its existing state behaviour. Linear interpolation is intentionally simple and provides no
high-quality antialiasing. Discontinuous file endpoints or instant toggles can
click; no crossfade is added. If the message thread is busy, adopting a subsequent
load can be deferred until it reclaims the retired file; active playback continues.
EOF itself inserts no silence or envelope decay.
