/**
 * ENet (port/include/external/enet.h, MIT - its licence is at the top of
 * that file), the single-header build, compiled here and nowhere else.
 *
 * Taken from upstream's port-net branch with two changes, marked "PD:" in
 * enet.h: a reliable resend backs off, up to a ceiling, and a peer's RTT
 * estimate starts at 200 ms rather than 1. Only this file and
 * port/src/net/nettransport.c include enet.h: it brings <stdbool.h>, whose
 * bool is one byte, and the game's types.h makes bool an s32. A struct with a
 * bool in it seen through both would have two sizes (bool-two-sizes).
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#define ENET_IMPLEMENTATION 1
#define ENET_NO_PRAGMA_LINK 1

#ifdef _WIN32
/**
 * ENet's clock is clock_gettime, which mingw has only from winpthreads. The
 * game links winpthreads whole and static and SDL2 brings its import library
 * in front of it, so a reference from here was met twice (and pd-nettest,
 * which has no winpthreads at all, not once). Its clock here is the
 * performance counter instead, which is what winpthreads uses underneath.
 */
#include <winsock2.h>
#include <windows.h>
#include <time.h>

static int pdEnetClockGettime(int clock, struct timespec *ts)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;

	(void)clock;

	if (freq.QuadPart == 0) {
		QueryPerformanceFrequency(&freq);
	}

	QueryPerformanceCounter(&now);
	ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
	ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);

	return 0;
}

#define clock_gettime pdEnetClockGettime
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "external/enet.h"
#pragma GCC diagnostic pop

/**
 * Hands ENet a datagram as though its socket had just received it from
 * address. The loss/latency simulator in nettransport.c holds delayed
 * datagrams back from the intercept callback and gives them over here when
 * they fall due; the receive path that would take them is static to this file.
 *
 * Called outside enet_host_service with no event, so any event the datagram
 * makes is queued and comes out of the next service or check_events.
 */
int enet_host_inject_received(ENetHost *host, const ENetAddress *address, const void *data, size_t len)
{
	if (host == NULL || data == NULL || len == 0 || len > sizeof(host->packetData[0])) {
		return -1;
	}

	// Acks are timed against the service clock, which otherwise still says
	// when the last service began
	host->serviceTime = enet_time_get();

	memcpy(host->packetData[0], data, len);
	host->receivedAddress = *address;
	host->receivedData = host->packetData[0];
	host->receivedDataLength = len;

	return enet_protocol_handle_incoming_commands(host, NULL);
}
