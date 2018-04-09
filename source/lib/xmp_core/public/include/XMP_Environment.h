#ifndef __XMP_Environment_h__
#define __XMP_Environment_h__ 1

// =================================================================================================
// XMP_Environment.h - Build environment flags for the XMP toolkit.
// ================================================================
//
// This header is just C preprocessor macro definitions to set up the XMP toolkit build environment.
// It must be the first #include in any chain since it might affect things in other #includes.
//
// =================================================================================================

// =================================================================================================
// Copyright 2002 Adobe Systems Incorporated
// All Rights Reserved.
//
// NOTICE:  Adobe permits you to use, modify, and distribute this file in accordance with the terms
// of the Adobe license agreement accompanying it.
// =================================================================================================

// =================================================================================================
// Determine the Platform
// =================================================================================================

#ifdef _WIN32

    #define XMP_MacBuild  0
    #define XMP_iOSBuild  0
    #define XMP_WinBuild  1
    #define XMP_UNIXBuild 0

#elif __APPLE__

    #include "TargetConditionals.h"

    #if TARGET_OS_IPHONE

        #define XMP_MacBuild  0
        #define XMP_iOSBuild  1
        #define XMP_WinBuild  0
        #define XMP_UNIXBuild 0

    #else

        #define XMP_MacBuild  1
        #define XMP_iOSBuild  0
        #define XMP_WinBuild  0
        #define XMP_UNIXBuild 0

    #endif

#elif __ANDROID__

#elif __linux__ || __unix__

    #define XMP_MacBuild  0
    #define XMP_WinBuild  0
    #define XMP_UNIXBuild 1
    #define XMP_iOSBuild  0

#else
    #error "XMP environment error - Unknown compiler"
#endif

// =================================================================================================
// Common Macros
// =============

#if defined ( DEBUG )
    #if defined ( NDEBUG )
        #error "XMP environment error - both DEBUG and NDEBUG are defined"
    #endif
    #define XMP_DebugBuild 1
#endif

#if defined ( NDEBUG )
    #define XMP_DebugBuild 0
#endif

#ifndef XMP_DebugBuild
    #define XMP_DebugBuild 0
#endif

#if XMP_DebugBuild
    #include <stdio.h>  // The assert macro needs printf.
#endif

#ifndef DISABLE_SERIALIZED_IMPORT_EXPORT 
	#define DISABLE_SERIALIZED_IMPORT_EXPORT 0
#endif

#ifndef XMP_64
	#if _WIN64 || defined(_LP64)
		#define XMP_64 1
	#else
		#define XMP_64 0
	#endif
#endif

// =================================================================================================
// Macintosh Specific Settings
// ===========================
#if (XMP_MacBuild)
	#define XMP_HELPER_DLL_IMPORT __attribute__((visibility("default")))
	#define XMP_HELPER_DLL_EXPORT __attribute__((visibility("default")))
	#define XMP_HELPER_DLL_PRIVATE __attribute__((visibility("hidden")))
#endif

// =================================================================================================
// Windows Specific Settings
// =========================
#if (XMP_WinBuild)
	#define XMP_HELPER_DLL_IMPORT
	#define XMP_HELPER_DLL_EXPORT
	#define XMP_HELPER_DLL_PRIVATE
#endif

// =================================================================================================
// UNIX Specific Settings
// ======================
#if (XMP_UNIXBuild)
	#define XMP_HELPER_DLL_IMPORT
	#define XMP_HELPER_DLL_EXPORT
	#define XMP_HELPER_DLL_PRIVATE
#endif

// =================================================================================================
// IOS Specific Settings
// ===========================
#if (XMP_iOSBuild)
	#include <TargetConditionals.h>
	#if (TARGET_CPU_ARM)
		#define XMP_IOS_ARM 1
	#else
		#define XMP_IOS_ARM 0
	#endif
	#define XMP_HELPER_DLL_IMPORT __attribute__((visibility("default")))
	#define XMP_HELPER_DLL_EXPORT __attribute__((visibility("default")))
	#define XMP_HELPER_DLL_PRIVATE __attribute__((visibility("hidden")))
#endif

// =================================================================================================
// Banzai Specific Settings
// ======================
#if (XMP_Banzai)
	#define XMP_HELPER_DLL_IMPORT
	#define XMP_HELPER_DLL_EXPORT
	#define XMP_HELPER_DLL_PRIVATE
#endif


// =================================================================================================

#if (XMP_DynamicBuild)
	#define XMP_PUBLIC XMP_HELPER_DLL_EXPORT
	#define XMP_PRIVATE XMP_HELPER_DLL_PRIVATE
#elif (XMP_StaticBuild)
	#define XMP_PUBLIC
	#define XMP_PRIVATE
#else
	#define XMP_PUBLIC XMP_HELPER_DLL_IMPORT
	#define XMP_PRIVATE XMP_HELPER_DLL_PRIVATE
#endif

#endif  // __XMP_Environment_h__
