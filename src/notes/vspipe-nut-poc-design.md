# VSPipe NUT PoC Design (v1)

## Scope
- Add `-c nut` to `vspipe`.
- Implement a minimal in-tree NUT mux writer for single-stream RGB/YUV/Gray video.
- Keep stream output pipe-oriented with no trailing index generation.

## Integration points
- `vspipe/vspipe.cpp`
  - Added `VSPipeHeaders::NUT`.
  - Extended container parsing and help text.
  - Added NUT validation in video initialization.
  - Added per-frame NUT frame header emission in the frame output callback.
  - Added explicit v1 rejection path for audio + NUT.
- `vspipe/nut.h`
  - NUT writer API and format tag resolution interface.
- `vspipe/nut.cpp`
  - NUT packet writing, syncpoint writing, and format-to-fourcc mapping.

## NUT packets written
- File ID string: `nut/multimedia container\0`
- Main header packet
- Stream header packet
- Initial syncpoint packet
- Frame headers before each frame payload
- Syncpoint `back_ptr_div16` is derived from the distance to the previous syncpoint start, matching reference muxer behavior.

Header packet/footer layout follows the reference behavior:
- Startcode (64-bit big-endian)
- `forward_ptr` as NUT varint
- Optional header checksum when required
- Payload
- Payload CRC32 footer (NUT polynomial behavior)

## Frame strategy
- One NUT packet per VapourSynth frame.
- One stream (`stream_id = 0`).
- Monotonic PTS in frame order.
- PTS source:
  - Prefer `_AbsoluteTime` frame property when present.
  - Advance with `_DurationNum/_DurationDen` frame duration converted to NUT ticks.
  - Fallback to CFR-derived duration ticks when duration properties are missing.
- Keyframe flag set on every frame for v1 simplicity.
- Frame payload uses existing `vspipe` raw plane output behavior:
  - RGB planes in current GBR remap order.
  - YUV/Gray planes in native plane order.

## Rawvideo fourcc mapping used
- RGB family (existing mapping, unchanged):
  - Integer: `G3[0][8]`, `G3[0][9]`, `G3[0][10]`, `G3[0][12]`, `G3[0][14]`, `G3[0][16]`
  - Float: `G3[0][17]` (16-bit float), `G3[0][33]` (32-bit float)
- Gray family (new):
  - Integer 8-bit: `Y800` (legacy compatibility tag)
  - Integer >8-bit: `Y1[0][9/10/12/14/16]`
- YUV family (new):
  - Integer 8-bit:
    - 4:2:0: `I420` (legacy compatibility tag)
    - 4:2:2: `422P` (legacy compatibility tag)
    - 4:4:4: `444P` (legacy compatibility tag)
  - Integer >8-bit:
    - 4:2:0: `Y3[11][9/10/12/14/16]`
    - 4:2:2: `Y3[10][9/10/12/14/16]`
    - 4:4:4: `Y3[0][9/10/12/14/16]`

## v1 validation rules in `vspipe`
- `-c nut` requires:
  - Video output
  - no alpha output node
  - format accepted by `VSPipeNUTWriter::getVideoFourCC(...)`
- Accepted families:
  - RGB (existing behavior)
  - Gray integer `8/9/10/12/14/16`
  - YUV planar `420/422/444`, integer `8/9/10/12/14/16`
- Rejected:
  - Audio output with NUT
  - YUV/Gray float formats
  - Unsupported YUV subsampling or bit depth

## Comparison to existing Y4M output
- Current status is not full feature parity with Y4M yet.
- What NUT currently matches:
  - Video-only container path.
  - No alpha output.
  - YUV planar `420/422/444` support for integer `8/9/10/12/14/16`.
  - Gray integer `8/9/10/12/14/16`.
- What NUT is still missing compared to Y4M:
  - YUV subsampling variants that Y4M accepts in current code: `410`, `411`, `440`.
  - YUV float-format path that Y4M currently accepts.
  - Gray float-format path is intentionally not implemented in NUT v1.
  - NUT currently enforces fixed known FPS, while Y4M path does not apply the same explicit check.
- What NUT is better at than Y4M:
  - RGB support (including RGB integer and RGB float mappings), which Y4M output path does not support.
  - Rich per-frame container metadata and checksummed packet structure instead of plain text frame markers.

## Build wiring
- Meson: `src/vspipe/nut.cpp` included in `vspipe` executable sources.
- MSVC project: `nut.cpp` and `nut.h` included in `VSPipe.vcxproj` and filters.

## Known limitations for future work
- No audio/subtitle muxing yet.
- No alpha stream support yet.
- No index writing or periodic header repetition yet.
