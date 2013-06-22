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

#include "stdafx.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <vfw.h>

#include "resource.h"

#include <vd2/system/list.h>
#include <vd2/system/error.h>
#include <vd2/system/filesys.h>
#include <vd2/system/registry.h>
#include <vd2/system/VDString.h>
#include <vd2/system/log.h>
#include <vd2/system/text.h>
#include <vd2/system/file.h>
#include <vd2/system/w32assist.h>
#include <vd2/system/strutil.h>
#include <vd2/Dita/services.h>
#include "InputFile.h"
#include "AudioFilterSystem.h"

#include "gui.h"
#include "job.h"
#include "command.h"
#include "dub.h"
#include "script.h"
#include "misc.h"
#include "project.h"
#include "filters.h"
#include "oshelper.h"

#include "JobControl.h"

///////////////////////////////////////////////////////////////////////////

extern HINSTANCE g_hInst;
extern FilterFunctions g_filterFuncs;
extern wchar_t g_szInputAVIFile[];
extern wchar_t g_szInputWAVFile[];
extern InputFileOptions *g_pInputOpts;
extern VDProject *g_project;
extern COMPVARS g_Vcompression;
extern VDJobQueue g_VDJobQueue;

///////////////////////////////////////////////////////////////////////////

static INT_PTR CALLBACK JobCtlDlgProc(HWND hdlg, UINT uiMsg, WPARAM wParam, LPARAM lParam);

///////////////////////////////////////////////////////////////////////////

VDStringA VDEncodeBase64A(const void *src, unsigned len) {
	unsigned enclen = (len+2)/3*4;
	std::vector<char> buf(enclen);

	membase64(&buf.front(), (const char *)src, len);

	VDStringA str(&buf.front(), enclen);

	return str;
}

///////////////////////////////////////////////////////////////////////////

class JobScriptOutput {
public:
	typedef vdfastvector<char> Script;

	JobScriptOutput();
	~JobScriptOutput();

	void clear();
	void write(const char *s, long l);
	void adds(const char *s);
	void addf(const char *fmt, ...);

	const char *data() const { return mScript.data(); }
	size_t size() const { return mScript.size(); }

	const Script& getscript();

protected:
	Script		mScript;
};

///////

JobScriptOutput::JobScriptOutput() {
	clear();
}

JobScriptOutput::~JobScriptOutput() {
	clear();
}

void JobScriptOutput::clear() {
	mScript.clear();
}

void JobScriptOutput::write(const char *s, long l) {
	mScript.insert(mScript.end(), s, s+l);
}

void JobScriptOutput::adds(const char *s) {
	write(s, strlen(s));
	write("\r\n",2);
}

void JobScriptOutput::addf(const char *fmt, ...) {
	char buf[8192];
	va_list val;
	long l;

	va_start(val, fmt);
	_vsnprintf(buf, sizeof buf, fmt, val);
	va_end(val);
	buf[sizeof buf-1]=0;

	l = strlen(buf);
	adds(buf);
}

const JobScriptOutput::Script& JobScriptOutput::getscript() {
	return mScript;
}

///////////////////////////////////////////////////////////////////////////

void JobCreateScript(JobScriptOutput& output, const DubOptions *opt, bool bIncludeEditList = true, bool bIncludeTextInfo = true) {
	char *mem= NULL;
	char buf[4096];
	long l;

	int audioSourceMode = g_project->GetAudioSourceMode();

	switch(audioSourceMode) {

	case kVDAudioSourceMode_External:
		{
			const VDStringA& encodedFileName = VDEncodeScriptString(VDStringW(g_szInputWAVFile));
			const VDStringA& encodedDriverName = VDEncodeScriptString(VDTextWToU8(g_project->GetAudioSourceDriverName(), -1));

			// check if we have options to write out
			const InputFileOptions *opts = g_project->GetAudioSourceOptions();
			if (opts) {
				int l;
				char buf[256];

				l = opts->write(buf, (sizeof buf)/7*3);

				if (l) {
					membase64(buf+l, (char *)buf, l);

					output.addf("VirtualDub.audio.SetSource(\"%s\", \"%s\", \"%s\");", encodedFileName.c_str(), encodedDriverName.c_str(), buf+l);
					break;
				}
			}

			// no options
			output.addf("VirtualDub.audio.SetSource(\"%s\", \"%s\");", encodedFileName.c_str(), encodedDriverName.c_str());
		}
		break;

	default:
		if (audioSourceMode >= kVDAudioSourceMode_Source) {
			int index = audioSourceMode - kVDAudioSourceMode_Source;

			if (!index)
				output.addf("VirtualDub.audio.SetSource(1);");
			else
				output.addf("VirtualDub.audio.SetSource(1,%d);", index);
			break;
		}
		// fall through
	case kVDAudioSourceMode_None:
		output.addf("VirtualDub.audio.SetSource(0);");
		break;
	
	}

	output.addf("VirtualDub.audio.SetMode(%d);", opt->audio.mode);

	output.addf("VirtualDub.audio.SetInterleave(%d,%d,%d,%d,%d);",
			opt->audio.enabled,
			opt->audio.preload,
			opt->audio.interval,
			opt->audio.is_ms,
			opt->audio.offset);

	output.addf("VirtualDub.audio.SetClipMode(%d,%d);",
			opt->audio.fStartAudio,
			opt->audio.fEndAudio);

	output.addf("VirtualDub.audio.SetConversion(%d,%d,%d,0,%d);",
			opt->audio.new_rate,
			opt->audio.newPrecision,
			opt->audio.newChannels,
			opt->audio.fHighQuality);

	if (opt->audio.mVolume >= 0.0f)
		output.addf("VirtualDub.audio.SetVolume(%d);", VDRoundToInt(256.0f * opt->audio.mVolume));
	else
		output.addf("VirtualDub.audio.SetVolume();");

	if (g_ACompressionFormat) {
		if (g_ACompressionFormat->mExtraSize) {
			mem = (char *)allocmem(((g_ACompressionFormat->mExtraSize+2)/3)*4 + 1);
			if (!mem) throw MyMemoryError();

			membase64(mem, (char *)(g_ACompressionFormat+1), g_ACompressionFormat->mExtraSize);
			output.addf("VirtualDub.audio.SetCompressionWithHint(%d,%d,%d,%d,%d,%d,%d,\"%s\",\"%s\");"
						,g_ACompressionFormat->mTag
						,g_ACompressionFormat->mSamplingRate
						,g_ACompressionFormat->mChannels
						,g_ACompressionFormat->mSampleBits
						,g_ACompressionFormat->mDataRate
						,g_ACompressionFormat->mBlockSize
						,g_ACompressionFormat->mExtraSize
						,mem
						,VDEncodeScriptString(g_ACompressionFormatHint).c_str()
						);

			freemem(mem);
		} else
			output.addf("VirtualDub.audio.SetCompressionWithHint(%d,%d,%d,%d,%d,%d,\"%s\");"
						,g_ACompressionFormat->mTag
						,g_ACompressionFormat->mSamplingRate
						,g_ACompressionFormat->mChannels
						,g_ACompressionFormat->mSampleBits
						,g_ACompressionFormat->mDataRate
						,g_ACompressionFormat->mBlockSize
						,VDEncodeScriptString(g_ACompressionFormatHint).c_str()
						);
	} else
		output.addf("VirtualDub.audio.SetCompression();");

	output.addf("VirtualDub.audio.EnableFilterGraph(%d);", opt->audio.bUseAudioFilterGraph);

	output.addf("VirtualDub.video.SetInputFormat(%d);", opt->video.mInputFormat);
	output.addf("VirtualDub.video.SetOutputFormat(%d);", opt->video.mOutputFormat);

	output.addf("VirtualDub.video.SetMode(%d);", opt->video.mode);
	output.addf("VirtualDub.video.SetSmartRendering(%d);", opt->video.mbUseSmartRendering);
	output.addf("VirtualDub.video.SetPreserveEmptyFrames(%d);", opt->video.mbPreserveEmptyFrames);

	output.addf("VirtualDub.video.SetFrameRate2(%u,%u,%d);",
			opt->video.mFrameRateAdjustHi,
			opt->video.mFrameRateAdjustLo,
			opt->video.frameRateDecimation);

	if (opt->video.frameRateTargetLo) {
		output.addf("VirtualDub.video.SetTargetFrameRate(%u,%u);",
				opt->video.frameRateTargetHi,
				opt->video.frameRateTargetLo);
	}

	output.addf("VirtualDub.video.SetIVTC(%d,%d,%d,%d);",
			opt->video.fInvTelecine,
			opt->video.fIVTCMode,
			opt->video.nIVTCOffset,
			opt->video.fIVTCPolarity);

	if ((g_Vcompression.dwFlags & ICMF_COMPVARS_VALID) && g_Vcompression.fccHandler) {
		output.addf("VirtualDub.video.SetCompression(0x%08lx,%d,%d,%d);",
				g_Vcompression.fccHandler,
				g_Vcompression.lKey,
				g_Vcompression.lQ,
				g_Vcompression.lDataRate);

		l = ICGetStateSize(g_Vcompression.hic);

		if (l>0) {
			mem = (char *)allocmem(l + ((l+2)/3)*4 + 1);
			if (!mem) throw MyMemoryError();

			if (ICGetState(g_Vcompression.hic, mem, l)<0) {
				freemem(mem);
//				throw MyError("Bad state data returned from compressor");

				// Fine then, be that way.  Stupid Pinnacle DV200 driver.
				mem = NULL;
			}

			if (mem) {
				membase64(mem+l, mem, l);
				// urk... Windows Media 9 VCM uses a very large configuration struct (~7K pre-BASE64).
				sprintf(buf, "VirtualDub.video.SetCompData(%d,\"", l);

				VDStringA line(buf);
				line += (mem+l);
				line += "\");";
				output.adds(line.c_str());
				freemem(mem);
			}
		}

	} else
		output.addf("VirtualDub.video.SetCompression();");

	output.addf("VirtualDub.video.filters.Clear();");

	// Add video filters

	FilterInstance *fa = (FilterInstance *)g_listFA.tail.next, *fa_next;
	int iFilter = 0;

	while(fa_next = (FilterInstance *)fa->next) {
		output.addf("VirtualDub.video.filters.Add(\"%s\");", strCify(fa->filter->name));

		if (fa->mCropX1 || fa->mCropY1 || fa->mCropX2 || fa->mCropY2)
			output.addf("VirtualDub.video.filters.instance[%d].SetClipping(%d,%d,%d,%d%s);"
						,iFilter
						,fa->mCropX1
						,fa->mCropY1
						,fa->mCropX2
						,fa->mCropY2
						,fa->mbPreciseCrop ? "" : ",0"
						);

		if (fa->filter->fssProc && fa->filter->fssProc(fa->AsVDXFilterActivation(), &g_filterFuncs, buf, sizeof buf))
			output.addf("VirtualDub.video.filters.instance[%d].%s;", iFilter, buf);

		if (!fa->IsEnabled())
			output.addf("VirtualDub.video.filters.instance[%d].SetEnabled(false);", iFilter);

		VDParameterCurve *pc = fa->GetAlphaParameterCurve();
		if (pc) {
			output.addf("declare curve = VirtualDub.video.filters.instance[%d].AddOpacityCurve();", iFilter);

			const VDParameterCurve::PointList& pts = pc->Points();
			for(VDParameterCurve::PointList::const_iterator it(pts.begin()), itEnd(pts.end()); it!=itEnd; ++it) {
				const VDParameterCurvePoint& pt = *it;

				output.addf("curve.AddPoint(%g, %g, %d);", pt.mX, pt.mY, pt.mbLinear);
			}
		}

		++iFilter;
		fa = fa_next;
	}

	// Add audio filters

	{
		VDAudioFilterGraph::FilterList::const_iterator it(g_audioFilterGraph.mFilters.begin()), itEnd(g_audioFilterGraph.mFilters.end());
		int connidx = 0;
		int srcfilt = 0;

		output.addf("VirtualDub.audio.filters.Clear();");

		for(; it!=itEnd; ++it, ++srcfilt) {
			const VDAudioFilterGraph::FilterEntry& fe = *it;

			output.addf("VirtualDub.audio.filters.Add(\"%s\");", strCify(VDTextWToU8(fe.mFilterName).c_str()));

			for(unsigned i=0; i<fe.mInputPins; ++i) {
				const VDAudioFilterGraph::FilterConnection& conn = g_audioFilterGraph.mConnections[connidx++];
				output.addf("VirtualDub.audio.filters.Connect(%d, %d, %d, %d);", conn.filt, conn.pin, srcfilt, i);
			}

			VDPluginConfig::const_iterator itc(fe.mConfig.begin()), itcEnd(fe.mConfig.end());

			for(; itc!=itcEnd; ++itc) {
				const unsigned idx = (*itc).first;
				const VDPluginConfigVariant& var = (*itc).second;

				switch(var.GetType()) {
				case VDPluginConfigVariant::kTypeU32:
					output.addf("VirtualDub.audio.filters.instance[%d].SetInt(%d, %d);", srcfilt, idx, var.GetU32());
					break;
				case VDPluginConfigVariant::kTypeS32:
					output.addf("VirtualDub.audio.filters.instance[%d].SetInt(%d, %d);", srcfilt, idx, var.GetS32());
					break;
				case VDPluginConfigVariant::kTypeU64:
					output.addf("VirtualDub.audio.filters.instance[%d].SetLong(%d, %I64d);", srcfilt, idx, var.GetU64());
					break;
				case VDPluginConfigVariant::kTypeS64:
					output.addf("VirtualDub.audio.filters.instance[%d].SetLong(%d, %I64d);", srcfilt, idx, var.GetS64());
					break;
				case VDPluginConfigVariant::kTypeDouble:
					output.addf("VirtualDub.audio.filters.instance[%d].SetDouble(%d, %g);", srcfilt, idx, var.GetDouble());
					break;
				case VDPluginConfigVariant::kTypeAStr:
					output.addf("VirtualDub.audio.filters.instance[%d].SetString(%d, \"%s\");", srcfilt, idx, strCify(VDTextWToU8(VDTextAToW(var.GetAStr())).c_str()));
					break;
				case VDPluginConfigVariant::kTypeWStr:
					output.addf("VirtualDub.audio.filters.instance[%d].SetString(%d, \"%s\");", srcfilt, idx, strCify(VDTextWToU8(var.GetWStr(), -1).c_str()));
					break;
				case VDPluginConfigVariant::kTypeBlock:
					output.addf("VirtualDub.audio.filters.instance[%d].SetBlock(%d, %d, \"%s\");", srcfilt, idx, var.GetBlockLen(), VDEncodeBase64A(var.GetBlockPtr(), var.GetBlockLen()).c_str());
					break;
				}
			}
		}
	}

	// Add subset information

	if (bIncludeEditList) {
		const FrameSubset& fs = g_project->GetTimeline().GetSubset();

		output.addf("VirtualDub.subset.Clear();");

		for(FrameSubset::const_iterator it(fs.begin()), itEnd(fs.end()); it!=itEnd; ++it)
			output.addf("VirtualDub.subset.Add%sRange(%I64d,%I64d);", it->bMask ? "Masked" : "", it->start, it->len);

		// Note that this must be AFTER the subset (we used to place it before, which was a bug).
		if (g_project->IsSelectionPresent()) {
			output.addf("VirtualDub.video.SetRangeFrames(%I64d,%I64d);",
				g_project->GetSelectionStartFrame(),
				g_project->GetSelectionEndFrame());
		} else {
			output.addf("VirtualDub.video.SetRange();");
		}
	}

	// Add text information
	if (bIncludeTextInfo) {
		typedef std::list<std::pair<uint32, VDStringA> > tTextInfo;
		const tTextInfo& textInfo = g_project->GetTextInfo();

		output.addf("VirtualDub.project.ClearTextInfo();");
		for(tTextInfo::const_iterator it(textInfo.begin()), itEnd(textInfo.end()); it!=itEnd; ++it) {
			char buf[5]={0};
			
			memcpy(buf, &(*it).first, 4);

			output.addf("VirtualDub.project.AddTextInfo(\"%s\", \"%s\");", buf, VDEncodeScriptString((*it).second).c_str());
		}
	}
}

void JobAddConfigurationInputs(JobScriptOutput& output, const wchar_t *szFileInput, const wchar_t *pszInputDriver, List2<InputFilenameNode> *pListAppended) {
	do {
		const VDStringA filename(strCify(VDTextWToU8(VDStringW(szFileInput)).c_str()));

		if (g_pInputOpts) {
			int req = g_pInputOpts->write(NULL, 0);

			vdfastvector<char> srcbuf(req);

			int srcsize = g_pInputOpts->write(srcbuf.data(), req);

			if (srcsize) {
				vdfastvector<char> encbuf((srcsize + 2) / 3 * 4 + 1);
				membase64(encbuf.data(), srcbuf.data(), srcsize);

				const VDStringA filename(strCify(VDTextWToU8(VDStringW(szFileInput)).c_str()));

				output.addf("VirtualDub.Open(\"%s\",\"%s\",0,\"%s\");", filename.c_str(), pszInputDriver?strCify(VDTextWToU8(VDStringW(pszInputDriver)).c_str()):"", encbuf.data());
				break;
			}
		}

		output.addf("VirtualDub.Open(\"%s\",\"%s\",0);", filename.c_str(), pszInputDriver?strCify(VDTextWToU8(VDStringW(pszInputDriver)).c_str()):"");

	} while(false);

	if (pListAppended) {
		InputFilenameNode *ifn = pListAppended->AtHead(), *ifn_next;

		if (ifn = ifn->NextFromHead())
			while(ifn_next = ifn->NextFromHead()) {
				output.addf("VirtualDub.Append(\"%s\");", strCify(VDTextWToU8(VDStringW(ifn->name)).c_str()));
				ifn = ifn_next;
			}
	}
}

void JobAddConfiguration(const DubOptions *opt, const wchar_t *szFileInput, const wchar_t *pszInputDriver, const wchar_t *szFileOutput, bool fCompatibility, List2<InputFilenameNode> *pListAppended, long lSpillThreshold, long lSpillFrameThreshold, bool bIncludeEditList) {
	VDJob *vdj = new VDJob;
	JobScriptOutput output;

	try {
		JobAddConfigurationInputs(output, szFileInput, pszInputDriver, pListAppended);
		JobCreateScript(output, opt, bIncludeEditList);

		// Add magic flag
		output.adds("  // -- $reloadstop --");

		// Add actual run option

		if (lSpillThreshold)
			output.addf("VirtualDub.SaveSegmentedAVI(\"%s\", %d, %d);", strCify(VDTextWToU8(VDStringW(szFileOutput)).c_str()), lSpillThreshold, lSpillFrameThreshold);
		else
			output.addf("VirtualDub.Save%sAVI(\"%s\");", fCompatibility ? "Compatible" : "", strCify(VDTextWToU8(VDStringW(szFileOutput)).c_str()));
		output.adds("VirtualDub.audio.SetSource(1);");		// required to close a WAV file
		output.adds("VirtualDub.Close();");

		///////////////////

		vdj->SetInputFile(VDTextWToA(szFileInput).c_str());
		vdj->SetOutputFile(VDTextWToA(szFileOutput).c_str());

		const JobScriptOutput::Script& script = output.getscript();
		vdj->SetScript(script.data(), script.size(), true);
		g_VDJobQueue.Add(vdj, false);
	} catch(...) {
		freemem(vdj);
		throw;
	}
}

void JobAddConfigurationImages(const DubOptions *opt, const wchar_t *szFileInput, const wchar_t *pszInputDriver, const wchar_t *szFilePrefix, const wchar_t *szFileSuffix, int minDigits, int imageFormat, int quality, List2<InputFilenameNode> *pListAppended) {
	VDJob *vdj = new VDJob;
	JobScriptOutput output;

	try {
		JobAddConfigurationInputs(output, szFileInput, pszInputDriver, pListAppended);
		JobCreateScript(output, opt);

		// Add magic flag
		output.adds("  // -- $reloadstop --");

		// Add actual run option

		VDStringA s(strCify(VDTextWToU8(VDStringW(szFilePrefix)).c_str()));

		output.addf("VirtualDub.SaveImageSequence(\"%s\", \"%s\", %d, %d, %d);", s.c_str(), strCify(VDTextWToU8(VDStringW(szFileSuffix)).c_str()), minDigits, imageFormat, quality);

		output.adds("VirtualDub.audio.SetSource(1);");		// required to close a WAV file
		output.adds("VirtualDub.Close();");

		///////////////////

		vdj->SetInputFile(VDTextWToA(szFileInput).c_str());
		VDStringA outputFile;
		outputFile.sprintf("%ls*%ls", szFilePrefix, szFileSuffix);
		vdj->SetOutputFile(outputFile.c_str());

		const JobScriptOutput::Script& script = output.getscript();
		vdj->SetScript(script.data(), script.size(), true);
		g_VDJobQueue.Add(vdj, false);
	} catch(...) {
		freemem(vdj);
		throw;
	}
}

void JobWriteConfiguration(const wchar_t *filename, DubOptions *opt, bool bIncludeEditList, bool bIncludeTextInfo) {
	JobScriptOutput output;

	JobCreateScript(output, opt, bIncludeEditList, bIncludeTextInfo);

	VDFile f(filename, nsVDFile::kWrite | nsVDFile::kDenyAll | nsVDFile::kCreateAlways);
	f.write(output.data(), output.size());
}

///////////////////////////////////////////////////////////////////////////

bool InitJobSystem() {
	g_VDJobQueue.SetJobFilePath(NULL, false);

	return true;
}

void DeinitJobSystem() {
	g_VDJobQueue.Flush();
	g_VDJobQueue.Shutdown();
}

void JobLockDubber() {
	g_VDJobQueue.SetBlocked(true);
}

void JobUnlockDubber() {
	g_VDJobQueue.SetBlocked(false);
}

void JobClearList() {
	g_VDJobQueue.ListClear();
}

void JobRunList() {
	g_VDJobQueue.RunAll();
}

bool JobPollAutoRun() {
	return g_VDJobQueue.PollAutoRun();
}

void JobSetQueueFile(const wchar_t *filename, bool distributed, bool autorun) {
	g_VDJobQueue.SetAutoRunEnabled(false);
	g_VDJobQueue.SetJobFilePath(filename, distributed);
	g_VDJobQueue.SetAutoRunEnabled(autorun);
}

void JobAddBatchFile(const wchar_t *lpszSrc, const wchar_t *lpszDst) {
	JobAddConfiguration(&g_dubOpts, lpszSrc, 0, lpszDst, false, NULL, 0, 0, false);
}

void JobAddBatchDirectory(const wchar_t *lpszSrc, const wchar_t *lpszDst) {
	// Scan source directory

	HANDLE				h;
	WIN32_FIND_DATA		wfd;
	wchar_t *s, *t;
	wchar_t szSourceDir[MAX_PATH], szDestDir[MAX_PATH];

	wcsncpyz(szSourceDir, lpszSrc, MAX_PATH);
	wcsncpyz(szDestDir, lpszDst, MAX_PATH);

	s = szSourceDir;
	t = szDestDir;

	if (*s) {

		// If the path string is just \ or starts with x: or ends in a slash
		// then don't append a slash

		while(*s) ++s;

		if ((s==szSourceDir || s[-1]!=L'\\') && (!isalpha(szSourceDir[0]) || szSourceDir[1]!=L':' || szSourceDir[2]))
			*s++ = L'\\';

	}
	
	if (*t) {

		// If the path string is just \ or starts with x: or ends in a slash
		// then don't append a slash

		while(*t) ++t;

		if ((t==szDestDir || t[-1]!=L'\\') && (!isalpha(szDestDir[0]) || szDestDir[1]!=L':' || szDestDir[2]))
			*t++ = L'\\';

	}

	wcscpy(s, L"*.*");

	h = FindFirstFile(VDTextWToA(szSourceDir).c_str(),&wfd);

	if (INVALID_HANDLE_VALUE != h) {
		do {
			if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				wchar_t *t2, *dot = NULL;

				VDTextAToW(s, (szSourceDir+MAX_PATH) - s, wfd.cFileName, -1);
				VDTextAToW(t, (szDestDir+MAX_PATH) - t, wfd.cFileName, -1);

				// Replace extension with .avi

				t2 = t;
				while(*t2) if (*t2++ == '.') dot = t2;

				if (dot)
					wcscpy(dot, L"avi");
				else
					wcscpy(t2, L".avi");

				// Add job!

				JobAddConfiguration(&g_dubOpts, szSourceDir, 0, szDestDir, false, NULL, 0, 0, false);
			}
		} while(FindNextFile(h,&wfd));
		FindClose(h);
	}
}
