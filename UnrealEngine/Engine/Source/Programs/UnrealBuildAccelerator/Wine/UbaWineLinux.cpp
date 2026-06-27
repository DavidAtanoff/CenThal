// Copyright Epic Games, Inc. All Rights Reserved.


#include <netinet/tcp.h>
#include <netinet/in.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <atomic>

//#include "../Common/Public/UbaLinuxNetworkWrappers.h"

int UnixGetsockopt(int fd, int level, int optname, char* optval, int* optlen)
{
	return getsockopt(fd, level, optname, optval, (socklen_t*)optlen);
}

int UnixSetsockopt(int fd, int level, int optname, char* optval, int optlen)
{
	return setsockopt(fd, level, optname, optval, (socklen_t)optlen);
}

int UnixGetTcpInfo(int fd, void* buf, int* len)
{
	socklen_t l = *len;
	if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, buf, &l) == -1)
		return -errno;

	*len = (int)l;
	return 0;
}

int UnixKillTcpHogs(unsigned short port)
{
	char lsofCommand[1024];
	snprintf(lsofCommand, sizeof(lsofCommand), "lsof -i :%u -sTCP:LISTEN -Pn -t", (unsigned int)port);

	FILE* lsof = popen(lsofCommand, "r");
	if (!lsof)
	{
		printf("Failed run lsof while trying to kill processes holding port %u (not installed?)", (unsigned int)port);
		return -1;
	}
	char pidStr[16];
	while (fgets(pidStr, sizeof(pidStr), lsof))
	{
		pid_t pid = (pid_t)atoi(pidStr);
		if (pid <= 0)
			continue;
		if (kill(pid, SIGKILL) != 0)
		{
			printf("Failed to kill process %d while trying to kill processes holding port %u", pid, (unsigned int)port);
			return -1;
		}
		printf("Process %d killed successfully", pid);
	}
	pclose(lsof);
	return 0;
}

#define PLATFORM_WINDOWS 0
#define PLATFORM_MAC 0
#define PLATFORM_LINUX 1

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include "../Core/Public/UbaBase.h"
#include "../Common/Public/UbaSocket.h"

#define UBA_ASSERT(...)
#define UBA_ASSERTF(...) 
#define UBA_FM_STRING_TYPE wchar_t

#include "../Core/Public/UbaFileHandle.h"
#include "../Core/Public/UbaFileMappingHandle.h"
#include "../Core/Public/UbaFileMapping.h"

namespace uba
{
	class Logger { public: void Logf(...) {} void Info(...) {} bool Warning(...) { return false; } bool Error(...) { return false; } };
	template<typename Type> using Atomic = std::atomic<Type>;
	void SetLastError(int err) {}
	enum LogEntryType : u8 { LogEntryType_Error = 0, LogEntryType_Warning = 1, };

	#include "../Core/Public/UbaFileMapping.inl"

	static constexpr u32 KeepAliveProbeCount = 10;
	#include "../Common/Public/UbaSocket.inl"
}
