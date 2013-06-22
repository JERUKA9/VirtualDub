//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2003 Avery Lee
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

#include "stdafx.h"

#define f_DUB_CPP


#include <process.h>
#include <time.h>
#include <vector>
#include <deque>
#include <utility>

#include <windows.h>
#include <vfw.h>

#include "resource.h"

#include "crash.h"
#include <vd2/system/thread.h>
#include <vd2/system/tls.h>
#include <vd2/system/time.h>
#include <vd2/system/atomic.h>
#include <vd2/system/fraction.h>
#include <vd2/system/vdalloc.h>
#include <vd2/system/VDRingBuffer.h>
#include <vd2/system/profile.h>
#include <vd2/system/protscope.h>
#include <vd2/system/w32assist.h>
#include <vd2/Dita/resources.h>
#include <vd2/Kasumi/pixmap.h>
#include <vd2/Kasumi/pixmapops.h>
#include <vd2/Kasumi/pixmaputils.h>
#include <vd2/Riza/bitmap.h>
#include <vd2/Riza/display.h>
#include <vd2/Riza/videocodec.h>
#include "AudioFilterSystem.h"
#include "convert.h"
#include "filters.h"
#include "gui.h"
#include "prefs.h"
#include "command.h"
#include "misc.h"
#include "timeline.h"

#include <vd2/system/error.h>
#include "AsyncBlitter.h"
#include "AVIOutputPreview.h"
#include "AVIOutput.h"
#include "AVIOutputWAV.h"
#include "AVIOutputImages.h"
#include "AVIOutputStriped.h"
#include "AudioSource.h"
#include "VideoSource.h"
#include "AVIPipe.h"
#include "VBitmap.h"
#include "FrameSubset.h"
#include "InputFile.h"
#include "VideoTelecineRemover.h"

#include "Dub.h"
#include "DubOutput.h"
#include "DubStatus.h"
#include "DubUtils.h"
#include "DubIO.h"
#include "DubProcess.h"

using namespace nsVDDub;

/// HACK!!!!
#define vSrc mVideoSources[0]
#define aSrc mAudioSources[0]


///////////////////////////////////////////////////////////////////////////

extern const char g_szError[];
extern HWND g_hWnd;
extern bool g_fWine;
extern uint32& VDPreferencesGetRenderVideoBufferCount();
///////////////////////////////////////////////////////////////////////////

namespace {
	enum { kVDST_Dub = 1 };

	enum {
		kVDM_SegmentOverflowOccurred,
		kVDM_BeginningNextSegment,
		kVDM_IOThreadLivelock,
		kVDM_ProcessingThreadLivelock,
		kVDM_CodecDelayedDuringDelayedFlush,
		kVDM_CodecLoopingDuringDelayedFlush,
		kVDM_FastRecompressUsingFormat,
		kVDM_SlowRecompressUsingFormat,
		kVDM_FullUsingInputFormat,
		kVDM_FullUsingOutputFormat
	};

	enum {
		kLiveLockMessageLimit = 5
	};
};

///////////////////////////////////////////////////////////////////////////

DubOptions g_dubOpts = {
	{
		-1.0f,			// no amp
		500,			// preload by 500ms
		1,				// every frame
		0,				// no new rate
		0,				// offset: 0ms
		false,			// period is in frames
		true,			// audio interleaving enabled
		true,			// yes, offset audio with video
		true,			// yes, clip audio to video length
		false,			// no high quality
		false,			// use fixed-function audio pipeline
		DubAudioOptions::P_NOCHANGE,		// no precision change
		DubAudioOptions::C_NOCHANGE,		// no channel change
		DubAudioOptions::M_NONE,
	},

	{
		0,								// input: autodetect
		nsVDPixmap::kPixFormat_RGB888,	// output: 24bit
		DubVideoOptions::M_FULL,	// mode: full
		false,						// use smart encoding
		false,						// preserve empty frames
		0,							// max video compression threads
		true,						// show input video
		true,						// show output video
		false,						// decompress output video before display
		true,						// sync to audio
		1,							// no frame rate decimation
		0,0,						// no target
		0,0,						// no change in frame rate
		0,							// start offset: 0ms
		0,							// end offset: 0ms
		false,						// No inverse telecine
		false,						// (IVTC mode)
		-1,							// (IVTC offset)
		false,						// (IVTC polarity)
		DubVideoOptions::kPreviewFieldsProgressive,	// progressive preview
	},

	{
		true,					// dynamic enable
		false,
		false,					// directdraw,
		true,					// drop frames
	},

	true,			// show status
	false,			// move slider
	100,			// run at 100%
};

static const int g_iPriorities[][2]={

	// I/O							processor
	{ THREAD_PRIORITY_IDLE,			THREAD_PRIORITY_IDLE,			},
	{ THREAD_PRIORITY_LOWEST,		THREAD_PRIORITY_LOWEST,			},
	{ THREAD_PRIORITY_BELOW_NORMAL,	THREAD_PRIORITY_LOWEST,			},
	{ THREAD_PRIORITY_NORMAL,		THREAD_PRIORITY_BELOW_NORMAL,	},
	{ THREAD_PRIORITY_NORMAL,		THREAD_PRIORITY_NORMAL,			},
	{ THREAD_PRIORITY_ABOVE_NORMAL,	THREAD_PRIORITY_NORMAL,			},
	{ THREAD_PRIORITY_HIGHEST,		THREAD_PRIORITY_ABOVE_NORMAL,	},
	{ THREAD_PRIORITY_HIGHEST,		THREAD_PRIORITY_HIGHEST,		}
};

/////////////////////////////////////////////////
void AVISTREAMINFOtoAVIStreamHeader(AVIStreamHeader_fixed *dest, const VDAVIStreamInfo *src) {
	dest->fccType			= src->fccType;
	dest->fccHandler		= src->fccHandler;
	dest->dwFlags			= src->dwFlags;
	dest->wPriority			= src->wPriority;
	dest->wLanguage			= src->wLanguage;
	dest->dwInitialFrames	= src->dwInitialFrames;
	dest->dwStart			= src->dwStart;
	dest->dwScale			= src->dwScale;
	dest->dwRate			= src->dwRate;
	dest->dwLength			= src->dwLength;
	dest->dwSuggestedBufferSize = src->dwSuggestedBufferSize;
	dest->dwQuality			= src->dwQuality;
	dest->dwSampleSize		= src->dwSampleSize;
	dest->rcFrame.left		= (short)src->rcFrameLeft;
	dest->rcFrame.top		= (short)src->rcFrameTop;
	dest->rcFrame.right		= (short)src->rcFrameRight;
	dest->rcFrame.bottom	= (short)src->rcFrameBottom;
}

///////////////////////////////////////////////////////////////////////////

namespace {
	bool CheckFormatSizeCompatibility(int format, int w, int h) {
		const VDPixmapFormatInfo& formatInfo = VDPixmapGetInfo(format);

		if ((w & ((1<<formatInfo.qwbits)-1))
			|| (h & ((1<<formatInfo.qhbits)-1))
			|| (w & ((1<<formatInfo.auxwbits)-1))
			|| (h & ((1<<formatInfo.auxhbits)-1))
			)
		{
			return false;
		}

		return true;
	}

	int DegradeFormat(int format, uint32& rgbMask) {
		using namespace nsVDPixmap;

		rgbMask |= (1 << format);

		switch(format) {
		case kPixFormat_YUV410_Planar:	format = kPixFormat_YUV420_Planar; break;
		case kPixFormat_YUV411_Planar:	format = kPixFormat_YUV422_YUYV; break;
		case kPixFormat_YUV420_Planar:	format = kPixFormat_YUV422_YUYV; break;
		case kPixFormat_YUV422_Planar:	format = kPixFormat_YUV422_YUYV; break;
		case kPixFormat_YUV444_Planar:	format = kPixFormat_YUV422_YUYV; break;
		case kPixFormat_YUV422_YUYV:	format = kPixFormat_RGB888; break;
		case kPixFormat_YUV422_UYVY:	format = kPixFormat_RGB888; break;
		case kPixFormat_Y8:				format = kPixFormat_RGB888; break;

		// RGB formats are a bit tricky, as we must always be sure to try the
		// three major formats: 8888, 1555, 888.  The possible chains:
		//
		// 8888 -> 888 -> 1555 -> Pal8
		// 888 -> 8888 -> 1555 -> Pal8
		// 565 -> 8888 -> 888 -> 1555 -> Pal8
		// 1555 -> 8888 -> 888 -> Pal8

		case kPixFormat_RGB888:
			if (!(rgbMask & (1 << kPixFormat_XRGB8888)))
				format = kPixFormat_XRGB8888;
			else if (!(rgbMask & (1 << kPixFormat_XRGB1555)))
				format = kPixFormat_XRGB1555;
			else
				format = kPixFormat_Pal8;
			break;

		case kPixFormat_XRGB8888:
			if (rgbMask & (1 << kPixFormat_RGB888))
				format = kPixFormat_XRGB1555;
			else
				format = kPixFormat_RGB888;
			break;

		case kPixFormat_RGB565:
			format = kPixFormat_XRGB8888;
			break;

		case kPixFormat_XRGB1555:
			if (!(rgbMask & (1 << kPixFormat_XRGB8888)))
				format = kPixFormat_XRGB8888;
			else
				format = kPixFormat_Pal8;
			break;

		default:
			format = kPixFormat_Null;
			break;
		};

		if (rgbMask & (1 << format))
			format = kPixFormat_Null;

		return format;
	}
}

int VDRenderSetVideoSourceInputFormat(IVDVideoSource *vsrc, int format) {
	uint32 rgbTrackMask = 0;

	do {
		if (vsrc->setTargetFormat(format))
			return vsrc->getTargetFormat().format;

		format = DegradeFormat(format, rgbTrackMask);
	} while(format);

	return format;
}

///////////////////////////////////////////////////////////////////////////
//
//	Dubber
//
///////////////////////////////////////////////////////////////////////////

class Dubber : public IDubber, public IDubberInternal {
private:
	MyError				err;
	bool				fError;

	VDAtomicInt			mStopLock;

	DubOptions			*opt;

	typedef vdfastvector<AudioSource *> AudioSources;
	AudioSources		mAudioSources;

	typedef vdfastvector<IVDVideoSource *> VideoSources;
	VideoSources		mVideoSources;

	IVDDubberOutputSystem	*mpOutputSystem;
	COMPVARS			*compVars;

	DubAudioStreamInfo	aInfo;
	DubVideoStreamInfo	vInfo;

	bool				mbDoVideo;
	bool				mbDoAudio;
	bool				fPreview;
	bool				mbCompleted;
	VDAtomicInt			mbAbort;
	VDAtomicInt			mbUserAbort;
	bool				fADecompressionOk;
	bool				fVDecompressionOk;

	int					mLiveLockMessages;

	VDDubIOThread		*mpIOThread;
	VDDubProcessThread	mProcessThread;
	VDAtomicInt			mIOThreadCounter;

	vdautoptr<IVDVideoCompressor>	mpVideoCompressor;

	AVIPipe *			mpVideoPipe;
	VDAudioPipeline		mAudioPipe;

	IVDVideoDisplay *	mpInputDisplay;
	IVDVideoDisplay *	mpOutputDisplay;
	bool				mbInputDisplayInitialized;

	vdstructex<VDAVIBitmapInfoHeader>	mpCompressorVideoFormat;

	std::vector<AudioStream *>	mAudioStreams;
	AudioStream			*audioStream;
	AudioStream			*audioStatusStream;
	AudioStreamL3Corrector	*audioCorrector;
	vdautoptr<VDAudioFilterGraph> mpAudioFilterGraph;

	const FrameSubset		*inputSubsetActive;
	FrameSubset				*inputSubsetAlloc;
	VideoTelecineRemover	*pInvTelecine;

	vdstructex<WAVEFORMATEX> mAudioCompressionFormat;
	VDStringA			mAudioCompressionFormatHint;

	VDPixmapLayout		mVideoFilterOutputPixmapLayout;

	bool				fPhantom;

	IDubStatusHandler	*pStatusHandler;

	long				lVideoSizeEstimate;

	// interleaving
	VDStreamInterleaver		mInterleaver;
	VDRenderFrameMap		mVideoFrameMap;
	VDRenderFrameIterator	mVideoFrameIterator;

	///////

	int					mLastProcessingThreadCounter;
	int					mProcessingThreadFailCount;
	int					mLastIOThreadCounter;
	int					mIOThreadFailCount;

	///////

	VDEvent<IDubber, bool>	mStoppedEvent;

public:
	Dubber(DubOptions *);
	~Dubber();

	void SetAudioCompression(const VDWaveFormat *wf, uint32 cb, const char *pShortNameHint);
	void SetPhantomVideoMode();
	void SetInputDisplay(IVDVideoDisplay *);
	void SetOutputDisplay(IVDVideoDisplay *);
	void SetAudioFilterGraph(const VDAudioFilterGraph& graph);

	void InitAudioConversionChain();
	void InitOutputFile();
	bool AttemptInputOverlays();

	void InitDirectDraw();
	bool NegotiateFastFormat(const BITMAPINFOHEADER& bih);
	bool NegotiateFastFormat(int format);
	void InitSelectInputFormat();
	void Init(IVDVideoSource *const *pVideoSources, uint32 nVideoSources, AudioSource *const *pAudioSources, uint32 nAudioSources, IVDDubberOutputSystem *outsys, void *videoCompVars, const FrameSubset *);
	void Go(int iPriority = 0);
	void Stop();

	void InternalSignalStop();
	void Abort(bool userAbort);
	void ForceAbort();
	bool isRunning();
	bool IsAborted();
	bool isAbortedByUser();
	bool IsPreviewing();

	void SetStatusHandler(IDubStatusHandler *pdsh);
	void SetPriority(int index);
	void UpdateFrames();
	void SetThrottleFactor(float throttleFactor);

	VDEvent<IDubber, bool>& Stopped() { return mStoppedEvent; }
};


///////////////////////////////////////////////////////////////////////////

IDubber::~IDubber() {
}

IDubber *CreateDubber(DubOptions *xopt) {
	return new Dubber(xopt);
}

Dubber::Dubber(DubOptions *xopt)
	: mpIOThread(0)
	, mIOThreadCounter(0)
	, mpAudioFilterGraph(NULL)
	, mStopLock(0)
	, mpVideoPipe(NULL)
	, mVideoFrameIterator(mVideoFrameMap)
	, mLastProcessingThreadCounter(0)
	, mProcessingThreadFailCount(0)
	, mLastIOThreadCounter(0)
	, mIOThreadFailCount(0)
{
	opt				= xopt;

	// clear the workin' variables...

	fError				= false;

	mbAbort				= false;
	mbUserAbort			= false;

	pStatusHandler		= NULL;

	fADecompressionOk	= false;
	fVDecompressionOk	= false;

	mpInputDisplay		= NULL;
	mpOutputDisplay		= NULL;
	vInfo.total_size	= 0;
	aInfo.total_size	= 0;
	vInfo.fAudioOnly	= false;

	audioStream			= NULL;
	audioStatusStream	= NULL;
	audioCorrector		= NULL;

	inputSubsetActive	= NULL;
	inputSubsetAlloc	= NULL;

	mbCompleted			= false;
	fPhantom = false;

	pInvTelecine		= NULL;

	mLiveLockMessages = 0;
}

Dubber::~Dubber() {
	Stop();
}

/////////////////////////////////////////////////

void Dubber::SetAudioCompression(const VDWaveFormat *wf, uint32 cb, const char *pShortNameHint) {
	mAudioCompressionFormat.assign((const WAVEFORMATEX *)wf, cb);
	if (pShortNameHint)
		mAudioCompressionFormatHint = pShortNameHint;
	else
		mAudioCompressionFormatHint.clear();
}

void Dubber::SetPhantomVideoMode() {
	fPhantom = true;
	vInfo.fAudioOnly = true;
}

void Dubber::SetStatusHandler(IDubStatusHandler *pdsh) {
	pStatusHandler = pdsh;
}


/////////////

void Dubber::SetInputDisplay(IVDVideoDisplay *pDisplay) {
	mpInputDisplay = pDisplay;
}

void Dubber::SetOutputDisplay(IVDVideoDisplay *pDisplay) {
	mpOutputDisplay = pDisplay;
}

/////////////

void Dubber::SetAudioFilterGraph(const VDAudioFilterGraph& graph) {
	mpAudioFilterGraph = new VDAudioFilterGraph(graph);
}

void VDConvertSelectionTimesToFrames(const DubOptions& opt, const FrameSubset& subset, const VDFraction& subsetRate, VDPosition& startFrame, VDPosition& endFrame) {
	startFrame = 0;
	if (opt.video.lStartOffsetMS)
		startFrame = VDRoundToInt64(subsetRate.asDouble() * (double)opt.video.lStartOffsetMS / 1000.0);

	endFrame = subset.getTotalFrames();;
	if (opt.video.lEndOffsetMS) {
		endFrame -= VDRoundToInt64(subsetRate.asDouble() * (double)opt.video.lEndOffsetMS / 1000.0);
		if (endFrame < 0)
			endFrame = 0;
	}
}

void VDTranslateSubsetDirectMode(FrameSubset& dst, const FrameSubset& src, IVDVideoSource *const *pVideoSources, VDPosition& selectionStart, VDPosition& selectionEnd) {
	bool selectionStartFixed = false;
	bool selectionEndFixed = false;
	VDPosition srcEnd = 0;
	VDPosition dstEnd = 0;

	for(FrameSubset::const_iterator it(src.begin()), itEnd(src.end()); it != itEnd; ++it) {
		const FrameSubsetNode& srcRange = *it;
		sint64 start = srcRange.start;
		int srcIndex = srcRange.source;

		IVDVideoSource *src = NULL;
		VDPosition srcStart;
		if (srcIndex >= 0) {
			src = pVideoSources[srcIndex];
			srcStart = src->asStream()->getStart();

			start = src->nearestKey(start + srcStart);
			if (start < 0)
				start = 0;
			else
				start -= srcStart;
		}

		srcEnd += srcRange.len;
		FrameSubset::iterator itNew(dst.addRange(srcRange.start, srcRange.len, srcRange.bMask, srcRange.source));
		dstEnd += itNew->len;

		// Mask ranges never need to be extended backwards, because they don't hold any
		// data of their own.  If an include range needs to be extended backwards, though,
		// it may need to extend into a previous merge range.  To avoid this problem,
		// we do a delete of the range before adding the tail.

		if (!itNew->bMask) {
			if (start < itNew->start) {
				FrameSubset::iterator it2(itNew);

				while(it2 != dst.begin()) {
					--it2;

					sint64 prevtail = it2->start + it2->len;

					// check for overlap
					if (prevtail < start || prevtail > itNew->start + itNew->len)
						break;

					if (it2->start >= start || !it2->bMask) {	// within extension range: absorb
						sint64 offset = itNew->start - it2->start;
						dstEnd += offset;
						dstEnd -= it2->len;
						itNew->len += offset;
						itNew->start = it2->start;
						it2 = dst.erase(it2);
					} else {									// before extension range and masked: split merge
						sint64 offset = start - itNew->start;
						it2->len -= offset;
						itNew->start -= offset;
						itNew->len += offset;
						break;
					}
				}

				sint64 left = itNew->start - start;
				
				if (left > 0) {
					itNew->start = start;
					itNew->len += left;
					dstEnd += left;
				}
			}
		}

		VDASSERT(dstEnd == dst.getTotalFrames());

		// Check whether one of the selection pointers needs to be updated.
		if (!selectionStartFixed && selectionStart < srcEnd) {
			sint64 frame = (selectionStart - (srcEnd - srcRange.len)) + srcRange.start;

			if (src) {
				frame = src->nearestKey(srcStart + frame);
				if (frame < 0)
					frame = 0;
				else
					frame -= srcStart;
			}

			selectionStart = (dstEnd - itNew->len) + (frame - itNew->start);
			selectionStartFixed = true;
		}

		if (!selectionEndFixed && selectionEnd < srcEnd) {
			selectionEnd += dstEnd - srcEnd;
			selectionEndFixed = true;
		}
	}

	if (!selectionStartFixed)
		selectionStart = dstEnd;

	if (!selectionEndFixed)
		selectionEnd = dstEnd;
}

void InitVideoStreamValuesStatic(DubVideoStreamInfo& vInfo, IVDVideoSource *video, AudioSource *audio, const DubOptions *opt, const FrameSubset *pfs, const VDPosition *pSelectionStartFrame, const VDPosition *pSelectionEndFrame) {
	vInfo.start_src		= 0;
	vInfo.end_src		= 0;
	vInfo.cur_dst		= 0;
	vInfo.end_dst		= 0;
	vInfo.cur_proc_dst	= 0;
	vInfo.end_proc_dst	= 0;
	vInfo.cur_proc_src = -1;

	if (!video)
		return;

	IVDStreamSource *pVideoStream = video->asStream();

	vInfo.start_src		= 0;
	vInfo.end_src		= pfs->getTotalFrames();

	if (pSelectionStartFrame && *pSelectionStartFrame >= vInfo.start_src)
		vInfo.start_src = *pSelectionStartFrame;

	if (pSelectionEndFrame && *pSelectionEndFrame <= vInfo.end_src)
		vInfo.end_src = *pSelectionEndFrame;

	if (vInfo.end_src < vInfo.start_src)
		vInfo.end_src = vInfo.start_src;

	// compute new frame rate

	VDFraction framerate(pVideoStream->getRate());

	if (opt->video.mFrameRateAdjustLo == 0) {
		if (opt->video.mFrameRateAdjustHi == DubVideoOptions::kFrameRateAdjustSameLength) {
			if (audio && audio->getLength())
				framerate = VDFraction((double)pVideoStream->getLength() * audio->getRate().asDouble() / audio->getLength());
		}
	} else
		framerate = VDFraction(opt->video.mFrameRateAdjustHi, opt->video.mFrameRateAdjustLo);

	vInfo.frameRateIn	= framerate;

	if (opt->video.frameRateDecimation==1 && opt->video.frameRateTargetLo)
		vInfo.frameRate	= VDFraction(opt->video.frameRateTargetHi, opt->video.frameRateTargetLo);
	else
		vInfo.frameRate	= framerate / opt->video.frameRateDecimation;

	if (vInfo.end_src <= vInfo.start_src)
		vInfo.end_dst = 0;
	else
		vInfo.end_dst		= (long)(vInfo.frameRate / vInfo.frameRateIn).scale64t(vInfo.end_src - vInfo.start_src);

	vInfo.end_proc_dst	= vInfo.end_dst;
}

void InitAudioStreamValuesStatic(DubAudioStreamInfo& aInfo, AudioSource *audio, const DubOptions *opt) {
	aInfo.start_src		= 0;

	if (!audio)
		return;

	aInfo.start_src		= audio->getStart();

	// offset the start of the audio appropriately...
	aInfo.start_us = -(sint64)1000*opt->audio.offset;
	aInfo.start_src += audio->TimeToPositionVBR(aInfo.start_us);

	// resampling audio?

	aInfo.resampling = false;
	aInfo.converting = false;

	if (opt->audio.mode > DubAudioOptions::M_NONE) {
		if (opt->audio.new_rate) {
			aInfo.resampling = true;
		}

		if (opt->audio.newPrecision != DubAudioOptions::P_NOCHANGE || opt->audio.newChannels != DubAudioOptions::C_NOCHANGE) {
			aInfo.converting = true;

			aInfo.is_16bit = opt->audio.newPrecision==DubAudioOptions::P_16BIT
							|| (opt->audio.newPrecision==DubAudioOptions::P_NOCHANGE && audio->getWaveFormat()->mSampleBits>8);
			aInfo.is_stereo = opt->audio.newChannels==DubAudioOptions::C_STEREO
							|| (opt->audio.newChannels==DubAudioOptions::C_NOCHANGE && audio->getWaveFormat()->mChannels>1);
			aInfo.is_right = (opt->audio.newChannels==DubAudioOptions::C_MONORIGHT);
			aInfo.single_channel = (opt->audio.newChannels==DubAudioOptions::C_MONOLEFT || opt->audio.newChannels==DubAudioOptions::C_MONORIGHT);
		}
	}
}

//////////////////////////////////////////////////////////////////////////////

// may be called at any time in Init() after streams setup

void Dubber::InitAudioConversionChain() {

	// ready the audio stream for streaming operation

	aSrc->streamBegin(fPreview, false);
	fADecompressionOk = true;

	// Initialize audio conversion chain

	bool bUseAudioFilterGraph = (opt->audio.mode > DubAudioOptions::M_NONE && mpAudioFilterGraph);

	uint32 audioSourceCount = mAudioSources.size();
	vdfastvector<AudioStream *> sourceStreams(audioSourceCount);

	for(uint32 i=0; i<audioSourceCount; ++i) {
		AudioSource *asrc = mAudioSources[i];

		if (bUseAudioFilterGraph) {
			audioStream = new_nothrow AudioFilterSystemStream(*mpAudioFilterGraph, aInfo.start_us);
			if (!audioStream)
				throw MyMemoryError();

			mAudioStreams.push_back(audioStream);
		} else {
			// First, create a source.

			if (!(audioStream = new_nothrow AudioStreamSource(asrc, asrc->getEnd() - aInfo.start_src, opt->audio.mode > DubAudioOptions::M_NONE, aInfo.start_us)))
				throw MyMemoryError();

			mAudioStreams.push_back(audioStream);
		}

		// check the stream format and coerce to first stream if necessary
		if (i > 0) {
			const VDWaveFormat *format1 = sourceStreams[0]->GetFormat();
			const VDWaveFormat *format2 = audioStream->GetFormat();

			if (format1->mChannels != format2->mChannels || format1->mSampleBits != format2->mSampleBits) {
				audioStream = new_nothrow AudioStreamConverter(audioStream, format1->mSampleBits > 8, format1->mChannels > 1, false);
				mAudioStreams.push_back(audioStream);
			}

			if (format1->mSamplingRate != format2->mSamplingRate) {
				audioStream = new_nothrow AudioStreamResampler(audioStream, format1->mSamplingRate, true);
				mAudioStreams.push_back(audioStream);
			}
		}

		sourceStreams[i] = audioStream;
	}

	// Tack on a subset filter as well...
	sint64 offset = 0;
	
	if (opt->audio.fStartAudio)
		offset = vInfo.frameRate.scale64ir((sint64)1000000 * vInfo.start_src);

	bool applyTail = false;

	if (!opt->audio.fEndAudio && (inputSubsetActive->empty() || inputSubsetActive->back().end() >= vSrc->asStream()->getEnd()))
		applyTail = true;

	if (!(audioStream = new_nothrow AudioSubset(sourceStreams, inputSubsetActive, vInfo.frameRateIn, offset, applyTail)))
		throw MyMemoryError();

	mAudioStreams.push_back(audioStream);

	if (!bUseAudioFilterGraph) {
		// Attach a converter if we need to...

		if (aInfo.converting) {
			bool is_16bit = aInfo.is_16bit;

			// fix precision guess based on actual stream output if we are not changing it
			if (opt->audio.newPrecision == DubAudioOptions::P_NOCHANGE)
				is_16bit = audioStream->GetFormat()->mSampleBits > 8;

			if (aInfo.single_channel)
				audioStream = new_nothrow AudioStreamConverter(audioStream, is_16bit, aInfo.is_right, true);
			else
				audioStream = new_nothrow AudioStreamConverter(audioStream, is_16bit, aInfo.is_stereo, false);

			if (!audioStream)
				throw MyMemoryError();

			mAudioStreams.push_back(audioStream);
		}

		// Attach a converter if we need to...

		if (aInfo.resampling) {
			if (!(audioStream = new_nothrow AudioStreamResampler(audioStream, opt->audio.new_rate ? opt->audio.new_rate : aSrc->getWaveFormat()->mSamplingRate, opt->audio.fHighQuality)))
				throw MyMemoryError();

			mAudioStreams.push_back(audioStream);
		}

		// Attach an amplifier if needed...

		if (opt->audio.mode > DubAudioOptions::M_NONE && opt->audio.mVolume >= 0) {
			if (!(audioStream = new_nothrow AudioStreamAmplifier(audioStream, opt->audio.mVolume)))
				throw MyMemoryError();

			mAudioStreams.push_back(audioStream);
		}
	}

	// Make sure we only get what we want...

	if (!mVideoSources.empty() && opt->audio.fEndAudio) {
		const sint64 nFrames = (sint64)(vInfo.end_src - vInfo.start_src);
		const VDFraction& audioRate = audioStream->GetSampleRate();
		const VDFraction audioPerVideo(audioRate / vInfo.frameRateIn);

		audioStream->SetLimit(audioPerVideo.scale64r(nFrames));
	}

	audioStatusStream = audioStream;

	// Tack on a compressor if we want...

	AudioCompressor *pCompressor = NULL;

	if (opt->audio.mode > DubAudioOptions::M_NONE && !mAudioCompressionFormat.empty()) {
		if (!(pCompressor = new_nothrow AudioCompressor(audioStream, (const VDWaveFormat *)&*mAudioCompressionFormat, mAudioCompressionFormat.size(), mAudioCompressionFormatHint.c_str())))
			throw MyMemoryError();

		audioStream = pCompressor;
		mAudioStreams.push_back(audioStream);
	}

	// Check the output format, and if we're compressing to
	// MPEG Layer III, compensate for the lag and create a bitrate corrector

	if (!g_prefs.fNoCorrectLayer3 && pCompressor && pCompressor->GetFormat()->mTag == WAVE_FORMAT_MPEGLAYER3) {
		pCompressor->CompensateForMP3();

		if (!(audioCorrector = new_nothrow AudioStreamL3Corrector(audioStream)))
			throw MyMemoryError();

		audioStream = audioCorrector;
		mAudioStreams.push_back(audioStream);
	}

}

void Dubber::InitOutputFile() {

	// Do audio.

	if (mbDoAudio) {
		// initialize AVI parameters...

		AVIStreamHeader_fixed	hdr;

		AVISTREAMINFOtoAVIStreamHeader(&hdr, &aSrc->getStreamInfo());
		hdr.dwStart			= 0;
		hdr.dwInitialFrames	= opt->audio.preload ? 1 : 0;

		if (opt->audio.mode > DubAudioOptions::M_NONE) {
			const VDWaveFormat *outputAudioFormat = audioStream->GetFormat();
			hdr.dwSampleSize	= outputAudioFormat->mBlockSize;
			hdr.dwRate			= outputAudioFormat->mDataRate;
			hdr.dwScale			= outputAudioFormat->mBlockSize;
			hdr.dwLength		= MulDiv(hdr.dwLength, outputAudioFormat->mSamplingRate, aSrc->getWaveFormat()->mSamplingRate);
		}

		mpOutputSystem->SetAudio(hdr, audioStream->GetFormat(), audioStream->GetFormatLen(), opt->audio.enabled, audioStream->IsVBR());
	}

	// Do video.

	if (mbDoVideo) {
		VDPixmap output;
		
		if (opt->video.mode >= DubVideoOptions::M_FULL)
			output = filters.GetOutput();
		else
			output = vSrc->getTargetFormat();

		AVIStreamHeader_fixed hdr;

		AVISTREAMINFOtoAVIStreamHeader(&hdr, &vSrc->asStream()->getStreamInfo());

		hdr.dwSampleSize = 0;

		if (opt->video.mode > DubVideoOptions::M_NONE && !opt->video.mbUseSmartRendering) {
			if (mpVideoCompressor) {
				hdr.fccHandler	= compVars->fccHandler;
				hdr.dwQuality	= compVars->lQ;
			} else {
				hdr.fccHandler	= mmioFOURCC('D','I','B',' ');
			}
		}

		hdr.dwRate			= vInfo.frameRate.getHi();
		hdr.dwScale			= vInfo.frameRate.getLo();
		hdr.dwLength		= vInfo.end_dst >= 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (DWORD)vInfo.end_dst;

		hdr.rcFrame.left	= 0;
		hdr.rcFrame.top		= 0;
		hdr.rcFrame.right	= (short)output.w;
		hdr.rcFrame.bottom	= (short)output.h;

		// initialize compression

		int outputFormatID = opt->video.mOutputFormat;
		int outputVariantID = 0;

		if (!outputFormatID)
			outputFormatID = vSrc->getTargetFormat().format;

		if (opt->video.mode >= DubVideoOptions::M_FASTREPACK) {
			const VDAVIBitmapInfoHeader *pSrcFormat = vSrc->getDecompressedFormat();

			if (opt->video.mode <= DubVideoOptions::M_SLOWREPACK) {
				mpCompressorVideoFormat.assign(pSrcFormat, VDGetSizeOfBitmapHeaderW32((const BITMAPINFOHEADER *)pSrcFormat));
			} else {
				// try to find a variant that works
				const int variants = VDGetPixmapToBitmapVariants(outputFormatID);
				int variant;

				vdstructex<VDAVIBitmapInfoHeader> srcFormat;
				srcFormat.assign(pSrcFormat, VDGetSizeOfBitmapHeaderW32((const BITMAPINFOHEADER *)pSrcFormat));

				for(variant=1; variant <= variants; ++variant) {
					if (!VDMakeBitmapFormatFromPixmapFormat(mpCompressorVideoFormat, srcFormat, outputFormatID, variant, output.w, output.h))
						continue;

					bool result = true;
					
					if (mpVideoCompressor)
						result = mpVideoCompressor->Query((LPBITMAPINFO)&*mpCompressorVideoFormat, NULL);

					if (result) {
						outputVariantID = variant;
						break;
					}
				}

				if (variant > variants)
					throw MyError("Unable to initialize the output video codec. Check that the video codec is compatible with the output video frame size and that the settings are correct, or try a different one.");
			}
		} else {
			const VDAVIBitmapInfoHeader *pFormat = vSrc->getImageFormat();

			mpCompressorVideoFormat.assign(pFormat, vSrc->asStream()->getFormatLen());
		}

		// Initialize output compressor.
		vdstructex<VDAVIBitmapInfoHeader>	outputFormat;

		if (mpVideoCompressor) {
			vdstructex<BITMAPINFOHEADER> outputFormatW32;
			mpVideoCompressor->GetOutputFormat(&*mpCompressorVideoFormat, outputFormatW32);
			outputFormat.assign((const VDAVIBitmapInfoHeader *)outputFormatW32.data(), outputFormatW32.size());

			// If we are using smart rendering, we have no choice but to match the source format.
			if (opt->video.mbUseSmartRendering) {
				IVDStreamSource *vsrcStream = vSrc->asStream();
				const VDAVIBitmapInfoHeader *srcFormat = vSrc->getImageFormat();

				if (!mpVideoCompressor->Query(&*mpCompressorVideoFormat, srcFormat))
					throw MyError("Cannot initialize smart rendering: The selected video codec is able to compress the source video, but cannot match the same compressed format.");

				outputFormat.assign(srcFormat, vsrcStream->getFormatLen());
			}

			mpVideoCompressor->Start(&*mpCompressorVideoFormat, mpCompressorVideoFormat.size(), &*outputFormat, outputFormat.size(), vInfo.frameRate, vInfo.end_proc_dst);

			lVideoSizeEstimate = mpVideoCompressor->GetMaxOutputSize();
		} else {
			if (opt->video.mode < DubVideoOptions::M_FASTREPACK) {

				if (vSrc->getImageFormat()->biCompression == 0xFFFFFFFF)
					throw MyError("The source video stream uses a compression algorithm which is not compatible with AVI files. "
								"Direct stream copy cannot be used with this video stream.");

				IVDStreamSource *pVideoStream = vSrc->asStream();

				outputFormat.assign(vSrc->getImageFormat(), pVideoStream->getFormatLen());

				// cheese
				const VDPosition videoFrameStart	= pVideoStream->getStart();
				const VDPosition videoFrameEnd		= pVideoStream->getEnd();

				lVideoSizeEstimate = 0;
				for(VDPosition frame = videoFrameStart; frame < videoFrameEnd; ++frame) {
					uint32 bytes = 0;

					if (!pVideoStream->read(frame, 1, 0, 0, &bytes, 0))
						if (lVideoSizeEstimate < bytes)
							lVideoSizeEstimate = bytes;
				}
			} else {
				if (opt->video.mbUseSmartRendering) {
					throw MyError("Cannot initialize smart rendering: No video codec is selected for compression.");
				}

				if (opt->video.mode == DubVideoOptions::M_FULL) {
					VDMakeBitmapFormatFromPixmapFormat(outputFormat, mpCompressorVideoFormat, outputFormatID, outputVariantID);
				} else
					outputFormat = mpCompressorVideoFormat;

				lVideoSizeEstimate = outputFormat->biSizeImage;
				lVideoSizeEstimate = (lVideoSizeEstimate+1) & -2;
			}
		}

		mpOutputSystem->SetVideo(hdr, &*outputFormat, outputFormat.size());

		if(opt->video.mode >= DubVideoOptions::M_FULL) {
			const VDPixmapLayout& bmout = filters.GetOutputLayout();

			VDMakeBitmapCompatiblePixmapLayout(mVideoFilterOutputPixmapLayout, bmout.w, bmout.h, outputFormatID, outputVariantID, bmout.palette);

			const char *s = VDPixmapGetInfo(mVideoFilterOutputPixmapLayout.format).name;

			VDLogAppMessage(kVDLogInfo, kVDST_Dub, kVDM_FullUsingOutputFormat, 1, &s);
		}
	}
}

bool Dubber::AttemptInputOverlays() {
	static const int kFormats[]={
		nsVDPixmap::kPixFormat_YUV420_Planar,
		nsVDPixmap::kPixFormat_YUV422_UYVY,
		nsVDPixmap::kPixFormat_YUV422_YUYV
	};

	for(int i=0; i<sizeof(kFormats)/sizeof(kFormats[0]); ++i) {
		const int format = kFormats[i];

		VideoSources::const_iterator it(mVideoSources.begin()), itEnd(mVideoSources.end());
		for(; it!=itEnd; ++it) {
			IVDVideoSource *vs = *it;

			if (!vs->setTargetFormat(format))
				break;
		}

		if (it == itEnd) {
			if (mpInputDisplay->SetSource(false, mVideoSources.front()->getTargetFormat(), 0, 0, false, opt->video.previewFieldMode>0))
				return true;
		}
	}

	return false;
}

void Dubber::InitDirectDraw() {

	if (!opt->perf.useDirectDraw)
		return;

	// Should we try and establish a DirectDraw overlay?

	if (opt->video.mode == DubVideoOptions::M_SLOWREPACK) {
		if (AttemptInputOverlays())
			mbInputDisplayInitialized = true;
	}
}

bool Dubber::NegotiateFastFormat(const BITMAPINFOHEADER& bih) {
	VideoSources::const_iterator it(mVideoSources.begin()), itEnd(mVideoSources.end());
	for(; it!=itEnd; ++it) {
		IVDVideoSource *vs = *it;

		if (!vs->setDecompressedFormat((const VDAVIBitmapInfoHeader *)&bih))
			return false;
	}
	
	const BITMAPINFOHEADER *pbih = (const BITMAPINFOHEADER *)mVideoSources.front()->getDecompressedFormat();

	if (mpVideoCompressor->Query(pbih)) {
		char buf[16]={0};
		const char *s = buf;

		if (pbih->biCompression >= 0x20000000)
			*(uint32 *)buf = pbih->biCompression;
		else
			sprintf(buf, "RGB%d", pbih->biBitCount);

		VDLogAppMessage(kVDLogInfo, kVDST_Dub, kVDM_FastRecompressUsingFormat, 1, &s);
		return true;
	}

	return false;
}

bool Dubber::NegotiateFastFormat(int format) {
	VideoSources::const_iterator it(mVideoSources.begin()), itEnd(mVideoSources.end());
	for(; it!=itEnd; ++it) {
		IVDVideoSource *vs = *it;

		if (!vs->setTargetFormat(format))
			return false;
	}
	
	const BITMAPINFOHEADER *pbih = (const BITMAPINFOHEADER *)mVideoSources.front()->getDecompressedFormat();

	if (mpVideoCompressor->Query(pbih)) {
		char buf[16]={0};
		const char *s = buf;

		if (pbih->biCompression >= 0x20000000)
			*(uint32 *)buf = pbih->biCompression;
		else
			sprintf(buf, "RGB%d", pbih->biBitCount);

		VDLogAppMessage(kVDLogInfo, kVDST_Dub, kVDM_FastRecompressUsingFormat, 1, &s);
		return true;
	}

	return false;
}

void Dubber::InitSelectInputFormat() {
	//	DIRECT:			Don't care.
	//	FASTREPACK:		Negotiate with output compressor.
	//	SLOWREPACK:		[Dub]		Use selected format.
	//					[Preview]	Negotiate with display driver.
	//	FULL:			Use selected format.

	if (opt->video.mode == DubVideoOptions::M_NONE)
		return;

	const BITMAPINFOHEADER& bih = *(const BITMAPINFOHEADER *)vSrc->getImageFormat();

	if (opt->video.mode == DubVideoOptions::M_FASTREPACK && mpVideoCompressor) {
		// Attempt source format.
		if (NegotiateFastFormat(bih)) {
			mpInputDisplay->Reset();
			mbInputDisplayInitialized = true;
			return;
		}

		// Don't use odd-width YUV formats.  They may technically be allowed, but
		// a lot of codecs crash.  For instance, Huffyuv in "Convert to YUY2"
		// mode will accept a 639x360 format for compression, but crashes trying
		// to decompress it.

		if (!(bih.biWidth & 1)) {
			if (NegotiateFastFormat(nsVDPixmap::kPixFormat_YUV422_UYVY)) {
				mpInputDisplay->SetSource(false, vSrc->getTargetFormat());
				mbInputDisplayInitialized = true;
				return;
			}

			// Attempt YUY2.
			if (NegotiateFastFormat(nsVDPixmap::kPixFormat_YUV422_YUYV)) {
				mpInputDisplay->SetSource(false, vSrc->getTargetFormat());
				mbInputDisplayInitialized = true;
				return;
			}

			if (!(bih.biHeight & 1)) {
				if (NegotiateFastFormat(nsVDPixmap::kPixFormat_YUV420_Planar)) {
					mpInputDisplay->SetSource(false, vSrc->getTargetFormat());
					mbInputDisplayInitialized = true;
					return;
				}
			}
		}

		// Attempt RGB format negotiation.
		int format = opt->video.mInputFormat;
		uint32 rgbTrackMask = 0;

		do {
			if (NegotiateFastFormat(format))
				return;

			format = DegradeFormat(format, rgbTrackMask);
		} while(format);

		throw MyError("Video format negotiation failed: use slow-repack or full mode.");
	}

	// Negotiate RGB format.

	int format = opt->video.mInputFormat;

	format = VDRenderSetVideoSourceInputFormat(vSrc, format);
	if (!format)
		throw MyError("The decompression codec cannot decompress to an RGB format. This is very unusual. Check that any \"Force YUY2\" options are not enabled in the codec's properties.");

	const char *s = VDPixmapGetInfo(vSrc->getTargetFormat().format).name;

	VDLogAppMessage(kVDLogInfo, kVDST_Dub, (opt->video.mode == DubVideoOptions::M_FULL) ? kVDM_FullUsingInputFormat : kVDM_SlowRecompressUsingFormat, 1, &s);
}

void Dubber::Init(IVDVideoSource *const *pVideoSources, uint32 nVideoSources, AudioSource *const *pAudioSources, uint32 nAudioSources, IVDDubberOutputSystem *pOutputSystem, void *videoCompVars, const FrameSubset *pfs) {
	mAudioSources.assign(pAudioSources, pAudioSources + nAudioSources);
	mVideoSources.assign(pVideoSources, pVideoSources + nVideoSources);

	mpOutputSystem		= pOutputSystem;
	mbDoVideo			= !mVideoSources.empty() && mpOutputSystem->AcceptsVideo();
	mbDoAudio			= !mAudioSources.empty() && mpOutputSystem->AcceptsAudio();

	fPreview			= mpOutputSystem->IsRealTime();

	inputSubsetActive	= pfs;
	compVars			= (COMPVARS *)videoCompVars;

	if (!fPreview && pOutputSystem->AcceptsVideo() && opt->video.mode>DubVideoOptions::M_NONE && compVars && (compVars->dwFlags & ICMF_COMPVARS_VALID) && compVars->hic)
		mpVideoCompressor = VDCreateVideoCompressorVCM(compVars->hic, compVars->lDataRate*1024, compVars->lQ, compVars->lKey);

	if (!(inputSubsetActive = inputSubsetAlloc = new_nothrow FrameSubset(*pfs)))
		throw MyMemoryError();

	VDPosition selectionStartFrame;
	VDPosition selectionEndFrame;
	VDConvertSelectionTimesToFrames(*opt, *inputSubsetActive, vSrc->asStream()->getRate(), selectionStartFrame, selectionEndFrame);

	// check the mode; if we're using DirectStreamCopy mode, we'll need to
	// align the subset to keyframe boundaries!
	if (!mVideoSources.empty() && opt->video.mode == DubVideoOptions::M_NONE) {
		vdautoptr<FrameSubset> newSubset(new_nothrow FrameSubset());
		if (!newSubset)
			throw MyMemoryError();

		VDTranslateSubsetDirectMode(*newSubset, *inputSubsetActive, mVideoSources.data(), selectionStartFrame, selectionEndFrame);

		delete inputSubsetAlloc;
		inputSubsetAlloc = newSubset.release();
		inputSubsetActive = inputSubsetAlloc;
	}

	// initialize stream values
	AudioSource *audioSrc = mAudioSources.empty() ? NULL : mAudioSources.front();
	InitVideoStreamValuesStatic(vInfo, vSrc, audioSrc, opt, inputSubsetActive, &selectionStartFrame, &selectionEndFrame);
	InitAudioStreamValuesStatic(aInfo, audioSrc, opt);

	vInfo.frameRateNoTelecine = vInfo.frameRate;
	if (opt->video.mode >= DubVideoOptions::M_FULL && opt->video.fInvTelecine) {
		vInfo.frameRate = vInfo.frameRate * VDFraction(4, 5);

		vInfo.end_dst -= vInfo.end_dst % 5;

		vInfo.end_proc_dst	= vInfo.end_dst * 4 / 5;
	}

	// initialize directdraw display if in preview

	mbInputDisplayInitialized = false;
	if (fPreview)
		InitDirectDraw();

	// Select an appropriate input format.  This is really tricky...

	vInfo.fAudioOnly = true;
	if (mbDoVideo) {
		if (!mbInputDisplayInitialized)
			InitSelectInputFormat();
		vInfo.fAudioOnly = false;
	}

	// Initialize filter system.

	int nVideoLagOutput = 0;		// Frames that will be buffered in the output frame space (video filters)
	int nVideoLagTimeline = 0;		// Frames that will be buffered in the timeline frame space (IVTC)

	if (mbDoVideo && opt->video.mode >= DubVideoOptions::M_FULL) {
		const VDPixmap& px = vSrc->getTargetFormat();

		filters.initLinearChain(&g_listFA, px.w, px.h, px.format, px.palette, vInfo.frameRate, -1);

		vInfo.frameRate = filters.GetOutputFrameRate();
		
		const VDPixmapLayout& output = filters.GetOutputLayout();

		int outputFormat = opt->video.mOutputFormat;

		if (!outputFormat)
			outputFormat = vSrc->getTargetFormat().format;

		if (!CheckFormatSizeCompatibility(outputFormat, output.w, output.h)) {
			const VDPixmapFormatInfo& formatInfo = VDPixmapGetInfo(outputFormat);

			throw MyError("The output frame size is not compatible with the selected output format. (%dx%d, %s)", output.w, output.h, formatInfo.name);
		}

		filters.ReadyFilters();

		nVideoLagTimeline = nVideoLagOutput = filters.getFrameLag();

		// Inverse telecine?

		if (opt->video.fInvTelecine) {
			if (opt->video.mbUseSmartRendering)
				throw MyError("Inverse telecine cannot be used with smart rendering.");

			const VDPixmapLayout& input = filters.GetInputLayout();
			if (!(pInvTelecine = CreateVideoTelecineRemover(input.w, input.h, !opt->video.fIVTCMode, opt->video.nIVTCOffset, opt->video.fIVTCPolarity)))
				throw MyMemoryError();

			nVideoLagTimeline = 10 + ((nVideoLagOutput+3)&~3)*5;
		}
	}

	if (vInfo.end_dst > 0)
		vInfo.cur_dst = -nVideoLagTimeline;

	// initialize input decompressor

	if (mbDoVideo) {
		VideoSources::const_iterator it(mVideoSources.begin()), itEnd(mVideoSources.end());
		for(; it!=itEnd; ++it) {
			IVDVideoSource *vs = *it;

			vs->streamBegin(fPreview, opt->video.mode == DubVideoOptions::M_NONE);
		}
		fVDecompressionOk = true;

	}

	// Initialize audio.
	if (mbDoAudio)
		InitAudioConversionChain();

	// Initialize input window display.

	if (!mbInputDisplayInitialized && mpInputDisplay) {
		if (mbDoVideo)
			mpInputDisplay->SetSource(false, vSrc->getTargetFormat(), NULL, 0, true, opt->video.previewFieldMode>0);
	}

	// initialize output parameters and output file

	InitOutputFile();

	// Initialize output window display.

	if (mpOutputDisplay && mbDoVideo) {
		if (opt->video.mode == DubVideoOptions::M_FULL) {
			VDPixmap px;
			px.w = mVideoFilterOutputPixmapLayout.w;
			px.h = mVideoFilterOutputPixmapLayout.h;
			px.format = mVideoFilterOutputPixmapLayout.format;
			px.pitch = mVideoFilterOutputPixmapLayout.pitch;
			px.pitch2 = mVideoFilterOutputPixmapLayout.pitch2;
			px.pitch3 = mVideoFilterOutputPixmapLayout.pitch3;
			px.palette = mVideoFilterOutputPixmapLayout.palette;
			px.data = NULL;
			px.data2 = NULL;
			px.data3 = NULL;
			mpOutputDisplay->SetSource(false, px, NULL, 0, true, opt->video.previewFieldMode>0);
		}
	}

	// initialize interleaver

	bool bAudio = mbDoAudio;

	mInterleaver.Init(bAudio ? 2 : 1);
	mInterleaver.EnableInterleaving(opt->audio.enabled);
	mInterleaver.InitStream(0, lVideoSizeEstimate, 0, 1, 1, 1);

	if (bAudio) {
		double audioBlocksPerVideoFrame;

		if (!opt->audio.interval)
			audioBlocksPerVideoFrame = 1.0;
		else if (opt->audio.is_ms) {
			// blocks / frame = (ms / frame) / (ms / block)
			audioBlocksPerVideoFrame = vInfo.frameRate.AsInverseDouble() * 1000.0 / (double)opt->audio.interval;
		} else
			audioBlocksPerVideoFrame = 1.0 / (double)opt->audio.interval;

		const VDWaveFormat *pwfex = audioStream->GetFormat();
		const VDFraction& samplesPerSec = audioStream->GetSampleRate();
		sint32 preload = (sint32)(samplesPerSec * Fraction(opt->audio.preload, 1000)).roundup32ul();

		double samplesPerFrame = samplesPerSec.asDouble() / vInfo.frameRate.asDouble();

		mInterleaver.InitStream(1, pwfex->mBlockSize, preload, samplesPerFrame, audioBlocksPerVideoFrame, 262144);		// don't write TOO many samples at once
	}

	// initialize frame iterator

	if (mbDoVideo) {
		mVideoFrameMap.Init(mVideoSources, vInfo.start_src, vInfo.frameRateIn / vInfo.frameRateNoTelecine, inputSubsetActive, vInfo.end_dst, opt->video.mode == DubVideoOptions::M_NONE, &filters);

		FilterSystem *filtsysToCheck = NULL;

		if (opt->video.mode >= DubVideoOptions::M_FULL && !filters.isEmpty() && opt->video.mbUseSmartRendering) {
			filtsysToCheck = &filters;
		}

		mVideoFrameIterator.Init(mVideoSources, opt->video.mode == DubVideoOptions::M_NONE, opt->video.mode != DubVideoOptions::M_NONE && opt->video.mbUseSmartRendering, filtsysToCheck);
	} else {
		mInterleaver.EndStream(0);
	}

	// Create data pipes.

	if (!(mpVideoPipe = new_nothrow AVIPipe(VDPreferencesGetRenderVideoBufferCount(), 16384)))
		throw MyMemoryError();

	if (mbDoAudio) {
		const VDWaveFormat *pwfex = audioStream->GetFormat();

		uint32 bytes = pwfex->mDataRate * 2;		// 2 seconds

		mAudioPipe.Init(bytes - bytes % pwfex->mBlockSize, pwfex->mBlockSize, audioStream->IsVBR());
	}
}

void Dubber::Go(int iPriority) {
	// check the version.  if NT, don't touch the processing priority!
	bool fNoProcessingPriority = VDIsWindowsNT();

	if (!iPriority)
		iPriority = fNoProcessingPriority || !mpOutputSystem->IsRealTime() ? 5 : 6;

	// Initialize threads.
	mProcessThread.SetParent(this);
	mProcessThread.SetAbortSignal(&mbAbort);
	mProcessThread.SetStatusHandler(pStatusHandler);

	if (mbDoVideo) {
		mProcessThread.SetInputDisplay(mpInputDisplay);
		mProcessThread.SetOutputDisplay(mpOutputDisplay);
		mProcessThread.SetVideoIVTC(pInvTelecine);
		mProcessThread.SetVideoCompressor(mpVideoCompressor, opt->video.mMaxVideoCompressionThreads);

		if(opt->video.mode >= DubVideoOptions::M_FULL)
			mProcessThread.SetVideoFilterOutput(mVideoFilterOutputPixmapLayout);
	}

	mProcessThread.SetAudioSourcePresent(!mAudioSources.empty() && mAudioSources[0]);
	mProcessThread.SetVideoSources(mVideoSources.data(), mVideoSources.size());
	mProcessThread.SetAudioCorrector(audioCorrector);
	mProcessThread.Init(*opt, &vInfo, mpOutputSystem, mpVideoPipe, &mAudioPipe, &mInterleaver);
	mProcessThread.ThreadStart();

	SetThreadPriority(mProcessThread.getThreadHandle(), g_iPriorities[iPriority-1][0]);

	// Continue with other threads.

	if (!(mpIOThread = new_nothrow VDDubIOThread(
				this,
				fPhantom,
				mVideoSources,
				mVideoFrameIterator,
				audioStream,
				mbDoVideo ? mpVideoPipe : NULL,
				mbDoAudio ? &mAudioPipe : NULL,
				mbAbort,
				aInfo,
				vInfo,
				mIOThreadCounter)))
		throw MyMemoryError();

	if (!mpIOThread->ThreadStart())
		throw MyError("Couldn't start I/O thread");

	SetThreadPriority(mpIOThread->getThreadHandle(), g_iPriorities[iPriority-1][1]);

	// We need to make sure that 100% actually means 100%.
	SetThrottleFactor((float)(opt->mThrottlePercent * 65536 / 100) / 65536.0f);

	// Create status window during the dub.
	if (pStatusHandler) {
		pStatusHandler->InitLinks(&aInfo, &vInfo, audioStatusStream, this, opt);
		pStatusHandler->Display(NULL, iPriority);
	}
}

//////////////////////////////////////////////

void Dubber::Stop() {
	if (mStopLock.xchg(1))
		return;

	mbAbort = true;

	if (mpVideoPipe)
		mpVideoPipe->abort();

	mAudioPipe.Abort();
	mProcessThread.Abort();

	int nObjectsToWaitOn = 0;
	HANDLE hObjects[3];

	if (VDSignal *pBlitterSigComplete = mProcessThread.GetBlitterSignal())
		hObjects[nObjectsToWaitOn++] = pBlitterSigComplete->getHandle();

	if (mProcessThread.isThreadAttached())
		hObjects[nObjectsToWaitOn++] = mProcessThread.getThreadHandle();

	if (mpIOThread && mpIOThread->isThreadAttached())
		hObjects[nObjectsToWaitOn++] = mpIOThread->getThreadHandle();

	uint32 startTime = VDGetCurrentTick();

	bool quitQueued = false;

	while(nObjectsToWaitOn > 0) {
		DWORD dwRes;

		dwRes = MsgWaitForMultipleObjects(nObjectsToWaitOn, hObjects, FALSE, 10000, QS_SENDMESSAGE);

		if (WAIT_OBJECT_0 + nObjectsToWaitOn == dwRes) {
			if (!guiDlgMessageLoop(NULL))
				quitQueued = true;

			continue;
		}
		
		uint32 currentTime = VDGetCurrentTick();

		if ((dwRes -= WAIT_OBJECT_0) < nObjectsToWaitOn) {
			if (dwRes+1 < nObjectsToWaitOn)
				hObjects[dwRes] = hObjects[nObjectsToWaitOn - 1];
			--nObjectsToWaitOn;
			startTime = currentTime;
			continue;
		}

		if (currentTime - startTime > 10000) {
			if (IDOK == MessageBox(g_hWnd, "Something appears to be stuck while trying to stop (thread deadlock). Abort operation and exit program?", "VirtualDub Internal Error", MB_ICONEXCLAMATION|MB_OKCANCEL)) {
				vdprotected("aborting process due to a thread deadlock") {
					ExitProcess(0);
				}
			}

			startTime = currentTime;
		}
	}

	if (quitQueued)
		PostQuitMessage(0);

	mbCompleted = mProcessThread.IsCompleted();

	if (!fError && mpIOThread)
		fError = mpIOThread->GetError(err);

	if (!fError)
		fError = mProcessThread.GetError(err);

	delete mpIOThread;
	mpIOThread = 0;

	mProcessThread.Shutdown();

	if (pStatusHandler)
		pStatusHandler->Freeze();

	mpVideoCompressor = NULL;

	if (mpVideoPipe)	{ delete mpVideoPipe; mpVideoPipe = NULL; }
	mAudioPipe.Shutdown();

	filters.DeinitFilters();

	if (fVDecompressionOk)	{ vSrc->asStream()->streamEnd(); }
	if (fADecompressionOk)	{ aSrc->streamEnd(); }

	{
		std::vector<AudioStream *>::const_iterator it(mAudioStreams.begin()), itEnd(mAudioStreams.end());

		for(; it!=itEnd; ++it)
			delete *it;

		mAudioStreams.clear();
	}

	if (inputSubsetAlloc)		{ delete inputSubsetAlloc; inputSubsetAlloc = NULL; }

	// deinitialize DirectDraw

	filters.DeallocateBuffers();
	
	delete pInvTelecine;	pInvTelecine = NULL;

	if (pStatusHandler && vInfo.cur_proc_src >= 0)
		pStatusHandler->SetLastPosition(vInfo.cur_proc_src);

	if (fError) {
		throw err;
		fError = false;
	}
}

///////////////////////////////////////////////////////////////////

void Dubber::InternalSignalStop() {
	if (!mbAbort.compareExchange(true, false) && !mStopLock)
		mStoppedEvent.Raise(this, false);
}

void Dubber::Abort(bool userAbort) {
	if (!mbAbort.compareExchange(true, false) && !mStopLock) {
		mbUserAbort = userAbort;
		mAudioPipe.Abort();
		mpVideoPipe->abort();

		mStoppedEvent.Raise(this, userAbort);
	}
}

bool Dubber::isRunning() {
	return !mbAbort;
}

bool Dubber::IsAborted() {
	return !mbCompleted;
}

bool Dubber::isAbortedByUser() {
	return mbUserAbort != 0;
}

bool Dubber::IsPreviewing() {
	return fPreview;
}

void Dubber::SetPriority(int index) {
	if (mpIOThread && mpIOThread->isThreadActive())
		SetThreadPriority(mpIOThread->getThreadHandle(), g_iPriorities[index][0]);

	if (mProcessThread.isThreadActive())
		SetThreadPriority(mProcessThread.getThreadHandle(), g_iPriorities[index][1]);
}

void Dubber::UpdateFrames() {
	mProcessThread.UpdateFrames();

	if (mLiveLockMessages < kLiveLockMessageLimit && !mStopLock) {
		uint32 curTime = VDGetCurrentTick();

		int iocount = mIOThreadCounter;
		int prcount = mProcessThread.GetActivityCounter();

		if (mLastIOThreadCounter != iocount) {
			mLastIOThreadCounter = iocount;
			mIOThreadFailCount = curTime;
		} else if (mLastIOThreadCounter && (curTime - mIOThreadFailCount - 30000) < 3600000) {		// 30s to 1hr
			if (mpIOThread->isThreadActive()) {
				void *eip = mpIOThread->ThreadLocation();
				const char *action = mpIOThread->GetCurrentAction();
				VDLogAppMessage(kVDLogInfo, kVDST_Dub, kVDM_IOThreadLivelock, 2, &eip, &action);
				++mLiveLockMessages;
			}
			mLastIOThreadCounter = 0;
		}

		if (mLastProcessingThreadCounter != prcount) {
			mLastProcessingThreadCounter = prcount;
			mProcessingThreadFailCount = curTime;
		} else if (mLastProcessingThreadCounter && (curTime - mProcessingThreadFailCount - 30000) < 3600000) {		// 30s to 1hr
			if (mProcessThread.isThreadActive()) {
				void *eip = mProcessThread.ThreadLocation();
				const char *action = mProcessThread.GetCurrentAction();
				VDLogAppMessage(kVDLogInfo, kVDST_Dub, kVDM_ProcessingThreadLivelock, 2, &eip, &action);
				++mLiveLockMessages;
			}
			mLastProcessingThreadCounter = 0;
		}
	}
}

void Dubber::SetThrottleFactor(float throttleFactor) {
	mProcessThread.SetThrottle(throttleFactor);
	if (mpIOThread)
		mpIOThread->SetThrottle(throttleFactor);
}
