// Experimental stock VC-5 sequence writer. Apache-2.0 OR MIT.
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include "vc5_encoder.h"
#include "vc5_decoder.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
static std::unique_ptr<unsigned char[]> arena;
static size_t capacity;
static std::atomic<size_t> used(0);
static void *allocate(size_t n) {
    if (n > SIZE_MAX - 63) return NULL;
    size_t aligned = (n + 63) & ~size_t(63);
    size_t offset = used.fetch_add(aligned);
    return offset <= capacity && aligned <= capacity - offset ? arena.get() + offset : NULL;
}
static void release(void *) {}
static void put(unsigned char *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
static void write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("output write failed");
        p += n; size -= size_t(n);
    }
}
struct File {
    int fd;
    explicit File(const std::string &path) : fd(open(path.c_str(), O_WRONLY|O_CREAT|O_EXCL, 0644)) {
        if (fd < 0) throw std::runtime_error("cannot create output (must not already exist): " + path);
    }
    ~File() { close(fd); }
};
static std::vector<unsigned char> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0) throw std::runtime_error("cannot read nonempty input: " + path);
    std::vector<unsigned char> data(size_t(f.tellg()));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(data.data()), data.size()))
        throw std::runtime_error("short input: " + path);
    return data;
}
static uint64_t number(const std::string &s) {
    size_t end = 0;
    if (s.empty() || s[0] == '-') throw std::runtime_error("expected unsigned integer");
    uint64_t v = std::stoull(s, &end);
    if (end != s.size()) throw std::runtime_error("invalid integer: " + s);
    return v;
}
static double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values[size_t(std::ceil(p * values.size())) - 1];
}
int main(int argc, char **argv) {
    try {
        unsigned width = 4096, height = 3072, fps = 30, quality = 4;
        size_t frames = 120, arena_mib = 192;
        bool paced = false, sync_each = false;
        std::string output, decode, cfa="RGGB";
        unsigned sample_bits=14;
        std::vector<std::string> inputs;
        for (int a = 1; a < argc; ++a) {
            std::string k = argv[a];
            if (k == "--help") {
                std::cout << "stock_video --input frame.raw [--input another.raw] --output clip.gvid\n"
                  "  [--width 4096 --height 3072 --quality 4 --fps 30 --frames 120]\n"
                  "  [--arena-mib 192] [--paced] [--fsync-each]\n"
                  "stock_video --decode frame.vc5 --output frame.raw --width 4096 --height 3072 [--cfa RGGB --sample-bits 14]\n";
                return 0;
            }
            if (k == "--paced") { paced = true; continue; }
            if (k == "--fsync-each") { sync_each = true; continue; }
            if (++a == argc) throw std::runtime_error("missing value: " + k);
            std::string v = argv[a];
            if (k == "--input") inputs.push_back(v);
            else if (k == "--output") output = v;
            else if (k == "--decode") decode = v;
            else if (k == "--cfa") cfa = v;
            else {
                uint64_t n = number(v);
                if (n > UINT32_MAX) throw std::runtime_error("option too large");
                if (k == "--width") width = unsigned(n);
                else if (k == "--sample-bits") sample_bits = unsigned(n);
                else if (k == "--height") height = unsigned(n);
                else if (k == "--fps") fps = unsigned(n);
                else if (k == "--quality") quality = unsigned(n);
                else if (k == "--frames") frames = size_t(n);
                else if (k == "--arena-mib") arena_mib = size_t(n);
                else throw std::runtime_error("unknown option: " + k);
            }
        }
        if (output.empty() || !width || !height || width % 8 || height % 8 ||
            width > 8192 || height > 6144 || !fps || fps > 240 || quality > 5 ||
            !frames || frames > 1000000 || arena_mib > 2048)
            throw std::runtime_error("invalid dimensions, quality, rate, frame count or workspace");
        if ((cfa!="RGGB" && cfa!="GBRG" && cfa!="GRBG" && cfa!="BGGR") ||
            (sample_bits!=12 && sample_bits!=14 && sample_bits!=16))
            throw std::runtime_error("invalid Bayer format");
        const size_t raw_size = size_t(width) * height * 2;
        if (!decode.empty()) {
            if (!inputs.empty()) throw std::runtime_error("decode and encode are exclusive");
            auto data = read_file(decode);
            gpr_buffer compressed = {data.data(), data.size()}, raw = {NULL, 0};
            vc5_decoder_parameters p = {};
            vc5_decoder_parameters_set_default(&p);
            p.mem_alloc = malloc; p.mem_free = free;
            static const VC5_DECODER_PIXEL_FORMAT formats[4][3] = {
                {VC5_DECODER_PIXEL_FORMAT_RGGB_12,VC5_DECODER_PIXEL_FORMAT_RGGB_14,VC5_DECODER_PIXEL_FORMAT_RGGB_16},
                {VC5_DECODER_PIXEL_FORMAT_GBRG_12,VC5_DECODER_PIXEL_FORMAT_GBRG_14,VC5_DECODER_PIXEL_FORMAT_GBRG_16},
                {VC5_DECODER_PIXEL_FORMAT_GRBG_12,VC5_DECODER_PIXEL_FORMAT_GRBG_14,VC5_DECODER_PIXEL_FORMAT_GRBG_16},
                {VC5_DECODER_PIXEL_FORMAT_BGGR_12,VC5_DECODER_PIXEL_FORMAT_BGGR_14,VC5_DECODER_PIXEL_FORMAT_BGGR_16}};
            unsigned phase=cfa=="RGGB" ? 0:cfa=="GBRG" ? 1:cfa=="GRBG" ? 2:3;
            p.pixel_format=formats[phase][(sample_bits-12)/2];
            CODEC_ERROR err = vc5_decoder_process(&p, &compressed, &raw, NULL);
            std::unique_ptr<void, decltype(&free)> owner(raw.buffer, free);
            if (err || !raw.buffer || raw.size != raw_size)
                throw std::runtime_error("decoder failed or decoded byte count differs");
            File out(output);
            write_all(out.fd, raw.buffer, raw.size);
            return 0;
        }
        if (inputs.empty() || inputs.size() > 16) throw std::runtime_error("supply 1..16 RAW inputs");
        if (cfa!="RGGB" || sample_bits!=14) throw std::runtime_error("GVID v2 writer requires RGGB14");
        std::vector<std::vector<unsigned char>> raw;
        for (const auto &path : inputs) {
            raw.push_back(read_file(path));
            if (raw.back().size() != raw_size) throw std::runtime_error("RAW size mismatch: " + path);
            for (size_t i = 0; i < raw_size; i += 2)
                if (unsigned(raw.back()[i]) + (unsigned(raw.back()[i+1]) << 8) > 16383)
                    throw std::runtime_error("input must be little-endian unpacked 14-bit RGGB");
        }
        capacity = arena_mib * size_t(1024 * 1024);
        const auto setup_begin = Clock::now();
        if (capacity) { arena.reset(new unsigned char[capacity]); memset(arena.get(), 0, capacity); }
        double workspace_setup_s = seconds(Clock::now() - setup_begin);
        vc5_encoder_parameters p = {};
        vc5_encoder_parameters_set_default(&p);
        p.input_width = width; p.input_height = height; p.input_pitch = width * 2;
        p.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_14;
        p.quality_setting = static_cast<VC5_ENCODER_QUALITY_SETTING>(quality);
        p.mem_alloc = capacity ? allocate : malloc; p.mem_free = capacity ? release : free;
        // These opt-in kernels are inherited from the architecture experiment.
        const char *flags[] = {"GPR_STREAM_UNPACK", "GPR_BAND_STREAM", "GPR_CHANNEL_ASM",
            "GPR_PIN_CHANNELS", "GPR_DIRECT_BANDS", "GPR_COMPACT_BANDS"};
        for (auto flag : flags) setenv(flag, "1", 1);
        File out(output);
        unsigned char header[64] = {};
        memcpy(header, "GVID", 4); put(header+4, 2, 2); put(header+6, 64, 2);
        memcpy(header+8, "VC5\0", 4); put(header+12, width, 4); put(header+16, height, 4);
        put(header+20, fps, 4); put(header+24, 1, 4); put(header+28, quality, 4);
        memcpy(header+32, "RGGB", 4); put(header+36, 14, 4); put(header+40, 32, 4);
        write_all(out.fd, header, sizeof(header));
        size_t payload_bytes = 0, workspace_peak = 0, late = 0;
        std::vector<double> encode_times, service_times;
        const auto begin = Clock::now();
        for (size_t i = 0; i < frames; ++i) {
            auto due = begin + std::chrono::nanoseconds(uint64_t(i) * 1000000000ULL / fps);
            if (paced) std::this_thread::sleep_until(due);
            auto start = Clock::now();
            auto &source = raw[i % raw.size()];
            gpr_buffer input = {source.data(), source.size()}, compressed = {NULL, 0};
            CODEC_ERROR err = vc5_encoder_process(&p, &input, &compressed, NULL);
            auto encoded = Clock::now();
            if (err || !compressed.buffer || !compressed.size)
                throw std::runtime_error("frame encode failed: " + std::to_string(int(err)));
            unsigned char frame[32] = {};
            memcpy(frame, "FRM2", 4); put(frame+4, 32, 4); put(frame+8, i, 8);
            put(frame+16, uint64_t(i) * 1000000000ULL / fps, 8);
            put(frame+24, compressed.size, 8);
            write_all(out.fd, frame, sizeof(frame));
            write_all(out.fd, compressed.buffer, compressed.size);
            payload_bytes += compressed.size;
            p.mem_free(compressed.buffer);
            workspace_peak = std::max(workspace_peak, used.load());
            used.store(0); // Output consumed, all workers joined: arena can now be reused.
            if (sync_each && fsync(out.fd)) throw std::runtime_error("frame fsync failed");
            auto done = Clock::now();
            double encode_s = seconds(encoded - start), service_s = seconds(done - start);
            bool missed = paced ? seconds(done - due) > 1.0 / fps : service_s > 1.0 / fps;
            late += missed;
            encode_times.push_back(encode_s); service_times.push_back(service_s);
            std::cout << "{\"event\":\"frame\",\"index\":" << i << ",\"input_index\":" << i % raw.size()
                << ",\"bytes\":" << compressed.size << ",\"encode_s\":" << encode_s
                << ",\"service_s\":" << service_s << ",\"deadline_missed\":" << (missed ? "true" : "false") << "}\n";
        }
        const auto sync_begin = Clock::now();
        if (fsync(out.fd)) throw std::runtime_error("final fsync failed");
        const auto end = Clock::now();
        double elapsed = seconds(end-begin), buffered_elapsed = seconds(sync_begin-begin), sum = 0;
        for (double s : service_times) sum += s;
        std::cout << "{\"event\":\"summary\",\"schema\":\"gpr.stock_video.v1\",\"frames\":" << frames
            << ",\"width\":" << width << ",\"height\":" << height << ",\"quality\":" << quality
            << ",\"fps_target\":" << fps << ",\"paced\":" << (paced ? "true" : "false")
            << ",\"fsync_each\":" << (sync_each ? "true" : "false")
            << ",\"elapsed_s\":" << elapsed << ",\"wall_fps\":" << frames / elapsed
            << ",\"buffered_write_elapsed_s\":" << buffered_elapsed
            << ",\"buffered_write_fps\":" << frames / buffered_elapsed
            << ",\"service_fps\":" << frames / sum << ",\"encode_median_ms\":" << percentile(encode_times,.5)*1000
            << ",\"service_p95_ms\":" << percentile(service_times,.95)*1000
            << ",\"service_max_ms\":" << *std::max_element(service_times.begin(),service_times.end())*1000
            << ",\"deadline_misses\":" << late << ",\"payload_bytes\":" << payload_bytes
            << ",\"required_MBps\":" << double(payload_bytes)/frames*fps/1e6
            << ",\"final_fsync_s\":" << seconds(end-sync_begin)
            << ",\"workspace_setup_s\":" << workspace_setup_s << ",\"workspace_capacity_bytes\":" << capacity
            << ",\"workspace_peak_bytes\":" << workspace_peak << "}\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
