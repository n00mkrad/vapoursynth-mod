# VSPipe NUT Feasibility (RGB/YUV/Gray PoC)

## Goal
Add native NUT output support to `vspipe` as a pipe-friendly alternative to Y4M, with support for RGB, YUV, and Gray video formats on Windows.

## Result
Feasible with low integration risk.

The `vspipe` architecture already exposes clean insertion points:
- CLI container parser (`-c/--container`)
- Video container initialization
- Per-frame header write path in the output callback
- Existing raw plane write path for payload data

## Why an in-tree writer
- Bundled `src/nut/` reference code is useful for protocol details but not ideal for direct integration in this PoC.
- A compact dedicated writer for single-stream video avoids C99/MSVC integration friction and keeps the change isolated.

## Interop findings used for this PoC
- Existing `vspipe` frame payload output already matches planar expectations for NUT rawvideo families:
  - RGB uses existing GBR plane order remap.
  - YUV and Gray use native plane order.
- A minimal NUT file is decodable by FFmpeg/ffprobe when it contains:
  - NUT file ID string
  - Main header packet
  - Stream header packet
  - Initial syncpoint packet
  - Frame packets
- Syncpoint `back_ptr` values must be valid and relative to the current syncpoint position; emitting constant zero back-pointers on all syncpoints is not robust.
- A trailing index is not required for pipe use, though demuxers can print a warning when absent.

## v1 constraints
- Video only for NUT container path.
- Alpha side output unsupported in v1.
- Supported formats:
  - RGB: unchanged from the initial PoC mapping.
  - YUV: planar `4:2:0`, `4:2:2`, `4:4:4`, integer `8/9/10/12/14/16` bit.
    - 8-bit uses legacy compatibility tags (`I420`, `422P`, `444P`).
    - >8-bit uses `Y3` generic scheme tags.
  - Gray: integer `8/9/10/12/14/16` bit.
    - 8-bit uses legacy compatibility tag (`Y800`).
    - >8-bit uses `Y1` generic scheme tags.
- YUV/Gray float formats are not supported in v1.
- VFR is supported through frame properties (`_DurationNum/_DurationDen`, `_AbsoluteTime`) with CFR fallback when durations are missing.

## Non-goals in v1
- NUT audio muxing.
- NUT alpha stream output.
- Trailing index writing or header repetition strategy changes.

## Expected v1 behavior
- `-c nut` succeeds for supported RGB/YUV/Gray clips with no alpha output.
- PTS is written from frame timing properties when available, so VFR clips preserve per-frame timing.
- Unsupported format families, unsupported YUV subsampling, unsupported bit depths, and unsupported YUV/Gray sample types fail with explicit error messages.
- Audio with `-c nut` fails with an explicit v1 limitation message.
- Existing `y4m`, `wav`, `w64`, and raw output behavior is unchanged.
