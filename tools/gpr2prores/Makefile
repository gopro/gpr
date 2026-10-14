# Makefile for gpr2prores

GPR_BUILD ?= ../../build
PKG_CONFIG ?= pkg-config
LIBRAW_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libraw)
LIBRAW_LIBS ?= $(shell $(PKG_CONFIG) --libs libraw)
FFMPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libswresample)
FFMPEG_LIBS ?= $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libswresample)
PREFIX    ?= /usr/local
BINDIR    = $(PREFIX)/bin
LIBDIR    = $(PREFIX)/lib/gpr2prores

# metal includes both the hybrid Metal and pure MPSGraph runtime choices.
CNN_BACKENDS ?= none
ifeq ($(filter none coreml metal both,$(CNN_BACKENDS)),)
    $(error CNN_BACKENDS must be none, coreml, metal, or both)
endif
ifneq ($(words $(CNN_BACKENDS)),1)
    $(error CNN_BACKENDS must be a single selection)
endif
HAVE_COREML := $(if $(filter coreml both,$(CNN_BACKENDS)),1,0)
HAVE_METAL := $(if $(filter metal both,$(CNN_BACKENDS)),1,0)
CNN_CPPFLAGS = -DGPR_HAVE_SUPER_RES_COREML=$(HAVE_COREML) -DGPR_HAVE_SUPER_RES_METAL=$(HAVE_METAL)

# Sanity check: $(GPR_BUILD) MUST be configured Release (or RelWithDebInfo),
# otherwise the decoder links against an -O0 libvc5_decoder.a and decode
# runs ~6× slower (63 ms vs 10 ms on M3 Max for half-res UHD), capping
# playback at ~15 fps instead of ~27 fps. The root CMakeLists.txt now
# defaults to Release when no build type is given, but an old
# CMakeCache.txt with CMAKE_BUILD_TYPE="" survives reconfigures, so we
# check explicitly here. To fix: `cmake -B $(GPR_BUILD) -DCMAKE_BUILD_TYPE=Release`.
BUILD_TYPE := $(shell grep '^CMAKE_BUILD_TYPE:' $(GPR_BUILD)/CMakeCache.txt 2>/dev/null | cut -d'=' -f2)
ifeq ($(BUILD_TYPE),)
    $(warning $(GPR_BUILD)/CMakeCache.txt has empty CMAKE_BUILD_TYPE — libs likely built at -O0)
    $(warning Reconfigure with: cmake -B $(GPR_BUILD) -DCMAKE_BUILD_TYPE=Release && cmake --build $(GPR_BUILD) -j8)
endif

CC      = clang
CFLAGS  = -O2 -g -Wall -Wno-deprecated-declarations -fobjc-arc \
          -I../../source/lib/vc5_encoder \
          -I../../source/lib/vc5_decoder \
          $(LIBRAW_CFLAGS) $(FFMPEG_CFLAGS) \
          -mmacosx-version-min=14.0

LDFLAGS = $(LIBRAW_LIBS) -lc++ \
          -framework Foundation \
          -framework CoreFoundation \
          -framework CoreGraphics \
          -framework ImageIO \
          -framework CoreServices \
          -framework Metal \
          -framework CoreImage \
          -framework CoreVideo \
          -framework IOSurface \
          -framework CoreMedia \
          -framework VideoToolbox \
          -framework AVFoundation

CNN_LDFLAGS =
CNN_OBJS =
METAL_SOURCES = Demosaic.metal Downscale.metal
ifeq ($(HAVE_COREML),1)
CNN_OBJS += SuperResCNN.o
CNN_LDFLAGS += -framework CoreML
endif
ifeq ($(HAVE_METAL),1)
CNN_OBJS += SuperResMetal.o
CNN_LDFLAGS += -framework MetalPerformanceShaders -framework MetalPerformanceShadersGraph
METAL_SOURCES += SuperResPost.metal NAFBlockKernels_LNConv.metal NAFBlockKernels_DWGateProjRes.metal NAFBlockKernels_GateProjRes.metal NAFBlockKernels_ANE_Conv1x1.metal NAFBlockKernels_ANE_DWSiLUProjRes.metal NAFBlockKernels_ANE_SiLUProjRes.metal
endif
ifneq ($(CNN_BACKENDS),none)
CNN_LDFLAGS += -framework Accelerate
endif
METAL_AIR = $(METAL_SOURCES:.metal=.air)

GPR_LIBS = $(GPR_BUILD)/source/lib/vc5_encoder/libvc5_encoder.a \
           $(GPR_BUILD)/source/lib/vc5_decoder/libvc5_decoder.a \
           $(GPR_BUILD)/source/lib/vc5_common/libvc5_common.a \
           $(GPR_BUILD)/source/lib/common/libcommon.a

OBJS = gpr2prores.o DNGReader.o GPRFileReader.o GPRPipeline.o Demosaic.o Downscale.o CIRawDemosaic.o CIDNGBuilder.o ProResWriter.o GPRCodec.o $(CNN_OBJS)

.PHONY: all clean install uninstall

all: gpr2prores gpr_mov_tool

gpr2prores: $(OBJS) Demosaic.metallib $(GPR_LIBS) .build-config
	$(CC) $(OBJS) $(GPR_LIBS) $(LDFLAGS) $(CNN_LDFLAGS) -o $@

# gpr_mov_tool: pack/unpack a sequence of .gpr files into a custom-tagged MOV.
gpr_mov_tool: gpr_mov_tool.o GPRMovWriter.o GPRMovReader.o GPRFileReader.o DNGExif.o gvid_mov.o $(GPR_LIBS) .build-config
	$(CC) $(filter-out .build-config,$^) $(LDFLAGS) $(FFMPEG_LIBS) -o $@

gvid_mov.o: ../gvid_mov/src/gvid_mov.c ../gvid_mov/include/gvid_mov.h .build-config
	$(CC) -O2 -g -Wall -std=c11 $(FFMPEG_CFLAGS) -I../gvid_mov/include -c $< -o $@

%.o: %.m .build-config $(wildcard *.h)
	$(CC) $(CFLAGS) $(CNN_CPPFLAGS) -c $< -o $@

# A changed signature is phony for this invocation so even make versions with
# second-resolution timestamps rebuild every consumer on rapid flag switches.
CONFIG_SIGNATURE = '$(CNN_BACKENDS)' '$(CC)' '$(CFLAGS)' '$(CNN_CPPFLAGS)' '$(LDFLAGS)' '$(CNN_LDFLAGS)' '$(FFMPEG_CFLAGS)' '$(FFMPEG_LIBS)' '$(GPR_BUILD)'
ifneq ($(shell printf '%s\n' $(CONFIG_SIGNATURE) | cmp -s - .build-config || printf changed),)
.PHONY: .build-config
endif
.build-config:
	@printf '%s\n' $(CONFIG_SIGNATURE) > $@.tmp
	@mv -f $@.tmp $@

# Build Metal library next to binary so MTLDevice newDefaultLibrary can find it.
# Compile each .metal to .air, then link all into default.metallib.
%.air: %.metal .build-config
	xcrun -sdk macosx metal -c $< -o $@

default.metallib: $(METAL_AIR) .build-config Makefile
	xcrun -sdk macosx metallib $(METAL_AIR) -o $@

Demosaic.metallib: default.metallib .build-config
	cp $< $@

clean:
	rm -f $(OBJS) gpr2prores gpr_mov_tool *.o *.air *.metallib .build-config .build-config.tmp

# Install gpr2prores + gpr_mov_tool into $(BINDIR) and metallibs into $(LIBDIR).
# Set PREFIX to relocate (default /usr/local). Requires sudo for /usr/local.
# Symlink the metallib next to the binary so gpr2prores can find it without
# changing its search code.
install: gpr2prores gpr_mov_tool Demosaic.metallib
	@echo "Installing to $(PREFIX)"
	install -d "$(BINDIR)"
	install -d "$(LIBDIR)"
	install -m 0755 gpr2prores "$(BINDIR)/gpr2prores"
	install -m 0755 gpr_mov_tool "$(BINDIR)/gpr_mov_tool"
	install -m 0644 default.metallib "$(LIBDIR)/default.metallib"
	install -m 0644 Demosaic.metallib "$(LIBDIR)/Demosaic.metallib"
	ln -sf "$(LIBDIR)/default.metallib" "$(BINDIR)/default.metallib"
	ln -sf "$(LIBDIR)/Demosaic.metallib" "$(BINDIR)/Demosaic.metallib"
	@echo "Installed:"
	@echo "  $(BINDIR)/gpr2prores"
	@echo "  $(BINDIR)/gpr_mov_tool"
	@echo "  $(LIBDIR)/default.metallib (linked into BINDIR)"

uninstall:
	rm -f "$(BINDIR)/gpr2prores" "$(BINDIR)/gpr_mov_tool"
	rm -f "$(BINDIR)/default.metallib" "$(BINDIR)/Demosaic.metallib"
	rm -rf "$(LIBDIR)"
