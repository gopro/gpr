"""Geometry changes for reconstructed Bayer DNGs (standard library only).

GainMap coordinates follow the bundled Adobe SDK: stage-2 active-image bounds,
pixel centers normalized by image dimensions, with separate Bayer-site grids.
Unsupported spatial operations fail rather than carrying stale coordinates.
"""
import copy
import math
import struct
from fractions import Fraction

import raw_media as media


def simulated_half(source):
    """Geometry for same-site 2x2 simulation, not native sensor binning.

    Full-frame GainMaps represent sample centroids. Applying a spatially
    varying gain and averaging do not in general commute.
    """
    media.validate(source)
    if source['width']%4 or source['height']%4:
        raise ValueError('simulation requires dimensions divisible by four')
    def half(tag):
        dtype=tag['type'];fmt={3:'H',4:'I',5:'I',10:'i'}.get(dtype)
        if fmt is None: raise ValueError('unsupported simulation spatial tag type')
        values=struct.unpack('<'+fmt*(tag['count']*(2 if dtype in (5,10) else 1)),media.unpack_record(tag))
        data=[]
        if dtype in (5,10):
            for a,b in zip(values[::2],values[1::2]):
                value=Fraction(a,b)/2;data.extend((value.numerator,value.denominator))
        else:
            if any(value%2 for value in values): raise ValueError('simulation requires even integral spatial calibration')
            data=[value//2 for value in values]
        return media.record(tag['code'],dtype,tag['count'],struct.pack('<'+fmt*len(data),*data))
    result=copy.deepcopy(source);result['width']//=2;result['height']//=2
    omitted=[];image=[]
    for tag in result['image']:
        code=tag['code']
        if code==700:
            omitted.append(dict(code=code,reason='opaque XMP may contain stale simulation geometry'));continue
        if code in (50715,50716): raise ValueError('simulation does not resample black-level delta arrays')
        if code in (50719,50720,50829,50830):tag=half(tag)
        if code==51008 and media.unpack_record(tag)!=bytes(4):
            raise ValueError('simulation requires an empty stage-1 opcode list')
        if code==51009 and media.unpack_record(tag)!=bytes(4):
            active=next((t for t in source['image'] if t['code']==50829),None)
            if active is not None and (active['type'] not in (3,4) or
                    media.values(active)!=(0,0,source['height'],source['width'])):
                raise ValueError('GainMap simulation requires a full-frame active area')
            data=_half_full_frame_gain_maps(media.unpack_record(tag),source['height'],source['width'])
            tag=media.record(code,7,len(data),data)
        if code==51022:opcode_list(media.unpack_record(tag),3,1,source['height'],source['width'])
        image.append(tag)
    result['image']=image
    result['exif']=[half(tag) if tag['code'] in (40962,40963,41486,41487,37390,37391) else tag for tag in result['exif']]
    media.validate(result)
    return result,omitted


def multiply(tag, factor):
    dtype=tag['type']; data=media.unpack_record(tag)
    fmt={3:'H',4:'I',5:'I',9:'i',10:'i'}.get(dtype)
    if fmt is None: raise ValueError(f'unsupported spatial tag type: {tag["code"]}')
    count=tag['count']*(2 if dtype in (5,10) else 1)
    values=list(struct.unpack('<'+fmt*count,data))
    for index in range(0,count,2 if dtype in (5,10) else 1): values[index]*=factor
    try: data=struct.pack('<'+fmt*count,*values)
    except struct.error as error: raise ValueError('scaled metadata exceeds TIFF numeric range') from error
    return media.record(tag['code'],dtype,tag['count'],data)


def gain_maps(body, factor, height, width):
    if len(body)<76: raise ValueError('truncated GainMap')
    top,left,bottom,right,plane,planes,rp,cp=struct.unpack_from('>8I',body)
    rows,cols,dy,dx,oy,ox,map_planes=struct.unpack_from('>2I4dI',body,32)
    if (not 0<=top<bottom<=height or not 0<=left<right<=width or plane!=0 or planes!=1 or
        rp not in (1,2) or cp not in (1,2) or not rows or not cols or map_planes!=1 or
        76+rows*cols*4!=len(body) or not all(math.isfinite(x) for x in (dy,dx,oy,ox)) or dy<=0 or dx<=0):
        raise ValueError('unsupported GainMap geometry/grid')
    values=struct.unpack_from('>'+str(rows*cols)+'f',body,76)
    if any(not math.isfinite(x) or x<=0 for x in values): raise ValueError('invalid GainMap entries')
    # A pitch-one map is split by Bayer site. For pitch two, only its original
    # site is transformed. Interpolation in each deinterleaved plane maps
    # old_i=(new_i+.5)/factor-.5; shift the normalized map origin accordingly.
    outputs=[]
    for sy in (range(2) if rp==1 else (top%2,)):
        for sx in (range(2) if cp==1 else (left%2,)):
            t=top+(sy-top)%2; l=left+(sx-left)%2
            if t>=bottom or l>=right: continue
            b=bottom-(bottom-1-sy)%2; r=right-(right-1-sx)%2
            area=(t//2*2*factor+sy,l//2*2*factor+sx,
                  min(height*factor,(b+1)//2*2*factor),min(width*factor,(r+1)//2*2*factor),0,1,2,2)
            origin_y=oy-(factor-1)*(sy-.5)/(height*factor)
            origin_x=ox-(factor-1)*(sx-.5)/(width*factor)
            outputs.append(struct.pack('>8I',*area)+struct.pack('>2I4dI',rows,cols,dy,dx,origin_y,origin_x,1)+body[76:])
    return outputs


def _half_full_frame_gain_maps(data, height, width):
    """Center-sampled calibration for same-site 2x2 simulation.

    Only four full-frame, pitch-two Bayer-site maps are supported. A low
    sample represents the centroid of its four high samples. This coordinate
    transform does not make spatially varying gains commute with averaging.
    """
    if len(data)<4 or struct.unpack_from('>I',data)[0]!=4:
        raise ValueError('simulation requires four full-frame Bayer GainMaps')
    pos=4;result=[];phases=set()
    for _ in range(4):
        if pos+16>len(data): raise ValueError('truncated simulation opcode header')
        op,version,flags,size=struct.unpack_from('>4I',data,pos);pos+=16
        if op!=9 or flags&~3 or version>0x01040000:
            raise ValueError('simulation supports only full-frame Bayer GainMaps')
        if pos+size>len(data): raise ValueError('truncated simulation GainMap')
        body=data[pos:pos+size];pos+=size
        # Reuse full grid/value validation, without changing source coordinates.
        gain_maps(body,1,height,width)
        top,left,bottom,right,plane,planes,rp,cp=struct.unpack_from('>8I',body)
        if (top not in (0,1) or left not in (0,1) or
                (bottom,right,plane,planes,rp,cp)!=(height,width,0,1,2,2) or
                (top,left) in phases):
            raise ValueError('simulation requires four distinct full-frame Bayer GainMaps')
        phases.add((top,left))
        rows,cols,dy,dx,oy,ox,map_planes=struct.unpack_from('>2I4dI',body,32)
        # Low site (2*j+s) averages high sites (4*j+s, 4*j+s+2).
        # With normalized pixel centers, shift origin by (s-.5)/high_size.
        low=struct.pack('>8I',top,left,height//2,width//2,0,1,2,2)
        low+=struct.pack('>2I4dI',rows,cols,dy,dx,oy+(top-.5)/height,
                         ox+(left-.5)/width,map_planes)+body[76:]
        result.append(struct.pack('>4I',op,version,flags,len(low))+low)
    if pos!=len(data): raise ValueError('simulation opcode list has trailing bytes')
    return struct.pack('>I',4)+b''.join(result)


def opcode_list(data, stage, factor, height, width):
    if len(data)<4: raise ValueError('truncated opcode list')
    count,=struct.unpack_from('>I',data); pos=4; result=[]
    if count>256: raise ValueError('too many source opcodes')
    for _ in range(count):
        if pos+16>len(data): raise ValueError('truncated opcode header')
        op,version,flags,size=struct.unpack_from('>4I',data,pos); pos+=16
        if pos+size>len(data) or flags&~3 or version>0x01040000: raise ValueError('unsupported opcode header')
        body=data[pos:pos+size]; pos+=size
        if stage==2 and op==9:
            bodies=gain_maps(body,factor,height,width)
        elif stage==3 and op in (1,2,3):
            # WarpRectilinear/Fisheye/Vignette use normalized lens parameters,
            # not stored pixel coordinates; their bytes survive uniform scale.
            if op==3:
                expected=56; offset=0
            else:
                if len(body)<4: raise ValueError('truncated normalized lens opcode')
                planes,=struct.unpack_from('>I',body)
                if not 1<=planes<=3: raise ValueError('invalid lens opcode plane count')
                expected=4+planes*(48 if op==1 else 32)+16; offset=4
            if len(body)!=expected: raise ValueError('invalid normalized lens opcode length')
            numbers=struct.unpack_from('>'+str((len(body)-offset)//8)+'d',body,offset)
            if any(not math.isfinite(x) for x in numbers) or any(not 0<=x<=1 for x in numbers[-2:]):
                raise ValueError('invalid normalized lens opcode parameters')
            bodies=[body]
        else:
            raise ValueError(f'2x metadata does not support opcode {op} in list {stage}')
        result.extend(struct.pack('>4I',op,version,flags,len(value))+value for value in bodies)
    if pos!=len(data): raise ValueError('opcode list has trailing bytes')
    return struct.pack('>I',len(result))+b''.join(result)


def scaled(metadata, factor):
    media.validate(metadata)
    if type(factor) is not int or factor not in (1,2): raise ValueError('unsupported reconstruction scale')
    result=copy.deepcopy(metadata)
    accounting=dict(scale=factor,changed_image_tags=[],changed_exif_tags=[],omitted=[])
    if factor==1: return result,accounting
    w,h=metadata['width'],metadata['height']
    result['width']=w*factor; result['height']=h*factor
    if result['width']>32768 or result['height']>32768: raise ValueError('reconstructed dimensions exceed DNG frontend limits')
    tags={t['code']:t for t in result['image']}
    if 50829 in tags and (tags[50829]['type'] not in (3,4) or tags[50829]['count']!=4):
        raise ValueError('2x reconstruction requires integral ActiveArea coordinates')
    top,left,bottom,right=map(int,media.values(tags[50829])) if 50829 in tags else (0,0,h,w)
    for code in (50715,50716):
        if code in tags: raise ValueError('2x black-level row/column delta resampling is not implemented')
    image=[]
    for tag in result['image']:
        code=tag['code']
        if code==700:
            accounting['omitted'].append(dict(group='image',code=code,reason='opaque XMP may contain stale pixel/crop geometry after reconstruction'))
            continue
        if code in (50719,50720,50829,50830):
            if code==50830:
                values=media.values(tag)
                if len(values)%4 or any(not (0<=t<b<=h and 0<=l<r<=w) for t,l,b,r in zip(values[::4],values[1::4],values[2::4],values[3::4])):
                    raise ValueError('invalid masked areas')
            tag=multiply(tag,factor); accounting['changed_image_tags'].append(code)
        elif code in (51008,51009,51022):
            data=media.unpack_record(tag)
            if code==51009 and len(data)>=4 and struct.unpack_from('>I',data)[0] and (top%2 or left%2):
                raise ValueError('2x GainMaps require an even active-area origin')
            data=opcode_list(data,{51008:1,51009:2,51022:3}[code],factor,bottom-top,right-left)
            tag=media.record(code,7,len(data),data)
            if data!=media.unpack_record(tags[code]): accounting['changed_image_tags'].append(code)
        image.append(tag)
    result['image']=image
    exif=[]
    for tag in result['exif']:
        if tag['code'] in (40962,40963,41486,41487,37390,37391):
            tag=multiply(tag,factor); accounting['changed_exif_tags'].append(tag['code'])
        exif.append(tag)
    result['exif']=exif
    media.validate(result)
    return result,accounting
