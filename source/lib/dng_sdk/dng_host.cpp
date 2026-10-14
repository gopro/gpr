/*****************************************************************************/
// Copyright 2006-2012 Adobe Systems Incorporated
// All Rights Reserved.
//
// NOTICE:  Adobe permits you to use, modify, and distribute this file in
// accordance with the terms of the Adobe license agreement accompanying it.
/*****************************************************************************/

/* $Id: //mondo/dng_sdk_1_4/dng_sdk/source/dng_host.cpp#2 $ */ 
/* $DateTime: 2012/06/14 20:24:41 $ */
/* $Change: 835078 $ */
/* $Author: tknoll $ */

/*****************************************************************************/

#include "dng_host.h"

#include "dng_abort_sniffer.h"
#include "dng_area_task.h"

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#endif

#include "dng_bad_pixels.h"
#include "dng_exceptions.h"
#include "dng_exif.h"
#include "dng_gain_map.h"
#include "dng_ifd.h"
#include "dng_lens_correction.h"
#include "dng_memory.h"
#include "dng_misc_opcodes.h"
#include "dng_negative.h"
#include "dng_resample.h"
#include "dng_shared.h"
#include "dng_simple_image.h"
#include "dng_xmp.h"

/*****************************************************************************/

dng_host::dng_host (dng_memory_allocator *allocator,
					dng_abort_sniffer *sniffer)

	:	fAllocator	(allocator)
	,	fSniffer	(sniffer)
	
	,	fNeedsMeta  		(true)
	,	fNeedsImage 		(true)
	,	fForPreview 		(false)
	,	fMinimumSize 		(0)
	,	fPreferredSize      (0)
	,	fMaximumSize    	(0)
	,	fCropFactor			(1.0)
	,	fSaveDNGVersion		(dngVersion_None)
	,	fSaveLinearDNG		(false)
	,	fKeepOriginalFile	(false)
	
	{
	
	}
	
/*****************************************************************************/

dng_host::~dng_host ()
	{
	
	}
	
/*****************************************************************************/

dng_memory_allocator & dng_host::Allocator ()
	{
	
	if (fAllocator)
		{
		
		return *fAllocator;
		
		}
		
	else
		{
		
		return gDefaultDNGMemoryAllocator;
		
		}
	
	}

/*****************************************************************************/

dng_memory_block * dng_host::Allocate (uint32 logicalSize)
	{
	
	return Allocator ().Allocate (logicalSize);
		
	}
		
/*****************************************************************************/

void dng_host::SniffForAbort ()
	{
	
	dng_abort_sniffer::SniffForAbort (Sniffer ());
	
	}
		
/*****************************************************************************/

void dng_host::ValidateSizes ()
	{
	
	// The maximum size limits the other two sizes.
	
	if (MaximumSize ())
		{
		SetMinimumSize   (Min_uint32 (MinimumSize   (), MaximumSize ()));
		SetPreferredSize (Min_uint32 (PreferredSize (), MaximumSize ()));
		}
		
	// If we have a preferred size, it limits the minimum size.
	
	if (PreferredSize ())
		{
		SetMinimumSize (Min_uint32 (MinimumSize (), PreferredSize ()));
		}
		
	// Else find default value for preferred size.
	
	else
		{
		
		// If preferred size is zero, then we want the maximim
		// size image.
		
		if (MaximumSize ())
			{
			SetPreferredSize (MaximumSize ());
			}
		
		}
		
	// If we don't have a minimum size, find default.
	
	if (!MinimumSize ())
		{
	
		// A common size for embedded thumbnails is 120 by 160 pixels,
		// So allow 120 by 160 pixels to be used for thumbnails when the
		// preferred size is 256 pixel.
		
		if (PreferredSize () >= 160 && PreferredSize () <= 256)
			{
			SetMinimumSize (160);
			}
			
		// Many sensors are near a multiple of 1024 pixels in size, but after
		// the default crop, they are a just under.  We can get an extra factor
		// of size reduction if we allow a slight undershoot in the final size
		// when computing large previews.
		
		else if (PreferredSize () >= 490 && PreferredSize () <= 512)
			{
			SetMinimumSize (490);
			}

		else if (PreferredSize () >= 980 && PreferredSize () <= 1024)
			{
			SetMinimumSize (980);
			}

		else if (PreferredSize () >= 1470 && PreferredSize () <= 1536)
			{
			SetMinimumSize (1470);
			}

		else if (PreferredSize () >= 1960 && PreferredSize () <= 2048)
			{
			SetMinimumSize (1960);
			}

		// Else minimum size is same as preferred size.
			
		else
			{
			SetMinimumSize (PreferredSize ());
			}
			
		}
	
	}

/*****************************************************************************/

uint32 dng_host::SaveDNGVersion () const
	{

	return fSaveDNGVersion;

	}

/*****************************************************************************/

bool dng_host::SaveLinearDNG (const dng_negative & /* negative */) const
	{

	return fSaveLinearDNG;

	}

/*****************************************************************************/

bool dng_host::IsTransientError (dng_error_code code)
	{
	
	switch (code)
		{
		
		case dng_error_memory:
		case dng_error_user_canceled:
			{
			return true;
			}
			
		default:
			break;
			
		}
		
	return false;
	
	}
		
/*****************************************************************************/

#if !defined(_WIN32)
namespace {

struct dng_area_task_worker_args
	{
	dng_area_task *task;
	uint32 threadIndex;
	dng_rect area;
	dng_point tileSize;
	dng_abort_sniffer *sniffer;
	bool failed;
	dng_error_code errorCode;
	};

extern "C" void *dng_area_task_worker (void *arg)
	{
	dng_area_task_worker_args *a = (dng_area_task_worker_args *) arg;
	try
		{
		a->task->ProcessOnThread (a->threadIndex, a->area, a->tileSize, a->sniffer);
		}
	catch (const dng_exception &e)
		{
		a->failed = true;
		a->errorCode = e.ErrorCode ();
		}
	catch (...)
		{
		a->failed = true;
		a->errorCode = dng_error_unknown;
		}
	return NULL;
	}

}
#endif

void dng_host::PerformAreaTask (dng_area_task &task,
								const dng_rect &area)
	{

#if !defined(_WIN32)
	uint32 threadCount = PerformAreaTaskThreads ();

	if (threadCount > 1 && task.MultiThreadSafe ())
		{
		dng_point tileSize (task.FindTileSize (area));
		uint32 tv = (tileSize.v > 0) ? (uint32) tileSize.v : 16u;
		uint32 th = (tileSize.h > 0) ? (uint32) tileSize.h : 16u;
		uint32 tilesV = (area.H () + tv - 1) / tv;
		uint32 tilesH = (area.W () + th - 1) / th;
		bool splitOnH = (tilesH >= tilesV);
		uint32 totalTiles = splitOnH ? tilesH : tilesV;

		if (totalTiles < 2)
			{
			dng_area_task::Perform (task, area, &Allocator (), Sniffer ());
			return;
			}

		uint32 actualThreads = Min_uint32 (totalTiles, threadCount);
		if (actualThreads > 32) actualThreads = 32;
		uint32 tilesPerThread = (totalTiles + actualThreads - 1) / actualThreads;

		task.Start (actualThreads, tileSize, &Allocator (), Sniffer ());

		pthread_t threads [32];
		dng_area_task_worker_args args [32];
		uint32 created = 0;
		uint32 workers = 0;

		for (uint32 t = 0; t < actualThreads; ++t)
			{
			args [t].task        = &task;
			args [t].threadIndex = t;
			args [t].tileSize    = tileSize;
			args [t].sniffer     = Sniffer ();
			args [t].failed      = false;
			args [t].errorCode   = dng_error_none;

			if (splitOnH)
				{
				int32 left  = area.l + (int32) (t * tilesPerThread * th);
				int32 right = left + (int32) (tilesPerThread * th);
				if (left >= area.r) break;
				if (right > area.r) right = area.r;
				args [t].area = dng_rect (area.t, left, area.b, right);
				}
			else
				{
				int32 top    = area.t + (int32) (t * tilesPerThread * tv);
				int32 bottom = top + (int32) (tilesPerThread * tv);
				if (top >= area.b) break;
				if (bottom > area.b) bottom = area.b;
				args [t].area = dng_rect (top, area.l, bottom, area.r);
				}

			++workers;

			if (pthread_create (&threads [created], NULL, dng_area_task_worker, &args [t]) == 0)
				{
				++created;
				}
			else
				{
				dng_area_task_worker (&args [t]);
				}
			}

		for (uint32 t = 0; t < created; ++t)
			{
			pthread_join (threads [t], NULL);
			}

		for (uint32 t = 0; t < workers; ++t)
			{
			if (args [t].failed)
				{
				Throw_dng_error (args [t].errorCode);
				}
			}

		task.Finish (workers);
		return;
		}
#endif

	dng_area_task::Perform (task,
							area,
							&Allocator (),
							Sniffer ());

	}

/*****************************************************************************/

uint32 dng_host::PerformAreaTaskThreads ()
	{

#if !defined(_WIN32)
	long ncpu = sysconf (_SC_NPROCESSORS_ONLN);
	if (ncpu < 1) ncpu = 1;
	if (ncpu > 8) ncpu = 8;
	return (uint32) ncpu;
#else
	return 1;
#endif

	}

/*****************************************************************************/

dng_exif * dng_host::Make_dng_exif ()
	{
	
	dng_exif *result = new dng_exif ();
	
	if (!result)
		{
		
		ThrowMemoryFull ();

		}
	
	return result;
	
	}

/*****************************************************************************/

dng_xmp * dng_host::Make_dng_xmp ()
	{
	
	dng_xmp *result = new dng_xmp (Allocator ());
	
	if (!result)
		{
		
		ThrowMemoryFull ();
		
		}
	
	return result;
	
	}

/*****************************************************************************/

dng_shared * dng_host::Make_dng_shared ()
	{
	
	dng_shared *result = new dng_shared ();
	
	if (!result)
		{
		
		ThrowMemoryFull ();

		}
	
	return result;
	
	}

/*****************************************************************************/

dng_ifd * dng_host::Make_dng_ifd ()
	{
	
	dng_ifd *result = new dng_ifd ();
	
	if (!result)
		{
		
		ThrowMemoryFull ();

		}
	
	return result;
	
	}

/*****************************************************************************/

dng_negative * dng_host::Make_dng_negative ()
	{
	
	return dng_negative::Make (*this);
	
	}

/*****************************************************************************/

dng_image * dng_host::Make_dng_image (const dng_rect &bounds,
									  uint32 planes,
									  uint32 pixelType)
	{
	
	dng_image *result = new dng_simple_image (bounds,
											  planes,
											  pixelType,
											  Allocator ());
	
	if (!result)
		{
		
		ThrowMemoryFull ();

		}
	
	return result;
	
	}
		
/*****************************************************************************/

dng_opcode * dng_host::Make_dng_opcode (uint32 opcodeID,
										dng_stream &stream)
	{
	
	dng_opcode *result = NULL;
	
	switch (opcodeID)
		{
		
		case dngOpcode_WarpRectilinear:
			{
			
			result = new dng_opcode_WarpRectilinear (stream);
			
			break;
			
			}
			
		case dngOpcode_WarpFisheye:
			{
			
			result = new dng_opcode_WarpFisheye (stream);
			
			break;
			
			}
			
		case dngOpcode_FixVignetteRadial:
			{
			
			result = new dng_opcode_FixVignetteRadial (stream);
			
			break;
			
			}
			
		case dngOpcode_FixBadPixelsConstant:
			{
			
			result = new dng_opcode_FixBadPixelsConstant (stream);
			
			break;
			
			}
			
		case dngOpcode_FixBadPixelsList:
			{
			
			result = new dng_opcode_FixBadPixelsList (stream);
			
			break;
			
			}

		case dngOpcode_TrimBounds:
			{
			
			result = new dng_opcode_TrimBounds (stream);
			
			break;
			
			}
			
		case dngOpcode_MapTable:
			{
			
			result = new dng_opcode_MapTable (*this,
											  stream);
			
			break;
			
			}

		case dngOpcode_MapPolynomial:
			{
			
			result = new dng_opcode_MapPolynomial (stream);
			
			break;
			
			}

		case dngOpcode_GainMap:
			{
			
			result = new dng_opcode_GainMap (*this,
											 stream);
			
			break;
			
			}
			
		case dngOpcode_DeltaPerRow:
			{
			
			result = new dng_opcode_DeltaPerRow (*this,
											     stream);
			
			break;
			
			}
			
		case dngOpcode_DeltaPerColumn:
			{
			
			result = new dng_opcode_DeltaPerColumn (*this,
											        stream);
			
			break;
			
			}
			
		case dngOpcode_ScalePerRow:
			{
			
			result = new dng_opcode_ScalePerRow (*this,
											     stream);
			
			break;
			
			}
			
		case dngOpcode_ScalePerColumn:
			{
			
			result = new dng_opcode_ScalePerColumn (*this,
											        stream);
			
			break;
			
			}
			
		default:
			{
			
			result = new dng_opcode_Unknown (*this,
											 opcodeID,
											 stream);
			
			}
		
		}

	if (!result)
		{
		
		ThrowMemoryFull ();

		}
	
	return result;
	
	}
		
/*****************************************************************************/

void dng_host::ApplyOpcodeList (dng_opcode_list &list,
								dng_negative &negative,
								AutoPtr<dng_image> &image)
	{
	
	list.Apply (*this,
				negative,
				image);
	
	}
		
/*****************************************************************************/

void dng_host::ResampleImage (const dng_image &srcImage,
							  dng_image &dstImage)
	{
	
	::ResampleImage (*this,
					 srcImage,
					 dstImage,
					 srcImage.Bounds (),
					 dstImage.Bounds (),
					 dng_resample_bicubic::Get ());
	
	}

/*****************************************************************************/
