# OpenScope PCM Encoder — Development History

This is a curated history of the OpenScope PCM Encoder. It records the changes that materially shaped the current application and deliberately omits short-lived experiments, diagnostic builds, abandoned control strategies, and intermediate UI tweaks.

## 0.2.x — Encoder core and hardware-compatible PAL PCM

The early 0.2 series established the encoder architecture and the PAL raster formats.

### Sony PCM-F1 / EIAJ

- Added Sony PCM-F1 16-bit and EIAJ 14-bit PAL PCM encoding.
- Established the hardware-compatible top-of-field line mapping with Control-H on the first visible PCM line.
- Corrected Sony PCM-F1 16-bit S-word packing while leaving 14-bit EIAJ framing unchanged.
- Refined PCM video levels and the per-line peak-white reference for real decoder compatibility.
- Set the normal horizontal PCM offset to 5 pixels.
- Added selectable pre-emphasis behaviour and the associated PCM metadata handling.

### Ham PCM

- Added the experimental Ham PCM format alongside Sony/EIAJ without replacing the standard modes.
- Implemented 48 kHz / 14-bit stereo operation with video-clock locking through the adaptive ASRC path.
- Added line coding, synchronization, scrambling, error protection and text/caption support.
- Added live switching between Sony PCM-F1, EIAJ and Ham PCM at PAL frame boundaries while keeping the required ASRC paths warm.

### Audio buffering and latency

- Reworked audio buffering from a simple fixed reserve into active ASRC-controlled buffering.
- Added user-selectable buffer targets, underrun indication and runtime diagnostics.
- Separated the audio/JIT reserve from the inherent 40 ms PAL/DeckLink frame period in the latency presentation.

### ASIO input

Version 0.2.74 replaced the Windows/WASAPI capture path with direct Steinberg ASIO input.

- ASIO drivers are enumerated directly by the application.
- Input runs at the driver's current hardware sample rate.
- Stereo samples are converted to float and feed both ASRC paths continuously.
- No extra frame-ahead audio buffer was introduced.
- The Steinberg ASIO SDK remains an external build dependency and is not bundled with the project.

By 0.2.84 the encoder used the actual frame-completion margin as the JIT control quantity rather than treating FIFO fill as a proxy for the DeckLink deadline.

## 0.3.x — JIT / ASRC control development

The 0.3 series concentrated on reducing latency while keeping DeckLink output reliable.

- Introduced a fast JIT loop to keep PCM frame production ahead of the DeckLink deadline.
- Added a slower ASRC correction path to absorb the long-term clock difference between the audio interface and PAL video output.
- Increased the acquisition range to allow large initial clock corrections while retaining a stable locked state.
- Added soft/hard-lock state and clearer graph feedback for ASRC trim and timing margin.
- Made the main window resizable and expanded the diagnostics needed to understand timing behaviour.

Many controller variants were evaluated during this period. Feed-forward trajectories, coarse/fine handover schemes and alternative phase controllers were development steps only and are intentionally not documented individually here.

## 0.4.x — Realtime architecture and stable buffer control

The 0.4 series turned the timing experiments into a cleaner realtime architecture.

### Event-driven realtime path

- Removed polling sleeps from the audio-to-DeckLink production path.
- ASIO callbacks publish audio availability and wake the DeckLink worker through a condition variable.
- Shutdown explicitly wakes blocked realtime workers.
- Minimized work performed on realtime threads and kept GUI activity out of the PCM production path.

### Centralized buffer control

- Consolidated buffer regulation into a single controller owned by `AsioCapture`.
- PCM-F1/EIAJ and Ham PCM share the same central clock correction while retaining separate ASRC FIFOs.
- Made control thresholds depend on the actual ASIO callback quantum rather than assuming a fixed buffer period.
- Added robust startup/recovery holdoff so early transient measurements do not immediately perturb the controller.

### Clock-domain measurement

- Added direct measurement of the relative ASIO and Blackmagic clock domains.
- Improved sample accounting and timing snapshots so rate estimates use matched sample-count/timestamp data.
- Moved the ASRC FIFO measurement to the actual Blackmagic frame-completion callback instant.
- FIFO depths are published atomically, avoiding locks in the DeckLink callback.

### Final direction of the controller

A large number of slope, nudge, micro-nudge and PID variants were tested during 0.4 development. The lasting design principles were:

- measure buffer position at a meaningful hardware event;
- derive thresholds from the real ASIO quantum;
- use a strong but bounded acquisition correction when far from target;
- preserve the learned steady clock offset instead of repeatedly resetting it;
- keep the control path continuous and independent of GUI timing.

The intermediate diagnostic and abandoned controller builds are omitted from this history.

## 0.5.x — Video output, operational UI and metering

The 0.5 series focused on the parts visible during normal on-air use.

### Video shaping and advanced controls

- Pulse shaping became permanently enabled for PCM output.
- Video bandwidth remained adjustable, with 5.0 MHz as the default when no setting is stored.
- Video bandwidth, horizontal offset and timing-related settings were moved to an Advanced page.
- The normal horizontal offset is 5 pixels.

### Operational UI

- The application returned to a compact main-window layout.
- The Start button doubles as the real ON-AIR indicator and only shows ON-AIR while DeckLink output is actually running.
- Encoder, audio and advanced controls were reorganized to keep normal operation simple while retaining detailed diagnostics.

### Broadcast-style PPM meters

- Replaced the earlier meters with RTW-inspired 201-segment stereo PPM meters.
- Range is -60 dBFS to 0 dBFS with a red final 1 dB region.
- Added immediate attack, fast decay and a discrete peak-hold segment.
- Added a non-linear broadcast-style scale with more resolution near full scale.
- Added a clean meter-only/fullscreen presentation for monitoring.

### ASIO Quantum Leap — 0.5.23

Version 0.5.23 removed the remaining fixed millisecond assumptions from the buffer controller.

- Control thresholds scale from the actual ASIO driver quantum `Q`.
- The proven behaviour of a 64-sample / 48 kHz ASIO buffer is preserved.
- The default ASRC target is derived from approximately `4 × Q`, rounded to whole milliseconds.
- The UI shows the actual ASIO buffer size, callback period and derived target.
- The ASRC target remains directly adjustable in whole milliseconds.

Examples at 48 kHz:

- 64 samples: `Q = 1.333 ms` → default target about `5 ms`
- 128 samples: `Q = 2.667 ms` → default target about `11 ms`

## 0.9.0 — First public GitHub release

Version 0.9.0 is the first deliberately packaged public release of OpenScope PCM Encoder.

It combines the mature parts of the previous development line:

- Sony PCM-F1 16-bit and EIAJ 14-bit PAL PCM encoding;
- Ham PCM support;
- direct ASIO audio capture;
- DeckLink PAL output;
- adaptive sample-rate conversion between independent audio and video clocks;
- low-latency, quantum-aware buffer control;
- always-on PCM pulse shaping with adjustable video bandwidth;
- compact operational UI with ON-AIR indication;
- RTW-inspired stereo PPM metering and meter-only display;
- persistent settings and extensive diagnostics for development and commissioning.

The source tree was cleaned for publication, external SDKs remain external dependencies, and the project is released under GPL-3.0.

---

The original development produced many temporary delta builds while timing and control behaviour were being measured on real hardware. Those builds were valuable during development but do not represent distinct supported features, so they are intentionally excluded from this public history.
