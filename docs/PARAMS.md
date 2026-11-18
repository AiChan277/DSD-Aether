# AETHER-2.5D Parameter Registry (`REQ-UI-010`)

Generated automatically from [`src/ui/params/param_registry.hpp`](../src/ui/params/param_registry.hpp) (`tools/gen_params_doc.py`).
Do not edit manually; every UI Inspector row, `settings.toml` / `project.toml` key, `/aether/param/<id>` OSC endpoint, Command Palette entry, and documentation table is derived from this single source.

| ID | Group | Label | Type | Owner | Default | Range | Unit | OSC Endpoint | Flags |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `avatar.transform.pos_x` | Avatar | Position X | `float` | T2 | `0.0` | `[-5.0, 5.0]` | m | `/aether/param/avatar.transform.pos_x` | persist, project, osc, graph, undoable |
| `avatar.transform.pos_y` | Avatar | Position Y | `float` | T2 | `0.0` | `[-5.0, 5.0]` | m | `/aether/param/avatar.transform.pos_y` | persist, project, osc, graph, undoable |
| `avatar.transform.pos_z` | Avatar | Position Z | `float` | T2 | `0.0` | `[-5.0, 5.0]` | m | `/aether/param/avatar.transform.pos_z` | persist, project, osc, graph, undoable |
| `avatar.transform.rot_x` | Avatar | Rotation X | `float` | T2 | `0.0` | `[-180.0, 180.0]` | deg | `/aether/param/avatar.transform.rot_x` | persist, project, osc, graph, undoable |
| `avatar.transform.rot_y` | Avatar | Rotation Y | `float` | T2 | `0.0` | `[-180.0, 180.0]` | deg | `/aether/param/avatar.transform.rot_y` | persist, project, osc, graph, undoable |
| `avatar.transform.rot_z` | Avatar | Rotation Z | `float` | T2 | `0.0` | `[-180.0, 180.0]` | deg | `/aether/param/avatar.transform.rot_z` | persist, project, osc, graph, undoable |
| `avatar.transform.scale` | Avatar | Uniform Scale | `float` | T2 | `1.0` | `[0.25, 4.0]` | x | `/aether/param/avatar.transform.scale` | persist, project, osc, graph, undoable |
| `sim.spring_stiffness` | Physics | Spring Stiffness | `float` | T2 | `1.0` | `[0.1, 3.0]` | x | `/aether/param/sim.spring_stiffness` | persist, project, osc, graph, undoable |
| `sim.spring_damping` | Physics | Spring Damping | `float` | T2 | `1.0` | `[0.1, 3.0]` | x | `/aether/param/sim.spring_damping` | persist, project, osc, graph, undoable |
| `sim.gain` | Physics | Expression Gain | `float` | T2 | `1.0` | `[0.0, 2.0]` | x | `/aether/param/sim.gain` | persist, project, osc, graph, undoable |
| `sim.ik_weight` | Physics | IK Blend Weight | `float` | T2 | `1.0` | `[0.0, 1.0]` | — | `/aether/param/sim.ik_weight` | persist, project, osc, graph, undoable |
| `render.flatness` | Render | Live2D Flatness | `float` | T3 | `0.75` | `[0.0, 1.0]` | — | `/aether/param/render.flatness` | persist, project, osc, graph, undoable |
| `render.light_scale` | Render | Light Scale | `float` | T3 | `1.0` | `[0.1, 4.0]` | x | `/aether/param/render.light_scale` | persist, project, osc, graph, undoable |
| `render.key_azimuth_deg` | Render | Key Azimuth | `float` | T3 | `32.0` | `[-180.0, 180.0]` | deg | `/aether/param/render.key_azimuth_deg` | persist, project, osc, graph, undoable |
| `render.key_elevation_deg` | Render | Key Elevation | `float` | T3 | `26.0` | `[-85.0, 85.0]` | deg | `/aether/param/render.key_elevation_deg` | persist, project, osc, graph, undoable |
| `render.key_intensity` | Render | Key Intensity | `float` | T3 | `1.00` | `[0.0, 6.0]` | EV | `/aether/param/render.key_intensity` | persist, project, osc, graph, undoable |
| `render.fill_intensity` | Render | Fill Intensity | `float` | T3 | `0.28` | `[0.0, 4.0]` | EV | `/aether/param/render.fill_intensity` | persist, project, osc, graph, undoable |
| `render.rim_intensity` | Render | Rim Intensity | `float` | T3 | `0.35` | `[0.0, 6.0]` | EV | `/aether/param/render.rim_intensity` | persist, project, osc, graph, undoable |
| `render.outline_scale` | Render | Outline Scale | `float` | T3 | `1.0` | `[0.0, 3.0]` | x | `/aether/param/render.outline_scale` | persist, project, osc, graph, undoable |
| `render.bloom_intensity` | Render | Premul Bloom | `float` | T3 | `0.06` | `[0.0, 1.5]` | — | `/aether/param/render.bloom_intensity` | persist, project, osc, graph, undoable |
| `render.rt_enabled` | Render | Ray Tracing | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | `/aether/param/render.rt_enabled` | persist, project, osc, undoable |
| `render.denoiser` | Render | Denoiser Backend | `enum` | T3 | `0.0` | `[0.0, 5.0]` | — | — | persist, app, undoable |
| `render.backend` | Render | Render Backend | `enum` | T3 | `0.0` | `[0.0, 2.0]` | — | — | persist, app, undoable |
| `render.target_fps` | Render | Target FPS | `enum` | T3 | `1.0` | `[0.0, 3.0]` | — | — | persist, app, undoable |
| `render.vsync` | Render | VSync | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `render.background_eco` | Render | Background Eco | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `render.resolution_scale_pct` | Render | Render Scale | `int` | T3 | `100.0` | `[50.0, 200.0]` | % | — | persist, app, undoable |
| `render.aa_mode` | Render | Anti-Aliasing | `enum` | T3 | `1.0` | `[0.0, 2.0]` | — | — | persist, app, undoable |
| `render.face_normal_flatten` | Render | Face Flatness | `float` | T3 | `0.85` | `[0.0, 1.0]` | — | `/aether/param/render.face_normal_flatten` | persist, project, osc, undoable |
| `render.cel_band_sharpness` | Render | Cel Feather | `float` | T3 | `0.025` | `[0.005, 0.25]` | — | `/aether/param/render.cel_band_sharpness` | persist, project, osc, undoable |
| `render.shadow_warm_cool` | Render | Shadow Hue | `float` | T3 | `0.75` | `[0.0, 1.0]` | — | `/aether/param/render.shadow_warm_cool` | persist, project, osc, undoable |
| `render.iro_tsuke_strength` | Render | Iro-Tsuke Ink | `float` | T3 | `0.85` | `[0.0, 1.0]` | — | `/aether/param/render.iro_tsuke_strength` | persist, project, osc, undoable |
| `render.crease_strength` | Render | Crease Ink | `float` | T3 | `0.75` | `[0.0, 1.0]` | — | `/aether/param/render.crease_strength` | persist, project, osc, undoable |
| `render.hair_highlight` | Render | Hair Highlight | `float` | T3 | `0.85` | `[0.0, 1.5]` | — | `/aether/param/render.hair_highlight` | persist, project, osc, undoable |
| `render.rim_step_strength` | Render | Rim Cutout | `float` | T3 | `0.65` | `[0.0, 1.5]` | — | `/aether/param/render.rim_step_strength` | persist, project, osc, undoable |
| `render.lighting_mode` | Render | Lighting Mode | `enum` | T3 | `1.0` | `[0.0, 2.0]` | — | — | persist, app, undoable |
| `render.indirect_bleed_strength` | Render | Pre-Cel GI Bleed | `float` | T3 | `0.55` | `[0.0, 1.5]` | — | `/aether/param/render.indirect_bleed_strength` | persist, project, osc, undoable |
| `render.rim_depth_offset_px` | Render | Rim Depth Offset | `float` | T3 | `2.8` | `[0.0, 6.0]` | px | `/aether/param/render.rim_depth_offset_px` | persist, project, osc, undoable |
| `render.filter_2d_pro` | Render | 2D Filter Pro | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `render.filter_kuwahara` | Render | Painterly Flatten | `float` | T3 | `0.55` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `render.filter_dog_ink` | Render | DoG Ink AA | `float` | T3 | `0.60` | `[0.0, 1.5]` | — | — | persist, project, undoable |
| `render.filter_paper_warmth` | Render | Anime Paper Warmth | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `render.filter_paper_warmth_amt` | Render | Warmth Strength | `float` | T3 | `0.25` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `render.circadian_enabled` | Render | Circadian Lighting | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `render.circadian_time_min` | Render | Time Override | `int` | T3 | `-1.0` | `[-1.0, 1439.0]` | min | — | persist, project, undoable |
| `rnd.debug.force_device_lost` | Diagnostics | Force Device Lost | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | runtime |
| `tracking.ep_mode` | Tracking | Execution Provider | `enum` | T1 | `0.0` | `[0.0, 4.0]` | — | — | persist, app |
| `tracking.smoothing_cutoff_hz` | Tracking | Filter Cutoff | `float` | T2 | `1.2` | `[0.2, 10.0]` | Hz | `/aether/param/tracking.smoothing_cutoff_hz` | persist, project, osc, undoable |
| `tracking.camera_fps` | Tracking | Camera Target FPS | `int` | T1 | `60.0` | `[30.0, 60.0]` | fps | — | persist, app |
| `tracking.anti_flicker` | Tracking | Anti-Flicker | `enum` | T1 | `2.0` | `[0.0, 2.0]` | — | — | persist, app |
| `tracking.exposure_lock` | Tracking | Lock Exposure | `bool` | T1 | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `tracking.synthetic_camera` | Tracking | Synthetic Feed | `bool` | T1 | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `tracking.audio_assist` | Tracking | Audio Lip-Sync | `bool` | T2 | `1.0` | `[0.0, 1.0]` | — | — | persist, app |
| `tracking.mic_gain` | Tracking | Mic Gain | `float` | T2 | `1.25` | `[0.10, 4.00]` | x | — | persist, app |
| `tracking.mic_noise_gate` | Tracking | Mic Noise Gate | `float` | T2 | `-48.0` | `[-72.0, -18.0]` | dB | — | persist, app |
| `tracking.hand_enabled` | Tracking | Hand Tracking | `bool` | T1 | `0.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `tracking.npu_bounds_enabled` | Tracking | NPU Safe-Zone Box | `bool` | T1 | `1.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `tracking.npu_anti_clip_enabled` | Tracking | Predictive Anti-Clip | `bool` | T1 | `1.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `tracking.npu_safe_box_preview` | Tracking | Show Safe Box | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `tracking.npu_bounds_x` | Tracking | Safe Box Width X | `float` | T1 | `0.35` | `[0.08, 1.20]` | m | — | persist, app, undoable |
| `tracking.npu_bounds_y` | Tracking | Safe Box Height Y | `float` | T1 | `0.22` | `[0.08, 1.00]` | m | — | persist, app, undoable |
| `tracking.npu_bounds_z` | Tracking | Safe Box Depth Z | `float` | T1 | `0.30` | `[0.08, 1.20]` | m | — | persist, app, undoable |
| `tracking.npu_soft_elasticity` | Tracking | Wall Elasticity | `float` | T1 | `0.35` | `[0.05, 0.95]` | — | — | persist, app, undoable |
| `tracking.npu_max_pitch_deg` | Tracking | Max Head Pitch | `float` | T1 | `42.0` | `[15.0, 78.0]` | deg | — | persist, app, undoable |
| `tracking.npu_anti_clip_margin` | Tracking | Anti-Clip Margin | `float` | T1 | `0.035` | `[0.005, 0.150]` | m | — | persist, app, undoable |
| `output.resolution` | Output | Resolution Preset | `enum` | T4 | `1.0` | `[0.0, 3.0]` | — | — | persist, project, undoable |
| `output.fps` | Output | Output Frame Rate | `int` | T4 | `60.0` | `[30.0, 120.0]` | fps | — | persist, project, undoable |
| `output.premul_alpha` | Output | Premultiplied Alpha | `bool` | T4 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `output.non_alpha_bg` | Output | Opaque Backdrop | `enum` | T4 | `0.0` | `[0.0, 2.0]` | — | — | persist, project, undoable |
| `output.spout_enabled` | Output | Spout2 Sender | `bool` | T4 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `output.spout_cpu_fallback` | Output | Spout CPU Sharing | `bool` | T4 | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `output.vcam_enabled` | Output | Virtual Camera | `bool` | T4 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `output.ndi_enabled` | Output | NDI Stream | `bool` | T4 | `0.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `output.ndi_format` | Output | NDI Pixel Format | `enum` | T4 | `0.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `output.osc_enabled` | Output | OSC Telemetry | `bool` | T4 | `1.0` | `[0.0, 1.0]` | — | — | persist, app |
| `output.ws_enabled` | Output | WebSocket Control | `bool` | App | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `dsp.user_offset_ms` | DSP | DSP Audio Offset | `float` | T4 | `0.0` | `[-150.0, 150.0]` | ms | `/aether/param/dsp.user_offset_ms` | persist, project, osc, undoable |
| `app.ui_scale_pct` | App | UI Scale | `int` | App | `100.0` | `[75.0, 200.0]` | % | — | persist, app |
| `app.reduce_motion` | App | Reduce Motion | `bool` | App | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `app.high_contrast` | App | High Contrast | `bool` | App | `0.0` | `[0.0, 1.0]` | — | — | persist, app |
| `app.telemetry_paused` | App | Pause Telemetry | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | runtime |
| `app.checkerboard_bg` | App | Alpha Checkerboard | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `app.orbit_orb_visible` | App | Light Orbit Orb | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, project, undoable |
| `app.skeleton_heatmap` | App | Confidence Heatmap | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | undoable |
| `app.viewport_aspect` | App | Viewport Aspect | `enum` | T3 | `0.0` | `[0.0, 3.0]` | — | — | persist, app, undoable |
| `app.tracking_hud_visible` | App | Tracking HUD | `bool` | T3 | `1.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `app.tracking_hud_collapsed` | App | HUD Pill Mode | `bool` | T3 | `0.0` | `[0.0, 1.0]` | — | — | persist, app, undoable |
| `app.camera_transition_speed` | App | Preset Ease | `float` | T3 | `0.22` | `[0.00, 1.00]` | s | — | persist, app, undoable |
