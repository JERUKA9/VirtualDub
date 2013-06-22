//	VirtualDub - Video processing and capture application
//	Copyright (C) 1998-2007 Avery Lee
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
#include <vd2/system/w32assist.h>
#include <vd2/Riza/display.h>
#include <vd2/Kasumi/pixmapops.h>
#include <vd2/Kasumi/pixel.h>
#include <vd2/VDLib/Dialog.h>
#include "FilterPreview.h"
#include "filters.h"
#include "PositionControl.h"
#include "ProgressDialog.h"
#include "project.h"
#include "VBitmap.h"
#include "command.h"
#include "VideoSource.h"
#include "resource.h"
#include "oshelper.h"
#include "dub.h"

extern HINSTANCE	g_hInst;
extern VDProject *g_project;
extern IVDPositionControlCallback *VDGetPositionControlCallbackTEMP();

namespace {
	static const UINT IDC_FILTDLG_POSITION = 500;

	static const UINT MYWM_REDRAW = WM_USER+100;
	static const UINT MYWM_RESTART = WM_USER+101;
}

///////////////////////////////////////////////////////////////////////////////

class VDVideoFilterPreviewZoomPopup : public VDDialogFrameW32 {
public:
	VDVideoFilterPreviewZoomPopup();
	~VDVideoFilterPreviewZoomPopup();

	void Update(int x, int y, const uint32 pixels[7][7]);

	VDZINT_PTR DlgProc(VDZUINT msg, VDZWPARAM wParam, VDZLPARAM lParam);

protected:
	bool OnLoaded();
	bool OnEraseBkgnd(HDC hdc);
	void OnPaint();

	RECT mBitmapRect;

	uint32	mBitmap[7][7];
	BITMAPINFO mBitmapInfo;
};

VDVideoFilterPreviewZoomPopup::VDVideoFilterPreviewZoomPopup()
	: VDDialogFrameW32(IDD_FILTER_PREVIEW_ZOOM)
{
	mBitmapInfo.bmiHeader.biSize			= sizeof(BITMAPINFOHEADER);
	mBitmapInfo.bmiHeader.biWidth			= 7;
	mBitmapInfo.bmiHeader.biHeight			= 7;
	mBitmapInfo.bmiHeader.biPlanes			= 1;
	mBitmapInfo.bmiHeader.biCompression		= BI_RGB;
	mBitmapInfo.bmiHeader.biSizeImage		= sizeof mBitmap;
	mBitmapInfo.bmiHeader.biBitCount		= 32;
	mBitmapInfo.bmiHeader.biXPelsPerMeter	= 0;
	mBitmapInfo.bmiHeader.biYPelsPerMeter	= 0;
	mBitmapInfo.bmiHeader.biClrUsed			= 0;
	mBitmapInfo.bmiHeader.biClrImportant	= 0;
}

VDVideoFilterPreviewZoomPopup::~VDVideoFilterPreviewZoomPopup() {
}

void VDVideoFilterPreviewZoomPopup::Update(int x, int y, const uint32 pixels[7][7]) {
	memcpy(mBitmap, pixels, sizeof mBitmap);

	uint32 px = pixels[2][2] & 0xffffff;
	SetControlTextF(IDC_POSITION, L"(%d,%d) = #%06X", x, y, px);
	SetControlTextF(IDC_RED, L"R: %d", px >> 16);
	SetControlTextF(IDC_GREEN, L"G: %d", (px >> 8) & 0xff);
	SetControlTextF(IDC_BLUE, L"B: %d", px & 0xff);

	InvalidateRect(mhdlg, &mBitmapRect, FALSE);
}

VDZINT_PTR VDVideoFilterPreviewZoomPopup::DlgProc(VDZUINT msg, VDZWPARAM wParam, VDZLPARAM lParam) {
	switch(msg) {
		case WM_NCHITTEST:
			SetWindowLongPtr(mhdlg, DWLP_MSGRESULT, HTTRANSPARENT);
			return TRUE;

		case WM_PAINT:
			OnPaint();
			return TRUE;

#if 0
		case WM_ERASEBKGND:
			SetWindowLongPtr(mhdlg, DWL_MSGRESULT, OnEraseBkgnd((HDC)wParam));
			return TRUE;
#endif
	}

	return VDDialogFrameW32::DlgProc(msg, wParam, lParam);
}

bool VDVideoFilterPreviewZoomPopup::OnLoaded() {
	HWND hwndImage = GetDlgItem(mhdlg, IDC_IMAGE);

	memset(&mBitmapRect, 0, sizeof mBitmapRect);

	if (hwndImage) {
		GetWindowRect(hwndImage, &mBitmapRect);
		MapWindowPoints(NULL, mhdlg, (LPPOINT)&mBitmapRect, 2);
	}

	VDDialogFrameW32::OnLoaded();
	return true;
}

bool VDVideoFilterPreviewZoomPopup::OnEraseBkgnd(HDC hdc) {
	RECT r;

	int saveHandle = SaveDC(hdc);
	if (saveHandle) {
		ExcludeClipRect(hdc, mBitmapRect.left, mBitmapRect.top, mBitmapRect.right, mBitmapRect.bottom);

		GetClientRect(mhdlg, &r);
		FillRect(hdc, &r, (HBRUSH)GetClassLongPtr(mhdlg, GCLP_HBRBACKGROUND));
		RestoreDC(hdc, saveHandle);
		return true;
	}

	return false;
}

void VDVideoFilterPreviewZoomPopup::OnPaint() {
	PAINTSTRUCT ps;

	HDC hdc = BeginPaint(mhdlg, &ps);
	if (hdc) {
		StretchDIBits(hdc,
				mBitmapRect.left,
				mBitmapRect.top,
				mBitmapRect.right - mBitmapRect.left,
				mBitmapRect.bottom - mBitmapRect.top,
				0,
				0,
				7,
				7,
				mBitmap, &mBitmapInfo, DIB_RGB_COLORS, SRCCOPY);
		EndPaint(mhdlg, &ps);
	}
}

///////////////////////////////////////////////////////////////////////////////

class FilterPreview : public IVDXFilterPreview2, public vdrefcounted<IVDVideoFilterPreviewDialog> {
public:
	FilterPreview(List *, FilterInstance *);
	~FilterPreview();

	IVDXFilterPreview2 *AsIVDXFilterPreview2() { return this; }

	void SetButtonCallback(VDXFilterPreviewButtonCallback, void *);
	void SetSampleCallback(VDXFilterPreviewSampleCallback, void *);

	bool isPreviewEnabled();
	bool IsPreviewDisplayed();
	void InitButton(VDXHWND);
	void Toggle(VDXHWND);
	void Display(VDXHWND, bool);
	void RedoFrame();
	void UndoSystem();
	void RedoSystem();
	void Close();
	bool SampleCurrentFrame();
	long SampleFrames();

private:
	static INT_PTR CALLBACK StaticDlgProc(HWND hdlg, UINT message, WPARAM wParam, LPARAM lParam);
	BOOL DlgProc(UINT message, WPARAM wParam, LPARAM lParam);

	void OnInit();
	void OnResize();
	void OnPaint();
	void OnVideoResize(bool bInitial);
	void OnVideoRedraw();
	bool OnCommand(UINT);

	VDPosition FetchFrame();
	VDPosition FetchFrame(VDPosition);

	void UpdateButton();

	HWND		mhdlg;
	HWND		mhwndButton;
	HWND		mhwndParent;
	HWND		mhwndPosition;
	HWND		mhwndDisplay;

	wchar_t		mButtonAccelerator;

	int			mWidth;
	int			mHeight;
	int			mDisplayX;
	int			mDisplayY;
	int			mDisplayW;
	int			mDisplayH;

	sint64		mLastOutputFrame;
	sint64		mLastTimelineFrame;
	sint64		mLastTimelineTimeMS;

	IVDPositionControl	*mpPosition;
	IVDVideoDisplay *mpDisplay;
	FilterSystem mFiltSys;
	List		*mpFilterList;
	FilterInstance *mpThisFilter;
	VDTimeline	*mpTimeline;

	VDXFilterPreviewButtonCallback	mpButtonCallback;
	void							*mpvButtonCBData;
	VDXFilterPreviewSampleCallback	mpSampleCallback;
	void							*mpvSampleCBData;

	MyError		mFailureReason;

	VDVideoFilterPreviewZoomPopup	mZoomPopup;

	ModelessDlgNode		mDlgNode;
};

bool VDCreateVideoFilterPreviewDialog(List *list, FilterInstance *finst, IVDVideoFilterPreviewDialog **pp) {
	IVDVideoFilterPreviewDialog *p = new_nothrow FilterPreview(list, finst);
	if (!p)
		return false;
	p->AddRef();
	*pp = p;
	return true;
}

FilterPreview::FilterPreview(List *pFilterList, FilterInstance *pfiThisFilter)
	: mhdlg(NULL)
	, mhwndButton(NULL)
	, mhwndParent(NULL)
	, mhwndPosition(NULL)
	, mhwndDisplay(NULL)
	, mButtonAccelerator(0)
	, mWidth(0)
	, mHeight(0)
	, mDisplayX(0)
	, mDisplayY(0)
	, mDisplayW(0)
	, mDisplayH(0)
	, mLastOutputFrame(0)
	, mLastTimelineFrame(0)
	, mLastTimelineTimeMS(0)
	, mpPosition(NULL)
	, mpDisplay(NULL)
	, mpFilterList(pFilterList)
	, mpThisFilter(pfiThisFilter)
	, mpTimeline(0)
	, mpButtonCallback(NULL)
	, mpSampleCallback(NULL)
{
}

FilterPreview::~FilterPreview() {
	if (mhdlg)
		DestroyWindow(mhdlg);
}

INT_PTR CALLBACK FilterPreview::StaticDlgProc(HWND hdlg, UINT message, WPARAM wParam, LPARAM lParam) {
	FilterPreview *fpd = (FilterPreview *)GetWindowLongPtr(hdlg, DWLP_USER);

	if (message == WM_INITDIALOG) {
		SetWindowLongPtr(hdlg, DWLP_USER, lParam);
		fpd = (FilterPreview *)lParam;
		fpd->mhdlg = hdlg;
	}

	return fpd && fpd->DlgProc(message, wParam, lParam);
}

BOOL FilterPreview::DlgProc(UINT message, WPARAM wParam, LPARAM lParam) {
	switch(message) {
	case WM_INITDIALOG:
		OnInit();
		OnVideoResize(true);
		return TRUE;

	case WM_DESTROY:
		if (mpDisplay) {
			mpDisplay->Destroy();
			mpDisplay = NULL;
			mhwndDisplay = NULL;
		}
		mDlgNode.Remove();
		return TRUE;

	case WM_SIZE:
		OnResize();
		return TRUE;

	case WM_PAINT:
		OnPaint();
		return TRUE;

	case WM_MOUSEMOVE:
		{
			POINT pt = {(SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam)};

			const VDPixmap& output = mFiltSys.GetOutput();

			int xoffset = pt.x - mDisplayX;
			int yoffset = pt.y - mDisplayY;

			if ((wParam & MK_SHIFT) && mFiltSys.isRunning() && (unsigned)xoffset < (unsigned)mDisplayW && (unsigned)yoffset < (unsigned)mDisplayH) {
				uint32 pixels[7][7];
				int x = VDFloorToInt((xoffset + 0.5) * (double)output.w / (double)mDisplayW);
				int y = VDFloorToInt((yoffset + 0.5) * (double)output.h / (double)mDisplayH);

				for(int i=0; i<7; ++i) {
					for(int j=0; j<7; ++j) {
						pixels[i][j] = 0xFFFFFF & VDPixmapSample(output, x+j-3, y+3-i);
					}
				}

				POINT pts = pt;
				ClientToScreen(mhdlg, &pts);
				mZoomPopup.Create((VDGUIHandle)mhdlg);
				SetWindowPos(mZoomPopup.GetWindowHandle(), NULL, pts.x + 32, pts.y + 32, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
				mZoomPopup.Update(x, y, pixels);
				ShowWindow(mZoomPopup.GetWindowHandle(), SW_SHOWNOACTIVATE);

				TRACKMOUSEEVENT tme={sizeof(TRACKMOUSEEVENT), TME_LEAVE, mhdlg, 0};
				TrackMouseEvent(&tme);
			} else {
				mZoomPopup.Destroy();
			}
		}
		return 0;

	case WM_MOUSELEAVE:
		mZoomPopup.Destroy();
		break;

	case WM_KEYUP:
		if (wParam == VK_SHIFT)
			mZoomPopup.Destroy();
		break;

	case WM_NOTIFY:
		if (((NMHDR *)lParam)->idFrom == IDC_FILTDLG_POSITION) {
			OnVideoRedraw();
		}
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_FILTDLG_POSITION) {
			VDTranslatePositionCommand(mhdlg, wParam, lParam);
			return TRUE;
		}
		return OnCommand(LOWORD(wParam));

	case MYWM_REDRAW:		// redraw modified frame
		OnVideoRedraw();
		return TRUE;

	case MYWM_RESTART:
		OnVideoResize(false);
		return TRUE;
	}

	return FALSE;
}

void FilterPreview::OnInit() {
	mpTimeline = &g_project->GetTimeline();

	mWidth = 0;
	mHeight = 0;

	mhwndPosition = CreateWindow(POSITIONCONTROLCLASS, NULL, WS_CHILD|WS_VISIBLE, 0, 0, 0, 64, mhdlg, (HMENU)IDC_FILTDLG_POSITION, g_hInst, NULL);
	mpPosition = VDGetIPositionControl((VDGUIHandle)mhwndPosition);

	mpPosition->SetRange(0, mpTimeline->GetLength());
	mpPosition->SetFrameTypeCallback(VDGetPositionControlCallbackTEMP());

	inputVideo->setTargetFormat(0);

	mhwndDisplay = (HWND)VDCreateDisplayWindowW32(0, WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS, 0, 0, 0, 0, (VDGUIHandle)mhdlg);
	if (mhwndDisplay)
		mpDisplay = VDGetIVideoDisplay((VDGUIHandle)mhwndDisplay);
	EnableWindow(mhwndDisplay, FALSE);

	mDlgNode.hdlg = mhdlg;
	mDlgNode.mhAccel = LoadAccelerators(g_hInst, MAKEINTRESOURCE(IDR_PREVIEW_KEYS));
	guiAddModelessDialog(&mDlgNode);
}

void FilterPreview::OnResize() {
	RECT r;

	GetClientRect(mhdlg, &r);

	mDisplayX	= 4;
	mDisplayY	= 4;
	mDisplayW	= r.right - 8;
	mDisplayH	= r.bottom - 72;

	if (mDisplayW < 0)
		mDisplayW = 0;

	if (mDisplayH < 0)
		mDisplayH = 0;

	SetWindowPos(mhwndPosition, NULL, 0, r.bottom - 64, r.right, 64, SWP_NOZORDER|SWP_NOACTIVATE);
	SetWindowPos(mhwndDisplay, NULL, mDisplayX, mDisplayY, mDisplayW, mDisplayH, SWP_NOZORDER|SWP_NOACTIVATE);

	InvalidateRect(mhdlg, NULL, TRUE);
}

void FilterPreview::OnPaint() {
	PAINTSTRUCT ps;

	HDC hdc = BeginPaint(mhdlg, &ps);

	if (!hdc)
		return;

	if (mFiltSys.isRunning()) {
		RECT r;

		GetClientRect(mhdlg, &r);

		Draw3DRect(hdc,	0, 0, r.right, r.bottom - 64, FALSE);
		Draw3DRect(hdc,	3, 3, r.right - 6, r.bottom - 70, TRUE);
	} else {
		RECT r;

		GetWindowRect(mhwndDisplay, &r);
		MapWindowPoints(NULL, mhdlg, (LPPOINT)&r, 2);

		FillRect(hdc, &r, (HBRUSH)(COLOR_3DFACE+1));
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, 0);

		HGDIOBJ hgoFont = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
		char buf[1024];
		const char *s = mFailureReason.gets();
		_snprintf(buf, sizeof buf, "Unable to start filters:\n%s", s?s:"(unknown)");

		RECT r2 = r;
		DrawText(hdc, buf, -1, &r2, DT_CENTER|DT_WORDBREAK|DT_NOPREFIX|DT_CALCRECT);

		int text_h = r2.bottom - r2.top;
		int space_h = r.bottom - r.top;
		if (text_h < space_h)
			r.top += (space_h - text_h) >> 1;

		DrawText(hdc, buf, -1, &r, DT_CENTER|DT_WORDBREAK|DT_NOPREFIX);
		SelectObject(hdc, hgoFont);
	}

	EndPaint(mhdlg, &ps);
}

void FilterPreview::OnVideoResize(bool bInitial) {
	RECT r;
	int w = 320;
	int h = 240;
	bool fResize;

	int oldw = mWidth;
	int oldh = mHeight;

	mWidth = 320;
	mHeight = 240;

	try {
		IVDStreamSource *pVSS = inputVideo->asStream();
		const VDAVIBitmapInfoHeader *pbih2 = inputVideo->getDecompressedFormat();
		VDFraction srcRate = pVSS->getRate();

		if (g_dubOpts.video.mFrameRateAdjustLo > 0)
			srcRate.Assign(g_dubOpts.video.mFrameRateAdjustHi, g_dubOpts.video.mFrameRateAdjustLo);

		sint64 len = pVSS->getLength();
		const VDPixmap& pxsrc = inputVideo->getTargetFormat();

		mFiltSys.prepareLinearChain(
				mpFilterList,
				pbih2->biWidth,
				abs(pbih2->biHeight),
				pxsrc.format,
				srcRate,
				pVSS->getLength());

		if (mFiltSys.GetOutputFrameRate() == srcRate)
			len = mpTimeline->GetLength();

		mFiltSys.initLinearChain(
				mpFilterList,
				pbih2->biWidth,
				abs(pbih2->biHeight),
				pxsrc.format,
				pxsrc.palette,
				srcRate,
				len);

		mFiltSys.ReadyFilters();

		const VDPixmapLayout& output = mFiltSys.GetOutputLayout();
		w = output.w;
		h = output.h;
		mWidth = w;
		mHeight = h;

		mpPosition->SetRange(0, mFiltSys.GetOutputFrameCount());
	} catch(const MyError& e) {
		mpDisplay->Reset();
		ShowWindow(mhwndDisplay, SW_HIDE);
		mFailureReason.assign(e);
		InvalidateRect(mhdlg, NULL, TRUE);
	}

	fResize = oldw != w || oldh != h;

	// if necessary, resize window

	if (fResize) {
		r.left = r.top = 0;
		r.right = w + 8;
		r.bottom = h + 8 + 64;

		AdjustWindowRect(&r, GetWindowLong(mhdlg, GWL_STYLE), FALSE);

		if (bInitial) {
			RECT rParent;
			UINT uiFlags = SWP_NOZORDER|SWP_NOACTIVATE;

			GetWindowRect(mhwndParent, &rParent);

			if (rParent.right + 32 >= GetSystemMetrics(SM_CXSCREEN))
				uiFlags |= SWP_NOMOVE;

			SetWindowPos(mhdlg, NULL,
					rParent.right + 16,
					rParent.top,
					r.right-r.left, r.bottom-r.top,
					uiFlags);
		} else
			SetWindowPos(mhdlg, NULL, 0, 0, r.right-r.left, r.bottom-r.top, SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
	}

	OnVideoRedraw();
}

void FilterPreview::OnVideoRedraw() {
	if (!mFiltSys.isRunning())
		return;

	bool bSuccessful = FetchFrame() >= 0;

	try {
		if (bSuccessful)
			mFiltSys.RunFilters(mLastOutputFrame, mLastTimelineFrame, mLastTimelineFrame, mLastTimelineTimeMS, NULL, VDXFilterStateInfo::kStatePreview);

		if (mpDisplay) {
			ShowWindow(mhwndDisplay, SW_SHOW);

			if (bSuccessful)
				mpDisplay->SetSourcePersistent(false, mFiltSys.GetOutput());
			else
				mpDisplay->SetSourceSolidColor(VDSwizzleU32(GetSysColor(COLOR_3DFACE)) >> 8);

			mpDisplay->Update(IVDVideoDisplay::kAllFields);
		}
	} catch(const MyError& e) {
		mpDisplay->Reset();
		ShowWindow(mhwndDisplay, SW_HIDE);
		mFailureReason.assign(e);
		InvalidateRect(mhdlg, NULL, TRUE);
		UndoSystem();
	}
}

bool FilterPreview::OnCommand(UINT cmd) {
	switch(cmd) {
	case IDCANCEL:
		if (mpButtonCallback)
			mpButtonCallback(false, mpvButtonCBData);

		DestroyWindow(mhdlg);
		mhdlg = NULL;

		UpdateButton();
		return true;

	case ID_EDIT_JUMPTO:
		{
			extern VDPosition VDDisplayJumpToPositionDialog(VDGUIHandle hParent, VDPosition currentFrame, IVDVideoSource *pVS, const VDFraction& realRate);

			VDPosition pos = VDDisplayJumpToPositionDialog((VDGUIHandle)mhdlg, mpPosition->GetPosition(), inputVideo, g_project->GetInputFrameRate());

			mpPosition->SetPosition(pos);
			OnVideoRedraw();
		}
		return true;

	default:
		if (VDHandleTimelineCommand(mpPosition, mpTimeline, cmd)) {
			OnVideoRedraw();
			return true;
		}
	}

	return false;
}

VDPosition FilterPreview::FetchFrame() {
	return FetchFrame(mpPosition->GetPosition());
}

VDPosition FilterPreview::FetchFrame(VDPosition pos) {
	try {
		IVDStreamSource *pVSS = inputVideo->asStream();
		const VDFraction frameRate(pVSS->getRate());

		// This is a pretty awful hack, but gets around the problem that the
		// timeline isn't updated for the new frame rate.
		if (mFiltSys.GetOutputFrameRate() != frameRate)
			mLastOutputFrame = pos;
		else
			mLastOutputFrame = mpTimeline->TimelineToSourceFrame(pos);
		
		mLastTimelineFrame	= pos;
		mLastTimelineTimeMS	= VDRoundToInt64(mFiltSys.GetOutputFrameRate().AsInverseDouble() * 1000.0 * (double)pos);

		sint64 srcFrame = mFiltSys.GetSourceFrame(mLastOutputFrame);

		if (!inputVideo->getFrame(srcFrame))
			return -1;

	} catch(const MyError&) {
		return -1;
	}

	VDPixmapBlt(mFiltSys.GetInput(), inputVideo->getTargetFormat());

	return pos;
}

bool FilterPreview::isPreviewEnabled() {
	return !!mpFilterList;
}

bool FilterPreview::IsPreviewDisplayed() {
	return mhdlg != NULL;
}

void FilterPreview::SetButtonCallback(VDXFilterPreviewButtonCallback pfpbc, void *pvData) {
	mpButtonCallback	= pfpbc;
	mpvButtonCBData	= pvData;
}

void FilterPreview::SetSampleCallback(VDXFilterPreviewSampleCallback pfpsc, void *pvData) {
	mpSampleCallback	= pfpsc;
	mpvSampleCBData	= pvData;
}

void FilterPreview::InitButton(VDXHWND hwnd) {
	mhwndButton = (HWND)hwnd;

	if (hwnd) {
		const VDStringW wintext(VDGetWindowTextW32((HWND)hwnd));

		// look for an accelerator
		mButtonAccelerator = 0;

		if (mpFilterList) {
			int pos = wintext.find(L'&');
			if (pos != wintext.npos) {
				++pos;
				if (pos < wintext.size()) {
					wchar_t c = wintext[pos];

					if (iswalpha(c))
						mButtonAccelerator = towlower(c);
				}
			}
		}

		EnableWindow((HWND)hwnd, mpFilterList ? TRUE : FALSE);
	}
}

void FilterPreview::Toggle(VDXHWND hwndParent) {
	Display(hwndParent, !mhdlg);
}

void FilterPreview::Display(VDXHWND hwndParent, bool fDisplay) {
	if (fDisplay == !!mhdlg)
		return;

	if (mhdlg) {
		DestroyWindow(mhdlg);
		mhdlg = NULL;
		UndoSystem();
	} else if (mpFilterList) {
		mhwndParent = (HWND)hwndParent;
		mhdlg = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_FILTER_PREVIEW), (HWND)hwndParent, StaticDlgProc, (LPARAM)this);
	}

	UpdateButton();

	if (mpButtonCallback)
		mpButtonCallback(!!mhdlg, mpvButtonCBData);
}

void FilterPreview::RedoFrame() {
	if (mhdlg)
		SendMessage(mhdlg, MYWM_REDRAW, 0, 0);
}

void FilterPreview::RedoSystem() {
	if (mhdlg)
		SendMessage(mhdlg, MYWM_RESTART, 0, 0);
}

void FilterPreview::UndoSystem() {
	if (mpDisplay)
		mpDisplay->Reset();
	mFiltSys.DeinitFilters();
	mFiltSys.DeallocateBuffers();
}

void FilterPreview::Close() {
	InitButton(NULL);
	if (mhdlg)
		Toggle(NULL);
	UndoSystem();
}

bool FilterPreview::SampleCurrentFrame() {
	if (!mpFilterList || !mhdlg || !mpSampleCallback)
		return false;

	if (!mFiltSys.isRunning()) {
		RedoSystem();

		if (!mFiltSys.isRunning())
			return false;
	}

	VDPosition pos = FetchFrame();

	if (pos >= 0) {
		try {
			mFiltSys.RunFilters(mLastOutputFrame, mLastTimelineFrame, mLastTimelineFrame, mLastTimelineTimeMS, mpThisFilter, VDXFilterStateInfo::kStatePreview);
			mpSampleCallback(&mpThisFilter->src, (long)pos, (long)mpTimeline->GetLength(), mpvSampleCBData);
		} catch(const MyError& e) {
			e.post(mhdlg, "Video sampling error");
		}
	}

	RedoFrame();

	return true;
}

void FilterPreview::UpdateButton() {
	if (mhwndButton) {
		VDStringW text(mhdlg ? L"Hide preview" : L"Show preview");

		if (mButtonAccelerator) {
			VDStringW::size_type pos = text.find(mButtonAccelerator);

			if (pos == VDStringW::npos)
				pos = text.find(towupper(mButtonAccelerator));

			if (pos != VDStringW::npos)
				text.insert(text.begin() + pos, L'&');
		}

		VDSetWindowTextW32(mhwndButton, text.c_str());
	}
}

///////////////////////

#define FPSAMP_KEYONESEC		(1)
#define	FPSAMP_KEYALL			(2)
#define	FPSAMP_ALL				(3)

static INT_PTR CALLBACK SampleFramesDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch(msg) {
	case WM_COMMAND:
		switch(LOWORD(wParam)) {
		case IDOK:
			if (IsDlgButtonChecked(hdlg, IDC_ONEKEYPERSEC))
				EndDialog(hdlg, FPSAMP_KEYONESEC);
			else if (IsDlgButtonChecked(hdlg, IDC_ALLKEYS))
				EndDialog(hdlg, FPSAMP_KEYALL);
			else
				EndDialog(hdlg, FPSAMP_ALL);
			return TRUE;
		case IDCANCEL:
			EndDialog(hdlg, 0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

long FilterPreview::SampleFrames() {
	static const char *const szCaptions[]={
		"Sampling one keyframe per second",
		"Sampling keyframes only",
		"Sampling all frames",
	};

	int iMode;
	long lCount = 0;

	if (!mpFilterList || !mhdlg || !mpSampleCallback)
		return -1;

	if (!mFiltSys.isRunning()) {
		RedoSystem();

		if (!mFiltSys.isRunning())
			return -1;
	}

	iMode = DialogBox(g_hInst, MAKEINTRESOURCE(IDD_FILTER_SAMPLE), mhdlg, SampleFramesDlgProc);

	if (!iMode)
		return -1;

	// Time to do the actual sampling.

	VDPosition first, last;

	first = mpTimeline->GetStart();
	last = mpTimeline->GetEnd();

	try {
		ProgressDialog pd(mhdlg, "Sampling input video", szCaptions[iMode-1], (long)(last-first), true);
		VDPosition lSample = first;
		IVDStreamSource *pVSS = inputVideo->asStream();
		VDPosition lSecondIncrement = pVSS->msToSamples(1000)-1;

		pd.setValueFormat("Sampling frame %ld of %ld");

		if (lSecondIncrement<0)
			lSecondIncrement = 0;

		while(lSample>=0 && lSample < last) {
			pd.advance((long)(lSample - first));
			pd.check();

			if (FetchFrame(mFiltSys.GetSourceFrame(lSample))>=0) {
				mFiltSys.RunFilters(mLastOutputFrame, mLastTimelineFrame, mLastTimelineFrame, mLastTimelineTimeMS, mpThisFilter, VDXFilterStateInfo::kStatePreview);
				mpSampleCallback(&mpThisFilter->src, (long)(lSample-first), (long)(last-first), mpvSampleCBData);
				++lCount;
			}

			switch(iMode) {
			case FPSAMP_KEYONESEC:
				lSample += lSecondIncrement;
			case FPSAMP_KEYALL:
				lSample = mpTimeline->GetNextKey(lSample);
				break;
			case FPSAMP_ALL:
				++lSample;
				break;
			}
		}
	} catch(MyUserAbortError e) {

		/* so what? */

	} catch(const MyError& e) {
		e.post(mhdlg, "Video sampling error");
	}

	RedoFrame();

	return lCount;
}
