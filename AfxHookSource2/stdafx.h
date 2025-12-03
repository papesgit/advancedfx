// Prevent inclusion of winsock.h by windows.h (use winsock2.h instead)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Include winsock2 before any windows headers
#include <winsock2.h>
#include <ws2tcpip.h>

// Include additional Windows headers that WIN32_LEAN_AND_MEAN excludes
#include <shellapi.h>

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif
