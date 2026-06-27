// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaBinaryReaderWriter.h"
#include "UbaDefinitions.h"
#include "UbaDetoursShared.h"
#include "UbaEvent.h"
#include "UbaProcessStats.h"
#include <dlfcn.h>
#if PLATFORM_LINUX
#include <sys/prctl.h>
#elif UBA_USE_NATIVE_MAC_SEMAPHORES
#include <mach/mach.h>
#include <servers/bootstrap.h>
#endif
using namespace uba;

namespace uba
{
	int g_comFd = -1;
	int g_sessionPid = -1;
	SharedEvent* g_cancelEvent;
	SharedEvent* g_readEvent;
	SharedEvent* g_writeEvent;
	u8* g_messageMappingMem;
	extern bool g_isCancelled;

	void PreInit(const char* logFile, bool fromExec);
	void Init();
	void Deinit();
	void AddExceptionHandler();
}

static void __attribute__((constructor(102))) PreInitCtor()
{
	using namespace uba;
	SuppressDetourScope s;

	#if PLATFORM_LINUX
	prctl(PR_SET_PDEATHSIG, SIGHUP, 0, 0, 0); // We want the process to die if the parent die
	#endif
	
	InitSharedVariables();

	g_runningRemote = getenv("UBA_REMOTE") != nullptr;
	
	bool fromExec = getenv("UBA_EXEC") != nullptr;
	unsetenv("UBA_EXEC");

	const char* logFile = getenv("UBA_LOGFILE");
	PreInit(logFile, fromExec);
	unsetenv("UBA_LOGFILE");

	const char* comId = getenv("UBA_COMID");
	//printf("Starting up %s... (comid: %s) %u\n", __progname, (comId && *comId) ? comId : "NOTSET", getpid());

	if (!comId || !*comId)
		return;

	StringBuffer<256> comIdName;
	const char* plusIndex = strchr(comId, '+');
	comIdName.Append(comId, u64(plusIndex - comId));
	u64 comIdUid;
	if (!comIdName.Parse(comIdUid))
		UBA_ASSERT(false);

	u32 comIdOffset = strtoul(plusIndex + 1, nullptr, 10);
	unsetenv("UBA_COMID");

	comIdName.Clear();
	GetMappingHandleName(comIdName, comIdUid);

	g_comFd = shm_open(comIdName.data, O_RDWR, S_IRUSR | S_IWUSR);

	if (g_comFd == -1)
	{
		printf("UbaDetours: Failed to open shared mem: %s\n", comIdName.data);
		return;
	}
	u8* rptr = (u8*)mmap(NULL, CommunicationMemSize, PROT_READ | PROT_WRITE, MAP_SHARED, g_comFd, s64(comIdOffset));
	if (rptr == MAP_FAILED)
	{
		printf("UbaDetours: Failed to mmap fd: %u\n", g_comFd);
		return;
	}
	fcntl(g_comFd, F_SETFD, FD_CLOEXEC); // Don't let grandchild processes inherit this fd

	g_messageMappingMem = rptr;

	if (UseSemaphoreForSharedEvents())
	{
		g_readEvent = new SharedEvent();
		g_writeEvent = new SharedEvent();
		g_cancelEvent = new SharedEvent();

		#if UBA_USE_NATIVE_MAC_SEMAPHORES

		const char* pidStr = getenv("UBA_PID");
		UBA_ASSERT(pidStr);
		u64 pid;
		Parse(pid, pidStr, TStrlen(pidStr));
		unsetenv("UBA_PID");

		StringBuffer<> service;
		service.Append("UbaSemaphoreService");
		service.Append(getenv("UBA_SESSION_ID"));
		mach_port_t server_send = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_look_up(bootstrap_port, service.data, &server_send);
		UBA_ASSERTF(kr == KERN_SUCCESS, "bootstrap_look_up failed to lookup %s (%u)", service.data, kr);
		
		mach_port_t reply_port = MACH_PORT_NULL;
		kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port);
		UBA_ASSERT(kr == KERN_SUCCESS);

		struct { mach_msg_header_t header; u32 pid; } req;
		memset(&req, 0, sizeof(req));
		req.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
		req.header.msgh_size = sizeof(req);
		req.header.msgh_remote_port = server_send;
		req.header.msgh_local_port  = reply_port;
		req.pid = u32(pid);
		kr = mach_msg(&req.header, MACH_SEND_MSG, req.header.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		UBA_ASSERTF(kr == KERN_SUCCESS, "Failed to send mach msg (%u)", kr);

		struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t sem_desc[3]; uint8_t extra[512]; } rep;
		memset(&rep, 0, sizeof(rep));
		kr = mach_msg(&rep.header, MACH_RCV_MSG, 0, sizeof(rep), reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		UBA_ASSERTF(kr == KERN_SUCCESS, "Failed to receive mach msg (%u)", kr);

		StringBuffer<32> b;
		b.AppendValue(rep.sem_desc[0].name);
		g_cancelEvent->Create(b, false);
		b.Clear().AppendValue(rep.sem_desc[1].name);
		g_readEvent->Create(b, false);
		b.Clear().AppendValue(rep.sem_desc[2].name);
		g_writeEvent->Create(b, false);

		#else

		StringBuffer<256> name;
		name.Append(TCV("uba")).AppendValue(comIdUid).Append('_').AppendValue(comIdOffset);
		u64 len = name.count;
		name.Append("_cancel");
		g_cancelEvent->Create(name, false);
		name.Resize(len).Append("_read");
		g_readEvent->Create(name, false);
		name.Resize(len).Append("_write");
		g_writeEvent->Create(name, false);

		#endif
	}
	else
	{
		g_cancelEvent = ((SharedEvent*)rptr);
		g_readEvent = ((SharedEvent*)rptr) + 1;
		g_writeEvent = ((SharedEvent*)rptr) + 2;
		g_messageMappingMem += sizeof(SharedEvent) * 3;
	}

	AddExceptionHandler();
	InitMemory();

	const char* sessionPidStr = getenv("UBA_SESSION_PROCESS");
	UBA_ASSERT(sessionPidStr);
	g_sessionPid = atoi(sessionPidStr);

	g_rulesIndex = strtoul(getenv("UBA_RULES"), nullptr, 10);
	g_rules = GetApplicationRules()[g_rulesIndex].rules;
	unsetenv("UBA_RULES");

	const char* realCwd = getenv("UBA_CWD");
	UBA_ASSERT(realCwd);
	chdir(realCwd);
	unsetenv("UBA_CWD");

	// TODO: Don't know if we want this
	//int old;
	//pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);

	Init();
}

void CloseCom()
{
	if (g_comFd == -1)
		return;
	void* mem = g_messageMappingMem;
	if (UseSemaphoreForSharedEvents())
	{
		delete g_cancelEvent;
		delete g_readEvent;
		delete g_writeEvent;
	}
	else
	{
		mem = g_cancelEvent;
	}
	g_cancelEvent = nullptr;
	g_readEvent = nullptr;
	g_writeEvent = nullptr;
	g_messageMappingMem = nullptr;
	munmap(mem, CommunicationMemSize);
	close(g_comFd);
	g_comFd = -1;
}

static void __attribute__((destructor(102))) InitDtor()
{
	using namespace uba;

	Deinit();
	CloseCom();
	//printf("Exiting... %s   %u \n", __progname, getpid());
}

namespace uba
{
	constexpr u32 WritableMemSize = CommunicationMemSize - sizeof(SharedEvent) * 3;

	BinaryWriter::BinaryWriter(ProcessCommunication)
	{
		m_begin = g_messageMappingMem;
		m_pos = m_begin;
		m_end = m_begin + WritableMemSize;

		#if UBA_PROTOCOL_GUARD
		WriteU32(0xdeadbeef);
		#endif
	}

	BinaryReader BinaryWriter::Flush(bool waitOnResponse)
	{
		#if UBA_PROTOCOL_GUARD
		if (BinaryReader(g_messageMappingMem, 0, WritableMemSize).ReadU32() != 0xdeadbeef)
			exit(2345);
		#endif

		g_writeEvent->Set();

		if (!waitOnResponse)
			return BinaryReader(nullptr, 0, 0);

		TimerScope ts(g_stats.waitOnResponse);
		do
		{
			if (g_readEvent->IsSet(1000))
				break;
			
			// check if session process is gone
			if (kill(g_sessionPid, 0) == -1 && errno == ESRCH)
			{
				g_isCancelled = true;
				exit(1337);
			}

			if (g_cancelEvent->IsSet(0))
			{
				g_isCancelled = true;
				exit(1339);
			}
		} while (true);

		BinaryReader reader(g_messageMappingMem, 0, WritableMemSize);

		#if UBA_PROTOCOL_GUARD
		u32 baadfood = reader.ReadU32();
		if (baadfood != 0xbaadf00d)
			exit(2345);
		BinaryWriter(g_messageMappingMem, 0, CommunicationMemSize).WriteU32(0xcafebabe);
		#endif

		m_begin = nullptr;
		m_pos = 0;
		m_end = nullptr;
		return reader;
	}
}
