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

## Implemented (commit "HLG capture path + HLG%/10-bit scales")
- HLG mode is automatic when `ovi.colorspace == VIDEO_CS_2100_HLG`; SDR path unchanged.
- common.c: capture `GS_RGBA16F` in `GS_CS_709_EXTENDED`; encode with
  `ConvertRGB_HLG` / `ConvertRGB_YUV2020_HLG` (common.effect, HLG code copied from
  libobs color.effect incl. inverse OOTF and >1000-nit EETF); stage `GS_R10G10B10A2`.
  Params: `obs_get_video_sdr_white_level()/10000`, `obs_get_video_hdr_nominal_peak_level()`,
  narrow/full range from `ovi.range`.
- `cm_surface_data.hlg/full_range/levels`; `cm_unpack_r10g10b10a2()` in common.h.
- waveform/histogram: analysis is 10-bit; display resolution is a property
  (`hdr_rows` / `hdr_cols`: 256 default = same source size as SDR, 4 codes per step;
  512; 1024). Properties `hdr_scale` (HLG % / 10-bit code) and `hdr_labels`;
  graticule marks + label layout (collision-avoiding) in `src/hdr-scale.c`. Labels are
  anti-aliased SDF text (`src/scope-text.c`, `data/scope-text.effect`, atlas
  `src/scope-font-atlas.h` from `tools/gen-scope-font.py`, Roboto Condensed Medium, OFL in
  `data/fonts/`). In the scope dock the source skips its labels (`cm_scope_labels_external`)
  and the dock draws them in pixel space at a fixed size (`wvs/his_draw_overlay_labels`).
  Cyan = reference lines (0%, 75% BT.2408 ref white, 100%; or 64/940).
- `src/hdr-lift-filter.c` + `data/hdr-lift.effect`: "HDR Lift" filter, renders its input
  in GS_RGBA16F / GS_CS_709_EXTENDED; modes Gain and Knee (quadratic, C1 at knee, maxRGB
  ratio, premultiplied-alpha aware). Replaces Compose SDR on HDR (480-nit cap) and
  obs-shaderfilter (observed clamping at SDR white on HDR sources in 2.6.0).
- Verified in OBS with a 10-bit HLG test file: bars read correctly 0-100%; >940 is
  clamped by OBS's own YUV_to_RGB (limited-range clamp) on input, not by the scope.
- vectorscope: decodes 10-bit (>>2) so it doesn't show garbage; graticule still 601/709.
- Known gaps: PQ canvas uses legacy SDR path; zebra/false colour/focus peaking
  bypass textures carry the HLG signal in HLG mode (untested); ROI display untested.

## Build / test
- No local toolchain. CI (`.github/workflows/main.yml`, "Plugin Build") runs on
  push to `hdr-*` branches and on manual dispatch. Use the
  `*-windows-obs32-x64` artifact.
- `.effect` files compile at plugin load, not in CI: a green build does not prove
  shaders are valid. Check the OBS log for effect compile errors.
- Shader-only iteration: edit the installed copy in
  `obs-plugins/.../data/obs-plugins/obs-color-monitor/*.effect`, restart OBS,
  then commit what works.
