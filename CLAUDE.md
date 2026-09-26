# Fork: HDR/HLG-aware scopes for obs-color-monitor

Branch `hdr-hlg`, based on upstream `main` (53d4bc0). Upstream tag 0.9.6 is on a
release branch, not on main; the scope pipeline files (common.c, waveform.c,
histogram.c, *.effect) are identical in both.

## Milestone 1 scope
- Target: OBS 32, Windows x64, HLG / Rec.2100 canvas (`GS_CS_709_EXTENDED`).
- Waveform + histogram only. Vectorscope, zebra, false colour: out of scope.
- Values above SDR white must survive to binning, never clip.
- Scale modes: HLG signal %, 10-bit narrow-range code values (64-940), later nits.
- SDR behaviour for existing users must remain unchanged when the canvas is SDR.

## Hard constraints
- No 8-bit quantisation anywhere before binning (no GS_BGRA in the analysis path).
- Render the source with `gs_texrender_begin_with_color_space()` into a
  `GS_RGBA16F` target, honouring `obs_source_get_color_space()` / canvas space.
- The linear -> HLG mapping must invert OBS's own HLG output encoding, using the
  live `obs_get_video_sdr_white_level()` / `obs_get_video_hdr_nominal_peak_level()`
  values. Verify against libobs `data/format_conversion.effect` / `color.effect`
  before implementing; do not hardcode 300/1000 nits.
- BT.2020 matrix for Y'CbCr when canvas is Rec.2100 (current code only has 601/709).

## Current pipeline (baseline, as found)
1. `common.c: render_target_to_texrender` - source/program rendered into a
   `GS_BGRA` texrender via plain `gs_texrender_begin` (sRGB space). <- HDR lost here.
2. `common.c: render_rgb_yuv` - second `GS_BGRA` texrender; RGB copy and/or
   YUV via `data/common.effect` (601/709 only, full-range).
3. `common.c: prepare_stagesurface` - `GS_BGRA` stagesurface, CPU readback on the
   `color-monitor` pipeline thread (`cm_pipeline_thread_loop`).
4. `waveform.c: wvs_draw_waveform` - CPU loop reads uint8 B,G,R,A and uses the byte
   directly as the row index; `WV_SIZE 256`; saturating uint8 accumulator
   uploaded as `GS_BGRX`.
5. `histogram.c: his_draw_histogram` - same uint8 read, `HI_SIZE 256` bins.
6. `data/waveform.effect`, `histogram.effect` - display only; graticule is a linear
   0..256 vertex buffer in C.
ROI sources (`roi.c`) share `cm.texrender`, so they inherit whatever common.c does.

## Planned design
- Capture texrenders -> `GS_RGBA16F`, begin_with_color_space(GS_CS_709_EXTENDED)
  when the canvas is HDR.
- New technique in common.effect: linear 709_EXTENDED -> BT.2020 primaries -> HLG
  OETF (+ inverse of OBS's scaling) -> optional BT.2020 Y'CbCr -> narrow-range.
- Stage the encoded result as `GS_R10G10B10A2` (exact 10-bit codes, half the
  readback of RGBA16F) or `GS_RGBA16F` if superwhite beyond code 1023 is needed.
- CPU binning: 1024 levels (WV_SIZE/HI_SIZE become runtime values), uint16
  accumulators; waveform texture format changes accordingly.
- Graticule labels per scale mode.

## Build / test
- No local toolchain. CI (`.github/workflows/main.yml`, "Plugin Build") runs on
  push to `hdr-*` branches and on manual dispatch. Use the
  `*-windows-obs32-x64` artifact.
- `.effect` files compile at plugin load, not in CI: a green build does not prove
  shaders are valid. Check the OBS log for effect compile errors.
- Shader-only iteration: edit the installed copy in
  `obs-plugins/.../data/obs-plugins/obs-color-monitor/*.effect`, restart OBS,
  then commit what works.
