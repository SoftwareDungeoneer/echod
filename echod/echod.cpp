#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <cstdio>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

constexpr unsigned short g_defaultPort{ 2272 };

struct WSAGuard {
	WSAGuard(WSADATA& data) { WSAStartup(MAKEWORD(2, 2), &data); }
	~WSAGuard() { WSACleanup(); }
};

struct SocketGuard {
	SOCKET sock;

	SocketGuard(SOCKET s = INVALID_SOCKET) : sock{ s } {}
	~SocketGuard() { if (sock != INVALID_SOCKET) closesocket(sock); }
};

constexpr unsigned buffer_size = 8 * 1024 - sizeof(SOCKET);
struct SocketInfo {
	SOCKET sock;
	char buffer[buffer_size];
};

void StartConnection(SOCKET listenSocket);

void BeginOverlappedRecv(SOCKET sock);
void BeginOverlappedSend(SocketInfo* sendInfo, unsigned length);

void CALLBACK RecvCompletionRoutine(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags);
void CALLBACK SendCompletionRoutine(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags);

void PrintUsage()
{
	printf(
		"echod [port]\n"
		"\tEcho daemon\n\n"
		"\tport specifies an optional port number to listen to\n"
	);
}

int SystemError(const char* errorMsg)
{
	DWORD dwError = GetLastError();
	DWORD size{ 4095 };
	TCHAR* buffer = new TCHAR[size + 1];
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, dwError, 0, buffer, size, NULL);
	printf("%s\nSystem error: %d\n\t%S\n", errorMsg, dwError, buffer);
	delete[] buffer;
	return dwError;
}

int SocketError(const char* errorMsg, int sockErr = 0)
{
	if (!sockErr)
		sockErr = WSAGetLastError();
	DWORD size{ 4095 };
	TCHAR* buffer = new TCHAR[size + 1];
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, sockErr, 0, buffer, size, NULL);
	printf("%s\nSocket error: %d\n\t%S\n", errorMsg, sockErr, buffer);
	delete[] buffer;
	return sockErr;
}

unsigned short PortFromStr(const char* str)
{
	unsigned short listenPort{ 0 };
	char* pEnd;
	unsigned long longPort = strtoul(str, &pEnd, 10);
	if (0 < longPort && longPort < USHRT_MAX)
		listenPort = (unsigned short)longPort;
	return listenPort;
}

int main(int argc, char* argv[])
{
	if (argc > 2)
	{
		PrintUsage();
		return EXIT_FAILURE;
	}

	unsigned short listenPort{ g_defaultPort };
	if (argc == 2)
	{
		listenPort = PortFromStr(argv[1]);
		if (!listenPort)
		{
			printf("Invalid port number, out of range\n");
			PrintUsage();
			return EXIT_FAILURE;
		}
	}

	WSADATA wsaData;
	WSAGuard wsaGuard{ wsaData };

	DWORD dwZero{ 0 };
	try
	{
		SetLastError(0);
		volatile HANDLE hQuitEvent{ INVALID_HANDLE_VALUE };
		hQuitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (!hQuitEvent || hQuitEvent == INVALID_HANDLE_VALUE)
			throw SystemError("Failed to create control event");

		SOCKET listenSocket{ INVALID_SOCKET };
		listenSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		SocketGuard socketGuard{ listenSocket };

		if (listenSocket == INVALID_SOCKET)
			throw SocketError("Failed to initialize socket");
		int result = setsockopt(listenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&dwZero, sizeof(DWORD));
		if (result)
			throw SocketError("Failed to convert socket to dual-stack");

		SOCKADDR_IN6 hostAddr{ AF_INET6, htons(listenPort), 0, IN6ADDR_ANY_INIT };
		if (bind(listenSocket, (sockaddr*)&hostAddr, sizeof(SOCKADDR_IN6)))
			throw SocketError("Failed to bind socket to requested port");
		if (listen(listenSocket, SOMAXCONN))
			throw SocketError("`listen` failed");

		HANDLE listenEvent{ WSACreateEvent() };
		if (listenEvent == WSA_INVALID_EVENT)
			throw SocketError("Failed to create listen event");

		if (WSAEventSelect(listenSocket, listenEvent, FD_ACCEPT | FD_CLOSE))
			throw SocketError("WSAEventSelect failed for listening socket");

		printf("Listening on port %d\n", listenPort);

		WSANETWORKEVENTS networkEvents;
		HANDLE waitHandles[2] = { hQuitEvent, listenEvent };
		while (hQuitEvent)
		{
			switch (WaitForMultipleObjectsEx(2, waitHandles, FALSE, INFINITE, TRUE))
			{
			case WAIT_OBJECT_0:
				CloseHandle(hQuitEvent);
				WSACloseEvent(listenEvent);
				hQuitEvent = 0;
				break;

			case WAIT_OBJECT_0 + 1:
				WSAEnumNetworkEvents(listenSocket, listenEvent, &networkEvents);
				if (networkEvents.lNetworkEvents & FD_CLOSE)
					SetEvent(hQuitEvent);
				if (networkEvents.lNetworkEvents & FD_ACCEPT)
				{
					if (networkEvents.iErrorCode[FD_ACCEPT_BIT])
						throw SocketError("Listener error");
					StartConnection(listenSocket);
				}
				break;

			case WAIT_FAILED:
				throw SystemError("Wait failed");
				break;

			default:   // We shouldn't get abandoned waits on socket events, no timeout
				break; // and we restart the wait on a spurios wakeup (WAIT_IO_COMPLETION)
			}
			WSASetLastError(0);
			SetLastError(0);
		}
	}
	catch (...) { return EXIT_FAILURE; }

	return EXIT_SUCCESS;
}

void StartConnection(SOCKET listenSocket)
{
	SOCKADDR_IN6 remoteAddr;
	int size = sizeof(remoteAddr);
	SOCKET remote{ WSAAccept(listenSocket, (sockaddr*)&remoteAddr, &size, NULL, NULL) };
	if (remote == INVALID_SOCKET)
		throw SocketError("Failed to accept incoming connection");

	unsigned remotePort{ ntohs(remoteAddr.sin6_port) };
	char remoteHostIpStr[64];
	inet_ntop(remoteAddr.sin6_family, &remoteAddr.sin6_addr, remoteHostIpStr, 64);
	printf(">>> Remote host connected: (%s, %u)\n", remoteHostIpStr, remotePort);

	// Initiate an overlapped read with a completion routine -
	// Connected sockets will execute in a preemptive context only!
	try
	{
		unsigned long nonblock{ 1 };
		if (ioctlsocket(remote, FIONBIO, &nonblock))
			throw SocketError("Failed to set incoming connection to non-blocking");
		BeginOverlappedRecv(remote);
	}
	catch (...)
	{
		closesocket(remote);
	}
}

void BeginOverlappedRecv(SOCKET sock)
{
	DWORD flags = 0;
	WSAOVERLAPPED* pOverlapped = new WSAOVERLAPPED;
	SecureZeroMemory(pOverlapped, sizeof(WSAOVERLAPPED));
	SocketInfo* info = new SocketInfo{ sock };
	pOverlapped->hEvent = (HANDLE)info;
	WSABUF buffer;
	buffer.buf = info->buffer;
	buffer.len = buffer_size;
	int rc = WSARecv(sock, &buffer, 1, NULL, &flags, pOverlapped, &RecvCompletionRoutine);
	if ((rc == SOCKET_ERROR) && (WSA_IO_PENDING != WSAGetLastError()))
		throw SocketError("WSARecv failed");
}

void BeginOverlappedSend(SocketInfo* sendInfo, unsigned length)
{
	DWORD flags = 0;
	WSAOVERLAPPED* pOverlapped = new WSAOVERLAPPED;
	SecureZeroMemory(pOverlapped, sizeof(WSAOVERLAPPED));
	pOverlapped->hEvent = (HANDLE)sendInfo;
	WSABUF buffer;
	buffer.buf = sendInfo->buffer;
	buffer.len = length;
	int rc = WSASend(sendInfo->sock, &buffer, 1, NULL, flags, pOverlapped, &SendCompletionRoutine);
	if ((rc == SOCKET_ERROR) && (WSA_IO_PENDING != WSAGetLastError()))
		throw SocketError("WSASend failed");
}

void CALLBACK RecvCompletionRoutine(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
{
	SocketInfo* pInfo = (SocketInfo*)lpOverlapped->hEvent;

	if (dwError)
	{
		SocketError("Error during socket communication", dwError);
		closesocket(pInfo->sock);
		delete pInfo;
	}
	else
	{
		if (cbTransferred == 0)
		{
			printf("Connection closed normally\n");
			closesocket(pInfo->sock);
			delete pInfo;
		}
		else
		{
			// Immediately issue a new overlapped read
			BeginOverlappedRecv(pInfo->sock);
			BeginOverlappedSend(pInfo, cbTransferred);
		}
	}

	delete lpOverlapped;
}

void CALLBACK SendCompletionRoutine(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
{
	SocketInfo* pInfo = (SocketInfo*)lpOverlapped->hEvent;
	delete pInfo;
	delete lpOverlapped;

	if (dwError)
		SocketError("Error sending data", dwError);
}
