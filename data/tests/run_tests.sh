#!/bin/sh

GPR_TOOLS=$1

# cp ../../xcode/source/app/gpr_tools/Release/gpr_tools .

# Start with one GPR file

rm *.GPR
rm *.DNG
rm *.PPM
rm *.JPG

rm -rf JPB
rm -rf PPM
rm -rf DNG
rm -rf GPR
rm -rf RAW

echo
echo Dump GPR parameters to a file
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -d 1 > GOPR0024.TXT


mkdir PPM
echo
echo Decode "GPR to 8-bit PPM (250x188)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-250x188-8-bit.PPM -r 16:1
echo
echo Decode "GPR to 8-bit PPM (500x375)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-500x375-8-bit.PPM -r 8:1
echo
echo Decode "GPR to 8-bit PPM (1000x750)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-1000x750-8-bit.PPM -r 4:1
echo
echo "Decode GPR to 8-bit PPM (2000x1500)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-2000x1500-8-bit.PPM -r 2:1
echo
echo Decode "GPR to 16-bit PPM (250x188)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-250x188-16-bit.PPM -r 16:1 -b 16
echo
echo Decode "GPR to 16-bit PPM (500x375)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-500x375-16-bit.PPM -r 8:1 -b 16
echo
echo Decode "GPR to 16-bit PPM (1000x750)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-1000x750-16-bit.PPM -r 4:1 -b 16
echo
echo "Decode GPR to 16-bit PPM (2000x1500)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o PPM/GOPR0024-2000x1500-16-bit.PPM -r 2:1 -b 16

mkdir JPG
echo
echo Decode "GPR to JPG (250x188)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o JPG/GOPR0024-250x188.JPG -r 16:1
echo
echo Decode "GPR to JPG (500x375)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o JPG/GOPR0024-500x375.JPG -r 8:1
echo
echo Decode "GPR to JPG (1000x750)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o JPG/GOPR0024-1000x750.JPG -r 4:1
echo
echo Decode "GPR to JPG (2000x1500)"
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o JPG/GOPR0024-2000x1500.JPG -r 2:1

mkdir RAW
echo
echo Decode GPR to RAW
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o RAW/GOPR0024.RAW


mkdir DNG
echo
echo Decode GPR to DNG
$GPR_TOOLS -i ../samples/Hero6/GOPR0024.GPR -o DNG/GOPR0024.DNG
echo
echo Encode RAW to DNG
$GPR_TOOLS -i RAW/GOPR0024.RAW -o DNG/GOPR0024-FROM-RAW.DNG -a GOPR0024.TXT

mkdir GPR
echo
echo Encode DNG to GPR and using thumbnail from JPG file that was previously written
$GPR_TOOLS -i DNG/GOPR0024.DNG -o GPR/GOPR0024-PREVIEW-250x188.GPR   -P JPG/GOPR0024-250x188.JPG -H 188 -W 250
$GPR_TOOLS -i DNG/GOPR0024.DNG -o GPR/GOPR0024-PREVIEW-500x375.GPR   -P JPG/GOPR0024-500x375.JPG -H 375 -W 500
$GPR_TOOLS -i DNG/GOPR0024.DNG -o GPR/GOPR0024-PREVIEW-1000x750.GPR  -P JPG/GOPR0024-1000x750.JPG -H 750 -W 1000
$GPR_TOOLS -i DNG/GOPR0024.DNG -o GPR/GOPR0024-PREVIEW-2000x1500.GPR -P JPG/GOPR0024-2000x1500.JPG -H 750 -W 1000

echo
echo Encode RAW to GPR
$GPR_TOOLS -i RAW/GOPR0024.RAW -o GPR/GOPR0024-FROM-RAW.GPR -a GOPR0024.TXT
