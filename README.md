# GPR Introduction

The General Purpose Raw (GPR) is 12-bit raw image coding format that is based on [Adobe DNG®](https://helpx.adobe.com/photoshop/digital-negative.html) standard. Image compression is a balance of speed, file size and photo quality, and typically one can only choose two. GPR was designed to provide a better tradeoff for all three parameters than what's possible with DNG or any other raw format. The intention of GPR is not to compete with DNG, rather to be as close as possible to DNG. This guarantees compatibility with applications that already understand DNG, but provide an alternate compression scheme in situations where compression and encoding/decoding speed matter.

Action cameras, like that from GoPro, have limited computing resources, so ability to compress data using fewest CPU cycles matters. File sizes matter because GoPro cameras can record thousands of images very quickly using timelapse and burst mode features. As the world shifts from desktop to mobile, people now shoot and process more and more photos on smartphones which are always limited on storage space and bandwidth. And last but not the least, image quality matters because we want GPR to provide visually transparent image quality when compared to uncompressed DNG. All this combined enables customers to capture DSLR-class image quality in a GPR file that has nearly same size as JPEG, on a camera that is as small and rugged as a GoPro.

DNG allows storage of RAW sensor data in three main formats: uncompressed, lossless JPEG or lossy JPEG. Lossless mode typically achieves 2:1 compression that is clearly not enough in the mobile-first age. Lossy mode uses the 8x8 DCT transform that was developed for JPEG more than 25 years ago (when photo resolutions were much smaller), achieving compression ratios around 4:1. In comparison, GPR achieves typical compression ratios between 10:1 and 4:1. This is due to Full-Frame Wavelet Transform (FFWT). FFWT has a few nice properties compared to DCT:
  - The compression performance increases as image resolutions go up - making it more future proof 
  - Better image quality as it does not suffer from ringing or blocky artifacts observed in JPEG files.

The wavelet codec in GPR is not new, but has been a SMPTE® standard under the name  [VC-5](https://kws.smpte.org/higherlogic/ws/public/projects/15/details). VC-5 shares a lot of technical barebones with the [CineForm®](https://gopro.github.io/cineform-sdk/), an open and cross-platform intermediate codec designed for high-resolution video editing.

## About this fork

This repository is a fork of [gopro/gpr](https://github.com/gopro/gpr) that extends the GPR/VC-5 **encoder** and the **DNG writer**, so that raw images from more cameras and phones can be written as GPR files that open correctly in Apple, Adobe and other DNG readers. The decoder is functionally unchanged apart from the additions the encoder round-trips need. What is new:

* **More sensor layouts.** BGGR Bayer mosaics at 12 and 14 bits (`bggr12`, `bggr14`) can be encoded alongside RGGB and GBRG, so a DNG from an Apple iPhone or another BGGR sensor converts to GPR. The raw's `ActiveArea` / `DefaultCrop` tags are honored (the visible crop is what gets encoded), a large sensor black level (iPhone: 528) is subtracted and the range stretched before the VC-5 log curve so the codec's precision lands on the signal, a `NoiseProfile` stored in the raw SubIFD is read, and already-demosaiced DNGs (Apple ProRAW) are rejected with a clear error instead of an assert.
* **Faithful metadata.** `CalibrationIlluminant1/2`, `BaselineExposure`, `BaselineNoise`, `BaselineSharpness` and per-channel `BlackLevel` repeat patterns are carried through, and `OpcodeList3 WarpRectilinear` round-trips at full fidelity (planes, center, flags, all radial and tangential terms).
* **Apple ImageIO compatibility.** Apple's RAW pipeline drops a DNG's entire `OpcodeList2` gain map unless the two green CFA planes carry byte-identical gains; the writer now equalizes them, identifying the greens by each opcode's own CFA cell rather than by list position.
* **Lens-distortion correction on DNG output.** Built-in geometric profiles for HERO5 through HERO13 and MISSION 1 PRO write a synthesized `WarpRectilinear` (`gpr_tools --lens_correction=auto|k0,k1,k2,k3[,cx,cy]`, `--lens_correction_strength`), also exposed as `gpr_parameters_apply_lens_profile` in the SDK.
* **Previews and thumbnails.** An embedded preview can be generated at 2:1, 4:1, 8:1 or 16:1 (`--preview=<ratio>`) or supplied as a JPEG whose dimensions are read from its header (`--preview=file.jpg`). The preview renders through the same pipeline as the RGB decode (camera color matrix with illuminant interpolation and Bradford adaptation, the ACR3 default tone curve, lens shading from the gain maps), so it matches a decode of the file it is embedded in. Plain DNG output gets a thumbnail in IFD 0, where readers look for it, and `dng_to_dng` carries an existing thumbnail across.
* **New conversions.** `gpr_convert_gpr_to_gpr` repackages the VC-5 bitstream with new metadata without re-encoding, or re-encodes it on request (`gpr_parameters::reencode`); `gpr_convert_gpr_to_ppm` / `gpr_convert_gpr_to_jpg` (with the EXIF orientation) move into the SDK; `gpr_convert_dng_to_vc5` now actually encodes; `gpr_parameters_parse_dng` / `gpr_parameters_parse_dng_file` fill `gpr_parameters` from a file's metadata.
* **Selectable encode quality.** `gpr_parameters::quality` (a `GPR_QUALITY_SETTING`) and `gpr_tools --quality` choose the VC-5 quantizer table (`low`, `medium`, `high`, `fs1`, `fsx`, `fs2`, `ultra`, the last of them new), defaulting to the encoder's own Filmscan-X; a GPR input is decoded and re-encoded at the requested level. The encoder's `VC5_ENCODER_QUALITY_SETTING` stays where upstream has it, and the SDK enum mirrors its values under a compile-time check.
* **Robustness.** VC-5 memory streams refuse to write past their buffer (no more silent heap corruption on large frames), the thumbnail JPEG buffer grows instead of assuming a compression ratio, the Bayer-phase shift no longer reads past the end of the raw buffer, DNG SDK exceptions are caught at the C API and reported as `false`, and the XMP toolkit is built with real locks so several files can be processed concurrently in one process.
* **Faster writing.** Output goes through a contiguous, growable stream handed to the caller without a final copy, `dng_to_dng` skips its redundant pixel copies (25-35% faster on large DNGs), and `tiny_jpeg` uses a batched bit-writer with 4:2:0 chroma at the two lower quality levels.
* **Build and test.** The `GPR_READING`, `GPR_WRITING`, `GPR_JPEG_AVAILABLE`, `GPR_TIMING` and `GPR_NEON` switches are CMake options (NEON is enabled automatically on arm64), `scripts/test_build_flags.sh` and the GitHub Actions workflow build every configuration, and `source/test` holds a conversion test suite (`gpr_tools_tests`) that drives every `gpr_convert_*` entry point over the bundled samples.

## File Types

Following file types are discussed in this document:

* `RAW or CFA RAW` - The Bayer RAW format is typically composed of 50% green, 25% red and 25% blue samples captured from sensor, e.g. RGGB and GBRG. The RAW image doesn't carry metadata about image development e.g. exposure, white balance or noise etc, thus cannot easily be turned into a well developed image.

* `DNG` - Widely regarded as a defacto standard, a DNG file can be opened natively on most operating systems and image development tools. As mentioned earlier, DNG stores compressed or uncompressed RAW sensor data along with accompanying metadata that is needed to properly develop image.

* `GPR` - General purpose RAW format file. GoPro cameras including Hero5/6 and Fusion record photos in this format. GPR is an extension of DNG, enabling high performance VC-5 compression for faster storage and smaller files without impacting image quality. Today, GPR files can be opened in Adobe products like Camera Raw®, Photoshop® and Lightroom®.

* `VC5` - A file with VC5 extension stores RAW sensor image data in a compressed format that is compatible with VC-5. Similar to RAW file, VC5 file does not store metadata needed for proper image development.  

* `PPM` - [Portable Pixel Map](http://netpbm.sourceforge.net/doc/ppm.html) is one of the simplest storage formats of uncompressed debayered RGB image. It is very easy to write and analyze programs to process this format, and that is why it is used here.

* `JPG or JPEG` One of the simplest formats for lossy compression of debayered RGB image.

## Conversion Matrix

| Input Format  | RAW | DNG | GPR | PPM | JPG |
| --------      | --- | --- | --- | --- | --- |
| RAW           |  N  |  Y  |  Y  |  N  |  N  |
| DNG           |  Y  |  Y  |  Y  |  N  |  N  |
| GPR           |  Y  |  Y  |  Y  |  Y  |  Y  |

# Included Within This Repository

* The complete source of GPR-SDK, a library that implements conversion of RAW, DNG, GPR, PPM or JPG formats. GPR-SDK has C-99 interface for maximum portability, yet is implemented in C/C++ for programming effectiveness.

* Source of VC-5 encoder and decoder library. Encoder is hand-optimized with NEON intrinsics for ARM processors.

* `gpr_tools` - a sample demo code that uses GPR-SDK to convert between GPR, DNG and RAW formats.

* `vc5_encoder_app` - a sample VC5 encoder application that encodes a RAW frame to VC5 file.

* `vc5_decoder_app` - a sample VC5 decoder application that decodes VC5 file to RAW file.

* CMake support for building all projects.

* Tested on:
  - macOS High Sierra with XCode v8 & v9, El Capitan with XCode v8
  - Windows 10 with Visual Studio 2015 & 2017
  - Ubuntu 16.04 with gcc v5.4

# License Terms

GPR is licensed under either:

* Apache License, Version 2.0, (LICENSE-APACHE or http://www.apache.org/licenses/LICENSE-2.0)
* MIT license (LICENSE-MIT or http://opensource.org/licenses/MIT)

at your option.

## Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in the work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without any additional terms or conditions.

# Quick Start for Developers

## Setup Source Code

Clone the project from Github (`git clone https://github.com/gopro/gpr`). You will need [CMake](https://cmake.org/download/) version 3.5.1 or better to compile source code.

### Compiling Source Code

Run following commands:
```
$ mkdir build
$ cd build
$ cmake ../
```

For Xcode, use command line switch `-G Xcode`. On Windows, CMake should automatically figure out installed version of Visual Studio and generate corresponding project files.

Mac build instructions, after running the above:
```
$ make .
$ ./source/app/gpr_tools/gpr_tools
```

Linux build instructions, after running the above:
```
$ make 
$ ./source/app/gpr_tools/gpr_tools
```

To build and run the conversion test suite (from the repository root):
```
$ cmake -S . -B build
$ cmake --build build --target gpr_tools_tests
$ ctest --test-dir build --output-on-failure
```

## Using gpr_tools

Some example commands are shown below:

Convert GPR to DNG: 

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.DNG
```

Extract RAW from GPR: 

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.RAW
```

Convert DNG to GPR:

```
$ gpr_tools -i INPUT.DNG -o OUTPUT.GPR
```

Repackage a GPR with new metadata, without decoding or re-encoding the image:

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.GPR -a PARAMETERS.TXT
```

Embed a preview while writing a GPR, either generated at a downscale ratio (2:1, 4:1, 8:1, 16:1) or from a JPEG file (no preview is written unless `--preview` is given):

```
$ gpr_tools -i INPUT.DNG -o OUTPUT.GPR --preview=8:1
$ gpr_tools -i INPUT.DNG -o OUTPUT.GPR --preview=THUMBNAIL.JPG
```

Choose the VC-5 quality, which selects the quantizer table for the wavelet highpass bands, when encoding to GPR. The levels, from the smallest files to the highest fidelity, are `low`, `medium`, `high`, `fs1`, `fsx` (the default), `fs2` and `ultra`. Without `--quality` a GPR input is repackaged as-is; with it, the image is decoded and encoded again at that level:

```
$ gpr_tools -i INPUT.DNG -o OUTPUT.GPR --quality=fs2
$ gpr_tools -i INPUT.GPR -o OUTPUT.GPR --quality=low
```

Convert a BGGR DNG (for example from an iPhone) to GPR. Shifting the mosaic by one column turns BGGR into GBRG, the phase GoPro cameras write; the mosaic can also be encoded as-is with `--input_pixel_format=bggr12`:

```
$ gpr_tools -i IPHONE.DNG -o OUTPUT.GPR --input_skip_cols=1 --input_pixel_format=gbrg12
```

Write a DNG with a geometric lens-distortion correction (a synthesized OpcodeList3 WarpRectilinear that DNG readers apply when rendering), using the built-in profile for the source camera or explicit coefficients:

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.DNG --lens_correction=auto
$ gpr_tools -i INPUT.GPR -o OUTPUT.DNG --lens_correction=auto --lens_correction_strength=1
$ gpr_tools -i INPUT.GPR -o OUTPUT.DNG --lens_correction=0.999,-0.547,0.410,-0.156
```

Analyze a GPR (or even DNG) and output parameters that define DNG metadata to a file:

```
$ gpr_tools -i INPUT.GPR -d > PARAMETERS.TXT
```      

Read RAW pixel data, along with parameters that define DNG metadata and apply to an output GPR (or DNG) file:

```
$ gpr_tools -i INPUT.RAW -o OUTPUT.DNG -a PARAMETERS.TXT
```

Read GPR file and output PPM preview (4:1 unless `--rgb_resolution` says otherwise; `--output_ppm_bits=16` for 16-bit samples):

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.PPM --rgb_resolution=2:1
```

Read GPR file and output JPG preview (the source orientation is written as an EXIF tag; `--output_jpg_quality` is 1 to 3):

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.JPG --rgb_resolution=2:1 --output_jpg_quality=3
```

Run `gpr_tools --help` for the complete option list. `scripts/test_conversions.sh <folder>` runs every conversion over a folder of GPR and DNG files, and `source/test/README.md` describes the test suite.

## Using vc5_encoder_app

vc5_encoder_app is an optional tool that can be used to convert RAW image data to VC5 essence, as shown below:

```
$ vc5_encoder_app -i INPUT.RAW -o OUTPUT.VC5 -w 4000 -h 3000 -p 8000
```

## Using vc5_decoder_app

vc5_decoder_app is an optional tool that can be used to decode VC5 essence into RAW image data, as shown below:

```
$ vc5_decoder_app -i INPUT.VC5 -o OUTPUT.RAW
```

## Source code organization

### Folder structure

Source code is organized inside `source` folder. All library and sdk code is located in `lib` folder, while all tools and applications that use `lib` are located in `app` folder.

The ``` lib``` folder is made up of following folders:

- `common` - common source code that is accessable to all libraries and applications
- `dng_sdk` - mostly borrowed from [Adobe DNG SDK](https://www.adobe.com/support/downloads/dng/dng_sdk.html) version 1.4.
- `xmp_core` - [Extensible Metadata Platform](https://en.wikipedia.org/wiki/Extensible_Metadata_Platform), mostly borrowed from Adobe DNG SDK version 1.4
- `expat_lib` - A stream-oriented XML parser borrowed from Adobe DNG SDK version 1.4
- `vc5_decoder` - vc5 decoder. If this is not present, cmake won't use it (and define `GPR_READING=0`); otherwise cmake uses it (and defines `GPR_READING=1`).
- `vc5_encoder` - vc5 encoder. If this is not present, cmake won't use it (and define `GPR_WRITING=0`); otherwise cmake uses it (and defines `GPR_WRITING=1`).
- `vc5_common` - common source code that is shared between vc5_encoder and vc5_decoder
- `md5_lib` - md5 checksum calculation library
- `tiny_jpeg` - lightweight jpeg encoder available [here](https://github.com/serge-rgb/TinyJPEG). If this folder is not present, cmake will not use it (and define `GPR_JPEG_AVAILABLE=0`); otherwise cmake defines `GPR_JPEG_AVAILABLE=1` and uses it.
- `gpr_sdk` - uses all modules above to read/write GPR files.

The `app` folder is made up of following folders:

- `common` - common application level source code that is accessable to sample applications
- `vc5_decoder_app` - sample vc5 decoder application
- `vc5_encoder_app` - sample vc5 encoder application
- `gpr_tools` - utility to convert to/from various formats mentioned above and measure runtime

The `source/test` folder holds `gpr_conversion_tests.cpp`, the conversion test suite (built as `gpr_tools_tests`, registered with CTest; see `source/test/README.md`), and `scripts` holds the build-flag matrix, the conversion pipeline over a folder of samples, and the tool that fits `WarpRectilinear` coefficients for new lens profiles.

### Important Defines

Here are some important compile time definitions:

* `GPR_TIMING` enables timing code that prints out time spent (in milliseconds) in different functions. In production builds, applications should define  `GPR_TIMING=0`. Set to a higher value to output greater timing information.

* `GPR_WRITING` enables all code that writes GPR files. If application does not need to write GPR, set `GPR_WRITING=0` to reduce code size.

* `GPR_READING` enables all code that reads GPR files. If application does not need to read GPR, set `GPR_READING=0` to reduce code size.

* `GPR_JPEG_AVAILABLE` enables lightweight jpeg encoder located in `source/lib/tiny_jpeg`. This is used to write a small thumbnail inside GPR file. If `GPR_JPEG_AVAILABLE=0`, thumbnail is not written, although you can still set pre-encoded jpeg file as thumbnail, by using the `--preview=file.jpg` command line option in `gpr_tools`.

* `GPR_NEON` enables arm neon intrinsics. The top-level CMakeLists.txt enables this automatically on arm64 targets; force the scalar path with `-DGPR_NEON=OFF`.

All of the above are also exposed as CMake options (`-DGPR_TIMING=OFF`, `-DGPR_WRITING=OFF`, `-DGPR_READING=OFF`, `-DGPR_JPEG_AVAILABLE=OFF`, `-DGPR_NEON=OFF`). Run `scripts/test_build_flags.sh` to verify that the project builds with each flag disabled individually and with all of them disabled together; `.github/workflows/build-flags.yml` does the same on every push, on x86_64 Linux and arm64 macOS.

### GPR-SDK API

GPR-SDK API is defined in header files in the following folders:

- source/lib/common/public
- source/lib/gpr_sdk/public

An application needs to include above two folders in order to access API. API defines various functions named ```gpr_convert_XXX_to_XXX``` which convert from one format to another. As an example, GPR to DNG conversion is done from ```gpr_convert_gpr_to_dng```.  When output file is GPR or DNR, ```gpr_parameters``` structure has to be specified. Fields in this structure map to DNG metadata tags, and we have tried to abstract low-level DNG details in a very clean and easy to use structure.

# Compression Technology

## Wavelet Transforms
The wavelet used within VC-5 is a 2D three-level 2-6 Wavelet. If you look up wavelets on Wikipedia, prepare to get confused fast. Wavelet compression of images is fairly simple if you don't get distracted by the theory. The wavelet is a one dimensional filter that separates low frequency data from high frequency data, and the math is simple. For each two pixels in an image simply add them (low frequency):
* low frequency sample = pixel[x] + pixel[x+1] 
- two inputs, is the '2' part of 2-6 Wavelet.

For high frequency it can be as simple as the difference of the same two pixels:
* high frequency sample = pixel[x] - pixel[x+1] 
- this would be a 2-2 Wavelet, also called a [HAAR wavelet](https://en.wikipedia.org/wiki/Haar_wavelet).

For a 2-6 wavelet this math is for the high frequency:
* high frequency sample = pixel[x] - pixel[x+1] + (-pixel[x-2] - pixel[x-1] + pixel[x+2] + pixel[x+3])/8 
- i.e 6 inputs for the high frequency, the '6' part of 2-6 Wavelet.

The math doesn't get much more complex than that. 

To wavelet compress a monochrome frame (color can be compressed as separate monochrome channels), we start with a 2D array of pixels (a.k.a image.)

![](data/readmegfx/source-640.png "Source image")

If you store data with low frequencies (low pass) on the left and the high frequencies (high pass) on the right you get the image below. A low pass image is basically the average, and high pass image is like an edge enhance.

![](data/readmegfx/level1D-640.png "1D Wavelet")

You repeat the same operation vertically using the previous output as the input image.

Resulting in a 1 level 2D wavelet:

![](data/readmegfx/level1-640.png "1D Wavelet")

For a two level wavelet, you repeat the same horizontal and vertical wavelet operations of the top left quadrant to provide:

![](data/readmegfx/level2-640.png "2 Level 2D-Wavelet")

Repeating again for the third level.

![](data/readmegfx/level3-640.png "3 Level 2D-Wavelet")

## Quantization

All that grey is easy to compress. The reason there is very little information in these high frequency regions is that the high frequency data of the image has been quantized. The human eye is not very good at seeing subtle changes in high frequency regions, so this is exploited by scaling the high-frequency samples before they are stored:

* high frequency sample = (wavelet output) / quantizer

## Entropy Coding

After the wavelet and quantization stages, you have the same number of samples as the original source. The compression is achieved as the samples are no longer evenly distributed (after wavelet and quantization.) There are many many zeros and ones, than higher values, so we can store all these values more efficiently, often up to 10 times more so.

### Run length

The output of the quantization stage has a lot of zeros, and many in a row. Additional compression is achieved by counting runs of zeros, and storing them like: a "z15" for 15 zeros, rather than "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"

### Variable length coding

After all previous steps, the high frequency samples are stored with a variable length coding scheme using [Huffman coding](https://en.wikipedia.org/wiki/Huffman_coding). A table then maps sample values to codewords with differing bit lengths where most common codewords are expressed in few bits and rare codewords are expressed in larger bits. 

The lack of complexity is what makes VC-5 fast. Low pass filter is just addition. High pass filter is 6 tap where all coefficients are rational numbers and no multiplication or division is required. Variable length coding can be implemented with a lookup table, an approach that is faster than other entropy coding techniques.

## To Decode

Reverse all the steps.

# Thumbnail and Preview Generation

A nice property of the Wavelet codec is scalability support: i.e. various resolutions ranging from original coded resolution to one-sixteenth resolution are encoded and can be retrieved efficiently. Scalability means that extracting lowest resolution is fastest and cost of extracting resolutions increases as resolution goes up. For application scenarios where rendering a smaller resolution suffices, a decoder can very cheaply extract lower resolution. Common examples of this use case are thumbnail previews in file browsers or rendering image on devices with smaller resolution e.g. mobile phones. 

Scalability is more efficient than decoding full resolution image, performing demosaicing and downsampling. To avoid demosaic, DNG allows mechanism to store a separate thumbnail and preview image (often encoded in JPG). File browers use thumbnail, while preview is useful for rendering higher resolution version of image. Since these are separately enoded images and do not exploit compression amongst each other or with original RAW image, file sizes add up quickly. 

As an example, GoPro Hero6 Black captures 4000x3000 RAW image in Bayer RGGB format. Red, blue and two green channels are split up and separately encoded into wavelet resolutions of 2000x1500 (or 2:1), 1000x750 (or 4:1), 500x375 (or 8:1). The Low-Low band of lowest resolution wavelet is a 250x188 (or 16:1) image, and it is stored uncompressed (generating thumbnail is essentially a memory copy). RGB images at other resolutions can be obtained at successive complexity levels, without performing demosaicing. To illustrate this, decoding speed of various resolutions is measured and shown using `gpr_tools` (in milliseconds inside square brackets).

```
Decode GPR to 8-bit PPM (250x188)
[    6-ms] [BEG] gpr_convert_gpr_to_rgb() gpr.cpp (line 1695)
[   16-ms] [END] gpr_convert_gpr_to_rgb() gpr.cpp (line 1738)
```

```
Decode GPR to 8-bit PPM (500x375)
[    6-ms] [BEG] gpr_convert_gpr_to_rgb() gpr.cpp (line 1695)
[   37-ms] [END] gpr_convert_gpr_to_rgb() gpr.cpp (line 1738)
```

```
Decode GPR to 8-bit PPM (1000x750)
[    5-ms] [BEG] gpr_convert_gpr_to_rgb() gpr.cpp (line 1695)
[  130-ms] [END] gpr_convert_gpr_to_rgb() gpr.cpp (line 1738)
```

```
Decode GPR to 8-bit PPM (2000x1500)
[    5-ms] [BEG] gpr_convert_gpr_to_rgb() gpr.cpp (line 1695)
[  357-ms] [END] gpr_convert_gpr_to_rgb() gpr.cpp (line 1738)
```

And here is the output of full GPR to DNG decoding.

```
Decode GPR to DNG
[    6-ms] [BEG] gpr_convert_gpr_to_dng() gpr.cpp (line 1748)
[  422-ms] [END] gpr_convert_gpr_to_dng() gpr.cpp (line 1768)
```

To summarize, here are speed gain factors over full resolution DNG decoding:

| Resolution | 250x188 | 500x375 | 1000x750 | 2000x1500
| :---: | :---: | :---: | :---: | :---:
| Speed factor | 41.6x | 13.4x | 3.3x | 1.2x

Demosaicing has a higher complexity than GPR decoding, so numbers for RGB output after demosaic will be higher. Similar speed improvements can also be seen when writing JPG file. 

```
GoPro and CineForm are trademarks of GoPro, Inc.
DNG, Photoshop and Lightroom is trademarks of Adobe Inc.
```
