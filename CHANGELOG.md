# Changelog

The detailed development history is preserved in `docs/history/`.

## 0.9.0

- First public GitHub release.
- Promoted the application version from the development 0.5.x series to 0.9.0 to mark the current feature-complete pre-1.0 state.
- Includes PCM-F1 / EIAJ encoding, ASIO capture, DeckLink PAL output, adaptive ASRC/buffer control, advanced video controls and RTW/MUDTW-style meters.

## 0.5.23

- Buffer-control thresholds scale from the actual ASIO driver quantum instead of fixed millisecond constants.
- Default ASRC target is derived from four ASIO quanta.
- Advanced diagnostics show ASIO buffer size and the derived default target.
- ASRC target slider uses direct whole milliseconds.

## 0.5.x

The 0.5 series added and refined the advanced video controls, on-air UI state, RTW/MUDTW-style 201-segment meters, fullscreen meter presentation, scale geometry and ASIO-quantum-aware latency behaviour.

## Earlier versions

See `docs/history/` for the complete incremental notes covering PCM mode support, EIAJ control-H, pre-emphasis, DeckLink raster mapping, ASIO capture, ASRC timing control and diagnostic experiments.
