#ifndef GPR_CNN_CONFIG_H
#define GPR_CNN_CONFIG_H

#import <Foundation/Foundation.h>

#ifndef GPR_HAVE_SUPER_RES_COREML
#define GPR_HAVE_SUPER_RES_COREML 0
#endif
#ifndef GPR_HAVE_SUPER_RES_METAL
#define GPR_HAVE_SUPER_RES_METAL 0
#endif
#define GPR_HAVE_CNN (GPR_HAVE_SUPER_RES_COREML || GPR_HAVE_SUPER_RES_METAL)

#if GPR_HAVE_SUPER_RES_COREML
#define GPR_DEFAULT_CNN_BACKEND @"coreml"
#elif GPR_HAVE_SUPER_RES_METAL
#define GPR_DEFAULT_CNN_BACKEND @"metal"
#else
#define GPR_DEFAULT_CNN_BACKEND @"none"
#endif

static inline BOOL GPRCNNBackendAvailable(NSString *backend) {
    if ([backend isEqualToString:@"coreml"]) return GPR_HAVE_SUPER_RES_COREML;
    if ([backend isEqualToString:@"metal"] || [backend isEqualToString:@"mpsgraph"])
        return GPR_HAVE_SUPER_RES_METAL;
    return NO;
}

#endif
