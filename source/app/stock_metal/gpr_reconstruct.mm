// Standalone RGGB14 reconstruction. Native inference and host math; no Torch.
// Repository Apache-2.0 / MIT license. Bicubic uses a=-0.75, align_corners=false.
#import <Foundation/Foundation.h>
#import <CoreML/CoreML.h>
#import <CommonCrypto/CommonDigest.h>
#include "StockMetalResidual.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#define GPR_NATIVE_NEON 1
#else
#define GPR_NATIVE_NEON 0
#endif

static void require(bool ok,const char *message) { if(!ok) throw std::runtime_error(message); }
static NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
static double seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static NSString *finishHash(CC_SHA256_CTX *ctx) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(bytes,ctx);
    NSMutableString *result=[NSMutableString string];
    for(auto byte:bytes)[result appendFormat:@"%02x",byte];return result;
}
static void updateHash(CC_SHA256_CTX *ctx,NSData *data) {
    const uint8_t *p=(const uint8_t *)data.bytes;
    for(NSUInteger offset=0;offset<data.length;) {
        CC_LONG n=(CC_LONG)std::min<NSUInteger>(1048576,data.length-offset);
        CC_SHA256_Update(ctx,p+offset,n);offset+=n;
    }
}
static NSString *hashData(NSData *data) {
    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);updateHash(&ctx,data);return finishHash(&ctx);
}
static NSData *readFile(NSString *path) {
    NSDictionary *attr=[[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    require([attr[NSFileType] isEqual:NSFileTypeRegular],"requires a regular artifact/input file");
    NSData *data=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    require(data!=nil,"cannot read file");return data;
}
static bool exists(NSString *path) { struct stat info;return !lstat(path.fileSystemRepresentation,&info); }
static NSString *absolute(NSString *path) {
    return [(path.isAbsolutePath?path:[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path]) stringByStandardizingPath];
}
static void publish(NSString *source,NSString *destination) {
    if(!link(source.fileSystemRepresentation,destination.fileSystemRepresentation))return;
    require(errno==EXDEV||errno==EPERM||errno==ENOTSUP||errno==EOPNOTSUPP,"cannot exclusively publish file");
    int output=open(destination.fileSystemRepresentation,O_CREAT|O_EXCL|O_WRONLY,0600);
    require(output>=0,"cannot exclusively create destination");int input=open(source.fileSystemRepresentation,O_RDONLY);
    bool ok=input>=0;char buffer[65536];
    while(ok) {
        ssize_t n=read(input,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;
        if(n<0){ok=false;break;}if(!n)break;
        for(ssize_t offset=0;offset<n;) {
            ssize_t written=write(output,buffer+offset,n-offset);
            if(written<0&&errno==EINTR)continue;
            if(written<=0){ok=false;break;}offset+=written;
        }
    }
    if(ok)ok=!fsync(output);if(input>=0)close(input);close(output);
    if(!ok){unlink(destination.fileSystemRepresentation);throw std::runtime_error("destination copy failed");}
}
static uint64_t integer(NSString *text) {
    require(text.length>0,"missing integer");const char *s=text.UTF8String;
    for(const char *p=s;*p;++p)require(*p>='0'&&*p<='9',"invalid integer");
    errno=0;char *end=nullptr;uint64_t value=strtoull(s,&end,10);
    require(!errno&&end&&!*end,"integer exceeds range");return value;
}
static uint64_t number(NSDictionary *dict,NSString *key) {
    id value=dict[key];require([value isKindOfClass:NSNumber.class],"missing numeric contract field");
    double x=[value doubleValue];require(std::isfinite(x)&&x>=0&&x<=UINT32_MAX&&floor(x)==x,"invalid numeric contract field");
    return [value unsignedLongLongValue];
}
static void writeData(NSData *data,NSString *path) {
    require([data writeToFile:path options:NSDataWritingWithoutOverwriting error:nil],"cannot stage artifact/receipt");
}
static NSString *copyTree(NSString *source,NSString *destination) {
    NSFileManager *fm=NSFileManager.defaultManager;
    require([[fm attributesOfItemAtPath:source error:nil][NSFileType] isEqual:NSFileTypeDirectory],"requires a model directory");
    require([fm createDirectoryAtPath:destination withIntermediateDirectories:YES attributes:nil error:nil],"cannot create model snapshot");
    NSArray *names=[fm subpathsAtPath:source];require(names!=nil,"cannot enumerate model package");
    std::vector<std::filesystem::path> paths;
    for(NSString *name in names)paths.emplace_back(name.UTF8String);
    std::sort(paths.begin(),paths.end());CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);size_t files=0;
    for(const auto &relative:paths) {
        NSString *name=ns(relative.generic_string());NSString *input=[source stringByAppendingPathComponent:name];
        NSString *output=[destination stringByAppendingPathComponent:name];
        NSString *type=[fm attributesOfItemAtPath:input error:nil][NSFileType];
        require(![type isEqual:NSFileTypeSymbolicLink],"model package symlinks are unsupported");
        if([type isEqual:NSFileTypeDirectory])continue;
        NSData *data=readFile(input);NSData *key=[name dataUsingEncoding:NSUTF8StringEncoding];
        uint32_t length=(uint32_t)key.length;uint64_t bytes=data.length;
        CC_SHA256_Update(&ctx,&length,4);updateHash(&ctx,key);CC_SHA256_Update(&ctx,&bytes,8);updateHash(&ctx,data);
        require([fm createDirectoryAtPath:output.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil],"cannot create package directory");
        writeData(data,output);++files;
    }
    require(files>0,"empty model package");return finishHash(&ctx);
}

struct Engine {
    NSString *snapshot=nil,*compiled=nil,*weights=nil,*kernels=nil;
    __strong MLModel *coreml=nil;NSDictionary *contract=nil;NSDictionary *modelInfo=nil;
    std::vector<std::pair<std::pair<int,int>,void *>> contexts;
    int scale=1,maxPlane=512;float residualScale=0;std::string backend;
    ~Engine() {
        for(auto &entry:contexts)gpr_metal_destroy(entry.second);
        coreml=nil;
        if(compiled)[NSFileManager.defaultManager removeItemAtPath:compiled error:nil];
        if(snapshot)[NSFileManager.defaultManager removeItemAtPath:snapshot error:nil];
    }
    void load(NSString *artifact,NSString *checkpoint,NSString *kernelPath,const std::string &selected,NSString *units,int requestedScale) {
        backend=selected;NSData *contractData=readFile([artifact stringByAppendingPathComponent:@"contract.json"]);
        contract=[NSJSONSerialization JSONObjectWithData:contractData options:0 error:nil];
        require([contract isKindOfClass:NSDictionary.class],"invalid artifact contract");
        bool isCore=backend=="coreml";
        require(isCore||backend=="mpsgraph"||backend=="hybrid","unsupported backend");
        require([contract[@"schema"] isEqual:isCore?@"gpr.stock_coreml.v1":@"gpr.stock_metal.v1"],"artifact/backend mismatch");
        require([contract[@"quality_approval"] isEqual:@NO],"artifact must retain explicit unapproved quality status");
        require([hashData(readFile(checkpoint)) isEqual:contract[@"checkpoint_sha256"]],"checkpoint differs from artifact");
        require(number(contract,@"runtime_raw_norm")==16383&&[contract[@"input_cfa"] isEqual:@"RGGB"],"requires RGGB14 normalization");
        scale=(int)number(contract,@"output_scale");require((scale==1||scale==2)&&scale==requestedScale,"artifact output scale mismatch");
        require([contract[@"residual_scale"] isKindOfClass:NSNumber.class],"missing residual scale");
        residualScale=[contract[@"residual_scale"] floatValue];
        require(std::isfinite(residualScale)&&residualScale>0&&residualScale<=1,"invalid residual scale");
        maxPlane=(int)number(contract,@"max_plane");require(maxPlane>=32&&maxPlane<=2048&&maxPlane%8==0,"invalid patch limit");
        snapshot=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"gpr-native-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        require([NSFileManager.defaultManager createDirectoryAtPath:snapshot withIntermediateDirectories:NO
            attributes:@{NSFilePosixPermissions:@0700} error:nil],"cannot create private model snapshot");
        NSMutableDictionary *info=[NSMutableDictionary dictionaryWithDictionary:@{
            @"runtime_backend":ns(backend),@"host_runtime":@"native",@"checkpoint_sha256":contract[@"checkpoint_sha256"],
            @"architecture_sha256":contract[@"architecture_sha256"],@"output_scale":@(scale),@"runtime_raw_norm":@16383,
            @"input_cfa":@"RGGB",@"residual_scale":@(residualScale),@"artifact_contract_sha256":hashData(contractData),
            @"precision":contract[@"precision"],@"quality_status":@"unvalidated_for_stock_video"}];
        if(isCore) {
            require([contract[@"input_name"] isEqual:@"bayer_planes"]&&[contract[@"output_name"] isEqual:@"residual"],"unsupported model tensor names");
            require([contract[@"precision"] isEqual:@"float32"]||[contract[@"precision"] isEqual:@"float16"],"unsupported model precision");
            NSString *package=[snapshot stringByAppendingPathComponent:@"model.mlpackage"];
            require([copyTree([artifact stringByAppendingPathComponent:@"model.mlpackage"],package) isEqual:contract[@"package_sha256"]],"model package differs from contract");
            NSError *error=nil;NSURL *url=[MLModel compileModelAtURL:[NSURL fileURLWithPath:package] error:&error];
            require(url!=nil,"Core ML model compilation failed");compiled=url.path;
            MLModelConfiguration *config=[MLModelConfiguration new];
            if([units isEqual:@"cpu"])config.computeUnits=MLComputeUnitsCPUOnly;
            else if([units isEqual:@"gpu"])config.computeUnits=MLComputeUnitsCPUAndGPU;
            else if([units isEqual:@"ane"])config.computeUnits=MLComputeUnitsCPUAndNeuralEngine;
            else { require([units isEqual:@"all"],"unsupported Core ML compute units");config.computeUnits=MLComputeUnitsAll; }
            coreml=[MLModel modelWithContentsOfURL:url configuration:config error:&error];
            require(coreml!=nil,"Core ML model load failed");info[@"compute_units"]=units;
            info[@"package_sha256"]=contract[@"package_sha256"];
        } else {
            require(number(contract,@"width")==16&&number(contract,@"depth")==3&&number(contract,@"dw_kernel")==3&&maxPlane==512,
                "unsupported Metal architecture");
            require([contract[@"precision"] isEqual:@"float16"],"Metal requires fp16 artifact");
            NSDictionary *files=contract[@"files"];require([files isKindOfClass:NSDictionary.class]&&files.count>0,"invalid weight manifest");
            NSString *original=[artifact stringByAppendingPathComponent:@"weights"];
            NSArray *actual=[NSFileManager.defaultManager contentsOfDirectoryAtPath:original error:nil];
            require([[NSSet setWithArray:actual] isEqual:[NSSet setWithArray:files.allKeys]],"weight directory entries differ");
            weights=[snapshot stringByAppendingPathComponent:@"weights"];
            require([NSFileManager.defaultManager createDirectoryAtPath:weights withIntermediateDirectories:NO attributes:nil error:nil],"cannot stage weights");
            for(NSString *name in files) {
                require([name isKindOfClass:NSString.class]&&[name.lastPathComponent isEqual:name]&&![name isEqual:@".."],"invalid weight name");
                NSData *data=readFile([original stringByAppendingPathComponent:name]);NSDictionary *expected=files[name];
                require(data.length==[expected[@"bytes"] unsignedLongLongValue]&&[hashData(data) isEqual:expected[@"sha256"]],"weight bytes differ");
                writeData(data,[weights stringByAppendingPathComponent:name]);
            }
            NSData *kernelData=readFile(kernelPath);kernels=[snapshot stringByAppendingPathComponent:@"kernels.metallib"];
            writeData(kernelData,kernels);info[@"metallib_sha256"]=hashData(kernelData);
        }
        modelInfo=info;
    }
    void predict(std::vector<float> &input,int h,int w,std::vector<float> &output) {
        output.resize((size_t)4*h*w*scale*scale);
        if(coreml) {
            NSError *error=nil;
            MLMultiArray *array=[[MLMultiArray alloc] initWithDataPointer:input.data() shape:@[@1,@4,@(h),@(w)]
                dataType:MLMultiArrayDataTypeFloat32 strides:@[@(4*h*w),@(h*w),@(w),@1] deallocator:^(void *){} error:&error];
            require(array!=nil,"cannot wrap Core ML input");
            MLDictionaryFeatureProvider *features=[[MLDictionaryFeatureProvider alloc]
                initWithDictionary:@{@"bayer_planes":[MLFeatureValue featureValueWithMultiArray:array]} error:&error];
            id<MLFeatureProvider> prediction=[coreml predictionFromFeatures:features error:&error];
            MLMultiArray *result=[prediction featureValueForName:@"residual"].multiArrayValue;
            require(result&&result.dataType==MLMultiArrayDataTypeFloat32&&
                [result.shape isEqual:@[@1,@4,@(h*scale),@(w*scale)]],"invalid Core ML residual shape/type");
            const float *data=(const float *)result.dataPointer;
            size_t sc=[result.strides[1] unsignedLongLongValue],sy=[result.strides[2] unsignedLongLongValue],
                   sx=[result.strides[3] unsignedLongLongValue];
            if(sx==1&&sy==(size_t)w*scale&&sc==(size_t)h*w*scale*scale) {
                memcpy(output.data(),data,output.size()*sizeof(float));
            } else {
            for(int c=0;c<4;++c)for(int y=0;y<h*scale;++y)for(int x=0;x<w*scale;++x)
                output[((size_t)c*h*scale+y)*w*scale+x]=data[c*sc+y*sy+x*sx];
            }
        } else {
            auto key=std::make_pair(h,w);auto found=std::find_if(contexts.begin(),contexts.end(),[&](const auto &v){return v.first==key;});
            void *context=nullptr;
            if(found!=contexts.end()){context=found->second;contexts.erase(found);}
            else {
                if(contexts.size()==2){gpr_metal_destroy(contexts.front().second);contexts.erase(contexts.begin());}
                context=gpr_metal_create(weights.fileSystemRepresentation,kernels.fileSystemRepresentation,h,w,scale,backend=="hybrid");
                require(context!=nullptr,"native Metal context creation failed");
            }
            contexts.push_back({key,context});
            require(!gpr_metal_predict(context,input.data(),input.size(),output.data(),output.size()),"native Metal inference failed");
        }
        for(float value:output)require(std::isfinite(value),"nonfinite native residual");
    }
};

static float clip(float x) { return std::max(0.f,std::min(1.f,x)); }
static const float cubicWeights[2][4]={{-.03515625f,.26171875f,.87890625f,-.10546875f},
                                     {-.10546875f,.87890625f,.26171875f,-.03515625f}};
static void horizontalFilter(const std::vector<float> &patch,int h,int w,std::vector<float> &out,size_t threads) {
    out.resize((size_t)4*h*w*2);
    auto worker=[&](size_t task) {
        for(size_t row=task*4*h/threads;row<(task+1)*4*h/threads;++row) {
            const float *input=patch.data()+row*w;float *output=out.data()+row*w*2;
            auto scalar=[&](int i) {
                for(int phase=0;phase<2;++phase) {
                    int ix=i-(phase==0);float value=0;
                    for(int j=0;j<4;++j)value+=input[std::max(0,std::min(w-1,ix+j-1))]*cubicWeights[phase][j];
                    output[2*i+phase]=value;
                }
            };
            int i=0;for(;i<std::min(2,w);++i)scalar(i);
#if GPR_NATIVE_NEON
            for(;i+5<w;i+=4) {
                float32x4_t a=vld1q_f32(input+i-2),b=vld1q_f32(input+i-1),c=vld1q_f32(input+i),
                            d=vld1q_f32(input+i+1),e=vld1q_f32(input+i+2);
                float32x4_t even=vmulq_n_f32(a,cubicWeights[0][0]),odd=vmulq_n_f32(b,cubicWeights[1][0]);
                even=vaddq_f32(even,vmulq_n_f32(b,cubicWeights[0][1]));odd=vaddq_f32(odd,vmulq_n_f32(c,cubicWeights[1][1]));
                even=vaddq_f32(even,vmulq_n_f32(c,cubicWeights[0][2]));odd=vaddq_f32(odd,vmulq_n_f32(d,cubicWeights[1][2]));
                even=vaddq_f32(even,vmulq_n_f32(d,cubicWeights[0][3]));odd=vaddq_f32(odd,vmulq_n_f32(e,cubicWeights[1][3]));
                float32x4x2_t values;values.val[0]=even;values.val[1]=odd;vst2q_f32(output+2*i,values);
            }
#endif
            for(;i<w;++i)scalar(i);
        }
    };
    dispatch_apply(threads,DISPATCH_APPLY_AUTO,^(size_t task){worker(task);});
}
#if GPR_NATIVE_NEON
static uint16x4_t nativeCounts4(const std::vector<float> &patch,const std::vector<float> &horizontal,
    const std::vector<float> &residual,int h,int w,int c,int y,int x,int scale,float residualScale) {
    float32x4_t base;
    if(scale==1)base=vld1q_f32(patch.data()+((size_t)c*h+y)*w+x);
    else {
        int iy=y/2-(y%2==0);const float *weights=cubicWeights[y%2];
        auto row=[&](int j){return vld1q_f32(horizontal.data()+((size_t)c*h+std::max(0,std::min(h-1,iy+j-1)))*w*2+x);};
        base=vmulq_n_f32(row(0),weights[0]);
        base=vaddq_f32(base,vmulq_n_f32(row(1),weights[1]));base=vaddq_f32(base,vmulq_n_f32(row(2),weights[2]));
        base=vaddq_f32(base,vmulq_n_f32(row(3),weights[3]));
        base=vminq_f32(vdupq_n_f32(1),vmaxq_f32(vdupq_n_f32(0),base));
    }
    float32x4_t delta=vmulq_n_f32(vld1q_f32(residual.data()+((size_t)c*h*scale+y)*w*scale+x),residualScale);
    float32x4_t value=vminq_f32(vdupq_n_f32(1),vmaxq_f32(vdupq_n_f32(0),vaddq_f32(base,delta)));
    value=vaddq_f32(vmulq_n_f32(value,16383.f),vdupq_n_f32(.5f));
    return vmovn_u32(vcvtq_u32_f32(value));
}
#endif
static float baseline(const std::vector<float> &patch,int h,int w,int c,int y,int x,int scale) {
    if(scale==1)return patch[((size_t)c*h+y)*w+x];
    // Exact dyadic coefficients of the a=-0.75 cubic at t=.75/.25.
    int iy=y/2-(y%2==0),ix=x/2-(x%2==0);float rows[4];
    const float *wy=cubicWeights[y%2],*wx=cubicWeights[x%2];
    for(int j=0;j<4;++j) {
        int sy=std::max(0,std::min(h-1,iy+j-1));float row=0;
        for(int i=0;i<4;++i)row+=patch[((size_t)c*h+sy)*w+std::max(0,std::min(w-1,ix+i-1))]*wx[i];
        rows[j]=row;
    }
    float result=0;for(int j=0;j<4;++j)result+=rows[j]*wy[j];return clip(result);
}

struct Stage {
    NSString *directory=nil;int fd=-1;uint16_t *data=nullptr;size_t bytes=0;
    ~Stage(){if(data)munmap(data,bytes);if(fd>=0)close(fd);if(directory)[NSFileManager.defaultManager removeItemAtPath:directory error:nil];}
    void create(NSString *parent,size_t length) {
        std::string pattern=std::string(parent.fileSystemRepresentation)+"/.gpr-reconstruct-XXXXXX";
        std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);
        require(mkdtemp(path.data())!=nullptr,"cannot create output staging directory");directory=ns(path.data());
        fd=open([directory stringByAppendingPathComponent:@"output.raw"].fileSystemRepresentation,O_CREAT|O_EXCL|O_RDWR,0600);
        require(fd>=0&&!ftruncate(fd,length),"cannot size output staging file");bytes=length;
        void *mapped=mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        require(mapped!=MAP_FAILED,"cannot map output staging file");data=(uint16_t *)mapped;
    }
};

static int run(int argc,const char **argv) {
    if(argc==2&&!strcmp(argv[1],"--help")) {
        puts("gpr_reconstruct --input RGGB14.raw --output restored.raw --width W --height H --scale 1|2\n"
             "  --checkpoint selected.pt --artifact exported-directory --backend coreml|mpsgraph|hybrid\n"
             "  [--kernels custom.metallib] [--units cpu|gpu|ane|all] [--tile 256 --halo 64 --threads 4]\n"
             "  [--host-kernel auto|scalar|neon] [--receipt new.json]\n"
             "Native inference and host processing; no Torch. Input/output: little-endian RGGB14 words.\n"
             "Outputs and receipts must be new. Reconstruction quality is not automatically approved.");
        return 0;
    }
    std::map<std::string,std::string> options;
    const std::vector<std::string> keys={"input","output","receipt","artifact","checkpoint","backend","kernels","units","width","height","scale","tile","halo","threads","host-kernel"};
    for(int i=1;i<argc;i+=2) {
        std::string key=argv[i];require(key.rfind("--",0)==0&&i+1<argc,"arguments must be --name value pairs");key=key.substr(2);
        require(std::find(keys.begin(),keys.end(),key)!=keys.end()&&!options.count(key),"unknown or repeated argument");options[key]=argv[i+1];
    }
    for(const char *name:{"input","output","artifact","checkpoint","backend","width","height","scale"})require(options.count(name),"missing required argument");
    auto numeric=[&](const char *name,uint64_t fallback){return options.count(name)?integer(ns(options[name])):fallback;};
    uint64_t width=numeric("width",0),height=numeric("height",0),scale=numeric("scale",0),tile=numeric("tile",256),halo=numeric("halo",64);
    uint64_t threads=numeric("threads",4);require(threads>=1&&threads<=64,"threads must be 1..64");
    std::string host=options.count("host-kernel")?options["host-kernel"]:"auto";
    require(host=="auto"||host=="scalar"||host=="neon","unsupported host kernel");
    require(host!="neon"||GPR_NATIVE_NEON,"NEON host kernel unavailable on this architecture");
    bool neon=GPR_NATIVE_NEON&&host!="scalar";
    require(width>=16&&height>=16&&width<=32768&&height<=32768&&width%2==0&&height%2==0&&(scale==1||scale==2),"invalid Bayer geometry/scale");
    require(width*height*scale*scale<=268435456ULL,"native output exceeds 268435456-pixel limit");
    require(tile>=8&&tile<=2048&&halo<=2048&&tile%8==0&&halo%8==0,"invalid tile/halo");
    NSString *input=absolute(ns(options["input"])),*output=absolute(ns(options["output"]));
    NSString *receipt=options.count("receipt")?absolute(ns(options["receipt"])):[output stringByAppendingString:@".json"];
    require(![output isEqual:receipt]&&!exists(output)&&!exists(receipt),"output/receipt must be distinct new paths");
    double start=seconds();Engine engine;
    engine.load(ns(options["artifact"]),ns(options["checkpoint"]),ns(options["kernels"]),options["backend"],
        ns(options.count("units")?options["units"]:"all"),(int)scale);
    int hp=(int)height/2,wp=(int)width/2;
    require(((std::min<int>(hp,tile+2*halo)+7)/8*8)<=engine.maxPlane&&
        ((std::min<int>(wp,tile+2*halo)+7)/8*8)<=engine.maxPlane,"patch plus halo exceeds artifact limit");
    double loadTime=seconds()-start;NSData *source=readFile(input);
    require(source.length==width*height*2,"input must contain tightly packed RGGB14 words");
    NSString *sourceHash=hashData(source);const uint16_t *raw=(const uint16_t *)source.bytes;
    for(size_t i=0;i<width*height;++i)require(raw[i]<=16383,"input exceeds RGGB14 count domain");
    for(NSString *path in @[output,receipt])require([NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:nil],"cannot create output parent");
    Stage stage;stage.create(output.stringByDeletingLastPathComponent,width*height*scale*scale*2);
    double begin=seconds();std::vector<float> patch,residual,horizontal;size_t tiles=0;
    int coreSize=(int)tile,context=(int)halo;
    for(int y=0;y<hp;y+=coreSize)for(int x=0;x<wp;x+=coreSize) { @autoreleasepool {
        int y1=std::min(hp,y+coreSize),x1=std::min(wp,x+coreSize),ya=std::max(0,y-context),xa=std::max(0,x-context);
        int yb=std::min(hp,y1+context),xb=std::min(wp,x1+context),ph=(yb-ya+7)/8*8,pw=(xb-xa+7)/8*8;
        patch.resize((size_t)4*ph*pw);
        for(int c=0;c<4;++c)for(int py=0;py<ph;++py)for(int px=0;px<pw;++px) {
            int sy=ya+std::min(py,yb-ya-1),sx=xa+std::min(px,xb-xa-1);
            patch[((size_t)c*ph+py)*pw+px]=raw[((size_t)2*sy+c/2)*width+2*sx+c%2]/16383.f;
        }
        engine.predict(patch,ph,pw,residual);
        if(neon&&scale==2)horizontalFilter(patch,ph,pw,horizontal,threads);
        auto convertRows=[&](size_t worker) {
        int first=(int)(worker*(y1-y)*scale/threads),last=(int)((worker+1)*(y1-y)*scale/threads);
        int vectorWidth=neon?((x1-x)*(int)scale/4*4):0;
#if GPR_NATIVE_NEON
        if(neon)for(int oy=first;oy<last;++oy)for(int pair=0;pair<2;++pair)for(int ox=0;ox<vectorWidth;ox+=4) {
            int py=(y-ya)*(int)scale+oy,px=(x-xa)*(int)scale+ox;
            uint16x4x2_t values;
            values.val[0]=nativeCounts4(patch,horizontal,residual,ph,pw,pair*2,py,px,(int)scale,engine.residualScale);
            values.val[1]=nativeCounts4(patch,horizontal,residual,ph,pw,pair*2+1,py,px,(int)scale,engine.residualScale);
            size_t sy=2*(y*scale+oy)+pair,sx=2*(x*scale+ox);
            vst2_u16(stage.data+sy*width*scale+sx,values);
        }
#endif
        for(int oy=first;oy<last;++oy)for(int c=0;c<4;++c)for(int ox=vectorWidth;ox<(x1-x)*scale;++ox) {
            int py=(y-ya)*scale+oy,px=(x-xa)*scale+ox;
            float base=baseline(patch,ph,pw,c,py,px,(int)scale);
            float delta=engine.residualScale*residual[((size_t)c*ph*scale+py)*pw*scale+px];
            float counts=clip(base+delta)*16383.f+.5f;
            size_t sy=2*(y*scale+oy)+c/2,sx=2*(x*scale+ox)+c%2;
            stage.data[sy*width*scale+sx]=(uint16_t)std::min(16383.f,counts);
        }};
        dispatch_apply(threads,DISPATCH_APPLY_AUTO,^(size_t worker){convertRows(worker);});
        ++tiles;
    }}
    double restoreTime=seconds()-begin;
    require([hashData(readFile(input)) isEqual:sourceHash],"input changed during reconstruction");
    require(!msync(stage.data,stage.bytes,MS_SYNC)&&!fsync(stage.fd),"cannot flush reconstructed RAW");
    NSString *stagedRaw=[stage.directory stringByAppendingPathComponent:@"output.raw"];
    uint32_t executableLength=0;_NSGetExecutablePath(nullptr,&executableLength);std::vector<char> executable(executableLength);
    require(!_NSGetExecutablePath(executable.data(),&executableLength),"cannot identify native executable");
    Dl_info linked{};require(dladdr((void *)&gpr_metal_create,&linked)&&linked.dli_fname,"cannot identify native library");
    NSDictionary *result=@{@"schema":@"gpr.native_reconstruct.v1",@"complete":@YES,@"quality_approval":@NO,
        @"input_sha256":sourceHash,@"output_sha256":hashData(readFile(stagedRaw)),@"model":engine.modelInfo,
        @"width":@(width*scale),@"height":@(height*scale),@"source_width":@(width),@"source_height":@(height),
        @"sample_bits":@14,@"cfa":@"RGGB",@"output_scale":@(scale),@"tile":@(tile),@"halo":@(halo),@"tiles":@(tiles),
        @"model_load_s":@(loadTime),@"restore_s":@(restoreTime),@"threads":@(threads),@"host_kernel":neon?@"neon":@"scalar",@"elapsed_before_publication_s":@(seconds()-start),
        @"executable_sha256":hashData(readFile([ns(executable.data()) stringByResolvingSymlinksInPath])),
        @"native_library_sha256":hashData(readFile([ns(linked.dli_fname) stringByResolvingSymlinksInPath])),
        @"host_baseline":@"float32_bicubic_a_minus_0_75_align_corners_false",@"torch_required":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];require(json!=nil,"cannot serialize receipt");
    NSString *stagedReceipt=[stage.directory stringByAppendingPathComponent:@"receipt.json"];writeData(json,stagedReceipt);
    publish(stagedRaw,output);
    try { publish(stagedReceipt,receipt); } catch(...) { unlink(output.fileSystemRepresentation);throw; }
    fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);return 0;
}
int main(int argc,const char **argv) {
    @autoreleasepool { @try { try { return run(argc,argv); }
        catch(const std::exception &error){fprintf(stderr,"gpr_reconstruct: %s\n",error.what());return 1;}
    } @catch(NSException *error){fprintf(stderr,"gpr_reconstruct: %s\n",error.reason.UTF8String);return 1;} }
}
