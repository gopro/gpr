#!/usr/bin/env python3
"""Calibrated Bayer metadata and editable DNG/GPR packaging (stdlib only).

GPR packaging copies an existing VC-5 payload; it never invokes an encoder.
Calibration is source supplied, not inferred from the compressed coefficients.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

SCHEMA = 'gpr.raw_metadata.v1'
# TIFF storage/pointer/digest tags are deliberately rebuilt, not copied.
IMAGE_TAGS = {270, 271, 272, 274, 306, 700, 33421, 33422,
              50706, 50707, 50708, 50709, 50710, 50711, 50712, 50713, 50714,
              50715, 50716, 50717, 50718, 50719, 50720, 50721, 50722, 50723,
              50724, 50725, 50726, 50727, 50728, 50729, 50730, 50731, 50732,
              50733, 50734, 50735, 50736, 50738, 50739, 50741,
              50778, 50779, 50780, 50827, 50828, 50829, 50830, 50831, 50832,
              50931, 50932, 50933, 50934, 50935, 50936, 50937, 50938, 50939,
              50940, 50941, 50942, 50964, 50965, 50966, 50967, 50968,
              50969, 50970, 50971, 50972, 50973, 51008, 51009, 51022,
              51041, 51125}
UNITS = {1: 1, 2: 1, 3: 2, 4: 4, 5: 4, 7: 1, 9: 4, 10: 4, 11: 4, 12: 8, 13: 4}
MAX_METADATA = 2 * 1024 * 1024
GLOBAL_TAGS = IMAGE_TAGS - {33421, 33422, 50711, 50712, 50713, 50714, 50715, 50716,
                           50717, 50718, 50719, 50720, 50829, 50830, 50831, 50832,
                           51008, 51009, 51022, 51125}
DOMAIN_LOOKUP = {bits: bytes(int(value >= 1 << (bits-8)) for value in range(256)) for bits in (12,14)}


def samples_in_domain(payload, bits):
    # bytes.translate performs the high-byte test in a C loop; avoid Python's
    # generic per-byte comparisons over large sensor images.
    return bits==16 or payload[1::2].translate(DOMAIN_LOOKUP[bits]).find(b'\1')<0


def size_of(dtype, count):
    if type(dtype) is not int or dtype not in UNITS or type(count) is not int or not 0 < count <= MAX_METADATA:
        raise ValueError('invalid TIFF metadata type/count')
    size = UNITS[dtype] * count * (2 if dtype in (5, 10) else 1)
    if size > MAX_METADATA:
        raise ValueError('TIFF metadata value exceeds limit')
    return size


def record(code, dtype, count, data):
    return dict(code=code, type=dtype, count=count,
                data=base64.b64encode(zlib.compress(data)).decode('ascii'))


def unpack_record(tag):
    if not isinstance(tag, dict) or set(tag) != {'code', 'type', 'count', 'data'}:
        raise ValueError('invalid metadata tag record')
    if type(tag['code']) is not int or not 0 <= tag['code'] < 65536:
        raise ValueError('invalid metadata tag code')
    size = size_of(tag['type'], tag['count'])
    if not isinstance(tag['data'], str) or len(tag['data']) > MAX_METADATA * 2:
        raise ValueError('invalid compressed metadata')
    packed = base64.b64decode(tag['data'], validate=True)
    decoder = zlib.decompressobj()
    data = decoder.decompress(packed, size + 1)
    if len(data) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('metadata decompression size/trailer mismatch')
    return data


def values(tag):
    data = unpack_record(tag)
    dtype, count = tag['type'], tag['count']
    fmt = {1: 'B', 3: 'H', 4: 'I', 5: 'I', 9: 'i', 10: 'i', 11: 'f', 12: 'd', 13: 'I'}.get(dtype)
    if fmt is None:
        raise ValueError('expected numeric metadata')
    result = struct.unpack('<' + fmt * (count * (2 if dtype in (5, 10) else 1)), data)
    if dtype in (5, 10):
        if any(x == 0 for x in result[1::2]):
            raise ValueError('zero rational denominator')
        result = tuple(a / b for a, b in zip(result[0::2], result[1::2]))
    if not all(math.isfinite(x) for x in result):
        raise ValueError('nonfinite calibration value')
    return result


def validate(meta, header=None):
    if not isinstance(meta, dict) or meta.get('schema') != SCHEMA:
        raise ValueError('requires gpr.raw_metadata.v1 calibration')
    w, h, bits = (meta.get(k) for k in ('width', 'height', 'sample_bits'))
    if (any(type(v) is not int for v in (w, h, bits)) or not 0 < w <= 32768 or
            not 0 < h <= 32768 or w % 2 or h % 2 or bits not in (12, 14, 16)):
        raise ValueError('unsupported Bayer geometry/domain')
    if meta.get('cfa') not in ('RGGB', 'GBRG', 'GRBG', 'BGGR'):
        raise ValueError('unsupported CFA phase')
    groups = []
    total = 0
    for group in ('image', 'exif', 'gps'):
        tags = meta.get(group)
        if not isinstance(tags, list) or len(tags) > 256:
            raise ValueError('invalid metadata directory')
        table = {}
        for tag in tags:
            data = unpack_record(tag); code = tag['code']
            if (code == 0 and group != 'gps') or tag['type'] == 13 or code in table or (group == 'image' and code not in IMAGE_TAGS) or code in (330, 34665, 34853, 37500, 40965):
                raise ValueError('duplicate, structural or unsupported metadata tag')
            if tag['type'] in (5, 10, 11, 12):
                values(tag)
            total += len(data)
            if total > MAX_METADATA:
                raise ValueError('total metadata exceeds limit')
            table[code] = tag
        groups.append(table)
    image = groups[0]
    if (50710 in image and (image[50710]['type']!=1 or values(image[50710])!=(0,1,2)) or
            50711 in image and values(image[50711])!=(1,)):
        raise ValueError('requires canonical RGB CFA palette and rectangular Bayer layout')
    for code in (271, 272, 50706, 50708, 33421, 33422, 50713, 50714, 50717, 50721, 50728, 50778):
        if code not in image:
            raise ValueError(f'missing required calibration tag {code}')
    for code, types, count in ((271, (2,), None), (272, (2,), None), (50708, (2,), None),
                              (50706, (1,), 4), (33421, (3,), 2), (33422, (1,), 4),
                              (50713, (3,), 2), (50714, (3, 4, 5), None),
                              (50717, (3, 4), 1), (50721, (10,), 9), (50722, (10,), 9),
                              (50728, (5,), 3), (50778, (3,), 1), (50779, (3,), 1)):
        if code in image and (image[code]['type'] not in types or (count and image[code]['count'] != count)):
            raise ValueError(f'invalid calibration type/count for {code}')
    if unpack_record(image[50706]) > b'\x01\x04\x00\x00':
        raise ValueError('requires DNG version no newer than 1.4')
    pattern = bytes({'R': 0, 'G': 1, 'B': 2}[c] for c in meta['cfa'])
    if (values(image[33421]) != (2, 2) or unpack_record(image[33422]) != pattern or
            values(image[50713]) not in ((1, 1), (1, 2), (2, 1), (2, 2))):
        raise ValueError('calibration CFA/black repeat mismatch')
    black, white, neutral = values(image[50714]), values(image[50717]), values(image[50728])
    repeat = values(image[50713])
    if (len(black) != int(repeat[0] * repeat[1]) or len(white) != 1 or
            not 0 < white[0] <= (1 << bits) - 1 or any(not 0 <= x < white[0] for x in black) or
            len(neutral) != 3 or any(x <= 0 for x in neutral)):
        raise ValueError('invalid black/white/neutral calibration')
    for code in (50721, 50722):
        if code in image:
            m = values(image[code])
            if len(m) != 9 or abs(m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6])) < 1e-12:
                raise ValueError('invalid color matrix')
    area = values(image[50829]) if 50829 in image else (0, 0, h, w)
    if len(area) != 4 or not (0 <= area[0] < area[2] <= h and 0 <= area[1] < area[3] <= w):
        raise ValueError('active area outside Bayer image')
    origin = values(image[50719]) if 50719 in image else (0, 0)
    crop = values(image[50720]) if 50720 in image else (area[3]-area[1], area[2]-area[0])
    if (len(origin) != 2 or len(crop) != 2 or any(x < 0 for x in origin) or any(x <= 0 for x in crop) or
            origin[0]+crop[0] > area[3]-area[1] or origin[1]+crop[1] > area[2]-area[0]):
        raise ValueError('default crop outside active area')
    if header is not None and (w, h, bits, meta['cfa']) != tuple(header[k] for k in ('width', 'height', 'sample_bits', 'bayer')):
        raise ValueError('calibration and recording domains disagree')
    return groups


def extract(path, sample_bits, with_payload=False):
    """Read classic TIFF/DNG metadata without decoding its image payload."""
    path = Path(path)
    with path.open('rb') as f:
        f.seek(0, 2); file_size = f.tell(); f.seek(0)
        prefix = f.read(8)
        if prefix[:2] not in (b'II', b'MM'):
            raise ValueError('requires TIFF/DNG/GPR source')
        endian = '<' if prefix[:2] == b'II' else '>'
        if len(prefix) != 8 or struct.unpack(endian+'H', prefix[2:4])[0] != 42:
            raise ValueError('requires classic TIFF (BigTIFF unsupported)')
        metadata_read = 0
        def read(offset, size):
            nonlocal metadata_read
            if offset < 0 or size > MAX_METADATA or offset + size > file_size:
                raise ValueError('metadata offset/size outside source')
            metadata_read += size
            if metadata_read > MAX_METADATA:
                raise ValueError('source metadata exceeds read limit')
            f.seek(offset); data = f.read(size)
            if len(data) != size:
                raise ValueError('truncated metadata')
            return data
        def directory(offset):
            count, = struct.unpack(endian+'H', read(offset, 2))
            if count > 256:
                raise ValueError('too many TIFF entries')
            entries = read(offset+2, count*12+4); table = {}
            for i in range(count):
                entry = entries[i*12:i*12+12]
                code, dtype, n = struct.unpack(endian+'HHI', entry[:8])
                size = size_of(dtype, n)
                raw = entry[8:8+size] if size <= 4 else read(struct.unpack(endian+'I', entry[8:])[0], size)
                if endian == '>' and UNITS[dtype] > 1:
                    unit = UNITS[dtype]; raw = b''.join(raw[j:j+unit][::-1] for j in range(0, size, unit))
                if code in table:
                    raise ValueError('duplicate source TIFF tag')
                table[code] = record(code, dtype, n, raw)
            return table, struct.unpack(endian+'I', entries[-4:])[0]
        pending = [struct.unpack(endian+'I', prefix[4:])[0]]; seen = set(); image = root = None
        while pending:
            offset = pending.pop()
            if not offset: continue
            if offset in seen or len(seen) >= 64:
                raise ValueError('cyclic/large TIFF directory tree')
            seen.add(offset); page, following = directory(offset)
            if root is None: root = page
            if 262 in page and values(page[262]) == (32803,):
                if image is not None:
                    raise ValueError('ambiguous multiple Bayer images')
                image = page
            pending.append(following)
            if 330 in page: pending.extend(int(x) for x in values(page[330]))
        if image is None:
            raise ValueError('no Bayer source image')
        root_omissions = sorted(set(root)-GLOBAL_TAGS-{330, 34665, 34853}) if image is not root else []
        if image is not root:
            image = {**{c: t for c, t in root.items() if c in GLOBAL_TAGS or c in (34665, 34853)}, **image}
        cfa = ''.join('RGB'[x] for x in unpack_record(image[33422]))
        result = dict(schema=SCHEMA, width=int(values(image[256])[0]), height=int(values(image[257])[0]),
                      sample_bits=sample_bits, cfa=cfa,
                      image=[image[c] for c in sorted(image) if c in IMAGE_TAGS], exif=[], gps=[],
                      omitted_image_tags=sorted(set(image)-IMAGE_TAGS-{34665, 34853}))
        digest = hashlib.sha256(); f.seek(0)
        for chunk in iter(lambda: f.read(1024*1024), b''): digest.update(chunk)
        result['source_sha256'] = digest.hexdigest()
        if root_omissions: result['omitted_root_tags'] = root_omissions
        for name, code in (('exif', 34665), ('gps', 34853)):
            if code in image:
                table, following = directory(int(values(image[code])[0]))
                if following or 40965 in table:
                    raise ValueError('unsupported linked/interoperability metadata directory')
                result[name] = [table[c] for c in sorted(table) if c != 37500]
                if 37500 in table: result['omitted_'+name+'_tags'] = [37500]
        validate(result)
        if with_payload:
            if values(image[259]) != (9,) or 324 not in image or 325 not in image:
                raise ValueError('requires one-tile VC-5 GPR')
            offsets, lengths = values(image[324]), values(image[325])
            if len(offsets)!=1 or len(lengths)!=1:
                raise ValueError('requires one-tile VC-5 GPR')
            if values(image[322])!=(result['width'],) or values(image[323])!=(result['height'],):
                raise ValueError('requires one full-image VC-5 tile')
            offset, length = int(offsets[0]), int(lengths[0])
            if (offset<8 or length<36 or offset+length>file_size or
                    length>result['width']*result['height']*3+1024*1024):
                raise ValueError('VC-5 tile outside source/size limit')
            f.seek(offset); payload=f.read(length)
            if len(payload)!=length: raise ValueError('truncated VC-5 tile')
            return result, payload
        return result


def write(path, meta, payload, kind='dng'):
    """Write uncompressed Bayer DNG or one-tile VC-5 GPR; preserve tag bytes."""
    groups = validate(meta)
    if kind not in ('dng', 'gpr') or not payload:
        raise ValueError('requires DNG RAW or GPR VC-5 payload')
    w, h = meta['width'], meta['height']
    if kind == 'dng' and len(payload) != w*h*2:
        raise ValueError('unpacked Bayer payload size mismatch')
    if kind == 'dng' and not samples_in_domain(payload,meta['sample_bits']):
        raise ValueError('Bayer sample exceeds declared bit domain')
    if kind == 'gpr' and (len(payload) < 4 or payload[:4] != b'VC-5'):
        raise ValueError('requires stock VC-5 payload')
    if kind == 'gpr':
        header = b''.join(struct.pack('>HH', tag, value) for tag, value in
                          ((12, 4), (20, w), (21, h), (84, 4), (106, 2), (107, 2), (108, 1), (102, 12)))
        if payload[4:36] != header:
            raise ValueError('requires matching stock four-channel RAW header/domain')
    image = dict(groups[0])
    def add(code, dtype, count, data): image[code] = record(code, dtype, count, data)
    def number(code, value, dtype=4): add(code, dtype, 1, struct.pack('<H' if dtype == 3 else '<I', value))
    number(254, 0); number(256, w); number(257, h); number(258, 16, 3)
    number(259, 9 if kind == 'gpr' else 1, 3); number(262, 32803, 3); number(277, 1, 3)
    number(284, 1, 3)
    offset_tag = 324 if kind == 'gpr' else 273
    number(offset_tag, 0)
    number(325 if kind == 'gpr' else 279, len(payload))
    if kind == 'gpr': number(322, w); number(323, h)
    else: number(278, h)
    add(305, 2, len(b'gpr-stock-media\0'), b'gpr-stock-media\0')
    directories = [image]; pointers = []
    for code, group in ((34665, groups[1]), (34853, groups[2])):
        if group:
            pointers.append((code, len(directories))); number(code, 0); directories.append(group)
    offsets = []; cursor = 8
    for directory in directories:
        offsets.append(cursor); cursor += 2 + len(directory)*12 + 4
    for code, index in pointers: number(code, offsets[index])
    tail = bytearray(); output_dirs = []
    for index, directory in enumerate(directories):
        output = bytearray(struct.pack('<H', len(directory)))
        for code, tag in sorted(directory.items()):
            raw = unpack_record(tag)
            if len(raw) <= 4: field = raw.ljust(4, b'\0')
            else:
                if (cursor + len(tail)) % 2: tail.append(0)
                field = struct.pack('<I', cursor+len(tail)); tail.extend(raw)
            output.extend(struct.pack('<HHI', code, tag['type'], tag['count'])+field)
        output.extend(bytes(4)); output_dirs.append(output)
    if (cursor+len(tail)) % 2: tail.append(0)
    payload_offset = cursor + len(tail)
    if payload_offset + len(payload) >= 2**32:
        raise ValueError('classic TIFF output exceeds 4 GiB')
    index = sorted(image).index(offset_tag)
    struct.pack_into('<I', output_dirs[0], 2+index*12+8, payload_offset)
    with Path(path).open('xb') as f:
        f.write(b'II*\0'+struct.pack('<I', 8))
        for directory in output_dirs: f.write(directory)
        f.write(tail); f.write(payload)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    meta = sub.add_parser('metadata'); meta.add_argument('source', type=Path)
    meta.add_argument('--sample-bits', type=int, choices=(12, 14, 16), required=True)
    meta.add_argument('--output', type=Path, required=True)
    export = sub.add_parser('export'); export.add_argument('payload', type=Path)
    export.add_argument('--metadata', type=Path, required=True)
    export.add_argument('--kind', choices=('dng', 'gpr'), required=True)
    export.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.action == 'metadata':
        result = extract(args.source, args.sample_bits)
        with args.output.open('x') as f: json.dump(result, f, indent=2); f.write('\n')
    else:
        from stock_video import decode_metadata
        metadata = decode_metadata(args.metadata.read_bytes())
        write(args.output, metadata, args.payload.read_bytes(), args.kind)


if __name__ == '__main__': main()
