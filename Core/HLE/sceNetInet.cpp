#include "Common/StringUtils.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Serialize/SerializeMap.h"

#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceNet.h"
#include "Core/HLE/sceNetAdhoc.h"
#include "Core/HLE/sceNetInet.h"
#include "Core/HLE/sceNp.h"
#include "Core/HLE/sceNp2.h"
#include "Core/HLE/NetInetConstants.h"
#include "Core/MIPS/MIPS.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/MemMapHelpers.h"
#include "Core/Util/PortManager.h"
#include "Core/Instance.h"

#ifndef MSG_NOSIGNAL
// Default value to 0x00 (do nothing) in systems where it's not supported.
#define MSG_NOSIGNAL 0x00
#endif

int inetLastErrno = 0; // TODO: since errno can only be read once, we should keep track the value to be used on sceNetInetGetErrno
int inetLastSocket = -1; // A workaround to keep the most recent socket id for sceNetInetSelect, until we have a socket class wrapper

bool netInetInited = false;

void __NetInetShutdown() {
	if (!netInetInited) {
		return;
	}

	netInetInited = false;
	// TODO: Shut down any open sockets here.
}

static int sceNetInetInit() {
	WARN_LOG(Log::sceNet, "UNIMPL sceNetInetInit()");
	if (netInetInited)
		return ERROR_NET_INET_ALREADY_INITIALIZED;
	netInetInited = true;
	return 0;
}

static int sceNetInetTerm() {
	WARN_LOG(Log::sceNet, "UNIMPL sceNetInetTerm()");
	__NetInetShutdown();
	return 0;
}

static int sceNetInetGetErrno() {
	if (inetLastErrno == 0)
		inetLastErrno = errno;
	int error = convertInetErrnoHost2PSP(inetLastErrno);
	inetLastErrno = 0;
	return hleLogSuccessI(Log::sceNet, error, " at %08x", currentMIPS->pc);
}

static int sceNetInetGetPspError() {
	if (inetLastErrno == 0)
		inetLastErrno = errno;
	int error = convertInetErrno2PSPError(convertInetErrnoHost2PSP(inetLastErrno));
	inetLastErrno = 0;
	return hleLogSuccessX(Log::sceNet, error, " at %08x", currentMIPS->pc);
}

static int sceNetInetInetPton(int af, const char* hostname, u32 inAddrPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED sceNetInetInetPton(%i, %s, %08x)", af, safe_string(hostname), inAddrPtr);
	if (!Memory::IsValidAddress(inAddrPtr)) {
		return hleLogError(Log::sceNet, 0, "invalid arg"); //-1
	}

	int retval = inet_pton(convertSocketDomainPSP2Host(af), hostname, (void*)Memory::GetPointer(inAddrPtr));
	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetInetAton(const char* hostname, u32 inAddrPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED sceNetInetInetAton(%s, %08x)", safe_string(hostname), inAddrPtr);
	if (!Memory::IsValidAddress(inAddrPtr)) {
		return hleLogError(Log::sceNet, 0, "invalid arg"); //-1
	}

	int retval = inet_pton(AF_INET, hostname, (void*)Memory::GetPointer(inAddrPtr));
	// inet_aton() returns nonzero if the address is valid, zero if not.
	return hleLogSuccessI(Log::sceNet, retval);
}

// TODO: Need to find out whether it's possible to get partial output or not, since Coded Arms Contagion is using a small bufsize(4)
static u32 sceNetInetInetNtop(int af, u32 srcInAddrPtr, u32 dstBufPtr, u32 bufsize) {
	WARN_LOG(Log::sceNet, "UNTESTED sceNetInetInetNtop(%i, %08x, %08x, %d)", af, srcInAddrPtr, dstBufPtr, bufsize);
	if (!Memory::IsValidAddress(srcInAddrPtr)) {
		return hleLogError(Log::sceNet, 0, "invalid arg");
	}
	if (!Memory::IsValidAddress(dstBufPtr) || bufsize < 1/*8*/) { // usually 8 or 16, but Coded Arms Contagion is using bufsize = 4
		inetLastErrno = ENOSPC;
		return hleLogError(Log::sceNet, 0, "invalid arg");
	}

	if (inet_ntop(convertSocketDomainPSP2Host(af), Memory::GetCharPointer(srcInAddrPtr), (char*)Memory::GetCharPointer(dstBufPtr), bufsize) == NULL) {
		//return hleLogDebug(Log::sceNet, 0, "invalid arg?"); // Temporarily commented out in case it's allowed to have partial output
	}
	return hleLogSuccessX(Log::sceNet, dstBufPtr, "%s", safe_string(Memory::GetCharPointer(dstBufPtr)));
}

static u32_le sceNetInetInetAddr(const char* hostname) {
	WARN_LOG(Log::sceNet, "UNTESTED sceNetInetInetAddr(%s)", safe_string(hostname));
	if (hostname == nullptr || hostname[0] == '\0')
		return hleLogError(Log::sceNet, INADDR_NONE, "invalid arg");

	u32 retval = INADDR_NONE; // inet_addr(hostname); // deprecated?
	inet_pton(AF_INET, hostname, &retval); // Alternative to the deprecated inet_addr

	return hleLogSuccessX(Log::sceNet, retval);
}

static int sceNetInetGetpeername(int sock, u32 namePtr, u32 namelenPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %08x)", __FUNCTION__, sock, namePtr, namelenPtr);
	if (!Memory::IsValidAddress(namePtr) || !Memory::IsValidAddress(namelenPtr)) {
		inetLastErrno = EFAULT;
		return hleLogError(Log::sceNet, -1, "invalid arg");
	}

	SceNetInetSockaddr* name = (SceNetInetSockaddr*)Memory::GetPointer(namePtr);
	int* namelen = (int*)Memory::GetPointer(namelenPtr);
	SockAddrIN4 saddr{};
	// TODO: Should've created convertSockaddrPSP2Host (and Host2PSP too) function as it's being used pretty often, thus fixing a bug on it will be tedious when scattered all over the places
	saddr.addr.sa_family = name->sa_family;
	int len = std::min(*namelen > 0 ? *namelen : 0, static_cast<int>(sizeof(saddr)));
	memcpy(saddr.addr.sa_data, name->sa_data, sizeof(name->sa_data));
	int retval = getpeername(sock, (sockaddr*)&saddr, (socklen_t*)&len);
	DEBUG_LOG(Log::sceNet, "Getpeername: Family = %s, Address = %s, Port = %d", inetSocketDomain2str(saddr.addr.sa_family).c_str(), ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
	*namelen = len;
	if (retval < 0) {
		inetLastErrno = errno;
		return hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	} else {
		memcpy(name->sa_data, saddr.addr.sa_data, len - (sizeof(name->sa_len) + sizeof(name->sa_family)));
		name->sa_len = len;
		name->sa_family = saddr.addr.sa_family;
	}
	return 0;
}

static int sceNetInetGetsockname(int sock, u32 namePtr, u32 namelenPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %08x)", __FUNCTION__, sock, namePtr, namelenPtr);
	if (!Memory::IsValidAddress(namePtr) || !Memory::IsValidAddress(namelenPtr)) {
		inetLastErrno = EFAULT;
		return hleLogError(Log::sceNet, -1, "invalid arg");
	}

	SceNetInetSockaddr* name = (SceNetInetSockaddr*)Memory::GetPointer(namePtr);
	int* namelen = (int*)Memory::GetPointer(namelenPtr);
	SockAddrIN4 saddr{};
	saddr.addr.sa_family = name->sa_family;
	int len = std::min(*namelen > 0 ? *namelen : 0, static_cast<int>(sizeof(saddr)));
	memcpy(saddr.addr.sa_data, name->sa_data, sizeof(name->sa_data));
	int retval = getsockname(sock, (sockaddr*)&saddr, (socklen_t*)&len);
	DEBUG_LOG(Log::sceNet, "Getsockname: Family = %s, Address = %s, Port = %d", inetSocketDomain2str(saddr.addr.sa_family).c_str(), ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
	*namelen = len;
	if (retval < 0) {
		inetLastErrno = errno;
		return hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	} else {
		memcpy(name->sa_data, saddr.addr.sa_data, len - (sizeof(name->sa_len) + sizeof(name->sa_family)));
		name->sa_len = len;
		name->sa_family = saddr.addr.sa_family;
	}
	return 0;
}

// FIXME: nfds is number of fd(s) as in posix poll, or was it maximum fd value as in posix select? Star Wars Battlefront Renegade seems to set the nfds to 64, while Coded Arms Contagion is using 256
int sceNetInetSelect(int nfds, u32 readfdsPtr, u32 writefdsPtr, u32 exceptfdsPtr, u32 timeoutPtr) {
	DEBUG_LOG(Log::sceNet, "UNTESTED sceNetInetSelect(%i, %08x, %08x, %08x, %08x) at %08x", nfds, readfdsPtr, writefdsPtr, exceptfdsPtr, timeoutPtr, currentMIPS->pc);
	int retval = -1;
	SceNetInetFdSet* readfds = (SceNetInetFdSet*)Memory::GetCharPointer(readfdsPtr);
	SceNetInetFdSet* writefds = (SceNetInetFdSet*)Memory::GetCharPointer(writefdsPtr);
	SceNetInetFdSet* exceptfds = (SceNetInetFdSet*)Memory::GetCharPointer(exceptfdsPtr);
	SceNetInetTimeval* timeout = (SceNetInetTimeval*)Memory::GetCharPointer(timeoutPtr);
	// TODO: Use poll instead of select since Windows' FD_SETSIZE is only 64 while PSP is 256, and select can only work for fd value less than FD_SETSIZE on some system
	fd_set rdfds{}, wrfds{}, exfds{};
	FD_ZERO(&rdfds); FD_ZERO(&wrfds); FD_ZERO(&exfds);
	int maxfd = nfds; // (nfds > PSP_NET_INET_FD_SETSIZE) ? nfds : PSP_NET_INET_FD_SETSIZE;
	int rdcnt = 0, wrcnt = 0, excnt = 0;
	for (int i = maxfd - 1; i >= 0 /*&& i >= maxfd - 64*/; i--) {
		bool windows_workaround = false;
#if PPSSPP_PLATFORM(WINDOWS)
		//windows_workaround = (i == nfds - 1);
#endif
		if (readfds != NULL && (NetInetFD_ISSET(i, readfds) || windows_workaround)) {
			VERBOSE_LOG(Log::sceNet, "Input Read FD #%i", i);
			if (rdcnt < FD_SETSIZE) {
				FD_SET(i, &rdfds); // This might pointed to a non-existing socket or sockets belonged to other programs on Windows, because most of the time Windows socket have an id above 1k instead of 0-255
				rdcnt++;
			}
		}
		if (writefds != NULL && (NetInetFD_ISSET(i, writefds) || windows_workaround)) {
			VERBOSE_LOG(Log::sceNet, "Input Write FD #%i", i);
			if (wrcnt < FD_SETSIZE) {
				FD_SET(i, &wrfds);
				wrcnt++;
			}
		}
		if (exceptfds != NULL && (NetInetFD_ISSET(i, exceptfds) || windows_workaround)) {
			VERBOSE_LOG(Log::sceNet, "Input Except FD #%i", i);
			if (excnt < FD_SETSIZE) {
				FD_SET(i, &exfds);
				excnt++;
			}
		}
	}
	// Workaround for games that set ndfs to 64 instead of socket id + 1
	if (inetLastSocket >= 0) {
		if (readfds != NULL && rdcnt == 0) {
			FD_SET(inetLastSocket, &rdfds);
			rdcnt++;
		}
		if (writefds != NULL && wrcnt == 0) {
			FD_SET(inetLastSocket, &wrfds);
			wrcnt++;
		}
		if (exceptfds != NULL && excnt == 0) {
			FD_SET(inetLastSocket, &exfds);
			excnt++;
		}
	}

	timeval tmout = { 5, 543210 }; // Workaround timeout value when timeout = NULL
	if (timeout != NULL) {
		tmout.tv_sec = timeout->tv_sec;
		tmout.tv_usec = timeout->tv_usec;
	}
	VERBOSE_LOG(Log::sceNet, "Select: Read count: %d, Write count: %d, Except count: %d, TimeVal: %u.%u", rdcnt, wrcnt, excnt, (int)tmout.tv_sec, (int)tmout.tv_usec);
	// TODO: Simulate blocking behaviour when timeout = NULL to prevent PPSSPP from freezing
	retval = select(nfds, (readfds == NULL) ? NULL : &rdfds, (writefds == NULL) ? NULL : &wrfds, (exceptfds == NULL) ? NULL : &exfds, /*(timeout == NULL) ? NULL :*/ &tmout);
	if (readfds != NULL && inetLastSocket < maxfd) NetInetFD_ZERO(readfds); // Clear it only when not needing a workaround
	if (writefds != NULL && inetLastSocket < maxfd) NetInetFD_ZERO(writefds); // Clear it only when not needing a workaround
	if (exceptfds != NULL) NetInetFD_ZERO(exceptfds);
	for (int i = maxfd - 1; i >= 0 /*&& i >= maxfd - 64*/; i--) {
		if (readfds != NULL && FD_ISSET(i, &rdfds))
			NetInetFD_SET(i, readfds);
		if (writefds != NULL && FD_ISSET(i, &wrfds))
			NetInetFD_SET(i, writefds);
		if (exceptfds != NULL && FD_ISSET(i, &exfds))
			NetInetFD_SET(i, exceptfds);
	}
	// Workaround for games that set ndfs to 64 instead of socket id + 1
	if (inetLastSocket >= 0) {
		if (readfds != NULL && rdcnt == 1 && FD_ISSET(inetLastSocket, &rdfds))
			NetInetFD_SET(inetLastSocket, readfds);
		if (writefds != NULL && wrcnt == 1 && FD_ISSET(inetLastSocket, &wrfds))
			NetInetFD_SET(inetLastSocket, writefds);
		if (exceptfds != NULL && excnt == 1 && FD_ISSET(inetLastSocket, &exfds))
			NetInetFD_SET(inetLastSocket, exceptfds);
	}

	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == 0)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else if (inetLastErrno < 0)
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return hleLogDebug(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 500)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented (ie. Coded Arms Contagion)
	}
	return hleLogSuccessI(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 500)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented (ie. Coded Arms Contagion)
}

int sceNetInetPoll(u32 fdsPtr, u32 nfds, int timeout) { // timeout in miliseconds just like posix poll? or in microseconds as other PSP timeout?
	DEBUG_LOG(Log::sceNet, "UNTESTED sceNetInetPoll(%08x, %d, %i) at %08x", fdsPtr, nfds, timeout, currentMIPS->pc);
	int retval = -1;
	int maxfd = 0;
	SceNetInetPollfd *fdarray = (SceNetInetPollfd*)Memory::GetPointer(fdsPtr); // SceNetInetPollfd/pollfd, sceNetInetPoll() have similarity to BSD poll() but pollfd have different size on 64bit

	if (nfds > FD_SETSIZE)
		nfds = FD_SETSIZE;

	fd_set readfds{}, writefds{}, exceptfds{};
	FD_ZERO(&readfds); FD_ZERO(&writefds); FD_ZERO(&exceptfds);
	for (int i = 0; i < (s32)nfds; i++) {
		if (fdarray[i].fd < 0) {
			inetLastErrno = EINVAL;
			return hleLogError(Log::sceNet, -1, "invalid socket id");
		}
		if (fdarray[i].fd > maxfd) maxfd = fdarray[i].fd;
		FD_SET(fdarray[i].fd, &readfds);
		FD_SET(fdarray[i].fd, &writefds);
		FD_SET(fdarray[i].fd, &exceptfds);
		fdarray[i].revents = 0;
	}
	timeval tmout = { 5, 543210 }; // Workaround timeout value when timeout = NULL
	if (timeout >= 0) {
		tmout.tv_sec = timeout / 1000000; // seconds
		tmout.tv_usec = (timeout % 1000000); // microseconds
	}
	// TODO: Simulate blocking behaviour when timeout is non-zero to prevent PPSSPP from freezing
	retval = select(maxfd + 1, &readfds, &writefds, &exceptfds, /*(timeout<0)? NULL:*/&tmout);
	if (retval < 0) {
		inetLastErrno = EINTR;
		return hleLogError(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 500)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented
	}

	retval = 0;
	for (int i = 0; i < (s32)nfds; i++) {
		if ((fdarray[i].events & (INET_POLLRDNORM | INET_POLLIN)) && FD_ISSET(fdarray[i].fd, &readfds))
			fdarray[i].revents |= (INET_POLLRDNORM | INET_POLLIN); //POLLIN_SET
		if ((fdarray[i].events & (INET_POLLWRNORM | INET_POLLOUT)) && FD_ISSET(fdarray[i].fd, &writefds))
			fdarray[i].revents |= (INET_POLLWRNORM | INET_POLLOUT); //POLLOUT_SET
		fdarray[i].revents &= fdarray[i].events;
		if (FD_ISSET(fdarray[i].fd, &exceptfds))
			fdarray[i].revents |= (INET_POLLRDBAND | INET_POLLPRI | INET_POLLERR); //POLLEX_SET; // Can be raised on revents regardless of events bitmask?
		if (fdarray[i].revents)
			retval++;
		VERBOSE_LOG(Log::sceNet, "Poll Socket#%d Fd: %d, events: %04x, revents: %04x, availToRecv: %d", i, fdarray[i].fd, fdarray[i].events, fdarray[i].revents, (int)getAvailToRecv(fdarray[i].fd));
	}
	//hleEatMicro(1000);
	return hleLogSuccessI(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 1000)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented
}

static int sceNetInetRecv(int socket, u32 bufPtr, u32 bufLen, u32 flags) {
	DEBUG_LOG(Log::sceNet, "UNTESTED sceNetInetRecv(%i, %08x, %i, %08x) at %08x", socket, bufPtr, bufLen, flags, currentMIPS->pc);
	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	int retval = recv(socket, (char*)Memory::GetPointer(bufPtr), bufLen, flgs | MSG_NOSIGNAL);
	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return hleDelayResult(retval, "workaround until blocking-socket", 500); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented
	}

	std::string datahex;
	DataToHexString(10, 0, Memory::GetPointer(bufPtr), retval, &datahex);
	VERBOSE_LOG(Log::sceNet, "Data Dump (%d bytes):\n%s", retval, datahex.c_str());

	return hleLogSuccessInfoI(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 500)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented
}

static int sceNetInetSend(int socket, u32 bufPtr, u32 bufLen, u32 flags) {
	DEBUG_LOG(Log::sceNet, "UNTESTED sceNetInetSend(%i, %08x, %i, %08x) at %08x", socket, bufPtr, bufLen, flags, currentMIPS->pc);

	std::string datahex;
	DataToHexString(10, 0, Memory::GetPointer(bufPtr), bufLen, &datahex);
	VERBOSE_LOG(Log::sceNet, "Data Dump (%d bytes):\n%s", bufLen, datahex.c_str());

	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	int retval = send(socket, (char*)Memory::GetPointer(bufPtr), bufLen, flgs | MSG_NOSIGNAL);

	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return retval;
	}

	return hleLogSuccessInfoI(Log::sceNet, retval);
}

static int sceNetInetSocket(int domain, int type, int protocol) {
	WARN_LOG(Log::sceNet, "UNTESTED sceNetInetSocket(%i, %i, %i) at %08x", domain, type, protocol, currentMIPS->pc);
	DEBUG_LOG(Log::sceNet, "Socket: Domain = %s, Type = %s, Protocol = %s", inetSocketDomain2str(domain).c_str(), inetSocketType2str(type).c_str(), inetSocketProto2str(protocol).c_str());
	int retval = socket(convertSocketDomainPSP2Host(domain), convertSocketTypePSP2Host(type), convertSocketProtoPSP2Host(protocol));
	if (retval < 0) {
		inetLastErrno = errno;
		return hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	}

	//InetSocket* sock = new InetSocket(domain, type, protocol);
	//retval = pspSockets.Create(sock);

	// Ignore SIGPIPE when supported (ie. BSD/MacOS)
	setSockNoSIGPIPE(retval, 1);
	// TODO: We should always use non-blocking mode and simulate blocking mode
	changeBlockingMode(retval, 1);
	// Enable Port Re-use, required for multiple-instance
	setSockReuseAddrPort(retval);
	// Disable Connection Reset error on UDP to avoid strange behavior
	setUDPConnReset(retval, false);

	inetLastSocket = retval;
	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetSetsockopt(int socket, int level, int optname, u32 optvalPtr, int optlen) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %i, %i, %08x, %i) at %08x", __FUNCTION__, socket, level, optname, optvalPtr, optlen, currentMIPS->pc);
	u32_le* optval = (u32_le*)Memory::GetPointer(optvalPtr);
	DEBUG_LOG(Log::sceNet, "SockOpt: Level = %s, OptName = %s, OptValue = %d", inetSockoptLevel2str(level).c_str(), inetSockoptName2str(optname, level).c_str(), *optval);
	timeval tval{};
	// InetSocket* sock = pspSockets.Get<InetSocket>(socket, error);
	// TODO: Ignoring SO_NBIO/SO_NONBLOCK flag if we always use non-bloking mode (ie. simulated blocking mode)
	if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_NBIO) {
		//memcpy(&sock->nonblocking, (int*)optval, std::min(sizeof(sock->nonblocking), optlen));
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// FIXME: Should we ignore SO_BROADCAST flag since we are using fake broadcast (ie. only broadcast to friends), 
	//        But Infrastructure/Online play might need to use broadcast for SSDP and to support LAN MP with real PSP
	/*else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_BROADCAST) {
		//memcpy(&sock->so_broadcast, (int*)optval, std::min(sizeof(sock->so_broadcast), optlen));
		return hleLogSuccessI(Log::sceNet, 0);
	}*/
	// TODO: Ignoring SO_REUSEADDR flag to prevent disrupting multiple-instance feature
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_REUSEADDR) {
		//memcpy(&sock->reuseaddr, (int*)optval, std::min(sizeof(sock->reuseaddr), optlen));
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// TODO: Ignoring SO_REUSEPORT flag to prevent disrupting multiple-instance feature (not sure if PSP has SO_REUSEPORT or not tho, defined as 15 on Android)
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_REUSEPORT) { // 15
		//memcpy(&sock->reuseport, (int*)optval, std::min(sizeof(sock->reuseport), optlen));
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// TODO: Ignoring SO_NOSIGPIPE flag to prevent crashing PPSSPP (not sure if PSP has NOSIGPIPE or not tho, defined as 0x1022 on Darwin)
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == 0x1022) { // PSP_NET_INET_SO_NOSIGPIPE ?
		//memcpy(&sock->nosigpipe, (int*)optval, std::min(sizeof(sock->nosigpipe), optlen));
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// It seems UNO game will try to set socket buffer size with a very large size and ended getting error (-1), so we should also limit the buffer size to replicate PSP behavior
	else if (level == PSP_NET_INET_SOL_SOCKET && (optname == PSP_NET_INET_SO_RCVBUF || optname == PSP_NET_INET_SO_SNDBUF)) { // PSP_NET_INET_SO_NOSIGPIPE ?
		// TODO: For SOCK_STREAM max buffer size is 8 Mb on BSD, while max SOCK_DGRAM is 65535 minus the IP & UDP Header size
		if (*optval > 8 * 1024 * 1024) {
			inetLastErrno = ENOBUFS; // FIXME: return ENOBUFS for SOCK_STREAM, and EINVAL for SOCK_DGRAM
			return hleLogError(Log::sceNet, -1, "buffer size too large?");
		}
	}
	int retval = 0;
	// PSP timeout are a single 32bit value (micro seconds)
	if (level == PSP_NET_INET_SOL_SOCKET && optval && (optname == PSP_NET_INET_SO_RCVTIMEO || optname == PSP_NET_INET_SO_SNDTIMEO)) {
		tval.tv_sec = *optval / 1000000; // seconds
		tval.tv_usec = (*optval % 1000000); // microseconds
		retval = setsockopt(socket, convertSockoptLevelPSP2Host(level), convertSockoptNamePSP2Host(optname, level), (char*)&tval, sizeof(tval));
	} else {
		retval = setsockopt(socket, convertSockoptLevelPSP2Host(level), convertSockoptNamePSP2Host(optname, level), (char*)optval, optlen);
	}
	if (retval < 0) {
		inetLastErrno = errno;
		hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	}
	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetGetsockopt(int socket, int level, int optname, u32 optvalPtr, u32 optlenPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %i, %i, %08x, %08x) at %08x", __FUNCTION__, socket, level, optname, optvalPtr, optlenPtr, currentMIPS->pc);
	u32_le* optval = (u32_le*)Memory::GetPointer(optvalPtr);
	socklen_t* optlen = (socklen_t*)Memory::GetPointer(optlenPtr);
	DEBUG_LOG(Log::sceNet, "SockOpt: Level = %s, OptName = %s", inetSockoptLevel2str(level).c_str(), inetSockoptName2str(optname, level).c_str());
	timeval tval{};
	// InetSocket* sock = pspSockets.Get<InetSocket>(socket, error);
	// TODO: Ignoring SO_NBIO/SO_NONBLOCK flag if we always use non-bloking mode (ie. simulated blocking mode)
	if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_NBIO) {
		//*optlen = std::min(sizeof(sock->nonblocking), *optlen);
		//memcpy((int*)optval, &sock->nonblocking, *optlen); 
		//if (sock->nonblocking && *optlen>0) *optval = 0x80; // on true, returning 0x80 when retrieved using getsockopt?
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// FIXME: Should we ignore SO_BROADCAST flag since we are using fake broadcast (ie. only broadcast to friends), 
	//        But Infrastructure/Online play might need to use broadcast for SSDP and to support LAN MP with real PSP
	/*else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_BROADCAST) {
		// *optlen = std::min(sizeof(sock->so_broadcast), *optlen);
		//memcpy((int*)optval, &sock->so_broadcast, *optlen);
		//if (sock->so_broadcast && *optlen>0) *optval = 0x80; // on true, returning 0x80 when retrieved using getsockopt?
		return hleLogSuccessI(Log::sceNet, 0);
	}*/
	// TODO: Ignoring SO_REUSEADDR flag to prevent disrupting multiple-instance feature
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_REUSEADDR) {
		//*optlen = std::min(sizeof(sock->reuseaddr), *optlen);
		//memcpy((int*)optval, &sock->reuseaddr, *optlen);
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// TODO: Ignoring SO_REUSEPORT flag to prevent disrupting multiple-instance feature (not sure if PSP has SO_REUSEPORT or not tho, defined as 15 on Android)
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == PSP_NET_INET_SO_REUSEPORT) { // 15
		//*optlen = std::min(sizeof(sock->reuseport), *optlen);
		//memcpy((int*)optval, &sock->reuseport, *optlen);
		return hleLogSuccessI(Log::sceNet, 0);
	}
	// TODO: Ignoring SO_NOSIGPIPE flag to prevent crashing PPSSPP (not sure if PSP has NOSIGPIPE or not tho, defined as 0x1022 on Darwin)
	else if (level == PSP_NET_INET_SOL_SOCKET && optname == 0x1022) { // PSP_NET_INET_SO_NOSIGPIPE ?
		//*optlen = std::min(sizeof(sock->nosigpipe), *optlen);
		//memcpy((int*)optval, &sock->nosigpipe, *optlen);
		return hleLogSuccessI(Log::sceNet, 0);
	}
	int retval = 0;
	// PSP timeout are a single 32bit value (micro seconds)
	if (level == PSP_NET_INET_SOL_SOCKET && optval && (optname == PSP_NET_INET_SO_RCVTIMEO || optname == PSP_NET_INET_SO_SNDTIMEO)) {
		socklen_t tvlen = sizeof(tval);
		retval = getsockopt(socket, convertSockoptLevelPSP2Host(level), convertSockoptNamePSP2Host(optname, level), (char*)&tval, &tvlen);
		if (retval != SOCKET_ERROR) {
			u64_le val = (tval.tv_sec * 1000000LL) + tval.tv_usec;
			memcpy(optval, &val, std::min(static_cast<socklen_t>(sizeof(val)), std::min(static_cast<socklen_t>(sizeof(*optval)), *optlen)));
		}
	} else {
		retval = getsockopt(socket, convertSockoptLevelPSP2Host(level), convertSockoptNamePSP2Host(optname, level), (char*)optval, optlen);
	}
	if (retval < 0) {
		inetLastErrno = errno;
		hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	}
	DEBUG_LOG(Log::sceNet, "SockOpt: OptValue = %d", *optval);
	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetBind(int socket, u32 namePtr, int namelen) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %i) at %08x", __FUNCTION__, socket, namePtr, namelen, currentMIPS->pc);
	SceNetInetSockaddr* name = (SceNetInetSockaddr*)Memory::GetPointer(namePtr);
	SockAddrIN4 saddr{};
	// TODO: Should've created convertSockaddrPSP2Host (and Host2PSP too) function as it's being used pretty often, thus fixing a bug on it will be tedious when scattered all over the places
	saddr.addr.sa_family = name->sa_family;
	int len = std::min(namelen > 0 ? namelen : 0, static_cast<int>(sizeof(saddr)));
	memcpy(saddr.addr.sa_data, name->sa_data, sizeof(name->sa_data));
	if (isLocalServer) {
		getLocalIp(&saddr.in);
	}
	// FIXME: On non-Windows broadcast to INADDR_BROADCAST(255.255.255.255) might not be received by the sender itself when binded to specific IP (ie. 192.168.0.2) or INADDR_BROADCAST.
	//        Meanwhile, it might be received by itself when binded to subnet (ie. 192.168.0.255) or INADDR_ANY(0.0.0.0).
	if (saddr.in.sin_addr.s_addr == INADDR_ANY || saddr.in.sin_addr.s_addr == INADDR_BROADCAST) {
		// Replace INADDR_ANY with a specific IP in order not to send data through the wrong interface (especially during broadcast)
		// Get Local IP Address
		sockaddr_in sockAddr{};
		getLocalIp(&sockAddr);
		DEBUG_LOG(Log::sceNet, "Bind: Address Replacement = %s => %s", ip2str(saddr.in.sin_addr).c_str(), ip2str(sockAddr.sin_addr).c_str());
		saddr.in.sin_addr.s_addr = sockAddr.sin_addr.s_addr;
	}
	// TODO: Make use Port Offset only for PPSSPP to PPSSPP communications (ie. IP addresses available in the group/friendlist), otherwise should be considered as Online Service thus should use the port as is.
	//saddr.in.sin_port = htons(ntohs(saddr.in.sin_port) + portOffset);
	DEBUG_LOG(Log::sceNet, "Bind: Family = %s, Address = %s, Port = %d", inetSocketDomain2str(saddr.addr.sa_family).c_str(), ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
	changeBlockingMode(socket, 0);
	int retval = bind(socket, (struct sockaddr*)&saddr, len);
	if (retval < 0) {
		inetLastErrno = errno;
		changeBlockingMode(socket, 1);
		return hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	}
	changeBlockingMode(socket, 1);
	// Update binded port number if it was 0 (any port)
	memcpy(name->sa_data, saddr.addr.sa_data, sizeof(name->sa_data));
	// Enable Port-forwarding
	// TODO: Check the socket type/protocol for SOCK_STREAM/SOCK_DGRAM or IPPROTO_TCP/IPPROTO_UDP instead of forwarding both protocol
	// InetSocket* sock = pspSockets.Get<InetSocket>(socket, error);
	// UPnP_Add((sock->type == SOCK_STREAM)? IP_PROTOCOL_TCP: IP_PROTOCOL_UDP, port, port);	
	unsigned short port = ntohs(saddr.in.sin_port);
	UPnP_Add(IP_PROTOCOL_UDP, port, port);
	UPnP_Add(IP_PROTOCOL_TCP, port, port);

	// Workaround: Send a dummy 0 size message to AdhocServer IP to make sure the socket actually bound to an address when binded with INADDR_ANY before using getsockname, seems to fix sending DGRAM from incorrect port issue on Android
	/*saddr.in.sin_addr.s_addr = g_adhocServerIP.in.sin_addr.s_addr;
	saddr.in.sin_port = 0;
	sendto(socket, dummyPeekBuf64k, 0, MSG_NOSIGNAL, (struct sockaddr*)&saddr, sizeof(saddr));
	*/

	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetConnect(int socket, u32 sockAddrPtr, int sockAddrLen) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %i) at %08x", __FUNCTION__, socket, sockAddrPtr, sockAddrLen, currentMIPS->pc);
	SceNetInetSockaddr* dst = (SceNetInetSockaddr*)Memory::GetPointer(sockAddrPtr);
	SockAddrIN4 saddr{};
	int dstlen = std::min(sockAddrLen > 0 ? sockAddrLen : 0, static_cast<int>(sizeof(saddr)));
	saddr.addr.sa_family = dst->sa_family;
	memcpy(saddr.addr.sa_data, dst->sa_data, sizeof(dst->sa_data));
	DEBUG_LOG(Log::sceNet, "Connect: Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));

	// Workaround to avoid blocking for indefinitely
	setSockTimeout(socket, SO_SNDTIMEO, 5000000);
	setSockTimeout(socket, SO_RCVTIMEO, 5000000);
	changeBlockingMode(socket, 0); // Use blocking mode as temporary fix for UNO, since we don't simulate blocking-mode yet
	int retval = connect(socket, (struct sockaddr*)&saddr.addr, dstlen);
	if (retval < 0) {
		inetLastErrno = errno;
		if (connectInProgress(inetLastErrno))
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		changeBlockingMode(socket, 1);
		// TODO: Since we're temporarily forcing blocking-mode we'll need to change errno from ETIMEDOUT to EAGAIN
		/*if (inetLastErrno == ETIMEDOUT)
			inetLastErrno = EAGAIN;
		*/
		return hleLogDebug(Log::sceNet, retval);
	}
	changeBlockingMode(socket, 1);

	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetListen(int socket, int backlog) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %i) at %08x", __FUNCTION__, socket, backlog, currentMIPS->pc);

	int retval = listen(socket, (backlog == PSP_NET_INET_SOMAXCONN ? SOMAXCONN : backlog));
	if (retval < 0) {
		inetLastErrno = errno;
		return hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
	}

	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetAccept(int socket, u32 addrPtr, u32 addrLenPtr) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %08x) at %08x", __FUNCTION__, socket, addrPtr, addrLenPtr, currentMIPS->pc);
	SceNetInetSockaddr* src = (SceNetInetSockaddr*)Memory::GetCharPointer(addrPtr);
	socklen_t* srclen = (socklen_t*)Memory::GetCharPointer(addrLenPtr);
	SockAddrIN4 saddr{};
	if (srclen)
		*srclen = std::min((*srclen) > 0 ? *srclen : 0, static_cast<socklen_t>(sizeof(saddr)));
	int retval = accept(socket, (struct sockaddr*)&saddr.addr, srclen);
	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return retval;
	}

	if (src) {
		src->sa_family = saddr.addr.sa_family;
		memcpy(src->sa_data, saddr.addr.sa_data, sizeof(src->sa_data));
		src->sa_len = srclen ? *srclen : 0;
	}
	DEBUG_LOG(Log::sceNet, "Accept: Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));

	return hleLogSuccessI(Log::sceNet, retval);
}

static int sceNetInetShutdown(int socket, int how) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i, %i) at %08x", __FUNCTION__, socket, how, currentMIPS->pc);
	// Convert HOW from PSP to Host
	int hostHow = how;
	switch (how) {
	case PSP_NET_INET_SHUT_RD: hostHow = SHUT_RD; break;
	case PSP_NET_INET_SHUT_WR: hostHow = SHUT_WR; break;
	case PSP_NET_INET_SHUT_RDWR: hostHow = SHUT_RDWR; break;
	}
	return hleLogSuccessI(Log::sceNet, shutdown(socket, hostHow));
}

static int sceNetInetSocketAbort(int socket) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i)", __FUNCTION__, socket);
	// FIXME: either using shutdown/close or select? probably using select if blocking mode is being simulated with non-blocking
	return hleLogSuccessI(Log::sceNet, shutdown(socket, SHUT_RDWR));
}

static int sceNetInetClose(int socket) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i) at %08x", __FUNCTION__, socket, currentMIPS->pc);
	return hleLogSuccessI(Log::sceNet, closesocket(socket));
}

static int sceNetInetCloseWithRST(int socket) {
	WARN_LOG(Log::sceNet, "UNTESTED %s(%i) at %08x", __FUNCTION__, socket, currentMIPS->pc);
	// Based on http://deepix.github.io/2016/10/21/tcprst.html
	struct linger sl {};
	sl.l_onoff = 1;		// non-zero value enables linger option in kernel 
	sl.l_linger = 0;	// timeout interval in seconds 
	setsockopt(socket, SOL_SOCKET, SO_LINGER, (const char*)&sl, sizeof(sl));
	return hleLogSuccessI(Log::sceNet, closesocket(socket));
}

static int sceNetInetRecvfrom(int socket, u32 bufferPtr, int len, int flags, u32 fromPtr, u32 fromlenPtr) {
	DEBUG_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %i, %08x, %08x, %08x) at %08x", __FUNCTION__, socket, bufferPtr, len, flags, fromPtr, fromlenPtr, currentMIPS->pc);
	SceNetInetSockaddr* src = (SceNetInetSockaddr*)Memory::GetCharPointer(fromPtr);
	socklen_t* srclen = (socklen_t*)Memory::GetCharPointer(fromlenPtr);
	SockAddrIN4 saddr{};
	if (srclen)
		*srclen = std::min((*srclen) > 0 ? *srclen : 0, static_cast<socklen_t>(sizeof(saddr)));
	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	int retval = recvfrom(socket, (char*)Memory::GetPointer(bufferPtr), len, flgs | MSG_NOSIGNAL, (struct sockaddr*)&saddr.addr, srclen);
	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return hleDelayResult(retval, "workaround until blocking-socket", 500); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented (ie. Coded Arms Contagion)
	}

	if (src) {
		src->sa_family = saddr.addr.sa_family;
		memcpy(src->sa_data, saddr.addr.sa_data, sizeof(src->sa_data));
		src->sa_len = srclen ? *srclen : 0;
	}
	DEBUG_LOG(Log::sceNet, "RecvFrom: Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));

	// Discard if it came from APIPA address (ie. self-received broadcasts from 169.254.x.x when broadcasting to INADDR_BROADCAST on Windows) on Untold Legends The Warrior's Code / Twisted Metal Head On
	/*if (isAPIPA(saddr.in.sin_addr.s_addr)) {
		inetLastErrno = EAGAIN;
		retval = -1;
		DEBUG_LOG(Log::sceNet, "RecvFrom: Ignoring Address = %s", ip2str(saddr.in.sin_addr).c_str());
		hleLogDebug(Log::sceNet, retval, "faked errno = %d", inetLastErrno);
		return hleDelayResult(retval, "workaround until blocking-socket", 500);
	}*/

	std::string datahex;
	DataToHexString(0, 0, Memory::GetPointer(bufferPtr), retval, &datahex);
	VERBOSE_LOG(Log::sceNet, "Data Dump (%d bytes):\n%s", retval, datahex.c_str());

	return hleLogSuccessInfoI(Log::sceNet, hleDelayResult(retval, "workaround until blocking-socket", 500)); // Using hleDelayResult as a workaround for games that need blocking-socket to be implemented (ie. Coded Arms Contagion)
}

static int sceNetInetSendto(int socket, u32 bufferPtr, int len, int flags, u32 toPtr, int tolen) {
	DEBUG_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %i, %08x, %08x, %d) at %08x", __FUNCTION__, socket, bufferPtr, len, flags, toPtr, tolen, currentMIPS->pc);
	SceNetInetSockaddr* dst = (SceNetInetSockaddr*)Memory::GetCharPointer(toPtr);
	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	SockAddrIN4 saddr{};
	int dstlen = std::min(tolen > 0 ? tolen : 0, static_cast<int>(sizeof(saddr)));
	if (dst) {
		saddr.addr.sa_family = dst->sa_family;
		memcpy(saddr.addr.sa_data, dst->sa_data, sizeof(dst->sa_data));
	}
	DEBUG_LOG(Log::sceNet, "SendTo: Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));

	std::string datahex;
	DataToHexString(0, 0, Memory::GetPointer(bufferPtr), len, &datahex);
	VERBOSE_LOG(Log::sceNet, "Data Dump (%d bytes):\n%s", len, datahex.c_str());

	int retval;
	bool isBcast = isBroadcastIP(saddr.in.sin_addr.s_addr);
	// Broadcast/Multicast, use real broadcast/multicast if there is no one in peerlist
	if (isBcast && getActivePeerCount() > 0) {
		// Acquire Peer Lock
		peerlock.lock();
		SceNetAdhocctlPeerInfo* peer = friends;
		for (; peer != NULL; peer = peer->next) {
			// Does Skipping sending to timed out friends could cause desync when players moving group at the time MP game started?
			if (peer->last_recv == 0)
				continue;

			saddr.in.sin_addr.s_addr = peer->ip_addr;
			retval = sendto(socket, (char*)Memory::GetPointer(bufferPtr), len, flgs | MSG_NOSIGNAL, (struct sockaddr*)&saddr.addr, dstlen);
			if (retval < 0) {
				DEBUG_LOG(Log::sceNet, "SendTo(BC): Socket error %d", errno);
			} else {
				DEBUG_LOG(Log::sceNet, "SendTo(BC): Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
			}
		}
		// Free Peer Lock
		peerlock.unlock();
		retval = len;
	}
	// Unicast or real broadcast/multicast
	else {
		// FIXME: On non-Windows(including PSP too?) broadcast to INADDR_BROADCAST(255.255.255.255) might not be received by the sender itself when binded to specific IP (ie. 192.168.0.2) or INADDR_BROADCAST.
		//        Meanwhile, it might be received by itself when binded to subnet (ie. 192.168.0.255) or INADDR_ANY(0.0.0.0).
		/*if (isBcast) {
			// TODO: Replace Broadcast with Multicast to be more consistent across platform
			// Replace Limited Broadcast(255.255.255.255) with Direct Broadcast(ie. 192.168.0.255) for accurate targetting when there are multiple interfaces, to avoid receiving it's own broadcasted data through IP 169.254.x.x on Windows (which is not recognized as it's own IP by the game)
			// Get Local IP Address
			sockaddr_in sockAddr{};
			getLocalIp(&sockAddr);
			// Change the last number to 255 to indicate a common broadcast address (the accurate way should be: ip | (~subnetmask))
			((u8*)&sockAddr.sin_addr.s_addr)[3] = 255;
			saddr.in.sin_addr.s_addr = sockAddr.sin_addr.s_addr;
			DEBUG_LOG(Log::sceNet, "SendTo(BC): Address Replacement = %s", ip2str(saddr.in.sin_addr).c_str());
		}*/
		retval = sendto(socket, (char*)Memory::GetPointer(bufferPtr), len, flgs | MSG_NOSIGNAL, (struct sockaddr*)&saddr.addr, dstlen);
	}
	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return retval;
	}

	return hleLogSuccessInfoI(Log::sceNet, retval);
}

// Similar to POSIX's sendmsg or Winsock2's WSASendMsg? Are their packets compatible one another?
// Games using this: The Warrior's Code
static int sceNetInetSendmsg(int socket, u32 msghdrPtr, int flags) {
	DEBUG_LOG(Log::sceNet, "UNTESTED %s(%i, %08x, %08x) at %08x", __FUNCTION__, socket, msghdrPtr, flags, currentMIPS->pc);
	// Note: sendmsg is concatenating iovec buffers before sending it, and send/sendto is just a wrapper for sendmsg according to https://stackoverflow.com/questions/4258834/how-sendmsg-works
	int retval = -1;
	if (!Memory::IsValidAddress(msghdrPtr)) {
		inetLastErrno = EFAULT;
		return hleLogError(Log::sceNet, retval);
	}
	InetMsghdr* pspMsghdr = (InetMsghdr*)Memory::GetPointer(msghdrPtr);
	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	SockAddrIN4 saddr{};
#if defined(_WIN32)
	WSAMSG hdr;
	WSACMSGHDR* chdr = NULL;
	size_t iovecsize = sizeof(WSABUF);
	WSABUF* iov = (WSABUF*)malloc(pspMsghdr->msg_iovlen * iovecsize);
#else
	msghdr hdr;
	cmsghdr* chdr = nullptr;
	size_t iovecsize = sizeof(iovec);
	iovec* iov = (iovec*)malloc(pspMsghdr->msg_iovlen * iovecsize);
#endif
	if (iov == NULL) {
		inetLastErrno = ENOBUFS;
		return hleLogError(Log::sceNet, retval);
	}
	memset(iov, 0, pspMsghdr->msg_iovlen * iovecsize);
	memset(&hdr, 0, sizeof(hdr));
	if (pspMsghdr->msg_name != 0) {
		SceNetInetSockaddr* pspSaddr = (SceNetInetSockaddr*)Memory::GetPointer(pspMsghdr->msg_name);
		saddr.addr.sa_family = pspSaddr->sa_family;
		size_t datalen = std::min(pspMsghdr->msg_namelen - (sizeof(pspSaddr->sa_len) + sizeof(pspSaddr->sa_family)), sizeof(saddr.addr.sa_data));
		memcpy(saddr.addr.sa_data, pspSaddr->sa_data, datalen);
		DEBUG_LOG(Log::sceNet, "SendMsg: Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
#if defined(_WIN32)
		hdr.name = &saddr.addr;
		hdr.namelen = static_cast<int>(datalen + sizeof(saddr.addr.sa_family));
#else
		hdr.msg_name = &saddr.addr;
		hdr.msg_namelen = static_cast<int>(datalen + sizeof(saddr.addr.sa_family));
#endif
	}
#if defined(_WIN32)
	hdr.lpBuffers = iov;
	hdr.dwBufferCount = pspMsghdr->msg_iovlen;
#else
	hdr.msg_iov = iov;
	hdr.msg_iovlen = pspMsghdr->msg_iovlen;
#endif
	if (pspMsghdr->msg_iov != 0) {
		SceNetIovec* pspIov = (SceNetIovec*)Memory::GetPointer(pspMsghdr->msg_iov);
		for (int i = 0; i < pspMsghdr->msg_iovlen; i++) {
			if (pspIov[i].iov_base != 0) {
#if defined(_WIN32)
				iov[i].buf = (char*)Memory::GetPointer(pspIov[i].iov_base);
				iov[i].len = pspIov[i].iov_len;
#else
				iov[i].iov_base = (char*)Memory::GetPointer(pspIov[i].iov_base);
				iov[i].iov_len = pspIov[i].iov_len;
#endif
			}
		}
	}
	// Control's Level (ie. host's SOL_SOCKET to/from psp's PSP_NET_INET_SOL_SOCKET) and Type (ie. SCM_RIGHTS) might need to be converted to be cross-platform
	if (pspMsghdr->msg_control != 0) {
#if defined(_WIN32)
		chdr = (WSACMSGHDR*)malloc(pspMsghdr->msg_controllen);
#else
		chdr = (cmsghdr*)malloc(pspMsghdr->msg_controllen);
#endif
		if (chdr == NULL) {
			inetLastErrno = ENOBUFS;
			free(iov);
			return hleLogError(Log::sceNet, retval);
		}
		InetCmsghdr* pspCmsghdr = (InetCmsghdr*)Memory::GetPointer(pspMsghdr->msg_control);
		// TODO: Convert InetCmsghdr into platform-specific struct as they're affected by 32/64bit
		memcpy(chdr, pspCmsghdr, pspMsghdr->msg_controllen);
#if defined(_WIN32)
		hdr.Control.buf = (char*)chdr; // (char*)pspCmsghdr;
		hdr.Control.len = pspMsghdr->msg_controllen;
		// Note: Many existing implementations of CMSG_FIRSTHDR never look at msg_controllen and just return the value of cmsg_control.
		if (pspMsghdr->msg_controllen >= sizeof(InetCmsghdr)) {
			// TODO: Creates our own CMSG_* macros (32-bit version of it, similar to the one on PSP) to avoid alignment/size issue that can lead to memory corruption/out of bound issue.
			for (WSACMSGHDR* cmsgptr = CMSG_FIRSTHDR(&hdr); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&hdr, cmsgptr)) {
				cmsgptr->cmsg_type = convertCMsgTypePSP2Host(cmsgptr->cmsg_type, cmsgptr->cmsg_level);
				cmsgptr->cmsg_level = convertSockoptLevelPSP2Host(cmsgptr->cmsg_level);
			}
		}
#else
		hdr.msg_control = (char*)chdr; // (char*)pspCmsghdr;
		hdr.msg_controllen = pspMsghdr->msg_controllen;
		// Note: Many existing implementations of CMSG_FIRSTHDR never look at msg_controllen and just return the value of cmsg_control.
		if (pspMsghdr->msg_controllen >= sizeof(InetCmsghdr)) {
			for (cmsghdr* cmsgptr = CMSG_FIRSTHDR(&hdr); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR(&hdr, cmsgptr)) {
				cmsgptr->cmsg_type = convertCMsgTypePSP2Host(cmsgptr->cmsg_type, cmsgptr->cmsg_level);
				cmsgptr->cmsg_level = convertSockoptLevelPSP2Host(cmsgptr->cmsg_level);
			}
		}
#endif
	}
	// Flags (ie. PSP_NET_INET_MSG_OOB) might need to be converted to be cross-platform
#if defined(_WIN32)
	hdr.dwFlags = convertMSGFlagsPSP2Host(pspMsghdr->msg_flags & ~PSP_NET_INET_MSG_DONTWAIT) | MSG_NOSIGNAL;
#else
	hdr.msg_flags = convertMSGFlagsPSP2Host(pspMsghdr->msg_flags & ~PSP_NET_INET_MSG_DONTWAIT) | MSG_NOSIGNAL;
#endif
	unsigned long sent = 0;
	bool isBcast = isBroadcastIP(saddr.in.sin_addr.s_addr);
	// Broadcast/Multicast, use real broadcast/multicast if there is no one in peerlist
	if (isBcast && getActivePeerCount() > 0) {
		// Acquire Peer Lock
		peerlock.lock();
		SceNetAdhocctlPeerInfo* peer = friends;
		for (; peer != NULL; peer = peer->next) {
			// Does Skipping sending to timed out friends could cause desync when players moving group at the time MP game started?
			if (peer->last_recv == 0)
				continue;

			saddr.in.sin_addr.s_addr = peer->ip_addr;
#if defined(_WIN32)
			int result = WSASendMsg(socket, &hdr, flgs | MSG_NOSIGNAL, &sent, NULL, NULL);
			if (static_cast<int>(sent) > retval)
				retval = sent;
#else
			size_t result = sendmsg(socket, &hdr, flgs | MSG_NOSIGNAL);
			if (static_cast<int>(result) > retval)
				retval = result;
#endif
			if (retval != SOCKET_ERROR) {
				DEBUG_LOG(Log::sceNet, "SendMsg(BC): Address = %s, Port = %d", ip2str(saddr.in.sin_addr).c_str(), ntohs(saddr.in.sin_port));
			} else {
				DEBUG_LOG(Log::sceNet, "SendMsg(BC): Socket error %d", errno);
			}
		}
		// Free Peer Lock
		peerlock.unlock();
		// TODO: Calculate number of bytes supposed to be sent
		retval = std::max(retval, 0); // Broadcast always success?
	}
	// Unicast or real broadcast/multicast
	else {
		// FIXME: On non-Windows(including PSP too?) broadcast to INADDR_BROADCAST(255.255.255.255) might not be received by the sender itself when binded to specific IP (ie. 192.168.0.2) or INADDR_BROADCAST.
		//        Meanwhile, it might be received by itself when binded to subnet (ie. 192.168.0.255) or INADDR_ANY(0.0.0.0).
		/*if (isBcast) {
			// TODO: Replace Broadcast with Multicast to be more consistent across platform
			// Replace Limited Broadcast(255.255.255.255) with Direct Broadcast(ie. 192.168.0.255) for accurate targetting when there are multiple interfaces, to avoid receiving it's own broadcasted data through IP 169.254.x.x on Windows (which is not recognized as it's own IP by the game)
			// Get Local IP Address
			sockaddr_in sockAddr{};
			getLocalIp(&sockAddr);
			// Change the last number to 255 to indicate a common broadcast address (the accurate way should be: ip | (~subnetmask))
			((u8*)&sockAddr.sin_addr.s_addr)[3] = 255;
			saddr.in.sin_addr.s_addr = sockAddr.sin_addr.s_addr;
			DEBUG_LOG(Log::sceNet, "SendMsg(BC): Address Replacement = %s", ip2str(saddr.in.sin_addr).c_str());
		}*/
#if defined(_WIN32)
		int result = WSASendMsg(socket, &hdr, flgs | MSG_NOSIGNAL, &sent, NULL, NULL);
		if (result != SOCKET_ERROR)
			retval = sent;
#else
		retval = sendmsg(socket, &hdr, flgs | MSG_NOSIGNAL);
#endif
	}
	free(chdr);
	free(iov);
	/*  // Example with 1 Msg buffer and without CMsg
		msghdr msg;
		iovec iov[1];
		int buflen = pspMsghdr->msg_iovlen;
		char* buf = (char*)malloc(buflen);

		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;
		iov[0].iov_base = buf;
		iov[0].iov_len = buflen;

		retval = sendmsg(socket, &msg, flags);
		free(buf);
	*/
	if (retval < 0) {
		inetLastErrno = errno;
		if (inetLastErrno == EAGAIN)
			hleLogDebug(Log::sceNet, retval, "errno = %d", inetLastErrno);
		else
			hleLogError(Log::sceNet, retval, "errno = %d", inetLastErrno);
		return retval;
	}
	return hleLogSuccessInfoI(Log::sceNet, retval); // returns number of bytes sent?
}

// Similar to POSIX's recvmsg or Mswsock's WSARecvMsg? Are their packets compatible one another?
// Games using this: World of Poker
static int sceNetInetRecvmsg(int socket, u32 msghdrPtr, int flags) {
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%i, %08x, %08x) at %08x", __FUNCTION__, socket, msghdrPtr, flags, currentMIPS->pc);
	// Reference: http://www.masterraghu.com/subjects/np/introduction/unix_network_programming_v1.3/ch14lev1sec5.html
	int retval = -1;
	if (!Memory::IsValidAddress(msghdrPtr)) {
		inetLastErrno = EFAULT;
		return hleLogError(Log::sceNet, retval);
	}
	InetMsghdr* pspMsghdr = (InetMsghdr*)Memory::GetPointer(msghdrPtr);
	int flgs = flags & ~PSP_NET_INET_MSG_DONTWAIT; // removing non-POSIX flag, which is an alternative way to use non-blocking mode
	flgs = convertMSGFlagsPSP2Host(flgs);
	SockAddrIN4 saddr{};
#if defined(_WIN32)
	WSAMSG hdr;
	WSACMSGHDR* chdr = NULL;
	size_t iovecsize = sizeof(WSABUF);
	WSABUF* iov = (WSABUF*)malloc(pspMsghdr->msg_iovlen * iovecsize);
#else
	msghdr hdr;
	cmsghdr* chdr = nullptr;
	size_t iovecsize = sizeof(iovec);
	iovec* iov = (iovec*)malloc(pspMsghdr->msg_iovlen * iovecsize);
#endif
	if (iov == NULL) {
		inetLastErrno = ENOBUFS;
		return hleLogError(Log::sceNet, retval);
	}
	memset(iov, 0, pspMsghdr->msg_iovlen * iovecsize);
	memset(&hdr, 0, sizeof(hdr));
	// TODO: Do similar to the already working sceNetInetSendmsg but in reverse
	//if (pspMsghdr->msg_name != 0) { ... }

	return hleLogError(Log::sceNet, retval); // returns number of bytes received?
}

// TODO: fix retmasks
const HLEFunction sceNetInet[] = {
	{0X17943399, &WrapI_V<sceNetInetInit>,           "sceNetInetInit",                  'i', ""       },
	{0X4CFE4E56, &WrapI_II<sceNetInetShutdown>,      "sceNetInetShutdown",              'i', "ii"     },
	{0XA9ED66B9, &WrapI_V<sceNetInetTerm>,           "sceNetInetTerm",                  'i', ""       },
	{0X8B7B220F, &WrapI_III<sceNetInetSocket>,       "sceNetInetSocket",                'i', "iii"    },
	{0X2FE71FE7, &WrapI_IIIUI<sceNetInetSetsockopt>, "sceNetInetSetsockopt",            'i', "iiixi"  },
	{0X4A114C7C, &WrapI_IIIUU<sceNetInetGetsockopt>, "sceNetInetGetsockopt",            'i', "iiixx"  },
	{0X410B34AA, &WrapI_IUI<sceNetInetConnect>,      "sceNetInetConnect",               'i', "ixi"    },
	{0X805502DD, &WrapI_I<sceNetInetCloseWithRST>,   "sceNetInetCloseWithRST",          'i', "i"      },
	{0XD10A1A7A, &WrapI_II<sceNetInetListen>,        "sceNetInetListen",                'i', "ii"     },
	{0XDB094E1B, &WrapI_IUU<sceNetInetAccept>,       "sceNetInetAccept",                'i', "ixx"    },
	{0XFAABB1DD, &WrapI_UUI<sceNetInetPoll>,         "sceNetInetPoll",                  'i', "xxi"    },
	{0X5BE8D595, &WrapI_IUUUU<sceNetInetSelect>,     "sceNetInetSelect",                'i', "ixxxx"  },
	{0X8D7284EA, &WrapI_I<sceNetInetClose>,          "sceNetInetClose",                 'i', "i"      },
	{0XCDA85C99, &WrapI_IUUU<sceNetInetRecv>,        "sceNetInetRecv",                  'i', "ixxx"   },
	{0XC91142E4, &WrapI_IUIIUU<sceNetInetRecvfrom>,  "sceNetInetRecvfrom",              'i', "ixiixx" },
	{0XEECE61D2, &WrapI_IUI<sceNetInetRecvmsg>,      "sceNetInetRecvmsg",               'i', "ixi"    },
	{0X7AA671BC, &WrapI_IUUU<sceNetInetSend>,        "sceNetInetSend",                  'i', "ixxx"   },
	{0X05038FC7, &WrapI_IUIIUI<sceNetInetSendto>,	 "sceNetInetSendto",                'i', "ixiixi" },
	{0X774E36F4, &WrapI_IUI<sceNetInetSendmsg>,      "sceNetInetSendmsg",               'i', "ixi"    },
	{0XFBABE411, &WrapI_V<sceNetInetGetErrno>,       "sceNetInetGetErrno",              'i', ""       },
	{0X1A33F9AE, &WrapI_IUI<sceNetInetBind>,         "sceNetInetBind",                  'i', "ixi"    },
	{0XB75D5B0A, &WrapU_C<sceNetInetInetAddr>,       "sceNetInetInetAddr",              'x', "s"      },
	{0X1BDF5D13, &WrapI_CU<sceNetInetInetAton>,      "sceNetInetInetAton",              'i', "sx"     },
	{0XD0792666, &WrapU_IUUU<sceNetInetInetNtop>,    "sceNetInetInetNtop",              'x', "ixxx"   },
	{0XE30B8C19, &WrapI_ICU<sceNetInetInetPton>,     "sceNetInetInetPton",              'i', "isx"    },
	{0X8CA3A97E, &WrapI_V<sceNetInetGetPspError>,    "sceNetInetGetPspError",           'i', ""       },
	{0XE247B6D6, &WrapI_IUU<sceNetInetGetpeername>,  "sceNetInetGetpeername",           'i', "ixx"    },
	{0X162E6FD5, &WrapI_IUU<sceNetInetGetsockname>,  "sceNetInetGetsockname",           'i', "ixx"    },
	{0X80A21ABD, &WrapI_I<sceNetInetSocketAbort>,    "sceNetInetSocketAbort",           'i', "i"      },
	{0X39B0C7D3, nullptr,                            "sceNetInetGetUdpcbstat",          '?', ""       },
	{0XB3888AD4, nullptr,                            "sceNetInetGetTcpcbstat",          '?', ""       },
};

void Register_sceNetInet() {
	RegisterModule("sceNetInet", std::size(sceNetInet), sceNetInet);
}
