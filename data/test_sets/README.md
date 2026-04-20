# Test Data Sets

Organized test data for GPR noise-aware compression validation.

## Structure

```
test_sets/
├── smoke/          — 5 images, ~30s, quick sanity check
│   ├── gopro/      — Hero6 outdoor, HERO7 scene
│   ├── hasselblad/ — X2D Austin ISO 200
│   └── nikon/      — Z8 sample
├── medium/         — 20 images, ~3min, corner case coverage
│   ├── gopro/      — Hero5/6/7/9, Fusion
│   ├── hasselblad/ — X2D ISO 64 + ISO 200
│   ├── nikon/      — Z8 samples at different ISOs
│   └── hero10_dark/— 3 dark frames at different ISOs
├── full/           — 60+ images, ~15min, comprehensive proof
│   ├── gopro/      — All samples
│   ├── hasselblad/ — All X2D DNGs + Phocus calibration
│   ├── nikon/      — 10 Z8 across dates/ISOs
│   └── hero10_dark/— 10 spread across ISO range
└── corner_cases/   — Specific challenging scenarios
    ├── dark_frames/ — ISO 100-1600 HERO10 darks
    ├── high_iso/    — ISO 3200+ (X2D, Z8)
    ├── saturated/   — Images with blown highlights
    └── low_light/   — Dark scenes with shadow noise
```

## Image Categories

### Representative (Normal)
- Well-lit outdoor GoPro scenes (Hero5/6/7/9)
- X2D landscape/portrait at ISO 64-200
- Z8 general photography at ISO 64-500

### Corner Cases
- Dark frames (lens cap): pure noise, tests noise floor detection
- High ISO (3200+): high noise, tests denoise aggressiveness
- Saturated highlights: tests clipping behavior
- Fusion stereo: different aspect ratio/resolution
- HERO10: different sensor dimensions (5568×4176)

## Source Locations
- GoPro samples: `data/samples/` (checked into repo)
- HERO10 darks: `/Users/dcliftreaves/Downloads/100GOPRO/`
- X2D: `/Users/dcliftreaves/Pictures/hassel/`
- Phocus: `/Users/dcliftreaves/Pictures/Phocus Captures.localized/`
- Z8: `/Volumes/Photos/DavidsPics/2025/` (external server)
