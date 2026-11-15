/* SPDX-License-Identifier: Apache-2.0 OR MIT
 * Stock GVID record interchange, retaining the FFmpeg container approach from
 * PR #95 tools/gvid_mov. GVS2/GVS3 are private framed-stock profiles, distinct
 * from fused GPRr/GPR1 and from standard CFHD. No codec is invoked here.
 */
#include "gpr_gvid_reader.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/timecode.h>
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
void require(bool good,const char *message) { if(!good) throw std::runtime_error(message); }
void checked(int code,const char *message) {
    if(code<0) { char text[AV_ERROR_MAX_STRING_SIZE];av_strerror(code,text,sizeof(text));
        throw std::runtime_error(std::string(message)+": "+text); }
}
typedef std::unique_ptr<FILE,decltype(&fclose)> File;
File input(const char *path) {
    int fd=open(path,O_RDONLY|O_NONBLOCK);require(fd>=0,"cannot open input");struct stat st;
    if(fstat(fd,&st)!=0||!S_ISREG(st.st_mode)){close(fd);throw std::runtime_error("input must be a regular local file");}
    FILE *f=fdopen(fd,"rb");if(!f){close(fd);throw std::runtime_error("input stream creation failed");}
    return File(f,fclose);
}
struct Sha {
    AVSHA *p=av_sha_alloc();
    Sha() { require(p!=nullptr,"SHA allocation failed");checked(av_sha_init(p,256),"SHA init"); }
    ~Sha(){av_free(p);}
    void add(const void *data,size_t size){if(size)av_sha_update(p,static_cast<const uint8_t *>(data),size);}
    std::string finish(){uint8_t digest[32];av_sha_final(p,digest);std::string result;
        const char *hex="0123456789abcdef";for(uint8_t x:digest){result+=hex[x>>4];result+=hex[x&15];}return result;}
};
std::string hex(const std::vector<uint8_t>& data) {
    const char *digits="0123456789abcdef";std::string result;result.reserve(data.size()*2);
    for(uint8_t x:data){result+=digits[x>>4];result+=digits[x&15];}return result;
}
std::vector<uint8_t> unhex(const char *text,size_t maximum) {
    size_t size=strlen(text);require(size%2==0 && size<=maximum*2,"invalid hex metadata size");
    std::vector<uint8_t> result(size/2);
    auto digit=[](char x)->int {if(x>='0'&&x<='9')return x-'0';if(x>='a'&&x<='f')return x-'a'+10;return -1;};
    for(size_t i=0;i<result.size();++i){int a=digit(text[i*2]),b=digit(text[i*2+1]);require(a>=0&&b>=0,"invalid hex metadata");result[i]=uint8_t(a*16+b);}
    return result;
}
uint64_t le64(const uint8_t *p){uint64_t value=0;for(unsigned i=0;i<8;++i)value|=uint64_t(p[i])<<(i*8);return value;}
uint64_t position(FILE *f){off_t n=ftello(f);require(n>=0,"input position failed");return uint64_t(n);}
void read_at(FILE *f,uint64_t offset,void *data,size_t size) {
    require(offset<=uint64_t(INT64_MAX)&&fseeko(f,off_t(offset),SEEK_SET)==0,"input seek failed");
    require(fread(data,1,size,f)==size,"input changed or read failed");
}
std::vector<uint8_t> read_at(FILE *f,uint64_t offset,size_t size) {
    std::vector<uint8_t> data(size);if(size)read_at(f,offset,data.data(),size);return data;
}
struct Reader {
    gpr_gvid_reader *p=nullptr;
    ~Reader(){gpr_gvid_reader_close(p);}
};
struct ReadFile { FILE *file;Sha *sha; };
ptrdiff_t read_file(void *opaque,uint8_t *data,size_t size) {
    ReadFile *r=static_cast<ReadFile *>(opaque);size_t n=fread(data,1,size,r->file);
    if(n)r->sha->add(data,n);return n?ptrdiff_t(n):(ferror(r->file)?-1:0);
}
struct Memory {const uint8_t *p;size_t size,offset;};
ptrdiff_t read_memory(void *opaque,uint8_t *data,size_t size) {
    Memory *m=static_cast<Memory *>(opaque);size=std::min(size,m->size-m->offset);
    if(size)memcpy(data,m->p+m->offset,size);m->offset+=size;return ptrdiff_t(size);
}
struct Sample {uint64_t offset,size,timestamp;};
struct Video {
    gpr_gvid_config config={};uint32_t version=0;
    std::vector<uint8_t> prefix,ending;std::vector<Sample> samples;std::string sha;
};
Video inspect(const char *path) {
    File file=input(path);Sha sha;ReadFile state={file.get(),&sha};Reader reader;
    require(gpr_gvid_reader_create(read_file,&state,0,&reader.p)==0,"invalid GVID header");
    gpr_gvid_stream_info info;require(gpr_gvid_reader_info(reader.p,&info)==0,"invalid GVID information");
    Video video;video.config=info.video;video.version=info.version;uint64_t prefix_size=position(file.get()),end_offset=0;
    for(;;){uint64_t start=position(file.get());gpr_gvid_frame_view frame;
        int status=gpr_gvid_reader_next(reader.p,&frame);
        if(status==GPR_GVID_DONE){end_offset=start;break;}
        require(status==GPR_GVID_FRAME,"invalid, incomplete or corrupt GVID recording");
        require(video.samples.size()<1000000,"MOV frame index exceeds one-million-frame operational limit");
        video.samples.push_back({start,position(file.get())-start,frame.timestamp_ns});
    }
    require(!video.samples.empty(),"MOV requires at least one frame");
    require(video.version!=3||gpr_gvid_reader_end_verified(reader.p)==1,"GVID end marker was not verified");
    uint64_t total=position(file.get());video.sha=sha.finish();
    require(prefix_size<=64+65536 && total-end_offset==(video.version==3?48u:0u),"invalid GVID framing extent");
    video.prefix=read_at(file.get(),0,size_t(prefix_size));video.ending=read_at(file.get(),end_offset,size_t(total-end_offset));
    return video;
}
struct Output {
    FILE *file=nullptr;std::string path;bool success=false;
    explicit Output(const char *name):path(name) {
        int fd=open(name,O_RDWR|O_CREAT|O_EXCL,0666);require(fd>=0,"output exists or cannot be created");
        file=fdopen(fd,"wb+");if(!file){close(fd);unlink(name);throw std::runtime_error("output stream creation failed");}
    }
    ~Output(){if(file)fclose(file);if(!success)unlink(path.c_str());}
    void write(const void *p,size_t n){if(n)require(fwrite(p,1,n,file)==n,"output write failed");}
    void finish(){require(fflush(file)==0,"output flush failed");FILE *f=file;file=nullptr;
        require(fclose(f)==0,"output close failed");success=true;}
};
#if LIBAVFORMAT_VERSION_MAJOR >= 61
int write_packet(void *opaque,const uint8_t *data,int size) {
#else
int write_packet(void *opaque,uint8_t *data,int size) {
#endif
    Output *out=static_cast<Output *>(opaque);return fwrite(data,1,size,out->file)==size_t(size)?size:AVERROR(EIO);
}
int64_t seek_output(void *opaque,int64_t offset,int whence) {
    Output *out=static_cast<Output *>(opaque);
    if(whence&AVSEEK_SIZE){struct stat st;return fflush(out->file)==0&&fstat(fileno(out->file),&st)==0?st.st_size:AVERROR(EIO);}
    if(fseeko(out->file,off_t(offset),whence&~AVSEEK_FORCE))return AVERROR(EIO);
    off_t n=ftello(out->file);return n<0?AVERROR(EIO):n;
}
struct IO {
    AVIOContext *p=nullptr;
    explicit IO(Output& output){uint8_t *buffer=static_cast<uint8_t *>(av_malloc(65536));require(buffer!=nullptr,"I/O allocation failed");
        p=avio_alloc_context(buffer,65536,1,&output,nullptr,write_packet,seek_output);
        if(!p){av_free(buffer);throw std::runtime_error("AVIO creation failed");}}
    ~IO(){if(p){av_freep(&p->buffer);avio_context_free(&p);}}
};
struct Mux {AVFormatContext *p=nullptr;~Mux(){avformat_free_context(p);}};
struct Demux {AVFormatContext *p=nullptr;~Demux(){if(p)avformat_close_input(&p);}};
struct Packet {AVPacket *p=av_packet_alloc();Packet(){require(p!=nullptr,"packet allocation failed");}~Packet(){av_packet_free(&p);}};
struct Dict {AVDictionary *p=nullptr;~Dict(){av_dict_free(&p);}void set(const char *key,const std::string& value){checked(av_dict_set(&p,key,value.c_str(),0),"set option");}};
uint64_t ticks(uint64_t ns,uint64_t unit){return ns/unit+(ns%unit>=(unit+1)/2);}
uint64_t nominal_ns(const gpr_gvid_config& c){return (uint64_t(1000000000)*c.fps_den+c.fps_num/2)/c.fps_num;}
uint64_t choose_unit(const Video& v) {
    uint64_t gap=nominal_ns(v.config),span=v.samples.back().timestamp-v.samples.front().timestamp,unit=1;
    for(size_t i=1;i<v.samples.size();++i)gap=std::max(gap,v.samples[i].timestamp-v.samples[i-1].timestamp);
    while(gap/unit+2>UINT32_MAX||span/unit>uint64_t(INT64_MAX)-UINT32_MAX){require(unit<1000000000,"timeline exceeds MOV operational range");unit*=10;}
    uint64_t previous=0;
    for(size_t i=1;i<v.samples.size();++i){uint64_t value=ticks(v.samples[i].timestamp-v.samples.front().timestamp,unit);
        require(value>previous,"MOV clock cannot represent both large gaps and fine frame spacing");previous=value;}
    return unit;
}
uint32_t tag(unsigned version){return version==3?MKTAG('G','V','S','3'):MKTAG('G','V','S','2');}
void report(const Video& v,uint64_t unit,unsigned audio=0,unsigned timecode=0,const char *label=nullptr) {
    // The label has been canonicalized by libavutil's timecode formatter.
    std::string text=label ? "\""+std::string(label)+"\"" : "null";
    printf("{\"profile\":\"GVS%u\",\"source_version\":%u,\"frames\":%zu,\"width\":%u,\"height\":%u,\"unit_ns\":%llu,\"libavformat_version\":%u,\"gvid_sha256\":\"%s\",\"audio_tracks\":%u,\"timecode_tracks\":%u,\"timecode\":%s}\n",
           v.version,v.version,v.samples.size(),v.config.width,v.config.height,(unsigned long long)unit,avformat_version(),v.sha.c_str(),audio,timecode,text.c_str());
}
void mux(const char *source,const char *destination) {
    Video v=inspect(source);uint64_t unit=choose_unit(v);File file=input(source);Output output(destination);IO io(output);Mux format;
    checked(avformat_alloc_output_context2(&format.p,nullptr,"mov",nullptr),"create MOV context");
    require(format.p!=nullptr,"MOV context absent");format.p->pb=io.p;format.p->flags|=AVFMT_FLAG_CUSTOM_IO;
    AVStream *stream=avformat_new_stream(format.p,nullptr);require(stream!=nullptr,"stream creation failed");
    stream->codecpar->codec_type=AVMEDIA_TYPE_VIDEO;stream->codecpar->codec_id=AV_CODEC_ID_NONE;
    stream->codecpar->codec_tag=tag(v.version);stream->codecpar->width=int(v.config.width);stream->codecpar->height=int(v.config.height);
    stream->codecpar->format=AV_PIX_FMT_NONE;stream->time_base={1,int(1000000000/unit)};
    // Standard glbl sample-description atom makes the original checked clip
    // configuration available to optional decoders without parsing MOV metadata.
    stream->codecpar->extradata=static_cast<uint8_t *>(av_mallocz(v.prefix.size()+AV_INPUT_BUFFER_PADDING_SIZE));
    require(stream->codecpar->extradata!=nullptr,"allocate stock codec configuration");
    std::memcpy(stream->codecpar->extradata,v.prefix.data(),v.prefix.size());
    stream->codecpar->extradata_size=int(v.prefix.size());
    stream->avg_frame_rate={int(v.config.fps_num),int(v.config.fps_den)};
    auto metadata=[&](const char *key,const std::string& value){checked(av_dict_set(&format.p->metadata,key,value.c_str(),0),"set MOV metadata");};
    metadata("gpr.stock.profile","framed_gvid.v1");metadata("gpr.stock.prefix_hex",hex(v.prefix));
    metadata("gpr.stock.end_hex",hex(v.ending));metadata("gpr.stock.sha256",v.sha);metadata("gpr.stock.unit_ns",std::to_string(unit));
    Dict options;options.set("movflags","use_metadata_tags");options.set("video_track_timescale",std::to_string(1000000000/unit));
    options.set("use_editlist","0");options.set("write_tmcd","0");checked(avformat_write_header(format.p,&options.p),"write MOV header");
    require(stream->time_base.num==1&&stream->time_base.den==int(1000000000/unit),"MOV muxer changed the declared time base");
    Sha copied;copied.add(v.prefix.data(),v.prefix.size());Packet packet;
    for(size_t i=0;i<v.samples.size();++i){const Sample& s=v.samples[i];require(s.size<=INT_MAX,"MOV packet size exceeds limit");
        av_packet_unref(packet.p);checked(av_new_packet(packet.p,int(s.size)),"allocate MOV packet");
        read_at(file.get(),s.offset,packet.p->data,size_t(s.size));copied.add(packet.p->data,size_t(s.size));
        uint64_t pts=ticks(s.timestamp-v.samples[0].timestamp,unit);
        uint64_t duration=i+1<v.samples.size()?ticks(v.samples[i+1].timestamp-v.samples[0].timestamp,unit)-pts:std::max(uint64_t(1),ticks(nominal_ns(v.config),unit));
        require(duration>0&&duration<=UINT32_MAX,"MOV sample duration exceeds range");
        packet.p->stream_index=stream->index;packet.p->pts=packet.p->dts=int64_t(pts);packet.p->duration=int64_t(duration);packet.p->flags=AV_PKT_FLAG_KEY;
        checked(av_write_frame(format.p,packet.p),"write MOV sample");
    }
    copied.add(v.ending.data(),v.ending.size());require(copied.finish()==v.sha,"GVID input changed during wrapping");
    checked(av_write_trailer(format.p),"write MOV trailer");avio_flush(io.p);checked(io.p->error,"flush MOV I/O");output.finish();report(v,unit);
}
const char *metadata(AVFormatContext *f,const char *key) {
    AVDictionaryEntry *entry=av_dict_get(f->metadata,key,nullptr,0);require(entry&&entry->value,"missing stock MOV metadata");return entry->value;
}
uint64_t number(const char *text) {
    char *end=nullptr;errno=0;
    uint64_t value=strtoull(text,&end,10);
    require(!errno&&end!=text&&!*end&&std::to_string(value)==text,"invalid auxiliary numeric metadata");return value;
}
uint64_t number_metadata(AVFormatContext *f,const char *key) {return number(metadata(f,key));}
struct Auxiliary {
    AVStream *audio=nullptr,*timecode=nullptr;uint64_t samples=0;unsigned timecode_packets=0;
    uint64_t timecode_duration=0;unsigned bytes_per_sample=0;int timecode_start=0;std::string label;Sha sha;
    Auxiliary(AVFormatContext *format,AVStream *video,const Video& v) {
        require(format->nb_streams>=1&&format->nb_streams<=3,"unsupported stock MOV track count");
        for(unsigned i=0;i<format->nb_streams;++i){AVStream *s=format->streams[i];if(s==video)continue;
            if(s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO&&!audio){
                audio=s;const auto codec=s->codecpar->codec_id;const char *name=nullptr;
                if(codec==AV_CODEC_ID_PCM_S16LE){bytes_per_sample=2;name="pcm_s16le";}
                else if(codec==AV_CODEC_ID_PCM_S24LE){bytes_per_sample=3;name="pcm_s24le";}
                else if(codec==AV_CODEC_ID_PCM_F32LE){bytes_per_sample=4;name="pcm_f32le";}
                require(name!=nullptr&&s->codecpar->sample_rate>=8000&&s->codecpar->sample_rate<=192000&&
                        s->codecpar->ch_layout.nb_channels>=1&&s->codecpar->ch_layout.nb_channels<=2,"unsupported PCM audio track");
                require(!strcmp(metadata(format,"gpr.media.audio_codec"),name)&&
                        number_metadata(format,"gpr.media.audio_rate")==unsigned(s->codecpar->sample_rate)&&
                        number_metadata(format,"gpr.media.audio_channels")==unsigned(s->codecpar->ch_layout.nb_channels),"audio metadata disagrees with track");
                require(s->time_base.num==1&&s->time_base.den==s->codecpar->sample_rate,"unsupported audio time base");
            }else if(s->codecpar->codec_type==AVMEDIA_TYPE_DATA&&s->codecpar->codec_tag==MKTAG('t','m','c','d')&&!timecode){
                timecode=s;AVTimecode tc={};const char *text=metadata(format,"gpr.media.timecode");
                checked(av_timecode_init_from_string(&tc,{int(v.config.fps_num),int(v.config.fps_den)},text,nullptr),"invalid timecode");
                char canonical[AV_TIMECODE_STR_SIZE];av_timecode_make_string(&tc,canonical,0);
                require(!strcmp(text,canonical),"noncanonical or invalid drop-frame timecode");timecode_start=tc.start;
                label=canonical;int num,den;av_reduce(&num,&den,v.config.fps_num,v.config.fps_den,INT_MAX);
                require(std::string(metadata(format,"gpr.media.timecode_rate"))==std::to_string(num)+"/"+std::to_string(den),"timecode rate differs from stock profile");
                const auto *tagged=av_dict_get(s->metadata,"timecode",nullptr,0);
                require(tagged&&!strcmp(tagged->value,text),"timecode sample description disagrees with metadata");
                require(s->time_base.num==1&&s->time_base.den>0&&
                        number_metadata(format,"gpr.media.timecode_scale")==unsigned(s->time_base.den),"timecode clock differs from declaration");
            }else throw std::runtime_error("unexpected or duplicate stock MOV track");
        }
        require(bool(audio)==bool(av_dict_get(format->metadata,"gpr.media.audio_codec",nullptr,0))&&
                bool(timecode)==bool(av_dict_get(format->metadata,"gpr.media.timecode",nullptr,0)),"declared auxiliary tracks are missing");
        if(audio||timecode)require(!strcmp(metadata(format,"gpr.media.aux_profile"),"pcm_tmcd.v1"),"unsupported auxiliary track profile");
    }
    void packet(const AVPacket *p) {
        require(p->size>0&&p->size<=4*1024*1024&&!(p->flags&AV_PKT_FLAG_CORRUPT),"invalid auxiliary packet");
        if(audio&&p->stream_index==audio->index){
            unsigned block=bytes_per_sample*audio->codecpar->ch_layout.nb_channels;
            uint64_t count=unsigned(p->size)/block;
            require(unsigned(p->size)%block==0&&samples<=INT64_MAX-count&&p->pts==int64_t(samples)&&
                    p->dts==p->pts&&p->duration==int64_t(count),"audio samples or timestamps are discontinuous");
            sha.add(p->data,size_t(p->size));samples+=count;
        }else{
            require(timecode&&p->stream_index==timecode->index&&!timecode_packets++&&p->size==4&&
                    AV_RB32(p->data)==unsigned(timecode_start)&&p->pts==0&&p->dts==0&&p->duration>0,"invalid timecode sample");
            timecode_duration=uint64_t(p->duration);
        }
    }
    void finish(AVFormatContext *format,uint64_t end,int video_scale) {
        if(audio)require(samples&&samples==number_metadata(format,"gpr.media.audio_samples")&&
                         sha.finish()==metadata(format,"gpr.media.audio_sha256"),"PCM audio sample count or digest differs");
        if(timecode)require(end<=INT64_MAX&&timecode_packets==1&&timecode_duration==uint64_t(av_rescale_rnd(
            int64_t(end),timecode->time_base.den,video_scale,AV_ROUND_NEAR_INF)),"timecode duration differs from video");
    }
};
int deny_external_io(AVFormatContext *,AVIOContext **,const char *,int,AVDictionary **){return AVERROR(EPERM);}
void movie_extents(FILE *file) {
    require(fseeko(file,0,SEEK_END)==0,"MOV size query failed");uint64_t size=position(file),offset=0;size_t count=0;
    bool moov=false,mdat=false;
    // libavformat can salvage a truncated final metadata atom. Strict
    // interchange must distinguish that from a completely stored MOV.
    while(offset<size){require(++count<=1000000&&size-offset>=8,"truncated MOV atom header");uint8_t header[16];
        read_at(file,offset,header,8);uint64_t length=AV_RB32(header);size_t bytes=8;
        if(length==1){require(size-offset>=16,"truncated extended MOV atom");read_at(file,offset+8,header+8,8);length=AV_RB64(header+8);bytes=16;}
        else if(length==0){require(!memcmp(header+4,"mdat",4),"only final mdat may extend to EOF");length=size-offset;}
        require(length>=bytes&&length<=size-offset,"MOV atom extends outside complete file");
        moov=moov||!memcmp(header+4,"moov",4);mdat=mdat||!memcmp(header+4,"mdat",4);offset+=length;
    }
    require(moov&&mdat,"MOV is missing movie or media data atoms");
}
// Generate only a timecode carrier, using one copied video packet to let the
// MOV muxer create the standard tmcd description. Its clock is independent of
// the nanosecond source video so a whole-clip sample fits the muxer's limits.
void timecode_template(const char *source,const char *destination,unsigned num,unsigned den,uint64_t end_ns,const char *label) {
    require(num&&num<=240000&&den&&den<=10000&&end_ns&&end_ns<=INT64_MAX,"invalid timecode clock parameters");
    int rate_num,rate_den;av_reduce(&rate_num,&rate_den,num,den,INT_MAX);
    AVRational rate={rate_num,rate_den};AVTimecode tc={};
    checked(av_timecode_init_from_string(&tc,rate,label,nullptr),"invalid timecode label");
    char canonical[AV_TIMECODE_STR_SIZE];av_timecode_make_string(&tc,canonical,0);
    require(!strcmp(canonical,label),"noncanonical timecode label");
    uint64_t scale=0;int64_t duration=0;
    for(unsigned multiplier=1000;multiplier;multiplier/=10){
        scale=uint64_t(rate_num)*multiplier;
        duration=av_rescale_rnd(int64_t(end_ns),int64_t(scale),1000000000,AV_ROUND_NEAR_INF);
        if(scale<=INT_MAX&&duration>0&&duration<=INT_MAX)break;
    }
    require(scale&&scale<=INT_MAX&&duration>0&&duration<=INT_MAX,"timecode timeline exceeds operational range");
    File regular=input(source);movie_extents(regular.get());regular.reset();Demux input_format;
    input_format.p=avformat_alloc_context();require(input_format.p!=nullptr,"timecode input allocation failed");
    input_format.p->io_open=deny_external_io;
    char resolved[PATH_MAX];require(realpath(source,resolved)!=nullptr,"cannot resolve timecode source");
    checked(avio_open(&input_format.p->pb,resolved,AVIO_FLAG_READ),"open timecode source");
    AVIOContext *primary=input_format.p->pb;input_format.p->flags|=AVFMT_FLAG_CUSTOM_IO;
    struct Close{AVFormatContext **format;AVIOContext *p;~Close(){if(*format)avformat_close_input(format);avio_closep(&p);}} close{&input_format.p,primary};
    Dict read_options;read_options.set("enable_drefs","0");read_options.set("use_absolute_path","0");
    checked(avformat_open_input(&input_format.p,nullptr,av_find_input_format("mov"),&read_options.p),"read timecode source");
    AVStream *original=nullptr;
    for(unsigned i=0;i<input_format.p->nb_streams;++i)if(input_format.p->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){require(!original,"multiple video tracks");original=input_format.p->streams[i];}
    require(original!=nullptr,"timecode source lacks video");
    Output output(destination);IO io(output);Mux format;
    checked(avformat_alloc_output_context2(&format.p,nullptr,"mov",nullptr),"create timecode carrier");
    require(format.p!=nullptr,"timecode carrier context absent");format.p->pb=io.p;format.p->flags|=AVFMT_FLAG_CUSTOM_IO;
    AVStream *stream=avformat_new_stream(format.p,nullptr);require(stream!=nullptr,"timecode carrier stream failed");
    checked(avcodec_parameters_copy(stream->codecpar,original->codecpar),"copy timecode carrier description");
    stream->avg_frame_rate=rate;stream->time_base={1,int(scale)};
    checked(av_dict_set(&stream->metadata,"timecode",label,0),"set timecode label");
    Dict options;options.set("video_track_timescale",std::to_string(scale));options.set("movie_timescale","1000");
    options.set("write_tmcd","1");options.set("use_editlist","0");checked(avformat_write_header(format.p,&options.p),"write timecode carrier header");
    Packet packet;
    do{av_packet_unref(packet.p);checked(av_read_frame(input_format.p,packet.p),"read first timecode carrier packet");}while(packet.p->stream_index!=original->index);
    packet.p->stream_index=stream->index;packet.p->pts=packet.p->dts=0;packet.p->duration=duration;
    checked(av_write_frame(format.p,packet.p),"write timecode carrier packet");
    checked(av_write_trailer(format.p),"write timecode carrier trailer");avio_flush(io.p);checked(io.p->error,"flush timecode carrier");output.finish();
    printf("{\"schema\":\"gpr.timecode_template.v1\",\"timescale\":%llu,\"duration\":%lld}\n",(unsigned long long)scale,(long long)duration);
}
void demux(const char *source,const char *destination) {
    File regular=input(source);movie_extents(regular.get());regular.reset();Demux format;format.p=avformat_alloc_context();require(format.p!=nullptr,"demux allocation failed");
    format.p->io_open=deny_external_io;
    // Open the primary input ourselves; nested/external data references cannot
    // cause the demuxer to open another file or network resource.
    char resolved[PATH_MAX];require(realpath(source,resolved)!=nullptr,"cannot resolve MOV input");
    checked(avio_open(&format.p->pb,resolved,AVIO_FLAG_READ),"open MOV input");
    AVIOContext *primary=format.p->pb;format.p->flags|=AVFMT_FLAG_CUSTOM_IO;
    struct ClosePrimary{AVFormatContext **format;AVIOContext *p;
        ~ClosePrimary(){if(*format)avformat_close_input(format);avio_closep(&p);}} close_primary{&format.p,primary};
    Dict options;options.set("enable_drefs","0");options.set("use_absolute_path","0");
    checked(avformat_open_input(&format.p,nullptr,av_find_input_format("mov"),&options.p),"read MOV header");
    AVStream *stream=nullptr;
    for(unsigned i=0;i<format.p->nb_streams;++i)if(format.p->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){
        require(!stream,"stock MOV requires exactly one video track");stream=format.p->streams[i];}
    require(stream!=nullptr,"stock MOV video track absent");
    require(!strcmp(metadata(format.p,"gpr.stock.profile"),"framed_gvid.v1"),"unsupported MOV profile");
    Video v;v.prefix=unhex(metadata(format.p,"gpr.stock.prefix_hex"),64+65536);v.ending=unhex(metadata(format.p,"gpr.stock.end_hex"),48);
    v.sha=metadata(format.p,"gpr.stock.sha256");require(v.sha.size()==64&&unhex(v.sha.c_str(),32).size()==32,"invalid source digest");
    const char *unit_text=metadata(format.p,"gpr.stock.unit_ns");char *end=nullptr;errno=0;
    uint64_t unit=strtoull(unit_text,&end,10);require(!errno&&end!=unit_text&&!*end,"invalid MOV time unit");
    uint64_t power=1;while(power<unit&&power<1000000000)power*=10;require(unit==power&&unit<=1000000000,"unsupported MOV time unit");
    Memory memory={v.prefix.data(),v.prefix.size(),0};Reader header;
    require(gpr_gvid_reader_create(read_memory,&memory,0,&header.p)==0&&memory.offset==v.prefix.size(),"invalid embedded GVID prefix");
    gpr_gvid_stream_info info;require(gpr_gvid_reader_info(header.p,&info)==0,"invalid embedded GVID information");v.config=info.video;v.version=info.version;
    require(v.ending.size()==(v.version==3?48u:0u),"invalid embedded GVID ending");
    require(stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO&&stream->codecpar->codec_tag==tag(v.version)&&
            stream->codecpar->width==int(v.config.width)&&stream->codecpar->height==int(v.config.height),"MOV sample description disagrees with stock profile");
    require(stream->time_base.num==1&&stream->time_base.den==int(1000000000/unit),"MOV timeline disagrees with declared time unit");
    Auxiliary auxiliary(format.p,stream,v);
    Output output(destination);Sha sha;output.write(v.prefix.data(),v.prefix.size());sha.add(v.prefix.data(),v.prefix.size());Packet packet;
    uint64_t first=0,previous=0;int64_t previous_duration=0;size_t count=0;
    for(;;){int status=av_read_frame(format.p,packet.p);if(status==AVERROR_EOF)break;checked(status,"read MOV packet");
        if(packet.p->stream_index!=stream->index){auxiliary.packet(packet.p);av_packet_unref(packet.p);continue;}
        size_t header_bytes=v.version==3?48:32;
        require(packet.p->size>=int(header_bytes)&&
                uint64_t(packet.p->size)<=uint64_t(v.config.width)*v.config.height*8+65536+48&&
                !(packet.p->flags&AV_PKT_FLAG_CORRUPT)&&(packet.p->flags&AV_PKT_FLAG_KEY),"invalid stock MOV packet extent/flags");
        require(!memcmp(packet.p->data,v.version==3?"FRM3":"FRM2",4),"MOV sample is not a stock GVID record");
        uint64_t ns=le64(packet.p->data+16);if(!count)first=ns;require(ns>=first,"invalid packet timestamp");
        uint64_t pts=ticks(ns-first,unit);require(pts<=INT64_MAX&&packet.p->pts==int64_t(pts)&&packet.p->dts==int64_t(pts),"MOV packet timestamps disagree with original record");
        require(packet.p->duration>0&&packet.p->duration<=UINT32_MAX,"invalid MOV sample duration");
        if(count)require(pts>previous&&uint64_t(previous_duration)==pts-previous,"MOV sample durations disagree with record timing");
        previous=pts;previous_duration=packet.p->duration;require(++count<=1000000,"MOV frame count exceeds operational limit");
        output.write(packet.p->data,size_t(packet.p->size));sha.add(packet.p->data,size_t(packet.p->size));av_packet_unref(packet.p);
    }
    require(count>0&&uint64_t(previous_duration)==std::max(uint64_t(1),ticks(nominal_ns(v.config),unit)),"MOV final sample duration disagrees with profile");
    auxiliary.finish(format.p,previous+uint64_t(previous_duration),stream->time_base.den);
    output.write(v.ending.data(),v.ending.size());sha.add(v.ending.data(),v.ending.size());require(sha.finish()==v.sha,"recovered GVID digest mismatch");
    require(fflush(output.file)==0,"flush recovered GVID failed");Video verified=inspect(destination);
    require(verified.samples.size()==count&&verified.sha==v.sha,"recovered recording failed final verification");output.finish();report(verified,unit,auxiliary.audio?1:0,auxiliary.timecode?1:0,auxiliary.timecode?auxiliary.label.c_str():nullptr);
}
}
int main(int argc,char **argv) {
    av_log_set_level(AV_LOG_ERROR);
    av_max_alloc(size_t(256)*1024*1024);
    if(argc==2&&!strcmp(argv[1],"--version")){printf("libavformat %u; %s\n",avformat_version(),avformat_license());return 0;}
    try{
        if(argc==8&&!strcmp(argv[1],"timecode")){
            const auto num=number(argv[4]),den=number(argv[5]);require(num<=240000&&den<=10000,"timecode rate too large");
            timecode_template(argv[2],argv[3],unsigned(num),unsigned(den),number(argv[6]),argv[7]);return 0;
        }
        require(argc==4,"usage: gpr_mov mux|demux input output; or timecode input output fps_num fps_den duration_ns label");
        if(!strcmp(argv[1],"mux"))mux(argv[2],argv[3]);else if(!strcmp(argv[1],"demux"))demux(argv[2],argv[3]);
        else throw std::runtime_error("unknown MOV action");return 0;
    }catch(const std::exception& e){fprintf(stderr,"gpr_mov: %s\n",e.what());return 1;}
}
