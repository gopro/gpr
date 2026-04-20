#!/bin/bash
# Install Phocus Capture Sequences for sensor calibration
DEST="$HOME/Library/Application Support/Phocus/Settings/Capture Sequences"
mkdir -p "$DEST"
cp "$(dirname "$0")"/*.xml "$DEST/"
echo "Installed calibration sequences to Phocus:"
ls "$DEST/"*.xml 2>/dev/null | while read f; do echo "  $(basename "$f")"; done
echo ""
echo "Open Phocus → Capture → Capture Sequencer to use them."
