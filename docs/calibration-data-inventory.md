# Sensor Calibration Data Inventory

Available image data for noise characterization (no formal calibration session).

## GoPro HERO10 Black (5568×4176, 14-bit)

### Night Timelapse — Best for FPN characterization
- **Path**: `/Volumes/Photos/DavidsPics/gopro_raw/2022-07-27__GOPRO_TL/`
- **Files**: 259 GPR frames (G0011779–G0012037)
- **Settings**: All 30-second exposures, ISO 235–445
- **Time span**: 10:36 PM – 12:55 AM (2.5 hours)
- **Use**: Average multiple frames → FPN map. Frame-to-frame variation → temporal noise.
- **Darkest frames**: G0011779 (ISO 235, 9.6 MB), G0011781 (ISO 239)

### Sunset-to-Dark Ramp — Best for photon transfer curve
- **Path**: `/Volumes/Photos/DavidsPics/gopro_raw/2022-07-25__100GOPRO_TL3/`
- **Files**: 1,408 GPR frames
- **Settings**: 1/151s ISO 100 → 2s ISO 800 (automatic exposure ramp)
- **Time span**: 8:56 PM – 10:57 PM (sunset to full darkness)
- **Use**: Natural PTC dataset. Last ~456 frames (ISO 800) are darkest.

### High-ISO Daylight
- **Path**: `/Volumes/Photos/DavidsPics/gopro_raw/2022-06-05__GOPRO_TL_1/G0030214.GPR`
- **Settings**: ISO 1600, 1/212s
- **Use**: High-noise characterization (noise_scale = 0.0025)

## Hasselblad X2D 100C (11664×8750, 16-bit)

### ISO 12800 Night Burst — Best for high-ISO noise
- **Path**: `/Volumes/Photos/DavidsPics/Hassel/2024/`
- **Files**: 2024_05_May_X2D_2868–2882.fff (15 frames)
- **Settings**: ISO 12800, 1/125–1/200s, night
- **Use**: Multiple same-condition frames for noise averaging

### Base ISO Long Exposures
- **Path**: `/Volumes/Photos/DavidsPics/Hassel/2024/`
- **Files**: 2024_Feb_X2D_1172–1183.fff (~12 frames)
- **Settings**: ISO 64, 0.5–1.3s
- **Use**: Minimal-noise baseline measurements

### Test Images (local copies)
- `/Users/dcliftreaves/Pictures/hassel/2024_12_Dec_Austin_0137.dng` — ISO 200, 1/250s
- `/Users/dcliftreaves/Pictures/hassel/2024_12_Dec_Austin_0186.dng` — ISO 64, 1/10s

## Hasselblad X1D / CFV II 50C (8384×6304, 16-bit)

### Longest Exposure Found
- **Path**: `/Volumes/Photos/DavidsPics/Hassel/2020/2020-07-04/B0010206.3FR`
- **Settings**: 50.5 seconds, ISO 800, XCD 21mm
- **Use**: Thermal noise and long-exposure dark current characterization

### Multi-ISO Night Session
- **Path**: `/Volumes/Photos/DavidsPics/Hassel/2020/2020-07-23/`
- **Files**: B0001261–B0001278
- **Settings**: ISO 400–1600, 7.5–21.9s exposures
- **Use**: ISO-dependent noise scaling

## What's Missing

No true dark frames (lens cap shots) exist in the collection. To create proper calibration data:
1. Shoot with body cap on at ISO 64, 200, 800, 3200, 12800
2. Multiple exposure times: 1/1000s, 1s, 10s, 30s
3. Multiple temperatures: cold start vs after 30 min use
4. This gives DSNU maps, read noise vs ISO, and thermal gradient data
