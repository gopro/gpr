#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stddef.h>

typedef NS_ENUM(NSInteger, StockMetalResidualBackend) {
    StockMetalResidualBackendMPSGraph=0,
    StockMetalResidualBackendHybrid=1
};

@interface StockMetalResidual : NSObject
- (instancetype)initWithWeightsDir:(NSString *)directory library:(NSString *)library
                           height:(uint32_t)height width:(uint32_t)width
                            scale:(uint32_t)scale backend:(uint32_t)backend;
- (int)runPlanes:(const float *)input count:(size_t)inputCount
          output:(float *)output count:(size_t)outputCount;
@end

// Private bridge for the optional Python workflow. Contexts are serialized by
// their caller; input/output are tightly packed float32 NCHW, batch one, C=4.
#ifdef __cplusplus
extern "C" {
#endif
void *gpr_metal_create(const char *,const char *,uint32_t,uint32_t,uint32_t,uint32_t);
int gpr_metal_predict(void *,const float *,size_t,float *,size_t);
void gpr_metal_destroy(void *);
#ifdef __cplusplus
}
#endif
