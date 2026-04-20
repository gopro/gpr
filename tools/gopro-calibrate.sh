#!/bin/bash
#
# GoPro Dark Frame Calibration Script
# Captures dark frames via USB HTTP API (Open GoPro)
#
# Prerequisites:
#   - GoPro powered on, USB mode = "GoPro Connect"
#   - USB-C cable connected to Mac
#   - Body cap / lens cap on (NO LIGHT)
#
# Usage: ./gopro-calibrate.sh [output_dir]
#

GOPRO_IP="172.20.116.51"
GOPRO_PORT="8080"
GOPRO_URL="http://${GOPRO_IP}:${GOPRO_PORT}"
OUTPUT_DIR="${1:-./gopro_darks}"
FRAMES_PER_ISO=50

# Check GoPro connectivity
echo "=== GoPro Dark Frame Calibration ==="
echo ""
echo "Checking GoPro connection at ${GOPRO_URL}..."

if ! curl -s --connect-timeout 3 "${GOPRO_URL}/gopro/camera/state" > /dev/null 2>&1; then
    echo "ERROR: Cannot reach GoPro at ${GOPRO_URL}"
    echo ""
    echo "Troubleshooting:"
    echo "  1. Is the GoPro powered on?"
    echo "  2. Is USB mode set to 'GoPro Connect' (not MTP)?"
    echo "     Settings → Connections → USB Connection → GoPro Connect"
    echo "  3. Is the USB cable connected?"
    echo "  4. Check: ifconfig | grep 172.20"
    echo ""
    echo "Trying alternate detection..."

    # Try to find GoPro on any 172.x network
    GOPRO_FOUND=$(ifconfig 2>/dev/null | grep -o "172\.[0-9]*\.[0-9]*\.[0-9]*" | head -1)
    if [ -n "$GOPRO_FOUND" ]; then
        echo "Found GoPro network at: $GOPRO_FOUND"
        GOPRO_IP=$(echo "$GOPRO_FOUND" | sed 's/\.[0-9]*$/.51/')
        GOPRO_URL="http://${GOPRO_IP}:${GOPRO_PORT}"
        echo "Trying ${GOPRO_URL}..."
        if ! curl -s --connect-timeout 3 "${GOPRO_URL}/gopro/camera/state" > /dev/null 2>&1; then
            echo "Still cannot connect. Exiting."
            exit 1
        fi
    else
        exit 1
    fi
fi

echo "Connected to GoPro!"
echo ""

# Get camera info
INFO=$(curl -s "${GOPRO_URL}/gopro/camera/state")
echo "Camera state retrieved."
echo ""

# Ensure camera is in photo mode
echo "Setting photo mode..."
curl -s "${GOPRO_URL}/gopro/camera/presets/set_group?id=1001" > /dev/null 2>&1
sleep 1

# Warning
echo "╔══════════════════════════════════════════╗"
echo "║  ⚠  BODY CAP / LENS CAP MUST BE ON!    ║"
echo "║  No light should reach the sensor.       ║"
echo "║  Press Enter to continue or Ctrl+C...    ║"
echo "╚══════════════════════════════════════════╝"
read -r

mkdir -p "$OUTPUT_DIR"

# GoPro ISOs: 100, 200, 400, 800, 1600
for ISO in 100 200 400 800 1600; do
    ISO_DIR="$OUTPUT_DIR/iso${ISO}"
    mkdir -p "$ISO_DIR"

    echo ""
    echo "=== ISO $ISO: Capturing $FRAMES_PER_ISO dark frames ==="

    # Set ISO (GoPro API: iso_min and iso_max to lock ISO)
    # Setting 75 = ISO lock via protune
    curl -s "${GOPRO_URL}/gopro/camera/setting?setting=75&option=${ISO}" > /dev/null 2>&1
    # Set shutter speed to fastest available (reduces dark current)
    # Setting 73 = shutter speed, but values vary by mode

    sleep 1

    for i in $(seq 1 $FRAMES_PER_ISO); do
        # Trigger shutter
        curl -s "${GOPRO_URL}/gopro/camera/shutter/start" > /dev/null 2>&1
        sleep 2  # Wait for capture + processing
        curl -s "${GOPRO_URL}/gopro/camera/shutter/stop" > /dev/null 2>&1
        sleep 1

        printf "  Frame %d/%d\r" "$i" "$FRAMES_PER_ISO"
    done
    echo "  ISO $ISO: $FRAMES_PER_ISO frames captured"
done

echo ""
echo "=== Capture Complete ==="
echo "Dark frames saved to GoPro SD card."
echo ""
echo "Next steps:"
echo "  1. Remove SD card and copy GPR files to: $OUTPUT_DIR/iso{N}/"
echo "  2. Decode: for f in $OUTPUT_DIR/iso200/*.GPR; do gpr_tools -i \"\$f\" -o \"\${f%.GPR}.RAW\"; done"
echo "  3. Calibrate: calibrate --dark-dir $OUTPUT_DIR/iso200/ --output gopro_calibration.json -w 5568 -h 4176 -b 14"
