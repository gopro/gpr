# Phocus Capture Sequences for Sensor Calibration

These XML files are Hasselblad Phocus Capture Sequencer presets for automated tethered shooting of calibration frames.

## Installation

Copy to your Phocus settings directory:

```bash
cp *.xml ~/Library/Application\ Support/Phocus/Settings/Capture\ Sequences/
```

Or run the install script:

```bash
./install.sh
```

They will appear in Phocus under **Capture > Capture Sequencer** when the camera is tethered.

## Sequences

| File | Shots | Delay | Use For |
|------|-------|-------|---------|
| `Dark_50_Shots.xml` | 50 | 1s between | Dark frames at each ISO (short exposure) |
| `Dark_20_Shots_LongExp.xml` | 20 | 1s between | Dark frames with long exposure (thermal characterization) |
| `Flat_50_Shots.xml` | 50 | 1s between | Flat field frames at each ISO |

## Calibration Workflow

### Dark Frames (body cap on, no light)
1. Connect X2D to Mac via USB, open Phocus
2. Put body cap on the camera (tape over EVF if needed)
3. Set camera to **Manual mode**
4. For each ISO (64, 200, 800, 3200, 12800):
   - Set the ISO on the camera
   - Set shutter to 1/1000s
   - In Phocus: **Capture > Capture Sequencer > Dark_50_Shots**
   - Click **Start** — Phocus fires 50 frames automatically
   - Move files to a folder: `darks/iso{N}/`
5. For thermal characterization:
   - Set ISO 64, shutter 10s
   - Use **Dark_20_Shots_LongExp** sequence
   - Repeat at ISO 800

### Flat Fields (evenly illuminated, defocused)
1. Keep camera tethered
2. Remove body cap, point at evenly lit white surface
3. Deliberately **defocus** the lens
4. For each ISO (64, 200, 800):
   - Set ISO, use aperture priority or auto exposure (~50% histogram)
   - Use **Flat_50_Shots** sequence
   - Move files to: `flats/iso{N}/`

### Processing
```bash
# Convert .fff to .dng
for f in darks/iso200/*.fff; do
  "/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter" -fl -d darks/iso200/dng/ "$f"
done

# Decode .dng to .RAW
for f in darks/iso200/dng/*.dng; do
  gpr_tools -i "$f" -o "${f%.dng}.RAW"
done

# Run calibration
calibrate --dark-dir darks/iso200/dng/ --flat-dir flats/iso200/dng/ \
  --output x2d_calibration.json -w 11664 -h 8750 -b 16
```
