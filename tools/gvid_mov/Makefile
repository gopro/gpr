# gvid_mov — GVIDMov MOV container reader/writer + pack/unpack CLI tools.
#
# Builds against Homebrew FFmpeg (libavformat + libavutil). Tested with
# FFmpeg 8.0.1 / lavf 62.3 / lavu 60.8 on macOS arm64.

CC      ?= clang
PKG     ?= pkg-config
AV_CFLAGS  := $(shell $(PKG) --cflags libavformat libavcodec libavutil)
AV_LDLIBS  := $(shell $(PKG) --libs   libavformat libavcodec libavutil)

CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 \
           -Iinclude $(AV_CFLAGS)
LDLIBS  := $(AV_LDLIBS)

OBJDIR   = build

LIB_OBJS    = $(OBJDIR)/gvid_mov.o
PACK_OBJS   = $(OBJDIR)/gvid_mov_pack.o   $(LIB_OBJS)
UNPACK_OBJS = $(OBJDIR)/gvid_mov_unpack.o $(LIB_OBJS)

BINS = gvid_mov_pack gvid_mov_unpack

.PHONY: all clean
all: $(BINS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

gvid_mov_pack: $(PACK_OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

gvid_mov_unpack: $(UNPACK_OBJS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(OBJDIR) $(BINS)
