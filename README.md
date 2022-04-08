# GPR Introduction

Testing...

The General Purpose Raw (GPR) is 12-bit raw image coding format that is based on [Adobe DNG®](https://helpx.adobe.com/photoshop/digital-negative.html) standard. Image compression is a balance of speed, file size and photo quality, and typically one can only choose two. GPR was designed to provide a better tradeoff for all three parameters than what's possible with DNG or any other raw format. The intention of GPR is not to compete with DNG, rather to be as close as possible to DNG. This guarantees compatibility with applications that already understand DNG, but provide an alternate compression scheme in situations where compression and encoding/decoding speed matter.

Action cameras, like that from GoPro, have limited computing resources, so ability to compress data using fewest CPU cycles matters. File sizes matter because GoPro cameras can record thousands of images very quickly using timelapse and burst mode features. As the world shifts from desktop to mobile, people now shoot and process more and more photos on smartphones which are always limited on storage space and bandwidth. And last but not the least, image quality matters because we want GPR to provide visually transparent image quality when compared to uncompressed DNG. All this combined enables customers to capture DSLR-class image quality in a GPR file that has nearly same size as JPEG, on a camera that is as small and rugged as a GoPro.

DNG allows storage of RAW sensor data in three main formats: uncompressed, lossless JPEG or lossy JPEG. Lossless mode typically achieves 2:1 compression that is clearly not enough in the mobile-first age. Lossy mode uses the 8x8 DCT transform that was developed for JPEG more than 25 years ago (when photo resolutions were much smaller), achieving compression ratios around 4:1. In comparison, GPR achieves typical compression ratios between 10:1 and 4:1. This is due to Full-Frame Wavelet Transform (FFWT). FFWT has a few nice properties compared to DCT:
  - The compression performance increases as image resolutions go up - making it more future proof 
  - Better image quality as it does not suffer from ringing or blocky artifacts observed in JPEG files.

The wavelet codec in GPR is not new, but has been a SMPTE® standard under the name  [VC-5](https://kws.smpte.org/higherlogic/ws/public/projects/15/details). VC-5 shares a lot of technical barebones with the [CineForm®](https://gopro.github.io/cineform-sdk/), an open and cross-platform intermediate codec designed for high-resolution video editing.

## File Types

Following file types are discussed in this document:

* `RAW or CFA RAW` - The Bayer RAW format is typically composed of 50% green, 25% red and 25% blue samples captured from sensor, e.g. RGGB and GBRG. The RAW image doesn't carry metadata about image development e.g. exposure, white balance or noise etc, thus cannot easily be turned into a well developed image.

* `DNG` - Widely regarded as a defacto standard, a DNG file can be opened natively on most operating systems and image development tools. As mentioned earlier, DNG stores compressed or uncompressed RAW sensor data along with accompanying metadata that is needed to properly develop image.

* `GPR` - General purpose RAW format file. GoPro cameras including Hero5/6 and Fusion record photos in this format. GPR is an extension of DNG, enabling high performance VC-5 compression for faster storage and smaller files without impacting image quality. Today, GPR files can be opened in Adobe products like Camera Raw®, Photoshop® and Lightroom®.

* `VC5` - A file with VC5 extension stores RAW sensor image data in a compressed format that is compatible with VC-5. Similar to RAW file, VC5 file does not store metadata needed for proper image development.  

* `PPM` - [Portable Pixel Map](http://netpbm.sourceforge.net/doc/ppm.html) is one of the simplest storage formats of uncompressed debayered RGB image. It is very easy to write and analyze programs to process this format, and that is why it is used here.

* `JPG or JPEG` One of the simplest formats for lossy compression of debayered RGB image.

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

Analyze a GPR (or even DNG) and output parameters that define DNG metadata to a file:

```
$ gpr_tools -i INPUT.GPR -d 1 > PARAMETERS.TXT
```      

Read RAW pixel data, along with parameters that define DNG metadata and apply to an output GPR (or DNG) file:

```
$ gpr_tools -i INPUT.RAW -o OUTPUT.DNG -a PARAMETERS.TXT
```

Read GPR file and output PPM preview:

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.PPM
```

Read GPR file and output JPG preview:

```
$ gpr_tools -i INPUT.GPR -o OUTPUT.JPG
```

For a complete list of commands, please refer to data/tests/run_tests.sh 

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

### Important Defines

Here are some important compile time definitions:

* `GPR_TIMING` enables timing code that prints out time spent (in milliseconds) in different functions. In production builds, applications should define  `GPR_TIMING=0`. Set to a higher value to output greater timing information.

* `GPR_WRITING` enables all code that writes GPR files. If application does not need to write GPR, set `GPR_WRITING=0` to reduce code size.

* `GPR_READING` enables all code that reads GPR files. If application does not need to read GPR, set `GPR_READING=0` to reduce code size.

* `GPR_JPEG_AVAILABLE` enables lightweight jpeg encoder located in `source/lib/tiny_jpeg`. This is used to write a small thumbnail inside GPR file. If `GPR_JPEG_AVAILABLE=0`, thumbnail is not written, although you can still set pre-encoded jpeg file as thumbnail, by using -P, -W and -H command line options in `gpr_tools`.

* `NEON` enables arm neon intrinsics (disabled by default). This can be enabled from CMake by `-DNEON=1` switch.

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
