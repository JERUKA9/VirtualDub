//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2004 Avery Lee
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

#include <stdafx.h>
#include <windows.h>
#include <vd2/system/filesys.h>
#include <vd2/system/file.h>
#include <vd2/system/thread.h>
#include <vd2/system/atomic.h>
#include <vd2/system/time.h>
#include <vd2/system/strutil.h>
#include <vd2/system/VDScheduler.h>
#include <vd2/Dita/services.h>
#include <vd2/Dita/resources.h>
#include <vd2/Kasumi/pixmap.h>
#include <vd2/Kasumi/pixmapops.h>
#include <vd2/Kasumi/pixmaputils.h>
#include <vd2/Riza/bitmap.h>
#include "project.h"
#include "VideoSource.h"
#include "AudioSource.h"
#include "InputFile.h"
#include "prefs.h"
#include "dub.h"
#include "DubOutput.h"
#include "DubStatus.h"
#include "filter.h"
#include "command.h"
#include "job.h"
#include "server.h"
#include "capture.h"
#include "script.h"
#include "SceneDetector.h"
#include "oshelper.h"
#include "resource.h"
#include "uiframe.h"
#include "filters.h"

///////////////////////////////////////////////////////////////////////////

namespace {
	enum {
		kVDST_Project = 9
	};

	enum {
		kVDM_ReopenChangesImminent,			// The new video file has fewer video frames than the current file. Switching to it will result in changes to the edit list. Do you want to continue?
		kVDM_DeleteFrame,					// delete frame %lld (Undo/Redo)
		kVDM_DeleteFrames,					// delete %lld frames at %lld (Undo/Redo)
		kVDM_CutFrame,						// cut frame %lld (Undo/Redo)
		kVDM_CutFrames,						// cut %lld frames at %lld (Undo/Redo)
		kVDM_MaskFrame,						// mask frame %lld (Undo/Redo)
		kVDM_MaskFrames,					// mask %lld frames at %lld (Undo/Redo)
		kVDM_Paste,							// paste (Undo/Redo)
		kVDM_ScanForErrors,					// scan for errors
		kVDM_ResetTimeline					// reset timeline
	};

	enum {
		kUndoLimit = 50,
		kRedoLimit = 50
	};
}

///////////////////////////////////////////////////////////////////////////

extern const char g_szError[];
extern const char g_szWarning[];

extern HINSTANCE g_hInst;

extern VDProject *g_project;
extern InputFileOptions	*g_pInputOpts;
extern COMPVARS g_Vcompression;

DubSource::ErrorMode	g_videoErrorMode			= DubSource::kErrorModeReportAll;
DubSource::ErrorMode	g_audioErrorMode			= DubSource::kErrorModeReportAll;

vdrefptr<AudioSource>	inputAudio;

extern bool				g_fDropFrames;
extern bool				g_fSwapPanes;
extern bool				g_bExit;

extern bool g_fJobMode;

extern wchar_t g_szInputAVIFile[MAX_PATH];
extern wchar_t g_szInputWAVFile[MAX_PATH];

extern uint32 VDPreferencesGetRenderThrottlePercent();

int VDRenderSetVideoSourceInputFormat(IVDVideoSource *vsrc, int format);

///////////////////////////////////////////////////////////////////////////

namespace {

	void CopyFrameToClipboard(HWND hwnd, const VDPixmap& px) {
		if (OpenClipboard(hwnd)) {
			if (EmptyClipboard()) {
				HANDLE hMem;
				void *lpvMem;

				VDPixmapLayout layout;
				uint32 imageSize = VDMakeBitmapCompatiblePixmapLayout(layout, px.w, px.h, nsVDPixmap::kPixFormat_RGB888, 0);

				vdstructex<VDAVIBitmapInfoHeader> bih;
				VDMakeBitmapFormatFromPixmapFormat(bih, nsVDPixmap::kPixFormat_RGB888, 0, px.w, px.h);

				uint32 headerSize = bih.size();

				if (hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, headerSize + imageSize)) {
					if (lpvMem = GlobalLock(hMem)) {
						memcpy(lpvMem, bih.data(), headerSize);

						VDPixmapBlt(VDPixmapFromLayout(layout, (char *)lpvMem + headerSize), px);

						GlobalUnlock(lpvMem);
						SetClipboardData(CF_DIB, hMem);
						CloseClipboard();
						return;
					}
					GlobalFree(hMem);
				}
			}
			CloseClipboard();
		}
	}
}

///////////////////////////////////////////////////////////////////////////

class VDProjectTimelineTimingSource : public vdrefcounted<IVDTimelineTimingSource> {
public:
	VDProjectTimelineTimingSource(IVDTimelineTimingSource *pTS, VDProject *pProject);
	~VDProjectTimelineTimingSource();

	sint64 GetStart();
	sint64 GetLength();
	const VDFraction GetRate();
	sint64 GetPrevKey(sint64 pos);
	sint64 GetNextKey(sint64 pos);
	sint64 GetNearestKey(sint64 pos);
	bool IsKey(sint64 pos);
	bool IsNullSample(sint64 pos);

protected:
	bool CheckFilters();

	vdrefptr<IVDTimelineTimingSource> mpTS;
	VDProject *mpProject;
};

VDProjectTimelineTimingSource::VDProjectTimelineTimingSource(IVDTimelineTimingSource *pTS, VDProject *pProject)
	: mpTS(pTS)
	, mpProject(pProject)
{
}

VDProjectTimelineTimingSource::~VDProjectTimelineTimingSource() {
}

sint64 VDProjectTimelineTimingSource::GetStart() {
	return 0;
}

sint64 VDProjectTimelineTimingSource::GetLength() {
	return CheckFilters() ? filters.GetOutputFrameCount() : mpTS->GetLength();
}

const VDFraction VDProjectTimelineTimingSource::GetRate() {
	return CheckFilters() ? filters.GetOutputFrameRate() : mpTS->GetRate();
}

sint64 VDProjectTimelineTimingSource::GetPrevKey(sint64 pos) {
	if (!CheckFilters())
		return mpTS->GetPrevKey(pos);

	while(--pos >= 0) {
		sint64 pos2 = filters.GetSourceFrame(pos);
		if (pos2 < 0 || mpTS->IsKey(pos2))
			break;
	}

	return pos;
}

sint64 VDProjectTimelineTimingSource::GetNextKey(sint64 pos) {
	if (!CheckFilters())
		return mpTS->GetNextKey(pos);

	sint64 len = filters.GetOutputFrameCount();

	while(++pos < len) {
		sint64 pos2 = filters.GetSourceFrame(pos);
		if (pos2 < 0 || mpTS->IsKey(pos2))
			return pos;
	}

	return -1;
}

sint64 VDProjectTimelineTimingSource::GetNearestKey(sint64 pos) {
	if (!CheckFilters())
		return mpTS->GetPrevKey(pos);

	while(pos >= 0) {
		sint64 pos2 = filters.GetSourceFrame(pos);
		if (pos2 < 0 || mpTS->IsKey(pos2))
			break;
		--pos;
	}

	return pos;
}

bool VDProjectTimelineTimingSource::IsKey(sint64 pos) {
	if (!CheckFilters())
		return mpTS->IsKey(pos);

	sint64 pos2 = filters.GetSourceFrame(pos);
	return pos2 < 0 || mpTS->IsKey(pos2);
}

bool VDProjectTimelineTimingSource::IsNullSample(sint64 pos) {
	if (!CheckFilters())
		return mpTS->IsKey(pos);

	sint64 pos2 = filters.GetSourceFrame(pos);
	return pos2 >= 0 && mpTS->IsNullSample(pos2);
}

bool VDProjectTimelineTimingSource::CheckFilters() {
	if (filters.isRunning())
		return true;

	try {
		mpProject->StartFilters();
	} catch(const MyError&) {
		// eat the error for now
	}

	return filters.isRunning();
}

///////////////////////////////////////////////////////////////////////////

VDProject::VDProject()
	: mhwnd(NULL)
	, mpCB(NULL)
	, mpSceneDetector(0)
	, mSceneShuttleMode(0)
	, mSceneShuttleAdvance(0)
	, mSceneShuttleCounter(0)
	, mpDubStatus(0)
	, mposCurrentFrame(0)
	, mposSelectionStart(0)
	, mposSelectionEnd(0)
	, mbPositionCallbackEnabled(false)
	, mbFilterChainLocked(false)
	, mDesiredInputFrame(-1)
	, mDesiredInputSample(-1)
	, mDesiredOutputFrame(-1)
	, mDesiredTimelineFrame(-1)
	, mDesiredNextInputFrame(-1)
	, mDesiredNextOutputFrame(-1)
	, mDesiredNextTimelineFrame(-1)
	, mLastDisplayedInputFrame(-1)
	, mLastDisplayedTimelineFrame(-1)
	, mPreviewRestartMode(kPreviewRestart_None)
	, mVideoInputFrameRate(0,0)
	, mAudioSourceMode(kVDAudioSourceMode_Source)
{
}

VDProject::~VDProject() {
#if 0
	// We have to issue prestops first to make sure that all threads have
	// spinners running.
	for(int i=0; i<mThreadCount; ++i)
		mpSchedulerThreads[i].PreStop();

	delete[] mpSchedulerThreads;
#endif
}

bool VDProject::Attach(VDGUIHandle hwnd) {	
	mhwnd = hwnd;
	return true;
}

void VDProject::Detach() {
	mhwnd = NULL;
}

void VDProject::SetUICallback(IVDProjectUICallback *pCB) {
	mpCB = pCB;
}

void VDProject::BeginTimelineUpdate(const wchar_t *undostr) {
	if (!undostr)
		ClearUndoStack();
	else {
		if (mUndoStack.size()+1 > kUndoLimit)
			mUndoStack.pop_back();

		mUndoStack.push_front(UndoEntry(mTimeline.GetSubset(), undostr, mposCurrentFrame, mposSelectionStart, mposSelectionEnd));
	}

	mRedoStack.clear();
}

void VDProject::EndTimelineUpdate() {
	UpdateDubParameters();
	if (mpCB)
		mpCB->UITimelineUpdated();
}

bool VDProject::Undo() {
	if (mUndoStack.empty())
		return false;

	UndoEntry& ue = mUndoStack.front();

	mTimeline.GetSubset().swap(ue.mSubset);

	if (mRedoStack.size()+1 > kRedoLimit)
		mRedoStack.pop_back();

	mRedoStack.splice(mRedoStack.begin(), mUndoStack, mUndoStack.begin());

	EndTimelineUpdate();
	MoveToFrame(ue.mFrame);
	SetSelection(mposSelectionStart, mposSelectionEnd, false);
	return true;
}

bool VDProject::Redo() {
	if (mRedoStack.empty())
		return false;

	UndoEntry& ue = mRedoStack.front();

	mTimeline.GetSubset().swap(ue.mSubset);

	if (mUndoStack.size()+1 > kUndoLimit)
		mUndoStack.pop_back();

	mUndoStack.splice(mUndoStack.begin(), mRedoStack, mRedoStack.begin());

	EndTimelineUpdate();
	MoveToFrame(ue.mFrame);
	SetSelection(mposSelectionStart, mposSelectionEnd, false);
	return true;
}

void VDProject::ClearUndoStack() {
	mUndoStack.clear();
	mRedoStack.clear();
}

const wchar_t *VDProject::GetCurrentUndoAction() {
	if (mUndoStack.empty())
		return NULL;

	const UndoEntry& ue = mUndoStack.front();

	return ue.mDescription.c_str();
}

const wchar_t *VDProject::GetCurrentRedoAction() {
	if (mRedoStack.empty())
		return NULL;

	const UndoEntry& ue = mRedoStack.front();

	return ue.mDescription.c_str();
}

bool VDProject::Tick() {
	bool active = false;

	if (inputVideo && mSceneShuttleMode) {
		if (!mpSceneDetector)
			mpSceneDetector = new_nothrow SceneDetector(inputVideo->getImageFormat()->biWidth, abs(inputVideo->getImageFormat()->biHeight));

		if (mpSceneDetector) {
			mpSceneDetector->SetThresholds(g_prefs.scene.iCutThreshold, g_prefs.scene.iFadeThreshold);

			SceneShuttleStep();
			active = true;
		} else
			SceneShuttleStop();
	} else {
		if (mpSceneDetector) {
			delete mpSceneDetector;
			mpSceneDetector = NULL;
		}
	}

	if (UpdateFrame())
		active = true;

	return active;
}

VDPosition VDProject::GetCurrentFrame() {
	return mposCurrentFrame;
}

VDPosition VDProject::GetFrameCount() {
	return mTimeline.GetLength();
}

VDFraction VDProject::GetInputFrameRate() {
	return mVideoInputFrameRate;
}

void VDProject::ClearSelection(bool notifyUser) {
	mposSelectionStart = 0;
	mposSelectionEnd = -1;
	g_dubOpts.video.lStartOffsetMS = 0;
	g_dubOpts.video.lEndOffsetMS = 0;
	if (mpCB)
		mpCB->UISelectionUpdated(notifyUser);
}

bool VDProject::IsSelectionEmpty() {
	return mposSelectionStart >= mposSelectionEnd;
}

bool VDProject::IsSelectionPresent() {
	return mposSelectionStart <= mposSelectionEnd;
}

VDPosition VDProject::GetSelectionStartFrame() {
	return mposSelectionStart;
}

VDPosition VDProject::GetSelectionEndFrame() {
	return mposSelectionEnd;
}

bool VDProject::IsClipboardEmpty() {
	return mClipboard.empty();
}

bool VDProject::IsSceneShuttleRunning() {
	return mSceneShuttleMode != 0;
}

void VDProject::SetPositionCallbackEnabled(bool enable) {
	mbPositionCallbackEnabled = enable;
}

void VDProject::Cut() {
	Copy();
	DeleteInternal(true, false);
}

void VDProject::Copy() {
	FrameSubset& s = mTimeline.GetSubset();
	mClipboard.assign(s, mposSelectionStart, mposSelectionEnd - mposSelectionStart);
}

void VDProject::Paste() {
	FrameSubset& s = mTimeline.GetSubset();

	BeginTimelineUpdate(VDLoadString(0, kVDST_Project, kVDM_Paste));
	if (!IsSelectionEmpty())
		DeleteInternal(false, true);
	s.insert(mposCurrentFrame, mClipboard);
	EndTimelineUpdate();
}

void VDProject::Delete() {
	DeleteInternal(false, false);
}

void VDProject::DeleteInternal(bool tagAsCut, bool noTag) {
	VDPosition pos = GetCurrentFrame();
	VDPosition start = GetSelectionStartFrame();
	VDPosition end = GetSelectionEndFrame();

	FrameSubset& s = mTimeline.GetSubset();
	VDPosition len = 1;

	if (IsSelectionEmpty())
		start = pos;
	else
		len = end-start;

	if (!noTag) {
		if (tagAsCut) {
			if (len > 1)
				BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_CutFrames), 2, &start, &len).c_str());
			else
				BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_CutFrame), 1, &start).c_str());
		} else {
			if (len > 1)
				BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_DeleteFrames), 2, &start, &len).c_str());
			else
				BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_DeleteFrame), 1, &start).c_str());
		}
	}

	s.deleteRange(start, len);

	if (!noTag)
		EndTimelineUpdate();

	ClearSelection(false);
	MoveToFrame(start);
}

void VDProject::MaskSelection(bool bNewMode) {
	VDPosition pos = GetCurrentFrame();
	VDPosition start = GetSelectionStartFrame();
	VDPosition end = GetSelectionEndFrame();

	FrameSubset& s = mTimeline.GetSubset();
	VDPosition len = 1;

	if (IsSelectionEmpty())
		start = pos;
	else
		len = end-start;

	if (len) {
		if (len > 1)
			BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_MaskFrames), 2, &start, &len).c_str());
		else
			BeginTimelineUpdate(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_MaskFrame), 1, &start).c_str());

		s.setRange(start, len, bNewMode, 0);

		EndTimelineUpdate();
	}
}

void VDProject::DisplayFrame(bool bDispInput) {
	VDPosition pos = mposCurrentFrame;
	VDPosition timeline_pos = pos;

	if (!mpCB)
		return;

	if (!inputVideo)
		return;

	if (!g_dubOpts.video.fShowInputFrame && !g_dubOpts.video.fShowOutputFrame)
		return;

	try {
		sint64 outpos = mTimeline.TimelineToSourceFrame(pos);

		if (!g_listFA.IsEmpty()) {
			if (!filters.isRunning()) {
				StartFilters();
			}

			pos = filters.GetSourceFrame(outpos);
		} else {
			pos = outpos;
		}

		IVDStreamSource *pVSS = inputVideo->asStream();
		if (pos < 0)
			pos = pVSS->getEnd();

		bool bShowOutput = !mSceneShuttleMode && !g_dubber && g_dubOpts.video.fShowOutputFrame;

		if (mLastDisplayedInputFrame != pos || mLastDisplayedTimelineFrame != timeline_pos || !inputVideo->isFrameBufferValid() || (bShowOutput && !filters.isRunning())) {
			if (bDispInput)
				mLastDisplayedInputFrame = pos;

			mLastDisplayedTimelineFrame = timeline_pos;

			if (pos >= pVSS->getEnd()) {
				mDesiredInputFrame = -1;
				mDesiredInputSample = -1;
				mDesiredOutputFrame = -1;
				mDesiredNextInputFrame = -1;
				mDesiredNextInputSample = -1;
				mDesiredNextOutputFrame = -1;

				if (g_dubOpts.video.fShowInputFrame && bDispInput)
					mpCB->UIRefreshInputFrame(false);
				if (bShowOutput)
					mpCB->UIRefreshOutputFrame(false);
			} else {

				if (mDesiredInputFrame < 0)
					inputVideo->streamBegin(false, false);

				bool replace = true;

				if (mDesiredInputFrame >= 0) {
					inputVideo->streamSetDesiredFrame(pos);
					int to_new = inputVideo->streamGetRequiredCount(NULL);
					inputVideo->streamSetDesiredFrame(mDesiredInputFrame);
					int to_current = inputVideo->streamGetRequiredCount(NULL);

					if (to_current <= to_new)
						replace = false;
				}

				if (replace) {
					inputVideo->streamSetDesiredFrame(pos);
					mDesiredInputFrame	= pos;
					mDesiredInputSample = inputVideo->displayToStreamOrder(pos);
					mDesiredOutputFrame = outpos;
					mDesiredTimelineFrame = timeline_pos;
					mDesiredNextInputFrame = -1;
					mDesiredNextOutputFrame = -1;
					mDesiredNextTimelineFrame = -1;
				} else {
					mDesiredNextInputFrame		= pos;
					mDesiredNextInputSample		= inputVideo->displayToStreamOrder(pos);
					mDesiredNextOutputFrame		= outpos;
					mDesiredNextTimelineFrame	= timeline_pos;
				}
				mbUpdateInputFrame	= bDispInput;
				mbUpdateOutputFrame	= bShowOutput;
				mbUpdateLong		= false;
				mFramesDecoded		= 0;
				mLastDecodeUpdate	= VDGetCurrentTick();

				UpdateFrame();
			}
		}

	} catch(const MyError& e) {
		const char *src = e.gets();
		char *dst = _strdup(src);

		if (!dst)
			guiSetStatus("%s", 255, e.gets());
		else {
			for(char *t = dst; *t; ++t)
				if (*t == '\n')
					*t = ' ';

			guiSetStatus("%s", 255, dst);
			free(dst);
		}
		SceneShuttleStop();
	}
}

bool VDProject::UpdateFrame() {
	if (mDesiredInputFrame < 0 || !mpCB)
		return false;

	if (!inputVideo) {
		if (g_dubOpts.video.fShowInputFrame && mbUpdateInputFrame)
			mpCB->UIRefreshInputFrame(false);

		if (mbUpdateOutputFrame)
			mpCB->UIRefreshOutputFrame(false);

		if (mbUpdateLong)
			guiSetStatus("", 255);

		mDesiredInputFrame = -1;
		mDesiredInputSample = -1;
		mDesiredOutputFrame = -1;
		mDesiredTimelineFrame = -1;
		mDesiredNextInputFrame = -1;
		mDesiredNextInputSample = -1;
		mDesiredNextOutputFrame = -1;
		mDesiredNextTimelineFrame = -1;
		return false;
	}

	uint32 startTime = VDGetCurrentTick();

	try {
		for(;;) {
			bool preroll;

			VDPosition frame = inputVideo->streamGetNextRequiredFrame(preroll);
			IVDStreamSource *pVSS = inputVideo->asStream();

			if (frame >= 0) {
				uint32 bytes, samples;

				int err = IVDStreamSource::kBufferTooSmall;
				
				uint32 pad = inputVideo->streamGetDecodePadding();

				if (mVideoSampleBuffer.size() > pad)
					err = pVSS->read(frame, 1, mVideoSampleBuffer.data(), mVideoSampleBuffer.size() - pad, &bytes, &samples);

				if (err == IVDStreamSource::kBufferTooSmall) {
					pVSS->read(frame, 1, NULL, 0, &bytes, &samples);
					if (!bytes)
						++bytes;

					uint32 newSize = (bytes + pad + 16383) & ~16383;
					mVideoSampleBuffer.resize(newSize);
					err = pVSS->read(frame, 1, mVideoSampleBuffer.data(), newSize, &bytes, &samples);
				}

				if (err != IVDStreamSource::kOK)
					throw MyAVIError("Display", err);

				if (samples > 0) {
					inputVideo->streamFillDecodePadding(mVideoSampleBuffer.data(), bytes);
					inputVideo->streamGetFrame(mVideoSampleBuffer.data(), bytes, preroll, frame, mDesiredInputSample);
				}

				++mFramesDecoded;

				if (preroll) {
					uint32 nCurrentTime = VDGetCurrentTick();

					if (nCurrentTime - mLastDecodeUpdate > 500) {
						mLastDecodeUpdate = nCurrentTime;
						mbUpdateLong = true;

						guiSetStatus("Decoding frame %lu: preloading frame %lu", 255, (unsigned long)mDesiredInputFrame, (unsigned long)inputVideo->streamToDisplayOrder(frame));
					}

					if (nCurrentTime - startTime > 100)
						break;
				}

			} else {
				if (!mFramesDecoded)
					inputVideo->streamGetFrame(NULL, 0, false, -1, mDesiredInputSample);

				if (g_dubOpts.video.fShowInputFrame && mbUpdateInputFrame)
					mpCB->UIRefreshInputFrame(true);

				if (mbUpdateOutputFrame) {
					RefilterFrame(mDesiredOutputFrame, mDesiredTimelineFrame);

					mpCB->UIRefreshOutputFrame(true);
				}

				if (mbUpdateLong)
					guiSetStatus("", 255);

				mDesiredInputFrame = mDesiredNextInputFrame;
				mDesiredInputSample = mDesiredNextInputSample;
				mDesiredOutputFrame = mDesiredNextOutputFrame;
				mDesiredTimelineFrame = mDesiredNextTimelineFrame;
				mDesiredNextInputFrame = -1;
				mDesiredNextOutputFrame = -1;
				mFramesDecoded = 0;

				if (mDesiredInputFrame >= 0)
					inputVideo->streamSetDesiredFrame(mDesiredInputFrame);
				break;
			}
		}
	} catch(const MyError& e) {
		guiSetStatus("%s", 255, e.gets());

		SceneShuttleStop();
		mDesiredInputFrame = -1;
		mDesiredOutputFrame = -1;
		mDesiredTimelineFrame = -1;
		mDesiredNextInputFrame = -1;
		mDesiredNextOutputFrame = -1;
		mDesiredNextTimelineFrame = -1;
	}

	return mDesiredInputFrame >= 0;
}

void VDProject::RefilterFrame(VDPosition outPos, VDPosition timelinePos) {
	if (!inputVideo)
        return;

	if (!filters.isRunning())
		StartFilters();


	VDPixmapBlt(filters.GetInput(), inputVideo->getTargetFormat());

	sint64 timelineTimeMS = VDRoundToInt64(mVideoTimelineFrameRate.AsInverseDouble() * 1000.0 * (double)timelinePos);
	filters.RunFilters(outPos, timelinePos, timelinePos, timelineTimeMS, NULL, VDXFilterStateInfo::kStatePreview);
}

void VDProject::LockFilterChain(bool enableLock) {
	mbFilterChainLocked = enableLock;
}

///////////////////////////////////////////////////////////////////////////

void VDProject::Quit() {
	VDUIFrame *pFrame = VDUIFrame::GetFrame((HWND)mhwnd);

	if (VDINLINEASSERT(pFrame))
		pFrame->Destroy();
}

void VDProject::Open(const wchar_t *pFilename, IVDInputDriver *pSelectedDriver, bool fExtendedOpen, bool fQuiet, bool fAutoscan, const char *pInputOpts, uint32 inputOptsLen) {
	Close();

	try {
		// attempt to determine input file type

		VDStringW filename(VDGetFullPath(pFilename));

		if (!pSelectedDriver) {
			pSelectedDriver = VDAutoselectInputDriverForFile(filename.c_str(), IVDInputDriver::kF_Video);
			mInputDriverName.clear();
		} else {
			mInputDriverName = pSelectedDriver->GetSignatureName();
		}

		// open file

		inputAVI = pSelectedDriver->CreateInputFile((fQuiet?IVDInputDriver::kOF_Quiet:0) + (fAutoscan?IVDInputDriver::kOF_AutoSegmentScan:0));
		if (!inputAVI) throw MyMemoryError();

		// Extended open?

		if (fExtendedOpen)
			g_pInputOpts = inputAVI->promptForOptions(mhwnd);
		else if (pInputOpts)
			g_pInputOpts = inputAVI->createOptions(pInputOpts, inputOptsLen);

		if (g_pInputOpts)
			inputAVI->setOptions(g_pInputOpts);

		inputAVI->Init(filename.c_str());

		mInputAudioSources.clear();

		{
			vdrefptr<AudioSource> pTempAS;
			for(int i=0; inputAVI->GetAudioSource(i, ~pTempAS); ++i) {
				mInputAudioSources.push_back(pTempAS);
				pTempAS->setDecodeErrorMode(g_audioErrorMode);
			}
		}

		if (!inputAVI->GetVideoSource(0, ~inputVideo))
			throw MyError("File \"%ls\" does not have a video stream.", filename.c_str());


		VDRenderSetVideoSourceInputFormat(inputVideo, g_dubOpts.video.mInputFormat);

		IVDStreamSource *pVSS = inputVideo->asStream();
		pVSS->setDecodeErrorMode(g_videoErrorMode);

		// How many items did we get?

		{
			InputFilenameNode *pnode = inputAVI->listFiles.AtHead();
			InputFilenameNode *pnode_next;
			int nFiles = 0;

			while(pnode_next = pnode->NextFromHead()) {
				++nFiles;
				pnode = pnode_next;
			}

			if (nFiles > 1)
				guiSetStatus("Autoloaded %d segments (last was \"%ls\")", 255, nFiles, pnode->NextFromTail()->name);
		}

		// Retrieve info text

		inputAVI->GetTextInfo(mTextInfo);

		// Set current filename

		wcscpy(g_szInputAVIFile, filename.c_str());

		vdrefptr<IVDTimelineTimingSource> pTS;
		VDCreateTimelineTimingSourceVS(inputVideo, ~pTS);
		pTS = new VDProjectTimelineTimingSource(pTS, this);
		mTimeline.SetTimingSource(pTS);
		mTimeline.SetFromSource();

		ClearSelection(false);
		mpCB->UITimelineUpdated();

		if (mAudioSourceMode >= kVDAudioSourceMode_Source)
			mAudioSourceMode = kVDAudioSourceMode_Source;
		SetAudioSource();
		UpdateDubParameters();
		mpCB->UISourceFileUpdated();
		mpCB->UIVideoSourceUpdated();
		mpCB->UIAudioSourceUpdated();
		MoveToFrame(0);
	} catch(const MyError&) {
		Close();
		throw;
	}
}

void VDProject::Reopen() {
	if (!inputAVI)
		return;

	// attempt to determine input file type

	VDStringW filename(VDGetFullPath(g_szInputAVIFile));

	IVDInputDriver *pSelectedDriver = pSelectedDriver = VDAutoselectInputDriverForFile(filename.c_str(), IVDInputDriver::kF_Video);

	// open file

	vdrefptr<InputFile> newInput(pSelectedDriver->CreateInputFile(0));
	if (!newInput)
		throw MyMemoryError();

	// Extended open?

	if (g_pInputOpts)
		newInput->setOptions(g_pInputOpts);

	// Open new source

	newInput->Init(filename.c_str());

	vdrefptr<IVDVideoSource> pVS;
	vdrefptr<AudioSource> pAS;
	newInput->GetVideoSource(0, ~pVS);
	newInput->GetAudioSource(0, ~pAS);

	VDRenderSetVideoSourceInputFormat(pVS, g_dubOpts.video.mInputFormat);

	IVDStreamSource *pVSS = pVS->asStream();
	pVSS->setDecodeErrorMode(g_videoErrorMode);

	if (pAS)
		pAS->setDecodeErrorMode(g_audioErrorMode);

	// Check for an irrevocable change to the edit list. Irrevocable changes will occur if
	// there are any ranges other than the last that extend beyond the new length.

	const VDPosition oldFrameCount = inputVideo->asStream()->getLength();
	const VDPosition newFrameCount = pVS->asStream()->getLength();

	FrameSubset& fs = mTimeline.GetSubset();

	if (newFrameCount < oldFrameCount) {
		FrameSubset::const_iterator it(fs.begin()), itEnd(fs.end());

		if (it != itEnd) {
			--itEnd;

			for(; it!=itEnd; ++it) {
				const FrameSubsetNode& fsn = *it;

				if (fsn.start + fsn.len > newFrameCount) {
					sint64 oldCount = oldFrameCount;
					sint64 newCount = newFrameCount;

					VDStringA msg(VDTextWToA(VDswprintf(VDLoadString(0, kVDST_Project, kVDM_ReopenChangesImminent), 2, &newCount, &oldCount)));

					if (IDCANCEL == MessageBox((HWND)mhwnd, msg.c_str(), g_szError, MB_OKCANCEL))
						return;

					break;
				}
			}
		}
	}

	// Swap the sources.

	inputAudio = NULL;
	inputAVI = newInput;
	inputVideo = pVS;
	mInputAudioSources.clear();
	if (pAS)
		mInputAudioSources.push_back(vdrefptr<AudioSource>(pAS));

	wcscpy(g_szInputAVIFile, filename.c_str());

	// Update vars.

	vdrefptr<IVDTimelineTimingSource> pTS;
	VDCreateTimelineTimingSourceVS(inputVideo, ~pTS);
	pTS = new VDProjectTimelineTimingSource(pTS, this);
	mTimeline.SetTimingSource(pTS);

	ClearUndoStack();

	if (oldFrameCount > newFrameCount)
		fs.trimInputRange(newFrameCount);
	else if (oldFrameCount < newFrameCount)
		fs.addRange(oldFrameCount, newFrameCount - oldFrameCount, false, 0);

	mpCB->UITimelineUpdated();
	SetAudioSource();
	UpdateDubParameters();
	mpCB->UISourceFileUpdated();
	mpCB->UIAudioSourceUpdated();
	mpCB->UIVideoSourceUpdated();

	if (newFrameCount < oldFrameCount) {
		if (!IsSelectionEmpty() && mposSelectionEnd > newFrameCount)
			SetSelectionEnd(newFrameCount, false);

		if (mposCurrentFrame > newFrameCount)
			MoveToFrame(newFrameCount);
	}

	// redisplay current frame
	DisplayFrame();

	guiSetStatus("Reloaded \"%ls\" (%I64d frames).", 255, filename.c_str(), newFrameCount);
}

void VDProject::OpenWAV(const wchar_t *szFile, IVDInputDriver *pSelectedDriver, bool automated, bool extOpts, const void *optdata, int optlen) {
	if (!pSelectedDriver) {
		pSelectedDriver = VDAutoselectInputDriverForFile(szFile, IVDInputDriver::kF_Audio);
		mAudioInputDriverName.clear();
	} else {
		mAudioInputDriverName = pSelectedDriver->GetSignatureName();
	}
	mpAudioInputOptions = NULL;

	vdrefptr<InputFile> ifile(pSelectedDriver->CreateInputFile(IVDInputDriver::kOF_Quiet));
	if (!ifile)
		throw MyMemoryError();

	if (pSelectedDriver) {
		if (pSelectedDriver->GetFlags() & IVDInputDriver::kF_PromptForOpts)
			extOpts = true;
	}

	if (!automated && extOpts) {
		mpAudioInputOptions = ifile->promptForOptions((VDGUIHandle)mhwnd);
		if (mpAudioInputOptions) {
			ifile->setOptions(mpAudioInputOptions);

			// force input driver name if we have options, since they have to match
			if (mAudioInputDriverName.empty())
				mAudioInputDriverName = pSelectedDriver->GetSignatureName();
		}
	} else if (optdata) {
		mpAudioInputOptions = ifile->createOptions(optdata, optlen);
		if (mpAudioInputOptions)
			ifile->setOptions(mpAudioInputOptions);
	}

	ifile->Init(szFile);

	vdrefptr<AudioSource> pNewAudio;
	if (!ifile->GetAudioSource(0, ~pNewAudio))
		throw MyError("The file \"%ls\" does not contain an audio track.", szFile);

	pNewAudio->setDecodeErrorMode(g_audioErrorMode);

	vdwcslcpy(g_szInputWAVFile, szFile, sizeof(g_szInputWAVFile)/sizeof(g_szInputWAVFile[0]));

	mAudioSourceMode = kVDAudioSourceMode_External;
	inputAudio = mpInputAudioExt = pNewAudio;
	if (mpCB)
		mpCB->UIAudioSourceUpdated();
}

void VDProject::CloseWAV() {
	mpAudioInputOptions = NULL;

	if (mpInputAudioExt) {
		if (inputAudio == mpInputAudioExt) {
			inputAudio = NULL;
			mAudioSourceMode = kVDAudioSourceMode_None;
		}
		mpInputAudioExt = NULL;
	}
}

void VDProject::PreviewInput() {
	VDPosition start = GetCurrentFrame();
	DubOptions dubOpt(g_dubOpts);

	LONG preload = inputAudio && inputAudio->getWaveFormat()->mTag != WAVE_FORMAT_PCM ? 1000 : 500;

	if (dubOpt.audio.preload > preload)
		dubOpt.audio.preload = preload;

	IVDStreamSource *pVSS = inputVideo->asStream();
	dubOpt.audio.enabled				= TRUE;
	dubOpt.audio.interval				= 1;
	dubOpt.audio.is_ms					= FALSE;
	dubOpt.video.lStartOffsetMS			= (long)pVSS->samplesToMs(start);

	dubOpt.audio.fStartAudio			= TRUE;
	dubOpt.audio.new_rate				= 0;
	dubOpt.audio.newPrecision			= DubAudioOptions::P_NOCHANGE;
	dubOpt.audio.newChannels			= DubAudioOptions::C_NOCHANGE;
	dubOpt.audio.mVolume				= -1.0f;
	dubOpt.audio.bUseAudioFilterGraph	= false;

	switch(g_prefs.main.iPreviewDepth) {
	case PreferencesMain::DEPTH_DISPLAY:
		{
			DEVMODE dm;
			dm.dmSize = sizeof(DEVMODE);
			dm.dmDriverExtra = 0;
			if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm))
				dm.dmBitsPerPel = 16;

			switch(dm.dmBitsPerPel) {
			case 24:
				dubOpt.video.mInputFormat = nsVDPixmap::kPixFormat_RGB888;
				break;
			case 32:
				dubOpt.video.mInputFormat = nsVDPixmap::kPixFormat_XRGB8888;
				break;
			default:
				dubOpt.video.mInputFormat = nsVDPixmap::kPixFormat_XRGB1555;
				break;
			}
		}
		break;
	case PreferencesMain::DEPTH_FASTEST:
	case PreferencesMain::DEPTH_16BIT:
		dubOpt.video.mInputFormat = nsVDPixmap::kPixFormat_XRGB1555;
		break;
	case PreferencesMain::DEPTH_24BIT:
		dubOpt.video.mInputFormat = nsVDPixmap::kPixFormat_RGB888;
		break;

	// Ignore: PreferencesMain::DEPTH_OUTPUT

	};

	dubOpt.video.mOutputFormat			= dubOpt.video.mInputFormat;

	dubOpt.video.mode					= DubVideoOptions::M_SLOWREPACK;
	dubOpt.video.fShowInputFrame		= TRUE;
	dubOpt.video.fShowOutputFrame		= FALSE;
	dubOpt.video.frameRateDecimation	= 1;
	dubOpt.video.lEndOffsetMS			= 0;
	dubOpt.video.mbUseSmartRendering	= false;

	dubOpt.audio.mode					= DubAudioOptions::M_FULL;

	dubOpt.fShowStatus = false;
	dubOpt.fMoveSlider = true;

	if (start < mTimeline.GetLength()) {
		mPreviewRestartMode = kPreviewRestart_Input;
		Preview(&dubOpt);
	}
}

void VDProject::PreviewOutput() {
	VDPosition start = GetCurrentFrame();
	DubOptions dubOpt(g_dubOpts);

	long preload = inputAudio && inputAudio->getWaveFormat()->mTag != WAVE_FORMAT_PCM ? 1000 : 500;

	if (dubOpt.audio.preload > preload)
		dubOpt.audio.preload = preload;

	IVDStreamSource *pVSS = inputVideo->asStream();
	dubOpt.audio.enabled				= TRUE;
	dubOpt.audio.interval				= 1;
	dubOpt.audio.is_ms					= FALSE;
	dubOpt.video.lStartOffsetMS			= (long)pVSS->samplesToMs(start);
	dubOpt.video.mbUseSmartRendering	= false;

	dubOpt.fShowStatus = false;
	dubOpt.fMoveSlider = true;

	if (start < mTimeline.GetLength()) {
		mPreviewRestartMode = kPreviewRestart_Output;
		Preview(&dubOpt);
	}
}

void VDProject::PreviewAll() {
	mPreviewRestartMode = kPreviewRestart_All;
	Preview(NULL);
}

void VDProject::Preview(DubOptions *options) {
	if (!inputVideo)
		throw MyError("No input video stream to process.");

	DubOptions opts(options ? *options : g_dubOpts);
	opts.audio.enabled = true;

	if (!options) {
		opts.video.fShowDecompressedFrame = g_drawDecompressedFrame;
		opts.fShowStatus = !!g_showStatusWindow;
	}

	VDAVIOutputPreviewSystem outpreview;
	RunOperation(&outpreview, false, &opts, g_prefs.main.iPreviewPriority, true, 0, 0);
}

void VDProject::PreviewRestart() {
	if (mPreviewRestartMode) {
		switch(mPreviewRestartMode) {
			case kPreviewRestart_Input:
				PreviewInput();
				break;
			case kPreviewRestart_Output:
				PreviewOutput();
				break;
			case kPreviewRestart_All:
				PreviewAll();
				break;
		}
	}
}

void VDProject::RunNullVideoPass() {
	if (!inputVideo)
		throw MyError("No input file to process.");

	VDAVIOutputNullVideoSystem nullout;
	RunOperation(&nullout, FALSE, NULL, g_prefs.main.iDubPriority, true);
}

void VDProject::CloseAVI() {
	// kill current seek
	mDesiredInputFrame = -1;
	mDesiredOutputFrame = -1;
	mDesiredTimelineFrame = -1;
	mDesiredNextInputFrame = -1;
	mDesiredNextOutputFrame = -1;
	mDesiredNextTimelineFrame = -1;

	mTimeline.SetTimingSource(NULL);

	if (g_pInputOpts) {
		delete g_pInputOpts;
		g_pInputOpts = NULL;
	}

	while(!mInputAudioSources.empty()) {
		if (inputAudio == mInputAudioSources.back())
			inputAudio = NULL;

		mInputAudioSources.pop_back();
	}

	inputVideo = NULL;
	inputAVI = NULL;

	mTextInfo.clear();

	StopFilters();

	ClearUndoStack();
}

void VDProject::Close() {
	CloseAVI();
	if (mpCB) {
		mpCB->UIVideoSourceUpdated();
		mpCB->UIAudioSourceUpdated();
		mpCB->UISourceFileUpdated();
	}
}

void VDProject::SaveAVI(const wchar_t *filename, bool compat, bool addAsJob) {
	if (!inputVideo)
		throw MyError("No input file to process.");

	if (addAsJob)
		JobAddConfiguration(&g_dubOpts, g_szInputAVIFile, mInputDriverName.c_str(), filename, compat, &inputAVI->listFiles, 0, 0);
	else
		::SaveAVI(filename, false, NULL, compat);
}

void VDProject::SaveFilmstrip(const wchar_t *pFilename) {
	if (!inputVideo)
		throw MyError("No input file to process.");

	VDAVIOutputFilmstripSystem out(pFilename);
	RunOperation(&out, TRUE, NULL, 0, false);
}

void VDProject::SaveAnimatedGIF(const wchar_t *pFilename, int loopCount) {
	if (!inputVideo)
		throw MyError("No input file to process.");

	VDAVIOutputGIFSystem out(pFilename);
	out.SetLoopCount(loopCount);
	RunOperation(&out, TRUE, NULL, 0, false);
}

void VDProject::SaveRawAudio(const wchar_t *pFilename) {
	if (!inputVideo)
		throw MyError("No input file to process.");

	if (!inputAudio)
		throw MyError("No audio stream to process.");

	VDAVIOutputRawSystem out(pFilename);
	RunOperation(&out, TRUE, NULL, 0, false);
}

void VDProject::StartServer() {
	VDGUIHandle hwnd = mhwnd;

	VDUIFrame *pFrame = VDUIFrame::GetFrame((HWND)hwnd);

	pFrame->SetNextMode(3);
	pFrame->Detach();
}

void VDProject::ShowInputInfo() {
	if (inputAVI)
		inputAVI->InfoDialog(mhwnd);
}

void VDProject::SetVideoMode(int mode) {
	g_dubOpts.video.mode = (char)mode;
}

void VDProject::CopySourceFrameToClipboard() {
	if (!inputVideo || !inputVideo->isFrameBufferValid())
		return;

	CopyFrameToClipboard((HWND)mhwnd, inputVideo->getTargetFormat());
}

void VDProject::CopyOutputFrameToClipboard() {
	if (!filters.isRunning())
		return;
	CopyFrameToClipboard((HWND)mhwnd, filters.GetOutput());
}

int VDProject::GetAudioSourceCount() const {
	return (int)mInputAudioSources.size();
}

int VDProject::GetAudioSourceMode() const {
	return mAudioSourceMode;
}

void VDProject::SetAudioSourceNone() {
	mAudioSourceMode = kVDAudioSourceMode_None;
	CloseWAV();
	SetAudioSource();
	if (mpCB)
		mpCB->UIAudioSourceUpdated();
}

void VDProject::SetAudioSourceNormal(int index) {
	CloseWAV();
	mAudioSourceMode = kVDAudioSourceMode_Source + index;
	SetAudioSource();
	if (mpCB)
		mpCB->UIAudioSourceUpdated();
}

void VDProject::SetAudioMode(int mode) {
	g_dubOpts.audio.mode = (char)mode;
	if (mpCB)
		mpCB->UIAudioSourceUpdated();
}

void VDProject::SetAudioErrorMode(int errorMode0) {
	DubSource::ErrorMode errorMode = (DubSource::ErrorMode)errorMode0;

	AudioSources::iterator it(mInputAudioSources.begin()), itEnd(mInputAudioSources.end());
	for(; it!=itEnd; ++it) {
		AudioSource *as = *it;
	
		as->setDecodeErrorMode(errorMode);
	}

	if (mpInputAudioExt)
		mpInputAudioExt->setDecodeErrorMode(errorMode);
}

void VDProject::SetSelectionStart() {
	if (inputAVI)
		SetSelectionStart(GetCurrentFrame());
}

void VDProject::SetSelectionStart(VDPosition pos, bool notifyUser) {
	if (inputAVI) {
		IVDStreamSource *pVSS = inputVideo->asStream();

		if (pos < 0)
			pos = 0;
		if (pos > GetFrameCount())
			pos = GetFrameCount();
		mposSelectionStart = pos;
		if (mposSelectionEnd < mposSelectionStart) {
			mposSelectionEnd = mposSelectionStart;
			g_dubOpts.video.lEndOffsetMS = (long)pVSS->samplesToMs(GetFrameCount() - pos);
		}

		g_dubOpts.video.lStartOffsetMS = (long)pVSS->samplesToMs(pos);

		if (mpCB)
			mpCB->UISelectionUpdated(notifyUser);
	}
}

void VDProject::SetSelectionEnd() {
	if (inputAVI)
		SetSelectionEnd(GetCurrentFrame());
}

void VDProject::SetSelectionEnd(VDPosition pos, bool notifyUser) {
	if (inputAVI) {
		IVDStreamSource *pVSS = inputVideo->asStream();

		if (pos < 0)
			pos = 0;
		if (pos > GetFrameCount())
			pos = GetFrameCount();

		mposSelectionEnd = pos;
		if (mposSelectionStart > mposSelectionEnd) {
			mposSelectionStart = mposSelectionEnd;
			g_dubOpts.video.lStartOffsetMS = (long)pVSS->samplesToMs(pos);
		}
		g_dubOpts.video.lEndOffsetMS = (long)pVSS->samplesToMs(GetFrameCount() - pos);

		if (mpCB)
			mpCB->UISelectionUpdated(notifyUser);
	}
}

void VDProject::SetSelection(VDPosition start, VDPosition end, bool notifyUser) {
	if (end < start)
		ClearSelection(notifyUser);
	else {
		IVDStreamSource *pVSS = inputVideo->asStream();
		const VDPosition count = GetFrameCount();
		if (start < 0)
			start = 0;
		if (start > count)
			start = count;
		if (end < 0)
			end = 0;
		if (end > count)
			end = count;

		mposSelectionStart = start;
		mposSelectionEnd = end;

		g_dubOpts.video.lStartOffsetMS = (long)pVSS->samplesToMs(start);
		g_dubOpts.video.lEndOffsetMS = (long)pVSS->samplesToMs(GetFrameCount() - end);

		if (mpCB)
			mpCB->UISelectionUpdated(notifyUser);
	}
}

void VDProject::MoveToFrame(VDPosition frame) {
	if (inputVideo) {
		frame = std::max<VDPosition>(0, std::min<VDPosition>(frame, mTimeline.GetLength()));

		mposCurrentFrame = frame;
		mbPositionCallbackEnabled = false;
		if (mpCB)
			mpCB->UICurrentPositionUpdated();

		if (!g_dubber)
			DisplayFrame();
	}
}

void VDProject::MoveToStart() {
	if (inputVideo)
		MoveToFrame(0);
}

void VDProject::MoveToPrevious() {
	if (inputVideo)
		MoveToFrame(GetCurrentFrame() - 1);
}

void VDProject::MoveToNext() {
	if (inputVideo)
		MoveToFrame(GetCurrentFrame() + 1);
}

void VDProject::MoveToEnd() {
	if (inputVideo)
		MoveToFrame(mTimeline.GetEnd());
}

void VDProject::MoveToSelectionStart() {
	if (inputVideo && IsSelectionPresent()) {
		VDPosition pos = GetSelectionStartFrame();

		if (pos >= 0)
			MoveToFrame(pos);
	}
}

void VDProject::MoveToSelectionEnd() {
	if (inputVideo && IsSelectionPresent()) {
		VDPosition pos = GetSelectionEndFrame();

		if (pos >= 0)
			MoveToFrame(pos);
	}
}

void VDProject::MoveToNearestKey(VDPosition pos) {
	if (!inputVideo)
		return;


	MoveToFrame(mTimeline.GetNearestKey(pos));
}

void VDProject::MoveToPreviousKey() {
	if (!inputVideo)
		return;

	VDPosition pos = mTimeline.GetPrevKey(GetCurrentFrame());

	if (pos < 0)
		pos = 0;

	MoveToFrame(pos);
}

void VDProject::MoveToNextKey() {
	if (!inputVideo)
		return;

	VDPosition pos = mTimeline.GetNextKey(GetCurrentFrame());

	if (pos < 0)
		pos = mTimeline.GetEnd();

	MoveToFrame(pos);
}

void VDProject::MoveBackSome() {
	if (inputVideo)
		MoveToFrame(GetCurrentFrame() - 50);
}

void VDProject::MoveForwardSome() {
	if (inputVideo)
		MoveToFrame(GetCurrentFrame() + 50);
}

void VDProject::StartSceneShuttleReverse() {
	if (!inputVideo)
		return;
	mSceneShuttleMode = -1;
	if (mpCB)
		mpCB->UIShuttleModeUpdated();
}

void VDProject::StartSceneShuttleForward() {
	if (!inputVideo)
		return;
	mSceneShuttleMode = +1;
	if (mpCB)
		mpCB->UIShuttleModeUpdated();
}

void VDProject::MoveToPreviousRange() {
	if (inputAVI) {
		VDPosition pos = mTimeline.GetPrevEdit(GetCurrentFrame());

		if (pos >= 0) {
			MoveToFrame(pos);

			sint64 len;
			bool masked;
			int source;
			sint64 start = mTimeline.GetSubset().lookupRange(pos, len, masked, source);
			guiSetStatus("Previous output frame %I64d-%I64d: included source range %I64d-%I64d%s", 255, pos, pos+len-1, start, start+len-1, masked ? " (masked)" : "");
			return;
		}
	}
	MoveToFrame(0);
	guiSetStatus("No previous edit.", 255);
}

void VDProject::MoveToNextRange() {
	if (inputAVI) {
		VDPosition pos = mTimeline.GetNextEdit(GetCurrentFrame());

		if (pos >= 0) {
			MoveToFrame(pos);

			sint64 len;
			bool masked;
			int source;
			sint64 start = mTimeline.GetSubset().lookupRange(pos, len, masked, source);
			guiSetStatus("Next output frame %I64d-%I64d: included source range %I64d-%I64d%s", 255, pos, pos+len-1, start, start+len-1, masked ? " (masked)" : "");
			return;
		}
	}
	MoveToFrame(GetFrameCount());
	guiSetStatus("No next edit.", 255);
}

void VDProject::MoveToPreviousDrop() {
	if (inputAVI) {
		VDPosition pos = mTimeline.GetPrevDrop(GetCurrentFrame());

		if (pos >= 0)
			MoveToFrame(pos);
		else
			guiSetStatus("No previous dropped frame found.", 255);
	}
}

void VDProject::MoveToNextDrop() {
	if (inputAVI) {
		VDPosition pos = mTimeline.GetNextDrop(GetCurrentFrame());

		if (pos >= 0)
			MoveToFrame(pos);
		else
			guiSetStatus("No next dropped frame found.", 255);
	}
}

void VDProject::ResetTimeline() {
	if (inputAVI) {
		BeginTimelineUpdate(VDLoadString(0, kVDST_Project, kVDM_ResetTimeline));

		mTimeline.SetFromSource();

		EndTimelineUpdate();
	}
}

void VDProject::ResetTimelineWithConfirmation() {
	if (inputAVI) {
		if (IDOK == MessageBox((HWND)mhwnd, "Discard edits and reset timeline?", g_szWarning, MB_OKCANCEL|MB_TASKMODAL|MB_SETFOREGROUND|MB_ICONEXCLAMATION)) {
			ResetTimeline();
		}
	}
}

void VDProject::ScanForErrors() {
	if (inputVideo) {
		BeginTimelineUpdate(VDLoadString(0, kVDST_Project, kVDM_ScanForErrors));
		ScanForUnreadableFrames(&mTimeline.GetSubset(), inputVideo);
		EndTimelineUpdate();
	}
}

void VDProject::RunOperation(IVDDubberOutputSystem *pOutputSystem, BOOL fAudioOnly, DubOptions *pOptions, int iPriority, bool fPropagateErrors, long lSpillThreshold, long lSpillFrameThreshold) {

	if (!inputAVI)
		throw MyError("No source has been loaded to process.");

	bool fError = false;
	bool bUserAbort = false;
	MyError prop_err;
	DubOptions *opts;

	vdautoptr<VDAVIOutputSegmentedSystem> segmentedOutput;

	{
		const wchar_t *pOpType = pOutputSystem->IsRealTime() ? L"preview" : L"dub";
		VDLog(kVDLogMarker, VDswprintf(L"Beginning %ls operation.", 1, &pOpType));
	}

	DubOptions tempOpts(pOptions ? *pOptions : g_dubOpts);

	mbPositionCallbackEnabled = true;

	try {
		VDAutoLogDisplay disp;

		StopFilters();

		// Create a dubber.

		opts = &tempOpts;
		if (!pOptions) {
			opts->video.fShowDecompressedFrame = g_drawDecompressedFrame;
			opts->fShowStatus = !!g_showStatusWindow;
		}
		opts->perf.fDropFrames = g_fDropFrames;
		opts->mThrottlePercent = pOutputSystem->IsRealTime() ? 100 : VDPreferencesGetRenderThrottlePercent();

		if (!(g_dubber = CreateDubber(opts)))
			throw MyMemoryError();

		// Create dub status window

		mpDubStatus = CreateDubStatusHandler();

		if (opts->fMoveSlider)
			mpDubStatus->SetPositionCallback(g_fJobMode ? JobPositionCallback : StaticPositionCallback, this);

		// Initialize the dubber.

		if (opts->audio.bUseAudioFilterGraph)
			g_dubber->SetAudioFilterGraph(g_audioFilterGraph);

		g_dubber->SetStatusHandler(mpDubStatus);

		if (!pOutputSystem->IsRealTime() && g_ACompressionFormat)
			g_dubber->SetAudioCompression((const VDWaveFormat *)g_ACompressionFormat, g_ACompressionFormatSize, g_ACompressionFormatHint.c_str());

		// As soon as we call Init(), this value is no longer ours to free.

		if (mpCB)
			mpCB->UISetDubbingMode(true, pOutputSystem->IsRealTime());

		IVDVideoSource *vsrc = inputVideo;
		AudioSource *asrc = inputAudio;

		if (lSpillThreshold) {
			segmentedOutput = new VDAVIOutputSegmentedSystem(pOutputSystem, opts->audio.is_ms, opts->audio.is_ms ? opts->audio.interval * 0.001 : opts->audio.interval, (double)opts->audio.preload / 500.0, (sint64)lSpillThreshold << 20, lSpillFrameThreshold);
			g_dubber->Init(&vsrc, 1, &asrc, asrc ? 1 : 0, segmentedOutput, &g_Vcompression, &mTimeline.GetSubset());
		} else {
			if (fAudioOnly == 2)
				g_dubber->SetPhantomVideoMode();

			g_dubber->Init(&vsrc, 1, &asrc, asrc ? 1 : 0, pOutputSystem, &g_Vcompression, &mTimeline.GetSubset());
		}

		if (!pOptions && mhwnd)
			RedrawWindow((HWND)mhwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE);

		g_dubber->Stopped() += mStoppedDelegate(this, &VDProject::OnDubAbort);
		g_dubber->Go(iPriority);

		if (mpCB)
			bUserAbort = !mpCB->UIRunDubMessageLoop();
		else {
			MSG msg;
			while(g_dubber->isRunning()) {
				BOOL result = GetMessage(&msg, NULL, 0, 0);

				if (result == (BOOL)-1)
					break;

				if (!result) {
					PostQuitMessage(msg.wParam);
					break;
				}

				TranslateMessage(&msg); 
				DispatchMessage(&msg); 
			}
		}

		g_dubber->Stop();

		if (g_dubber->isAbortedByUser()) {
			bUserAbort = true;
			mPreviewRestartMode = kPreviewRestart_None;
		} else {
			if (!g_dubber->IsAborted())
				mPreviewRestartMode = kPreviewRestart_None;

			if (!fPropagateErrors)
				disp.Post(mhwnd);
		}

	} catch(char *s) {
		mPreviewRestartMode = kPreviewRestart_None;
		if (fPropagateErrors) {
			prop_err.setf(s);
			fError = true;
		} else
			MyError(s).post((HWND)mhwnd, g_szError);
	} catch(MyError& err) {
		mPreviewRestartMode = kPreviewRestart_None;
		if (fPropagateErrors) {
			prop_err.TransferFrom(err);
			fError = true;
		} else
			err.post((HWND)mhwnd,g_szError);
	}

	if (g_dubber)
		g_dubber->SetStatusHandler(NULL);

	delete mpDubStatus;
	mpDubStatus = NULL;

	_CrtCheckMemory();

	delete g_dubber;
	g_dubber = NULL;

	VDRenderSetVideoSourceInputFormat(inputVideo, g_dubOpts.video.mInputFormat);

	if (mpCB)
		mpCB->UISetDubbingMode(false, false);

	VDLog(kVDLogMarker, VDStringW(L"Ending operation."));

	if (g_bExit)
		PostQuitMessage(0);
	else if (fPropagateErrors) {
		if (fError)
			throw prop_err;
		else if (bUserAbort)
			throw MyUserAbortError();
	}
}

void VDProject::AbortOperation() {
	if (g_dubber)
		g_dubber->Abort();
}

void VDProject::StopFilters() {
	filters.DeinitFilters();
	filters.DeallocateBuffers();
}

void VDProject::PrepareFilters() {
	if (filters.isRunning() || !inputVideo)
		return;

	IVDStreamSource *pVSS = inputVideo->asStream();
	VDFraction framerate(pVSS->getRate());

	if (g_dubOpts.video.mFrameRateAdjustLo)
		framerate.Assign(g_dubOpts.video.mFrameRateAdjustHi, g_dubOpts.video.mFrameRateAdjustLo);

	const VDPixmap& px = inputVideo->getTargetFormat();

	filters.prepareLinearChain(&g_listFA, px.w, px.h, px.format, framerate, pVSS->getLength());
}

void VDProject::StartFilters() {
	if (filters.isRunning() || !inputVideo || mbFilterChainLocked)
		return;

	IVDStreamSource *pVSS = inputVideo->asStream();
	VDFraction framerate(pVSS->getRate());

	if (g_dubOpts.video.mFrameRateAdjustLo)
		framerate.Assign(g_dubOpts.video.mFrameRateAdjustHi, g_dubOpts.video.mFrameRateAdjustLo);

	const VDPixmap& px = inputVideo->getTargetFormat();

	// We explicitly use the stream length here as we're interested in the *uncut* filtered length.
	filters.initLinearChain(&g_listFA, px.w, px.h, px.format, framerate, pVSS->getLength());
	filters.ReadyFilters();
}

void VDProject::UpdateFilterList() {
	mLastDisplayedTimelineFrame = -1;
	DisplayFrame();
	mpCB->UIVideoFiltersUpdated();
}

///////////////////////////////////////////////////////////////////////////

void VDProject::SceneShuttleStop() {
	if (mSceneShuttleMode) {
		mSceneShuttleMode = 0;
		mSceneShuttleAdvance = 0;
		mSceneShuttleCounter = 0;

		if (mpCB)
			mpCB->UIShuttleModeUpdated();

		if (inputVideo)
			MoveToFrame(GetCurrentFrame());
	}
}

void VDProject::SceneShuttleStep() {
	if (!inputVideo)
		SceneShuttleStop();

	VDPosition sample = GetCurrentFrame() + mSceneShuttleMode;
	VDPosition ls2 = mTimeline.TimelineToSourceFrame(sample);

	IVDStreamSource *pVSS = inputVideo->asStream();
	if (!inputVideo || ls2 < pVSS->getStart() || ls2 >= pVSS->getEnd()) {
		SceneShuttleStop();
		return;
	}

	if (mSceneShuttleAdvance < 1280)
		++mSceneShuttleAdvance;

	mSceneShuttleCounter += 32;

	mposCurrentFrame = sample;

	if (mSceneShuttleCounter >= mSceneShuttleAdvance) {
		mSceneShuttleCounter = 0;
		DisplayFrame(true);
	} else
		DisplayFrame(false);

	while(UpdateFrame())
		;

	if (mpCB)
		mpCB->UICurrentPositionUpdated();

	VBitmap framebm((void *)inputVideo->getFrameBuffer(), (BITMAPINFOHEADER *)inputVideo->getDecompressedFormat());
	if (mpSceneDetector->Submit(&framebm)) {
		SceneShuttleStop();
	}
}

void VDProject::StaticPositionCallback(VDPosition start, VDPosition cur, VDPosition end, int progress, void *cookie) {
	VDProject *pthis = (VDProject *)cookie;

	if (pthis->mbPositionCallbackEnabled) {
		VDPosition frame = std::max<VDPosition>(0, std::min<VDPosition>(cur, pthis->GetFrameCount()));

		pthis->mposCurrentFrame = frame;
		if (pthis->mpCB)
			pthis->mpCB->UICurrentPositionUpdated();
	}
}

void VDProject::UpdateDubParameters() {
	if (!inputVideo)
		return;

	mVideoInputFrameRate	= VDFraction(0,0);
	mVideoOutputFrameRate	= VDFraction(0,0);
	mVideoTimelineFrameRate	= VDFraction(0,0);

	DubVideoStreamInfo vInfo;

	if (inputVideo) {
		try {
			InitVideoStreamValuesStatic(vInfo, inputVideo, inputAudio, &g_dubOpts, &GetTimeline().GetSubset(), NULL, NULL);

			if (mVideoOutputFrameRate != vInfo.frameRate)
				StopFilters();

			mVideoInputFrameRate	= vInfo.frameRateIn;
			mVideoOutputFrameRate	= vInfo.frameRate;
			mVideoTimelineFrameRate	= vInfo.frameRate;

			StartFilters();

			if (filters.isRunning())
				mVideoTimelineFrameRate	= filters.GetOutputFrameRate();
		} catch(const MyError&) {
			// The input stream may throw an error here trying to obtain the nearest key.
			// If so, bail.
		}
	}

	if (mpCB)
		mpCB->UIDubParametersUpdated();
}

void VDProject::SetAudioSource() {
	switch(mAudioSourceMode) {
		case kVDAudioSourceMode_None:
			inputAudio = NULL;
			break;

		case kVDAudioSourceMode_External:
			inputAudio = mpInputAudioExt;
			break;

		default:
			if (mAudioSourceMode >= kVDAudioSourceMode_Source) {
				int index = mAudioSourceMode - kVDAudioSourceMode_Source;

				if ((unsigned)index < mInputAudioSources.size()) {
					inputAudio = mInputAudioSources[index];
					break;
				}
			}
			inputAudio = NULL;
			break;
	}
}

void VDProject::OnDubAbort(IDubber *, const bool&) {
	if (mpCB)
		mpCB->UIAbortDubMessageLoop();
}
