#include "stdafx.h"
#include "JobControl.h"
#include <vd2/system/filesys.h>
#include <vd2/system/file.h>
#include <vd2/system/hash.h>
#include <vd2/system/registry.h>
#include <vd2/system/time.h>
#include <vd2/system/w32assist.h>
#include <hash_map>

#include "misc.h"
#include "oshelper.h"
#include "script.h"
#include "dub.h"
#include "project.h"
#include "command.h"

extern const char g_szError[];
extern HWND g_hWnd;
extern VDProject *g_project;

HWND g_hwndJobs;

bool g_fJobMode;

VDJobQueue g_VDJobQueue;

static const char g_szRegKeyShutdownWhenFinished[] = "Shutdown after jobs finish";

IVDJobQueueStatusCallback *g_pVDJobQueueStatusCallback;

void JobPositionCallback(VDPosition start, VDPosition cur, VDPosition end, int progress, void *cookie) {
	if (g_pVDJobQueueStatusCallback)
		g_pVDJobQueueStatusCallback->OnJobProgressUpdate(*g_VDJobQueue.GetCurrentlyRunningJob(), (float)progress / 8192.0f);
}


bool VDUIRequestSystemShutdown(VDGUIHandle hParent);

///////////////////////////////////////////////////////////////////////////////

VDJobQueue::VDJobQueue() 
	: mJobCount(0)
	, mJobNumber(1)
	, mpRunningJob(NULL)
	, mbRunning(false)
	, mbRunAllStop(false)
	, mbModified(false)
	, mbBlocked(false)
	, mbOrderModified(false)
	, mbAutoRun(false)
	, mLastSignature(0)
	, mLastRevision(0)
{
	char name[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD len = MAX_COMPUTERNAME_LENGTH + 1;

	mComputerName = "unnamed";

	mRunnerId = (uint32)GetCurrentProcessId();
	mBaseSignature = (uint64)mRunnerId << 32;
	if (GetComputerName(name, &len)) {
		mComputerName = name;

		uint64 computerHash = (uint64)VDHashString32(name) << 32;
		mBaseSignature ^= computerHash;
		mRunnerId += computerHash;
	}

	mDefaultJobFilePath = VDMakePath(VDGetProgramPath().c_str(), L"VirtualDub.jobs");
}

VDJobQueue::~VDJobQueue() {
	Shutdown();
}

void VDJobQueue::Shutdown() {
	mpRunningJob = NULL;

	while(!mJobQueue.empty()) {
		delete mJobQueue.back();
		mJobQueue.pop_back();
	}

	mJobCount = 0;
}

const wchar_t *VDJobQueue::GetJobFilePath() const {
	return mJobFilePath.c_str();
}

const wchar_t *VDJobQueue::GetDefaultJobFilePath() const {
	return mDefaultJobFilePath.c_str();
}

void VDJobQueue::SetJobFilePath(const wchar_t *path, bool enableDistributedMode) {
	if (mpRunningJob) {
		VDASSERT(!"Can't change job file path while job is running.");
		return;
	}

	SetAutoUpdateEnabled(false);

	Shutdown();

	if (path)
		mJobFilePath = path;
	else
		mJobFilePath = mDefaultJobFilePath;

	SetAutoUpdateEnabled(enableDistributedMode);
	mLastSignature = 0;
	mLastRevision = 0;

	ListLoad(NULL, false);

	if (g_pVDJobQueueStatusCallback) {
		g_pVDJobQueueStatusCallback->OnJobQueueReloaded();
		g_pVDJobQueueStatusCallback->OnJobQueueStatusChanged(GetQueueStatus());
	}
}

int32 VDJobQueue::GetJobIndexById(uint64 id) const {
	int32 index = 0;
	JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
	for(; it != itEnd; ++it, ++index) {
		VDJob *job = *it;

		if (job->mId == id)
			return index;
	}

	return -1;
}

VDJob *VDJobQueue::GetJobById(uint64 id) const {
	JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
	for(; it != itEnd; ++it) {
		VDJob *job = *it;

		if (job->mId == id)
			return job;
	}

	return NULL;
}

VDJob *VDJobQueue::ListGet(int index) {
	if ((unsigned)index >= mJobQueue.size())
		return NULL;

	return mJobQueue[index];
}

int VDJobQueue::ListFind(VDJob *vdj_find) {
	JobQueue::const_iterator it(std::find(mJobQueue.begin(), mJobQueue.end(), vdj_find));

	if (it == mJobQueue.end())
		return -1;

	return it - mJobQueue.begin();
}

long VDJobQueue::ListSize() {
	return mJobCount;
}

// VDJobQueue::ListClear()
//
// Clears all jobs from the list.

void VDJobQueue::ListClear(bool force_no_update) {
	for(int i=mJobQueue.size()-1; i>=0; --i) {
		VDJob *vdj = mJobQueue[i];

		if (vdj->GetState() != VDJob::kStateInProgress) {
			VDASSERT(vdj->mpJobQueue == this);
			vdj->mpJobQueue = NULL;

			mJobQueue.erase(mJobQueue.begin() + i);
			--mJobCount;
			
			delete vdj;
		}
	}

	if (g_pVDJobQueueStatusCallback)
		g_pVDJobQueueStatusCallback->OnJobQueueReloaded();

	if (!force_no_update)
		SetModified();
}

void VDJobQueue::Refresh(VDJob *job) {
	int index = ListFind(job);

	if (index>=0) {
		if (g_pVDJobQueueStatusCallback)
			g_pVDJobQueueStatusCallback->OnJobUpdated(*job, index);
	}
}

void VDJobQueue::Add(VDJob *job, bool force_no_update) {
	VDASSERT(!job->mpJobQueue);
	job->mpJobQueue = this;

	if (!*job->GetName()) {
		VDStringA name;
		name.sprintf("Job %d", mJobNumber++);
		job->SetName(name.c_str());
	}

	if (job->mId == 0)
		job->mId = GetUniqueId();

	job->mCreationRevision = 0;
	job->mChangeRevision = 0;

	mJobQueue.push_back(job);
	++mJobCount;

	if (g_pVDJobQueueStatusCallback)
		g_pVDJobQueueStatusCallback->OnJobAdded(*job, mJobCount - 1);

	if (!force_no_update) SetModified();
}

void VDJobQueue::Delete(VDJob *job, bool force_no_update) {
	VDASSERT(job->mpJobQueue == this);
	job->mpJobQueue = NULL;

	int index = ListFind(job);

	if (index >= 0) {
		if (g_pVDJobQueueStatusCallback)
			g_pVDJobQueueStatusCallback->OnJobRemoved(*job, index);
	}

	mJobQueue.erase(mJobQueue.begin() + index);
	--mJobCount;
	
	if (!force_no_update) SetModified();
}

void VDJobQueue::Run(VDJob *job) {
	job->SetState(VDJob::kStateInProgress);

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);

	job->mDateStart = ((uint64)ft.dwHighDateTime << 32) + (uint32)ft.dwLowDateTime;
	job->mDateEnd = 0;
	job->SetState(VDJob::kStateInProgress);
	job->SetRunner(mRunnerId, mComputerName.c_str());

	job->Refresh();
	Flush();

	if (!job->IsRunning() || !job->IsLocal())
		return;

	mpRunningJob = job;
	if (g_pVDJobQueueStatusCallback) {
		NotifyStatus();
		g_pVDJobQueueStatusCallback->OnJobStarted(*job);
		g_pVDJobQueueStatusCallback->OnJobProgressUpdate(*job, 0.0f);
	}

	try {
		g_fJobMode = true;

		VDAutoLogger logger(kVDLogWarning);

		RunScriptMemory(job->GetScript(), false);

		job->mLogEntries = logger.GetEntries();
	} catch(const MyUserAbortError&) {
		job->SetState(VDJob::kStateAborted);
	} catch(const MyError& err) {
		job->SetState(VDJob::kStateError);
		job->SetError(err.gets());
	}

	if (g_project) {
		g_project->Close();
		g_project->SetAudioSourceNormal(0);
	}

	g_fJobMode = false;

	if (g_pVDJobQueueStatusCallback)
		g_pVDJobQueueStatusCallback->OnJobEnded(*job);
	mpRunningJob = NULL;

	if (job->GetState() == VDJob::kStateInProgress)
		job->SetState(VDJob::kStateCompleted);

	GetSystemTimeAsFileTime(&ft);

	job->mDateEnd = ((uint64)ft.dwHighDateTime << 32) + (uint32)ft.dwLowDateTime;
	SetModified();

	job->Refresh();

	try {
		Flush();
	} catch(const MyError&) {
		// Eat errors from the job flush.  The job queue on disk may be messed
		// up, but as long as our in-memory queue is OK, we can at least finish
		// remaining jobs.

		VDASSERT(false);		// But we'll at least annoy people with debuggers.
	}
}

void VDJobQueue::Reload(VDJob *job) {
	// safety guard
	if (mbRunning)
		return;

	mbRunning	= true;
	mbRunAllStop		= false;

	bool wasEnabled = !(GetWindowLong(g_hwndJobs, GWL_STYLE) & WS_DISABLED);
	if (g_hwndJobs)
		EnableWindow(g_hwndJobs, FALSE);

	MyError err;
	try {
		RunScriptMemory(job->GetScript(), true);
	} catch(MyError& e) {
		err.TransferFrom(e);
	}

	mbRunning = false;

	// re-enable job window
	if (g_hwndJobs && wasEnabled)
		EnableWindow(g_hwndJobs, TRUE);

	if (err.gets())
		err.post(g_hwndJobs, g_szError);
}

void VDJobQueue::Transform(int fromState, int toState) {
	bool modified = false;

	for(uint32 i=0; i<mJobCount; ++i) {
		VDJob *job = mJobQueue[i];

		if (job->GetState() == fromState) {
			job->SetState(toState);
			job->mChangeRevision = 0;
			modified = true;
			job->Refresh();
		}
	}

	SetModified();
}

// VDJobQueue::ListLoad()
//
// Loads the list from a file.

static char *findcmdline(char *s) {
	while(isspace((unsigned char)*s)) ++s;

	if (s[0] != '/' || s[1] != '/')
		return NULL;

	s+=2;

	while(isspace((unsigned char)*s)) ++s;
	if (*s++ != '$') return NULL;

	return s;
}

static void strgetarg(VDStringA& str, const char *s) {
	const char *t = s;

	if (*t == '"') {
		s = ++t;
		while(*s && *s!='"') ++s;
	} else
		while(*s && !isspace((unsigned char)*s)) ++s;

	str.assign(t, s);
}

static void strgetarg2(VDStringA& str, const char *s) {
	static char hexdig[]="0123456789ABCDEF";
	vdfastvector<char> buf;
	char stopchar = 0;

	if (*s == '"') {
		++s;
		stopchar = '"';
	}

	buf.reserve(strlen(s));

	while(char c = *s++) {
		if (c == stopchar)
			break;

		if (c=='\\') {
			switch(c=*s++) {
			case 'a': c='\a'; break;
			case 'b': c='\b'; break;
			case 'f': c='\f'; break;
			case 'n': c='\n'; break;
			case 'r': c='\r'; break;
			case 't': c='\t'; break;
			case 'v': c='\v'; break;
			case 'x':
				c = (char)(strchr(hexdig,toupper(s[0]))-hexdig);
				c = (char)((c<<4) | (strchr(hexdig,toupper(s[1]))-hexdig));
				s += 2;
				break;
			}
		}

		if (!c)
			break;

		buf.push_back(c);
	}

	str.assign(buf.data(), buf.size());
}

void VDJobQueue::ListLoad(const wchar_t *fileName, bool merge) {
	vdautoptr<VDJob> job;

	// Try to create VirtualDub.jobs in the same directory as VirtualDub.

	bool usingGlobalFile = false;
	if (!fileName) {
		usingGlobalFile = true;

		fileName = mJobFilePath.c_str();
	}

	try {
		VDFileStream fileStream(fileName);

		if (!Load(&fileStream, merge) || !merge) {
			mbOrderModified = false;
			mbModified = false;
		}
	} catch(const MyError& e) {
		if (!usingGlobalFile)
			throw MyError("Failure loading job list: %s.", e.c_str());
	}
}

bool VDJobQueue::Load(IVDStream *stream, bool merge) {
	JobQueue newJobs;
	vdautoptr<VDJob> job;

	bool modified = false;
	try {
		bool script_capture = false;
		bool script_reloadable = false;
		vdfastvector<char> script;

		uint64 newSignature		= mBaseSignature;
		uint32 newRevision		= 1;

		VDTextStream input(stream);

		vdfastvector<char> linebuffer;

		for(;;) {
			// read in the line

			const char *line = input.GetNextLine();
			if (!line)
				break;

			linebuffer.assign(line, line+strlen(line)+1);

			char *s = linebuffer.data();

			// scan for a command

			if (s = findcmdline(linebuffer.data())) {
				char *t = s;

				while(isalpha((unsigned char)*t) || *t=='_') ++t;

				if (*t) *t++=0;
				while(isspace((unsigned char)*t)) ++t;

				if (!_stricmp(s, "signature")) {
					uint64 sig;
					uint32 revision;

					if (2 != sscanf(t, "%llx %x", &sig, &revision))
						throw MyError("invalid signature");

					if (merge && sig == mLastSignature && revision == mLastRevision)
						return false;

					newSignature = sig;
					newRevision = revision;
				} else if (!_stricmp(s, "job")) {
					job = new_nothrow VDJob;
					if (!job) throw MyError("out of memory");

					job->mpJobQueue			= this;
					job->mCreationRevision	= newRevision;
					job->mChangeRevision	= newRevision;

					VDStringA name;
					strgetarg(name, t);
					job->SetName(name.c_str());

				} else if (!_stricmp(s, "input")) {

					VDStringA inputFile;
					strgetarg(inputFile, t);
					job->SetInputFile(inputFile.c_str());

				} else if (!_stricmp(s, "output")) {

					VDStringA outputFile;
					strgetarg(outputFile, t);
					job->SetOutputFile(outputFile.c_str());

				} else if (!_stricmp(s, "error")) {

					VDStringA error;
					strgetarg2(error, t);
					job->SetError(error.c_str());

				} else if (!_stricmp(s, "state")) {

					job->SetState(atoi(t));

				} else if (!_stricmp(s, "id")) {

					uint64 id;
					if (1 != sscanf(t, "%llx", &id))
						throw MyError("invalid ID");

					job->mId = id;

				} else if (!_stricmp(s, "runner_id")) {

					uint64 id;
					if (1 != sscanf(t, "%llx", &id))
						throw MyError("invalid runner ID");

					job->mRunnerId = id;

				} else if (!_stricmp(s, "runner_name")) {

					strgetarg2(job->mRunnerName, t);

				} else if (!_stricmp(s, "revision")) {

					uint32 createrev, changerev;

					if (2 != sscanf(t, "%x %x", &createrev, &changerev))
						throw MyError("invalid revisions");

					job->mCreationRevision = createrev;
					job->mChangeRevision = changerev;

				} else if (!_stricmp(s, "start_time")) {
					uint32 lo, hi;

					if (2 != sscanf(t, "%08lx %08lx", &hi, &lo))
						throw MyError("invalid start time");

					job->mDateStart = ((uint64)hi << 32) + lo;
				} else if (!_stricmp(s, "end_time")) {
					uint32 lo, hi;

					if (2 != sscanf(t, "%08lx %08lx", &hi, &lo))
						throw MyError("invalid start time");

					job->mDateEnd = ((uint64)hi << 32) + lo;

				} else if (!_stricmp(s, "script")) {

					script_capture = true;
					script_reloadable = false;
					script.clear();

				} else if (!_stricmp(s, "endjob")) {
					if (script_capture) {
						job->SetScript(script.begin(), script.size(), script_reloadable);
						script_capture = false;
					}

					// Check if the job is running and if the process corresponds to the same machine as
					// us. If so, check if the process is still running and if not, mark the job as aborted.

					int state = job->GetState();
					if (state == VDJob::kStateInProgress || state == VDJob::kStateAborting) {
						if (job->mRunnerId && job->mRunnerId != mRunnerId) {
							uint32 machineid = (uint32)(job->mRunnerId >> 32);
							if (machineid == (uint32)(mRunnerId >> 32)) {
								uint32 pid = (uint32)job->mRunnerId;
								HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);

								bool processExists = false;
								if (hProcess) {
									if (WAIT_TIMEOUT == WaitForSingleObject(hProcess, 0))
										processExists = true;

									CloseHandle(hProcess);
								}

								if (!processExists)
									job->SetState(VDJob::kStateAborted);
							}
						} else {
							if (!mpRunningJob || mpRunningJob->mId != job->mId)
								job->SetState(VDJob::kStateAborted);
						}
					}

					newJobs.push_back(job);
					job.release();

				} else if (!_stricmp(s, "logent")) {

					int severity;
					char dummyspace;
					int pos;

					if (2 != sscanf(t, "%d%c%n", &severity, &dummyspace, &pos))
						throw MyError("invalid log entry");

					job->mLogEntries.push_back(VDJob::tLogEntries::value_type(severity, VDTextAToW(t + pos, -1)));
				}
			} else if (script_capture) {
				// kill starting spaces

				s = linebuffer.data();

				while(isspace((unsigned char)*s)) ++s;

				// check for reload marker
				if (s[0] == '/' && s[1] == '/' && strstr(s, "$reloadstop"))
					script_reloadable = true;

				// don't add blank lines

				if (*s) {
					script.insert(script.end(), s, s+strlen(s));
					script.push_back('\r');
					script.push_back('\n');
				}
			}
		}

		if (merge) {
			// Merge the in-memory and on-disk job queues.
			typedef stdext::hash_map<uint64, int> JobQueueLookup;

			bool useJobsFromDstQueue = mbOrderModified;
			JobQueue dstQueue;
			JobQueue srcQueue;
			JobQueue dumpQueue;
			uint32 srcRevision;

			if (useJobsFromDstQueue) {
				dstQueue = mJobQueue;
				srcQueue = newJobs;
				srcRevision = newRevision;
			} else {
				dstQueue = newJobs;
				srcQueue = mJobQueue;
				srcRevision = mLastRevision;
			}

			// build lookup for src queue
			JobQueueLookup srcJobLookup;

			for(JobQueue::const_iterator it(srcQueue.begin()), itEnd(srcQueue.end()); it!=itEnd; ++it) {
				VDJob *job = *it;

				if (job->mId)
					srcJobLookup.insert(JobQueueLookup::value_type(job->mId, it - srcQueue.begin()));
			}

			// iterate over dst queue
			vdfastvector<uint32> jobIndicesToDelete;
			for(JobQueue::iterator it(dstQueue.begin()), itEnd(dstQueue.end()); it!=itEnd; ++it) {
				VDJob *job = *it;

				if (!job->mId)
					continue;

				JobQueueLookup::const_iterator itCrossJob(srcJobLookup.find(job->mId));
				if (itCrossJob != srcJobLookup.end()) {
					int crossIndex = itCrossJob->second;
					VDJob *crossJob = srcQueue[crossIndex];

					VDASSERT(crossJob);
					if (crossJob) {
						if (useJobsFromDstQueue) {
							modified |= job->Merge(*crossJob, true);
							dumpQueue.push_back(crossJob);
						} else {
							modified |= crossJob->Merge(*job, false);
							*it = crossJob;
							dumpQueue.push_back(job);
						}
						srcQueue[crossIndex] = NULL;
					}
				} else if (job->mCreationRevision < srcRevision) {
					dumpQueue.push_back(job);
					*it = NULL;

					if (useJobsFromDstQueue)
						modified = true;
				}
			}

			// compact destination queue by removing null job entries
			dstQueue.erase(std::remove_if(dstQueue.begin(), dstQueue.end(), std::bind2nd(std::equal_to<VDJob *>(), (VDJob *)NULL)), dstQueue.end());

			// merge any new jobs from the on-disk version
			for(JobQueue::iterator it(srcQueue.begin()), itEnd(srcQueue.end()); it!=itEnd; ++it) {
				VDJob *newJob = *it;

				if (newJob && (!newJob->mCreationRevision || newJob->mCreationRevision > mLastRevision)) {
					dstQueue.push_back(newJob);
					*it = NULL;

					if (useJobsFromDstQueue)
						modified = true;
				}
			}

			// swap over queues
			mJobQueue.swap(dstQueue);
			mJobCount = mJobQueue.size();

			mLastSignature = newSignature;
			mLastRevision = newRevision;

			newJobs.clear();

			while(!srcQueue.empty()) {
				VDJob *job = srcQueue.back();
				srcQueue.pop_back();
				if (job) {
					job->mpJobQueue = NULL;
					delete job;
				}
			}

			while(!dumpQueue.empty()) {
				VDJob *job = dumpQueue.back();
				dumpQueue.pop_back();
				if (job) {
					job->mpJobQueue = NULL;
					delete job;
				}
			}
		} else {
			// swap over queues
			mJobQueue.swap(newJobs);
			mJobCount = mJobQueue.size();

			mLastSignature = newSignature;
			mLastRevision = newRevision;

			while(!newJobs.empty()) {
				VDJob *job = newJobs.back();
				newJobs.pop_back();
				if (job) {
					job->mpJobQueue = NULL;
					delete job;
				}
			}
		}

		// assign IDs and revision IDs to any jobs that are missing them
		JobQueue::iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
		for(; it != itEnd; ++it) {
			VDJob *job = *it;

			VDASSERT(job->mpJobQueue == this);

			if (!job->mId)
				job->mId = VDJobQueue::GetUniqueId();

			if (!job->mCreationRevision)
				job->mCreationRevision = newRevision;

			if (!job->mChangeRevision)
				job->mChangeRevision = newRevision;
		}

		// notify
		if (g_pVDJobQueueStatusCallback)
			g_pVDJobQueueStatusCallback->OnJobQueueReloaded();

		// check if the current job is now marked as Aborting -- if so, issue an abort
		if (mpRunningJob && mpRunningJob->GetState() == VDJob::kStateAborting && g_dubber) {
			g_dubber->Abort();
		}

	} catch(const MyError&) {
		while(!newJobs.empty()) {
			VDJob *job = newJobs.back();
			newJobs.pop_back();
			delete job;
		}

		throw;
	}

	return modified;
}

void VDJobQueue::SetModified() {
	mbModified = true;

	mFlushTimer.SetOneShot(this, 1000);
}

// VDJobQueue::Flush()
//
// Flushes the job list out to disk.
//
// We store the job list in a file called VirtualDub.jobs.  It's actually a
// human-readable, human-editable Sylia script with extra comments to tell
// VirtualDub about each of the scripts.

bool VDJobQueue::Flush(const wchar_t *fileName) {
	// Try to create VirtualDub.jobs in the same directory as VirtualDub.

	bool usingGlobalFile = false;
	if (!fileName) {
		usingGlobalFile = true;

		fileName = mJobFilePath.c_str();
	}

	if (usingGlobalFile) {
		try {
			VDFileStream outputStream(fileName, nsVDFile::kReadWrite | nsVDFile::kDenyAll | nsVDFile::kOpenAlways);

			Load(&outputStream, true);
			
			uint64 signature = CreateListSignature();
			outputStream.seek(0);
			outputStream.truncate();

			uint32 revision = mLastRevision + 1;

			Save(&outputStream, signature, revision, false);

			mbModified = false;
			mbOrderModified = false;
			mLastSignature = signature;
			mLastRevision = revision;

			JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
			for(; it != itEnd; ++it) {
				VDJob *job = *it;

				if (!job->mCreationRevision)
					job->mCreationRevision = revision;

				if (!job->mChangeRevision)
					job->mChangeRevision = revision;
			}
		} catch(const MyError&) {
			return false;
		}
	} else {
		VDFileStream outputStream(fileName, nsVDFile::kWrite | nsVDFile::kDenyAll | nsVDFile::kCreateAlways);
		
		Save(&outputStream, 0, 1, true);
	}

	return true;
}

void VDJobQueue::Save(IVDStream *stream, uint64 signature, uint32 revision, bool resetJobRevisions) {
	VDTextOutputStream output(stream);

	output.PutLine("// VirtualDub job list (Sylia script format)");
	output.PutLine("// This is a program generated file -- edit at your own risk.");
	output.PutLine("//");
	output.FormatLine("// $signature %llx %x", signature, revision);
	output.FormatLine("// $numjobs %d", ListSize());
	output.PutLine("//");
	output.PutLine("");

	JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());

	for(; it != itEnd; ++it) {
		VDJob *vdj = *it;
		const int state = vdj->GetState();

		output.FormatLine("// $job \"%s\""		, vdj->GetName());
		output.FormatLine("// $input \"%s\""	, vdj->GetInputFile());
		output.FormatLine("// $output \"%s\""	, vdj->GetOutputFile());
		output.FormatLine("// $state %d"		, state);
		output.FormatLine("// $id %llx"			, vdj->mId);

		if (!resetJobRevisions)
			output.FormatLine("// $revision %x %x", vdj->mCreationRevision ? vdj->mCreationRevision : revision, vdj->mChangeRevision ? vdj->mChangeRevision : revision);

		if (state == VDJob::kStateInProgress || state == VDJob::kStateAborting || state == VDJob::kStateCompleted || state == VDJob::kStateError) {
			output.FormatLine("// $runner_id %llx", vdj->mRunnerId);
			output.FormatLine("// $runner_name \"%s\"", VDEncodeScriptString(VDStringSpanA(vdj->GetRunnerName())).c_str());
		}

		output.FormatLine("// $start_time %08lx %08lx", (unsigned long)(vdj->mDateStart >> 32), (unsigned long)vdj->mDateStart);
		output.FormatLine("// $end_time %08lx %08lx", (unsigned long)(vdj->mDateEnd >> 32), (unsigned long)vdj->mDateEnd);

		for(VDJob::tLogEntries::const_iterator it(vdj->mLogEntries.begin()), itEnd(vdj->mLogEntries.end()); it!=itEnd; ++it) {
			const VDJob::tLogEntries::value_type& ent = *it;
			output.FormatLine("// $logent %d %s", ent.severity, VDTextWToA(ent.text).c_str());
		}

		if (state == VDJob::kStateError)
			output.FormatLine("// $error \"%s\"", VDEncodeScriptString(VDStringSpanA(vdj->GetError())).c_str());

		output.PutLine("// $script");
		output.PutLine("");

		// Dump script

		const char *s = vdj->GetScript();

		while(*s) {
			const char *t = s;
			char c;

			while((c=*t) && c!='\r' && c!='\n')
				++t;

			if (t>s)
				output.Write(s, t-s);

			output.PutLine();

			// handle CR, CR/LF, LF, and NUL terminators

			if (*t == '\r') ++t;
			if (*t == '\n') ++t;

			s=t;
		}

		// Next...

		output.PutLine("");
		output.PutLine("// $endjob");
		output.PutLine("//");
		output.PutLine("//--------------------------------------------------");
	}

	output.PutLine("// $done");

	output.Flush();
}

void VDJobQueue::RunAll() {
	mbRunning	= true;
	mbRunAllStop		= false;

	NotifyStatus();

	ShowWindow(g_hWnd, SW_MINIMIZE);

	while(!mbRunAllStop) {
		VDJob *vdj = NULL;

		JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
		for(; it != itEnd; ++it) {
			VDJob *testJob = *it;

			if (testJob->GetState() == VDJob::kStateWaiting) {
				vdj = testJob;
				break;
			}
		}

		if (!vdj)
			break;

		vdj->Run();
	}

	mbRunning = false;
	NotifyStatus();

	if (VDRegistryAppKey().getBool(g_szRegKeyShutdownWhenFinished)) {
		if (g_hwndJobs)
			EnableWindow(g_hwndJobs, FALSE);

		bool do_shutdown = VDUIRequestSystemShutdown((VDGUIHandle)g_hWnd);

		if (g_hwndJobs)
			EnableWindow(g_hwndJobs, TRUE);

		if (do_shutdown) {
			VDInitiateSystemShutdown();
			PostQuitMessage(0);
		}
	}
}

void VDJobQueue::RunAllStop() {
	mbRunAllStop = true;
}

void VDJobQueue::Swap(int x, int y) {
	uint32 size = mJobQueue.size();
	if ((unsigned)x < size && (unsigned)y < size)
		std::swap(mJobQueue[x], mJobQueue[y]);

	mbOrderModified = true;
}

bool VDJobQueue::IsLocal(const VDJob *job) const {
	return job->mRunnerId == mRunnerId;
}

VDJobQueueStatus VDJobQueue::GetQueueStatus() const {
	if (mbBlocked)
		return kVDJQS_Blocked;
	else if (mbRunning)
		return kVDJQS_Running;
	else
		return kVDJQS_Idle;
}

int VDJobQueue::GetPendingJobCount() const {
	int count = 0;

	JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
	for(; it != itEnd; ++it) {
		VDJob *job = *it;

		switch(job->GetState()) {
		case VDJob::kStateInProgress:
		case VDJob::kStateWaiting:
			++count;
			break;
		}
	}

	return count;
}

uint64 VDJobQueue::GetUniqueId() {
	uint64 id = mBaseSignature + (uint32)VDGetCurrentTick();

	while(id == 0 || id == (uint64)(sint64)-1 || GetJobById(id))
		--id;

	return id;
}

const char *VDJobQueue::GetRunnerName() const {
	return mComputerName.c_str();
}

uint64 VDJobQueue::GetRunnerId() const {
	return mRunnerId;
}

bool VDJobQueue::IsAutoUpdateEnabled() const {
	return mFileWatcher.IsActive();
}

void VDJobQueue::SetAutoUpdateEnabled(bool enabled) {
	if (mFileWatcher.IsActive() == enabled)
		return;

	if (enabled) {
		try {
			mFileWatcher.Init(mJobFilePath.c_str(), this);
		} catch(const MyError&) {
			// for now, eat the error
		}
	} else {
		mFileWatcher.Shutdown();
	}
}

bool VDJobQueue::PollAutoRun() {
	if (!mbAutoRun || mbBlocked || mbRunning)
		return false;

	JobQueue::const_iterator it(mJobQueue.begin()), itEnd(mJobQueue.end());
	for(; it!=itEnd; ++it) {
		VDJob *job = *it;

		if (job->GetState() == VDJob::kStateWaiting) {
			RunAll();
			return true;
		}
	}

	return false;
}

bool VDJobQueue::IsAutoRunEnabled() const {
	return mbAutoRun;
}

void VDJobQueue::SetAutoRunEnabled(bool autorun) {
	mbAutoRun = autorun;
}

void VDJobQueue::SetBlocked(bool blocked) {
	mbBlocked = blocked;

	NotifyStatus();
}

void VDJobQueue::SetCallback(IVDJobQueueStatusCallback *cb) {
	g_pVDJobQueueStatusCallback = cb;
}

void VDJobQueue::NotifyStatus() {
	if (!g_pVDJobQueueStatusCallback)
		return;

	g_pVDJobQueueStatusCallback->OnJobQueueStatusChanged(GetQueueStatus());
}

uint64 VDJobQueue::CreateListSignature() {
	for(;;) {
		uint64 sig = mBaseSignature + VDGetCurrentTick();

		if (sig != mLastSignature)
			return sig;

		::Sleep(1);
	}
}

bool VDJobQueue::OnFileUpdated(const wchar_t *path) {
	if (!VDDoesPathExist(path))
		return true;

	HANDLE hTest;
	
	if (VDIsWindowsNT())
		hTest = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	else
		hTest = CreateFileA(VDTextWToA(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hTest == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();

		// if we're getting access denied, keep polling
		if (err == ERROR_ACCESS_DENIED)
			return false;
	} else {
		ListLoad(NULL, true);
		CloseHandle(hTest);
	}

	return true;
}

void VDJobQueue::TimerCallback() {
	Flush();

	if (mbModified)
		SetModified();
}

///////////////////////////////////////////////////////////////////////////////

VDJob::VDJob()
	: mpJobQueue(NULL)
	, mState(VDJob::kStateWaiting)
	, mId(0)
	, mRunnerId(0)
	, mDateStart(0)
	, mDateEnd(0)
	, mbContainsReloadMarker(false)
{
}

VDJob::~VDJob() {
}

bool VDJob::operator==(const VDJob& job) const {
#define TEST(field) if (field != job.field) return false
	TEST(mCreationRevision);
	TEST(mChangeRevision);
	TEST(mId);
	TEST(mDateStart);
	TEST(mDateEnd);
	TEST(mRunnerId);
	TEST(mRunnerName);
	TEST(mName);
	TEST(mInputFile);
	TEST(mOutputFile);
	TEST(mError);
	TEST(mScript);
	TEST(mState);
	TEST(mRunnerId);
	TEST(mbContainsReloadMarker);
#undef TEST

	return true;
}

void VDJob::SetState(int state) {
	mState = state;
	mChangeRevision = 0;

	switch(state) {
	case kStateInProgress:
	case kStateAborted:
	case kStateAborting:
	case kStateCompleted:
	case kStateError:
		break;
	default:
		mRunnerName.clear();
		mRunnerId = 0;
		break;
	}
}

void VDJob::SetRunner(uint64 id, const char *name) {
	mRunnerId = id;
	mRunnerName = name;
}

void VDJob::SetScript(const void *script, uint32 len, bool reloadable) {
	mScript.assign((const char *)script, (const char *)script + len);
	mbContainsReloadMarker = reloadable;
}

void VDJob::Refresh() {
	if (mpJobQueue)
		mpJobQueue->Refresh(this);
}

void VDJob::Run() {
	if (mpJobQueue)
		mpJobQueue->Run(this);
}

void VDJob::Reload() {
	if (mpJobQueue)
		mpJobQueue->Reload(this);
}

bool VDJob::Merge(const VDJob& src, bool srcHasInProgressPriority) {
	if (operator==(src))
		return false;

	if (IsRunning() && src.IsRunning() && mState != kStateAborting && src.mState != kStateAborting) {
		if (srcHasInProgressPriority) {
			mName		= src.mName;
			mError		= src.mError;
			mRunnerName	= src.mRunnerName;
			mState		= src.mState;
			mRunnerId	= src.mRunnerId;
			mDateStart	= src.mDateStart;
			mDateEnd	= src.mDateEnd;
			return true;
		}

		return false;
	}

	// Priority:
	//	1) Done
	//	2) In progress (us)
	//	3) In progress (someone else)
	//	4) Error
	//	5) Aborted
	//	6) Waiting
	//	7) Postponed

	int ourRevision = src.mChangeRevision ? src.mChangeRevision : 0xFFFFFFFFU;
	int loadedRevision = mChangeRevision ? mChangeRevision : 0xFFFFFFFFU;

	if (ourRevision > loadedRevision) {
		mName		= src.mName;
		mError		= src.mError;
		mRunnerName	= src.mRunnerName;
		mState		= src.mState;
		mRunnerId	= src.mRunnerId;
		mDateStart	= src.mDateStart;
		mDateEnd	= src.mDateEnd;
		return true;
	}

	return false;
}
