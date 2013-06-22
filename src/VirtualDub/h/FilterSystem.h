//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2001 Avery Lee
//
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version.
//
//	This program is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with this program; if not, write to the Free Software
//	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#ifndef f_VIRTUALDUB_FILTERSYSTEM_H
#define f_VIRTUALDUB_FILTERSYSTEM_H

#ifdef _MSC_VER
	#pragma once
#endif

#include <vd2/system/list.h>
#include <vd2/system/fraction.h>
#include <vd2/system/refcount.h>
#include "filter.h"

class FilterInstance;
class VDXFilterStateInfo;
class FilterSystemBitmap;
class VFBitmapInternal;
struct VDPixmap;
struct VDPixmapLayout;
class IVDFilterFrameSource;
class IVDFilterFrameClientRequest;
class VDFilterFrameRequest;

class IVDFilterSystemScheduler : public IVDRefCount {
public:
	virtual void Reschedule() = 0;
	virtual bool Block() = 0;
};

class FilterSystem {
	FilterSystem(const FilterSystem&);
	FilterSystem& operator=(const FilterSystem&);
public:
	FilterSystem();
	~FilterSystem();

	void SetAccelEnabled(bool enable);
	void SetVisualAccelDebugEnabled(bool enable);

	void prepareLinearChain(List *listFA, uint32 src_width, uint32 src_height, int src_format, const VDFraction& sourceFrameRate, sint64 sourceFrameCount, const VDFraction& sourcePixelAspect);
	void initLinearChain(IVDFilterSystemScheduler *scheduler, uint32 filterStateFlags, List *listFA, IVDFilterFrameSource *src, uint32 src_width, uint32 src_height, int src_format, const uint32 *palette, const VDFraction& sourceFrameRate, sint64 sourceFrameCount, const VDFraction& sourcePixelAspect);
	void ReadyFilters();

	bool RequestFrame(sint64 outputFrame, IVDFilterFrameClientRequest **creq);

	enum RunResult {
		kRunResult_Idle,		// All filters are idle.
		kRunResult_Running,		// There are still filters to run, and some can be run on this thread.
		kRunResult_Blocked		// There are still filters to run, but all are waiting for asynchronous operation.
	};

	RunResult Run(bool runToCompletion);

	void InvalidateCachedFrames(FilterInstance *startingFilter);

	void DeinitFilters();
	void DeallocateBuffers();
	const VDPixmapLayout& GetInputLayout() const;
	const VDPixmapLayout& GetOutputLayout() const;
	bool isRunning() const;
	bool isEmpty() const;

	bool GetDirectFrameMapping(VDPosition outputFrame, VDPosition& sourceFrame, int& sourceIndex) const;
	sint64	GetSourceFrame(sint64 outframe) const;
	sint64	GetSymbolicFrame(sint64 outframe, IVDFilterFrameSource *source) const;
	sint64	GetNearestUniqueFrame(sint64 outframe) const;

	const VDFraction GetOutputFrameRate() const;
	const VDFraction GetOutputPixelAspect() const;
	sint64	GetOutputFrameCount() const;

private:
	void AllocateVBitmaps(int count);
	void AllocateBuffers(uint32 lTotalBufferNeeded);

	struct Bitmaps;

	bool	mbFiltersInited;
	bool	mbFiltersError;
	bool	mbFiltersUseAcceleration;
	bool	mbAccelDebugVisual;
	bool	mbAccelEnabled;

	VDFraction	mOutputFrameRate;
	VDFraction	mOutputPixelAspect;
	sint64		mOutputFrameCount;

	Bitmaps *mpBitmaps;

	unsigned char *lpBuffer;
	long lRequiredSize;
	uint32	mFilterStateFlags;

	typedef vdfastvector<IVDFilterFrameSource *> Filters;
	Filters mFilters;
	Filters mActiveFilters;

	uint32	mPalette[256];
};

#endif
