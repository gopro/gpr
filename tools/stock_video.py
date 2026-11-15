#!/usr/bin/env python3
"""Inspect stock-VC5 GVID, extract frames, or run optional offline 1x/2x Bayer CNN.

This profile is deliberately distinct from the fork's fused-codec GVID v1.
Legacy BIBO architecture code is bundled; trained weights are caller-selected.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import time
import zlib

CLIP = struct.Struct("<4sHH4sIIIII4sII20s")
FRAME = struct.Struct("<4sIQQQ")
RECORD3 = struct.Struct("<4sIQQQIIII")


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_header(f):
    data = f.read(CLIP.size)
    if len(data) != CLIP.size:
        raise ValueError("truncated clip header")
    magic, version, size, codec, w, h, num, den, q, bayer, bits, fs, reserved = CLIP.unpack(data)
    if magic != b"GVID" or version not in (2, 3) or size != 64 or codec != b"VC5\0":
        raise ValueError("unsupported GVID profile (requires stock VC-5 v2/v3)")
    if version == 2 and (bayer, bits, fs, reserved) != (b"RGGB", 14, 32, bytes(20)):
        raise ValueError("unsupported GVID v2 profile")
    if not (0 < w <= 8192 and 0 < h <= 6144 and w % 8 == h % 8 == 0
            and 0 < num <= 240000 and 0 < den <= 10000 and num <= 240 * den and 0 <= q <= 5):
        raise ValueError("invalid clip dimensions, frame rate or quality")
    if version == 2 and (den != 1 or num > 240):
        raise ValueError("unsupported GVID v2 frame rate")
    header = dict(width=w, height=h, fps_num=num, fps_den=den, quality=q,
                  bayer=bayer.decode("ascii"), sample_bits=bits, codec="stock_vc5", version=version,
                  frame_header_bytes=fs)
    if version == 3:
        meta_size, flags, checksum, header_crc, zeros = struct.unpack("<IIIII", reserved)
        if (bayer not in (b"RGGB", b"GBRG", b"GRBG", b"BGGR") or bits not in (12, 14, 16) or fs != 48
                or meta_size > 65536 or flags != 1 or zeros or header_crc != zlib.crc32(data[:56])):
            raise ValueError("unsupported GVID v3 fields")
        metadata = f.read(meta_size)
        if len(metadata) != meta_size or zlib.crc32(metadata) != checksum:
            raise ValueError("invalid stream metadata length/checksum")
        header["metadata"] = decode_metadata(metadata)
        header["_metadata_size"] = meta_size
        header["_stream_crc"] = zlib.crc32(metadata, zlib.crc32(data))
    return header


def decode_metadata(data):
    def object_pairs(pairs):
        out = {}
        for key, value in pairs:
            if key in out:
                raise ValueError("duplicate GVID metadata key")
            out[key] = value
        return out
    def finite_float(text):
        result = float(text)
        if not math.isfinite(result):
            raise ValueError("nonfinite GVID metadata number")
        return result
    def invalid_constant(text):
        raise ValueError("nonfinite GVID metadata constant: " + text)
    value = json.loads(data.decode("utf-8"), object_pairs_hook=object_pairs,
                       parse_float=finite_float, parse_constant=invalid_constant) if data else {}
    if not isinstance(value, dict):
        raise ValueError("GVID metadata must be a JSON object")
    return value


def _gf2_matrix_times(mat, vec):
    total = 0
    idx = 0
    while vec:
        if vec & 1:
            total ^= mat[idx]
        vec >>= 1
        idx += 1
    return total


def _gf2_matrix_square(square, mat):
    for n in range(32):
        square[n] = _gf2_matrix_times(mat, mat[n])


def crc32_combine(crc1, crc2, len2):
    """Combine two IEEE 802.3 CRC-32 checksums in O(log(len2)) without scanning bytes."""
    if len2 <= 0:
        return crc1 & 0xFFFFFFFF
    even = [0] * 32
    odd = [0] * 32
    odd[0] = 0xEDB88320
    row = 1
    for n in range(1, 32):
        odd[n] = row
        row <<= 1
    _gf2_matrix_square(even, odd)
    _gf2_matrix_square(odd, even)
    crc1 &= 0xFFFFFFFF
    crc2 &= 0xFFFFFFFF
    while True:
        _gf2_matrix_square(even, odd)
        if len2 & 1:
            crc1 = _gf2_matrix_times(even, crc1)
        len2 >>= 1
        if len2 == 0:
            break
        _gf2_matrix_square(odd, even)
        if len2 & 1:
            crc1 = _gf2_matrix_times(odd, crc1)
        len2 >>= 1
        if len2 == 0:
            break
    return (crc1 ^ crc2) & 0xFFFFFFFF


def resolve_frame_calibration(header, frame_metadata, fallback=None):
    """Resolve per-frame calibration by inheriting clip-static raw_metadata + optical_stackup
    and applying compact per-frame dynamic overrides (e.g. exposure, gain, white balance)."""
    clip_meta = header.get("metadata", {})
    base_cal = clip_meta.get("raw_metadata", fallback)
    frame_cal = frame_metadata.get("raw_metadata")
    overrides = frame_metadata.get("raw_metadata_overrides")
    if frame_cal is not None:
        resolved = dict(frame_cal) if isinstance(frame_cal, dict) else frame_cal
    elif base_cal is not None:
        resolved = dict(base_cal) if isinstance(base_cal, dict) else base_cal
    else:
        return None
    if isinstance(resolved, dict):
        if isinstance(overrides, dict):
            resolved.update(overrides)
        if "optical_stackup" in clip_meta and "optical_stackup" not in resolved:
            resolved["optical_stackup"] = clip_meta["optical_stackup"]
    return resolved


def scan_index(f, header, recover=False):
    """Build an O(1) random-access frame index by reading only 48-byte FRM3 / 32-byte FRM2
    record headers and seeking over payloads, while algebraically verifying END3 stream_crc."""
    index = 0
    total = 0
    last_pts = 0
    version = header["version"]
    crc = header.get("_stream_crc", 0)
    record_size = FRAME.size if version == 2 else RECORD3.size
    header["_complete"] = False
    header["_end_marker_verified"] = False
    file_end = f.seek(0, 2)
    f.seek(CLIP.size + header.get("_metadata_size", 0))
    entries = []
    while True:
        try:
            record_offset = f.tell()
            data = f.read(record_size)
            if not data and version == 2:
                if not index:
                    raise ValueError("empty clip")
                header["_complete"] = True
                return entries
            if len(data) != record_size:
                raise ValueError("truncated frame header or missing END3")
            if version == 3 and data[:4] == b"END3":
                magic, size, count, timestamp, byte_count, stream_crc, end_crc, z1, z2 = RECORD3.unpack(data)
                if (size != 48 or count != index or timestamp != last_pts or byte_count != total
                        or stream_crc != crc or end_crc != zlib.crc32(data[:36]) or z1 or z2 or f.read(1)):
                    raise ValueError("invalid recording end marker or trailing data")
                header["_complete"] = header["_end_marker_verified"] = True
                return entries
            if version == 2:
                magic, size, number, pts, length = FRAME.unpack(data)
                if magic != b"FRM2" or size != 32 or pts != number * 1_000_000_000 // header["fps_num"]:
                    raise ValueError("invalid frame header or timestamp")
                meta_size, payload_crc, meta_crc, header_crc = 0, None, None, None
            else:
                magic, size, number, pts, length, meta_size, payload_crc, meta_crc, header_crc = RECORD3.unpack(data)
                if (magic != b"FRM3" or size != 48 or meta_size > 65536 or header_crc != zlib.crc32(data[:44])
                        or (index and pts <= last_pts)):
                    raise ValueError("invalid v3 frame header or timestamp")
            if number != index:
                raise ValueError("invalid frame sequence")
            if not 0 < length <= header["width"] * header["height"] * 8:
                raise ValueError("invalid payload length")
            meta_offset = f.tell()
            payload_offset = meta_offset + meta_size
            if payload_offset + length > file_end:
                raise ValueError("truncated payload")
            f.seek(payload_offset + length)
            if version == 3:
                crc = zlib.crc32(data, crc)
                crc = crc32_combine(crc, meta_crc, meta_size)
                crc = crc32_combine(crc, payload_crc, length)
            entries.append(dict(
                index=number,
                pts_ns=pts,
                record_offset=record_offset,
                metadata_offset=meta_offset,
                metadata_bytes=meta_size,
                payload_offset=payload_offset,
                bytes=length,
                payload_crc32=payload_crc,
                metadata_crc32=meta_crc,
                header_crc32=header_crc,
            ))
            index += 1
            last_pts = pts
            total += length
        except ValueError as error:
            if not recover:
                raise
            header["_recovery_error"] = str(error)
            return entries


def seek_record(f, header, entry):
    """Random-access read and CRC-verify a single GVID record in O(1) using its index entry."""
    version = header["version"]
    record_size = FRAME.size if version == 2 else RECORD3.size
    f.seek(entry["record_offset"])
    data = f.read(record_size)
    if len(data) != record_size:
        raise ValueError("truncated frame header at indexed offset")
    if version == 2:
        magic, size, number, pts, length = FRAME.unpack(data)
        if (magic != b"FRM2" or size != 32 or number != entry["index"]
                or pts != entry["pts_ns"] or length != entry["bytes"]):
            raise ValueError("indexed v2 frame header mismatch")
        meta_size, metadata = 0, b""
    else:
        magic, size, number, pts, length, meta_size, payload_crc, meta_crc, header_crc = RECORD3.unpack(data)
        if (magic != b"FRM3" or size != 48 or number != entry["index"] or pts != entry["pts_ns"]
                or length != entry["bytes"] or meta_size != entry["metadata_bytes"]
                or header_crc != zlib.crc32(data[:44])):
            raise ValueError("indexed v3 frame header mismatch")
        metadata = f.read(meta_size)
        if len(metadata) != meta_size or zlib.crc32(metadata) != meta_crc:
            raise ValueError("invalid frame metadata length/checksum at indexed offset")
    payload = f.read(length)
    if len(payload) != length:
        raise ValueError("truncated payload at indexed offset")
    if version == 3 and zlib.crc32(payload) != payload_crc:
        raise ValueError("payload checksum mismatch at indexed offset")
    return dict(index=number, pts_ns=pts, payload=payload,
                metadata=decode_metadata(metadata), metadata_bytes=meta_size)


def records(f, header, recover=False):
    index = 0
    total = 0
    last_pts = 0
    version = header["version"]
    crc = header.get("_stream_crc", 0)
    record_size = FRAME.size if version == 2 else RECORD3.size
    header["_complete"] = False
    header["_end_marker_verified"] = False
    file_end = f.seek(0, 2)
    f.seek(CLIP.size + header.get("_metadata_size", 0))
    while True:
        try:
            data = f.read(record_size)
            if not data and version == 2:
                if not index:
                    raise ValueError("empty clip")
                header["_complete"] = True
                return
            if len(data) != record_size:
                raise ValueError("truncated frame header or missing END3")
            if version == 3 and data[:4] == b"END3":
                magic, size, count, timestamp, byte_count, stream_crc, end_crc, z1, z2 = RECORD3.unpack(data)
                if (size != 48 or count != index or timestamp != last_pts or byte_count != total
                        or stream_crc != crc or end_crc != zlib.crc32(data[:36]) or z1 or z2 or f.read(1)):
                    raise ValueError("invalid recording end marker or trailing data")
                header["_complete"] = header["_end_marker_verified"] = True
                return
            if version == 2:
                magic, size, number, pts, length = FRAME.unpack(data)
                if magic != b"FRM2" or size != 32 or pts != number * 1_000_000_000 // header["fps_num"]:
                    raise ValueError("invalid frame header or timestamp")
                meta_size, metadata = 0, b""
            else:
                magic, size, number, pts, length, meta_size, payload_crc, meta_crc, header_crc = RECORD3.unpack(data)
                if (magic != b"FRM3" or size != 48 or meta_size > 65536 or header_crc != zlib.crc32(data[:44])
                        or (index and pts <= last_pts)):
                    raise ValueError("invalid v3 frame header or timestamp")
                metadata = f.read(meta_size)
                if len(metadata) != meta_size or zlib.crc32(metadata) != meta_crc:
                    raise ValueError("invalid frame metadata length/checksum")
            if number != index:
                raise ValueError("invalid frame sequence")
            if not 0 < length <= header["width"] * header["height"] * 8:
                raise ValueError("invalid payload length")
            if length > file_end - f.tell():
                raise ValueError("truncated payload")
            payload = f.read(length)
            if version == 3 and zlib.crc32(payload) != payload_crc:
                raise ValueError("payload checksum mismatch")
            frame_metadata = decode_metadata(metadata)
            if version == 3:
                crc = zlib.crc32(payload, zlib.crc32(metadata, zlib.crc32(data, crc)))
            index += 1
            last_pts = pts
            total += length
        except ValueError as error:
            if not recover:
                raise
            header["_recovery_error"] = str(error)
            return
        yield dict(index=number, pts_ns=pts, payload=payload, metadata=frame_metadata, metadata_bytes=meta_size)


def frames(f, header, recover=False):
    for row in records(f, header, recover):
        yield row["index"], row["pts_ns"], row["payload"]


def load_cnn(root, checkpoint, device, coreml_package=None, coreml_units='all',metal=None):
    import torch
    if coreml_package is not None and metal is not None:raise ValueError('select one native CNN backend')
    # Weights are always caller-selected. A caller may override the bundled
    # legacy BIBO architecture explicitly; nothing is downloaded.
    ckpt = torch.load(checkpoint, map_location="cpu", weights_only=True)
    output_scale = 1
    if "backbone_state" in ckpt:
        variants = {'F_ane_no_sr':1,'bibo_1x':1,'F_ane':2,'bibo_2x':2}
        if ckpt.get("variant") not in variants or ckpt.get("raw_norm") != 16383.0:
            raise ValueError("requires a supported BIBO checkpoint normalized by 16383")
        if ckpt.get('width',16)!=16 or ckpt.get('depth',3)!=3:
            raise ValueError('bundled legacy BIBO contract requires width 16 and depth 3')
        output_scale = variants[ckpt['variant']]
        module_path, entry = (Path(root)/"model.py" if root is not None else Path(__file__).with_name('stock_cnn_models.py')), "build"
    elif "config" in ckpt and "model" in ckpt:
        if root is None: raise ValueError('cleanup checkpoint requires an explicit external architecture directory')
        module_path, entry = Path(root) / "train_bayer_rgb_target_cleanup.py", "make_model"
    else:
        raise ValueError("unsupported CNN checkpoint schema")
    spec = importlib.util.spec_from_file_location("stock_video_external_cnn", module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if entry == "build":
        backbone = module.build(ckpt["variant"])
        backbone.load_state_dict(ckpt["backbone_state"], strict=True)
        scale = float(ckpt["residual_scale"])
        if not 0 < scale <= 1:
            raise ValueError("invalid checkpoint residual scale")

        class Residual(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.backbone = backbone

            def forward(self, x):
                base = x if output_scale==1 else torch.nn.functional.interpolate(
                    x, scale_factor=output_scale, mode='bicubic', align_corners=False).clamp(0,1)
                return (base + scale * self.backbone(x)).clamp(0, 1)

        model = Residual()
    else:
        model = module.make_model(dict(ckpt["config"]))
        model.load_state_dict(ckpt["model"], strict=True)
    info=dict(
        checkpoint_sha256=sha256(checkpoint), architecture_sha256=sha256(module_path),
        checkpoint_schema=entry, architecture_source='bundled' if root is None else 'external',
        output_scale=output_scale, runtime_raw_norm=16383, input_cfa='RGGB',
        device=str(device), quality_status="unvalidated_for_stock_video")
    if coreml_package is not None:
        if root is not None or entry!='build' or str(device)!='cpu':
            raise ValueError('Core ML requires bundled BIBO and CPU host tensors; native units are selected separately')
        from stock_coreml import load_runtime
        return load_runtime(coreml_package,info,scale,coreml_units),info
    if metal is not None:
        if root is not None or entry!='build' or str(device)!='cpu':
            raise ValueError('Metal requires bundled BIBO and CPU host tensors')
        from stock_metal import load_runtime
        return load_runtime(metal,info,scale),info
    return model.to(device).eval(),info


def restore(raw, width, height, model, device, tile, halo, scale=1):
    import numpy as np
    import torch
    import torch.nn.functional as functional
    if (type(scale) is not int or scale not in (1,2) or
        any(type(v) is not int for v in (width,height,tile,halo)) or
        width<=0 or height<=0 or width%2 or height%2 or tile<8 or tile%8 or halo<0 or halo%8):
        raise ValueError('invalid Bayer geometry, output scale or tile/halo')
    image = np.fromfile(raw, dtype="<u2").reshape(height, width)
    if np.any(image>16383): raise ValueError('CNN input exceeds RGGB14 count domain')
    planes = np.stack([image[0::2, 0::2], image[0::2, 1::2],
                       image[1::2, 0::2], image[1::2, 1::2]])
    out = np.empty((4,height//2*scale,width//2*scale),dtype='<u2')
    started = time.perf_counter()
    with torch.inference_mode():
        # Nonoverlapping output cores with context halos; padding respects the
        # three-level BIBO U-Net's spatial multiple of 8. Each output written once.
        for y in range(0, height // 2, tile):
            for x in range(0, width // 2, tile):
                y1, x1 = min(y + tile, height // 2), min(x + tile, width // 2)
                ya, xa = max(0, y - halo), max(0, x - halo)
                yb, xb = min(height // 2, y1 + halo), min(width // 2, x1 + halo)
                patch = torch.from_numpy(planes[:, ya:yb, xa:xb].astype(np.float32) / 16383.0)[None].to(device)
                ph, pw = patch.shape[-2:]
                patch = functional.pad(patch, (0, (-pw) % 8, 0, (-ph) % 8), mode="replicate")
                pred = model(patch)
                if pred.shape != (1,4,patch.shape[-2]*scale,patch.shape[-1]*scale) or not torch.isfinite(pred).all().item():
                    raise ValueError("CNN must return finite 4-plane Bayer data at the declared output scale")
                core = pred[0, :, (y-ya)*scale:(y1-ya)*scale, (x-xa)*scale:(x1-xa)*scale].cpu().numpy()
                out[:, y*scale:y1*scale, x*scale:x1*scale] = np.clip(core * 16383.0 + .5, 0, 16383).astype("<u2")
    restored = np.empty((height*scale,width*scale),dtype='<u2')
    restored[0::2, 0::2], restored[0::2, 1::2] = out[0], out[1]
    restored[1::2, 0::2], restored[1::2, 1::2] = out[2], out[3]
    return restored, time.perf_counter() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "index", "extract", "restore"))
    parser.add_argument("clip", type=Path)
    parser.add_argument("--out-dir", type=Path)
    parser.add_argument("--decoder", type=Path, help="stock_video executable")
    parser.add_argument("--start-frame", type=int, default=0, help="random-access starting frame index via O(1) header index")
    parser.add_argument("--limit", type=int, default=0, help="0 exports all; inspection always validates all")
    parser.add_argument("--cnn-root", type=Path, help="external architecture override; default: bundled BIBO")
    parser.add_argument("--checkpoint", type=Path)
    from stock_coreml import add_arguments
    add_arguments(parser)
    import stock_metal
    stock_metal.add_arguments(parser)
    import stock_native
    stock_native.add_arguments(parser)
    parser.add_argument('--scale',type=int,choices=(1,2),default=1,help='explicit reconstruction scale')
    parser.add_argument("--reference-raw", type=Path, action="append",
                        help="optional reference replay RAWs in input order, for distortion measurements")
    parser.add_argument("--device", choices=("cpu", "mps", "cuda"), default="cpu")
    parser.add_argument("--tile", type=int, default=256, help="output core size in Bayer-plane pixels")
    parser.add_argument("--halo", type=int, default=64, help="context on each side of the output core")
    parser.add_argument("--recover", action="store_true", help="export only verified complete prefix; report incomplete recording")
    parser.add_argument("--editable", choices=("dng", "gpr", "both"),
                        help="export calibrated editable RAWs; GPR copies the original VC-5 payload")
    parser.add_argument("--metadata-file", type=Path,
                        help="gpr.raw_metadata.v1 default calibration; per-frame/clip raw_metadata overrides it")
    args = parser.parse_args()
    metal=stock_metal.configuration(args)
    standalone=stock_native.configuration(args)
    if standalone is not None and (args.action!='restore' or args.cnn_root or args.coreml_package or metal or args.device!='cpu'):
        parser.error('standalone native requires restore and cannot combine Python backends/devices')
    if metal is not None and args.action!='restore':parser.error('Metal options require restore')
    if args.start_frame < 0 or args.limit < 0 or args.tile < 8 or args.tile % 8 or args.halo < 0 or args.halo % 8:
        parser.error("invalid start-frame, frame limit, tile or halo (spatial sizes must be multiples of 8)")
    if args.action not in ("inspect", "index") and args.out_dir is None:
        parser.error("--out-dir is required")
    if args.action == "restore" and not all((args.decoder, args.checkpoint)):
        parser.error("restore requires --decoder and --checkpoint")
    if args.scale!=1 and args.action!='restore': parser.error('--scale requires restore')
    if args.coreml_package and args.action!='restore':parser.error('--coreml-package requires restore')
    if args.scale!=1 and args.reference_raw: parser.error('2x reference comparison requires an explicit high-resolution benchmark')
    if args.reference_raw and (not args.decoder or args.action in ("inspect", "index")):
        parser.error("reference measurements require extraction/restore with --decoder")
    if args.editable and args.action in ("inspect", "index"):
        parser.error("editable exports require extract/restore")
    if args.editable in ("dng", "both") and not args.decoder:
        parser.error("editable DNG export requires --decoder")
    if args.action == "restore" and args.editable in ("gpr", "both"):
        parser.error("restored GPR requires a new encode; use --editable dng for reconstructed RAW")
    if args.metadata_file and not args.editable:
        parser.error("--metadata-file requires --editable")
    fallback_metadata = decode_metadata(args.metadata_file.read_bytes()) if args.metadata_file else None
    model, model_info = None, None
    if args.action == "restore":
        if standalone is not None:
            model_info=stock_native.inspect(args.checkpoint,standalone,args.scale)
            standalone['contract_sha256']=model_info['artifact_contract_sha256']
        else:
            import torch
            device = torch.device(args.device)
            model, model_info = load_cnn(args.cnn_root, args.checkpoint, device,args.coreml_package,args.coreml_units,metal)
        if model_info.get('output_scale',1)!=args.scale: raise ValueError('checkpoint output scale differs from requested reconstruction scale')
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=False)
    with args.clip.open("rb") as f:
        header = read_header(f)
        if (model is not None or standalone is not None) and (header["bayer"], header["sample_bits"]) != ("RGGB", 14):
            raise ValueError("the current CNN checkpoint adapter requires RGGB14")
        if args.action == "index":
            idx_entries = scan_index(f, header, args.recover)
            report = dict(
                schema="gpr.stock_video.index.v1",
                clip_sha256=sha256(args.clip),
                header={k: v for k, v in header.items() if not k.startswith("_")},
                frame_count=len(idx_entries),
                complete=header["_complete"],
                end_marker_verified=header["_end_marker_verified"],
                recovery_error=header.get("_recovery_error"),
                frames=idx_entries,
            )
            result = json.dumps(report, indent=2) + "\n"
            if args.out_dir:
                (args.out_dir / "index.json").write_text(result)
            print(result, end="")
            return
        report = dict(schema="gpr.stock_video.inspect.v1", clip_sha256=sha256(args.clip),
                      header={k: v for k, v in header.items() if not k.startswith("_")}, frames=[], cnn=model_info)
        if args.start_frame > 0 and args.action in ("extract", "restore"):
            idx_entries = scan_index(f, header, args.recover)
            selected_entries = idx_entries[args.start_frame : (args.start_frame + args.limit) if args.limit else None]
            record_iter = (seek_record(f, header, entry) for entry in selected_entries)
        else:
            record_iter = records(f, header, args.recover)
        for record in record_iter:
            index, pts, payload = record["index"], record["pts_ns"], record["payload"]
            row = dict(index=index, pts_ns=pts, bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
            if header["version"] == 3:
                row["metadata"] = record["metadata"]
                row["metadata_bytes"] = record["metadata_bytes"]
            calibration = None
            if args.editable:
                import raw_media
                calibration = resolve_frame_calibration(header, record["metadata"], fallback_metadata)
                raw_media.validate(calibration, header)
                row["raw_metadata_sha256"] = hashlib.sha256(json.dumps(calibration, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            if args.action != "inspect" and index >= args.start_frame and (not args.limit or (index - args.start_frame) < args.limit):
                vc5 = args.out_dir / f"{index:06d}.vc5"
                vc5.write_bytes(payload)
                if args.editable in ("gpr", "both"):
                    path = vc5.with_suffix(".gpr")
                    raw_media.write(path, calibration, payload, "gpr")
                    row["gpr_sha256"] = sha256(path)
                if args.decoder:
                    raw = vc5.with_suffix(".raw")
                    subprocess.run([str(args.decoder.resolve()), "--decode", str(vc5), "--output", str(raw),
                                    "--width", str(header["width"]), "--height", str(header["height"]),
                                    "--cfa", header["bayer"], "--sample-bits", str(header["sample_bits"])], check=True)
                    if raw.stat().st_size != header["width"] * header["height"] * 2:
                        raise ValueError("decoded RAW size mismatch")
                    row["raw_sha256"] = sha256(raw)
                    restored = None;restored_data=None
                    if model is not None:
                        restored, elapsed = restore(raw, header["width"], header["height"],
                                                    model, device, args.tile, args.halo,scale=args.scale)
                        path = vc5.with_suffix(".cnn.raw")
                        restored.tofile(path)
                        restored_data=restored.astype('<u2').tobytes()
                        row.update(cnn_s=elapsed, cnn_sha256=sha256(path),output_width=header['width']*args.scale,
                                   output_height=header['height']*args.scale,output_scale=args.scale)
                    elif standalone is not None:
                        path=vc5.with_suffix('.cnn.raw')
                        restored_data,native_receipt=stock_native.restore(raw,header['width'],header['height'],args.scale,
                            args.checkpoint,standalone,path,args.tile,args.halo)
                        report['cnn']=native_receipt['model']
                        row.update(cnn_s=native_receipt['restore_s'],cnn_sha256=sha256(path),
                            output_width=header['width']*args.scale,output_height=header['height']*args.scale,
                            output_scale=args.scale,native_reconstruction=native_receipt)
                    if args.editable in ("dng", "both"):
                        path = vc5.with_suffix(".dng")
                        output_calibration=calibration
                        if restored_data is not None:
                            from stock_cnn_metadata import scaled
                            output_calibration,geometry=scaled(calibration,args.scale)
                            row.update(geometry=geometry,output_metadata_sha256=hashlib.sha256(json.dumps(output_calibration,sort_keys=True,separators=(',',':')).encode()).hexdigest())
                        raw_media.write(path, output_calibration, restored_data if restored_data is not None else raw.read_bytes())
                        row["dng_sha256"] = sha256(path)
                    if args.reference_raw:
                        import numpy as np
                        ref_path = args.reference_raw[index % len(args.reference_raw)]
                        ref = np.fromfile(ref_path, dtype="<u2").reshape(header["height"], header["width"]).astype(np.float64)
                        def psnr(image):
                            mse = float(np.mean((image.astype(np.float64) - ref) ** 2))
                            peak=(1 << header["sample_bits"])-1
                            return float(10 * np.log10(float(peak ** 2) / mse)) if mse else None
                        row.update(reference_sha256=sha256(ref_path),
                                   stock_psnr_db=psnr(np.fromfile(raw, dtype="<u2").reshape(ref.shape)))
                        if restored is not None:
                            row["cnn_psnr_db"] = psnr(restored)
                        elif restored_data is not None:
                            row['cnn_psnr_db']=psnr(np.frombuffer(restored_data,dtype='<u2').reshape(ref.shape))
            report["frames"].append(row)
    report["frame_count"] = len(report["frames"])
    report["complete"] = header["_complete"]
    report["end_marker_verified"] = header["_end_marker_verified"]
    report["recovery_error"] = header.get("_recovery_error")
    report["required_payload_MBps"] = (sum(row["bytes"] for row in report["frames"]) / report["frame_count"]
                                        * header["fps_num"] / header["fps_den"] / 1e6) if report["frame_count"] else 0
    report["required_MBps"] = (sum(row["bytes"] + header["frame_header_bytes"] + row.get("metadata_bytes", 0)
                                     for row in report["frames"]) / report["frame_count"]
                               * header["fps_num"] / header["fps_den"] / 1e6) if report["frame_count"] else 0
    result = json.dumps(report, indent=2) + "\n"
    if args.out_dir:
        (args.out_dir / "receipt.json").write_text(result)
    print(result, end="")


if __name__ == "__main__":
    main()
