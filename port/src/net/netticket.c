#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "types.h"
#include "sha256.h"
#include "netint.h"

/**
 * The lobby's join ticket, as the host checks it in CONNECT
 * (tools/pdlobbyd/README.md, "Join ticket"; verify_ticket() in pdlobbyd.py is
 * the reference):
 *
 *   <user>|<room>|<expiry>|<nonce>|<mac>
 *
 * Checks 1 (format), 2 (mac, constant time), 3 (room), 4 (expiry on the
 * lobby's clock) and 6 (nonce replay) are here; the nonce is spent by
 * netTicketUse once the host accepts the join, so a join turned away for
 * another reason keeps its ticket. Check 5, the user in the
 * room's current roster, needs the host's lobby state poll, which is phase 6;
 * until then a direct-IP host runs with Net.RequireTicket 0 and never asks.
 */

#define NET_NONCES 64

static struct {
	char nonce[33];
	u64 expiry;
} s_NetNonces[NET_NONCES];

static s32 netTicketIsHex(const char *s, s32 len)
{
	s32 i;

	for (i = 0; i < len; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
			return 0;
		}
	}

	return 1;
}

static s32 netTicketIsUserChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
		|| c == '_' || c == '.' || c == '-';
}

static void netTicketWhy(char *why, s32 whysize, const char *text)
{
	if (why && whysize > 0) {
		snprintf(why, whysize, "%s", text);
	}
}

/**
 * Where a nonce would go: a free slot's index, -1 if it is live already
 * (a replay), -2 if every slot holds a live one. Expired ones are freed.
 */
static s32 netTicketNonceSlot(const char *nonce, u64 lobbynow)
{
	s32 freeslot = -1;
	s32 i;

	for (i = 0; i < NET_NONCES; i++) {
		if (s_NetNonces[i].expiry <= lobbynow) {
			s_NetNonces[i].expiry = 0;

			if (freeslot < 0) {
				freeslot = i;
			}
		} else if (memcmp(s_NetNonces[i].nonce, nonce, 32) == 0) {
			return -1;
		}
	}

	// never a live nonce evicted: that ticket could be used again
	return freeslot >= 0 ? freeslot : -2;
}

/**
 * The join was accepted: its nonce is spent until the ticket expires.
 * 0, or -1 if it cannot be recorded (then the join must be refused).
 */
s32 netTicketUse(const char *nonce, u64 expiry, u64 lobbynow)
{
	s32 slot = netTicketNonceSlot(nonce, lobbynow);

	if (slot < 0) {
		return -1;
	}

	memcpy(s_NetNonces[slot].nonce, nonce, 32);
	s_NetNonces[slot].nonce[32] = '\0';
	s_NetNonces[slot].expiry = expiry;

	return 0;
}

s32 netTicketVerify(const char *ticket, s32 len, const u8 *secret32, const char *roomid, u64 lobbynow,
		char *user, s32 usersize, char *nonce, u64 *expiryout, char *why, s32 whysize)
{
	const char *field[5];
	s32 flen[5];
	s32 n = 0;
	s32 start = 0;
	s32 i;
	u8 mac[32];
	char hex[65];
	u8 diff = 0;
	u64 expiry = 0;

	if (!ticket || len <= 0 || len > NET_MAXTICKET) {
		netTicketWhy(why, whysize, "no ticket, or one too long");
		return -1;
	}

	// 1. five fields, each its pattern, nothing after
	for (i = 0; i <= len; i++) {
		if (i == len || ticket[i] == '|') {
			if (n == 5) {
				netTicketWhy(why, whysize, "more than five fields");
				return -1;
			}

			field[n] = ticket + start;
			flen[n] = i - start;
			n++;
			start = i + 1;
		} else if ((u8)ticket[i] < 0x20 || (u8)ticket[i] > 0x7e) {
			netTicketWhy(why, whysize, "not printable ASCII");
			return -1;
		}
	}

	if (n != 5) {
		netTicketWhy(why, whysize, "not five fields");
		return -1;
	}

	if (flen[0] < 3 || flen[0] > 15) {
		netTicketWhy(why, whysize, "bad user");
		return -1;
	}

	for (i = 0; i < flen[0]; i++) {
		if (!netTicketIsUserChar(field[0][i])) {
			netTicketWhy(why, whysize, "bad user");
			return -1;
		}
	}

	if (flen[1] != 8 || !netTicketIsHex(field[1], 8)) {
		netTicketWhy(why, whysize, "bad room");
		return -1;
	}

	if (flen[2] != 10 || field[2][0] < '1' || field[2][0] > '9') {
		netTicketWhy(why, whysize, "bad expiry");
		return -1;
	}

	for (i = 0; i < 10; i++) {
		if (field[2][i] < '0' || field[2][i] > '9') {
			netTicketWhy(why, whysize, "bad expiry");
			return -1;
		}

		expiry = expiry * 10 + (u64)(field[2][i] - '0');
	}

	if (flen[3] != 32 || !netTicketIsHex(field[3], 32)) {
		netTicketWhy(why, whysize, "bad nonce");
		return -1;
	}

	if (flen[4] != 64 || !netTicketIsHex(field[4], 64)) {
		netTicketWhy(why, whysize, "bad mac");
		return -1;
	}

	// 2. the mac over the first four fields and their three bars
	sha256Hmac(secret32, 32, ticket, (u32)(field[4] - ticket - 1), mac);

	for (i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", mac[i]);
	}

	for (i = 0; i < 64; i++) {
		diff |= (u8)(hex[i] ^ field[4][i]);
	}

	if (diff) {
		netTicketWhy(why, whysize, "the ticket's mac is wrong (another room's, or altered)");
		return -1;
	}

	// 3. this host's room
	if (!roomid || strlen(roomid) != 8 || memcmp(field[1], roomid, 8) != 0) {
		netTicketWhy(why, whysize, "the ticket is for another room");
		return -1;
	}

	// 4. not expired, on the lobby's clock
	if (expiry <= lobbynow) {
		netTicketWhy(why, whysize, "the ticket has expired");
		return -1;
	}

	// 6. never the same nonce twice while it could still be used; recorded
	// only once the join is accepted (netTicketUse)
	switch (netTicketNonceSlot(field[3], lobbynow)) {
	case -1:
		netTicketWhy(why, whysize, "the ticket was used already");
		return -1;
	case -2:
		netTicketWhy(why, whysize, "too many joins at once; try again in a minute");
		return -1;
	}

	if (nonce) {
		memcpy(nonce, field[3], 32);
		nonce[32] = '\0';
	}

	if (expiryout) {
		*expiryout = expiry;
	}

	if (user && usersize > 0) {
		snprintf(user, usersize, "%.*s", flen[0], field[0]);
	}

	return 0;
}

/**
 * The vector pdlobbyd's TicketTests.test_known_vector pins: key bytes
 * 00..1f, the README's message. 0 if the mac and the checks agree.
 */
s32 netTicketSelfTest(void)
{
	static const char *msg = "joiner|0a1b2c3d|2000000000|abababababababababababababababab";
	static const char *want = "4985ec2af1530d6054140a70be302817cb3d097494cdad12dbec75ed3e9f8a01";
	char ticket[NET_MAXTICKET + 1];
	char user[16];
	char why[96];
	char nonce[33];
	u64 expiry = 0;
	u8 key[32];
	s32 i;

	for (i = 0; i < 32; i++) {
		key[i] = (u8)i;
	}

	snprintf(ticket, sizeof(ticket), "%s|%s", msg, want);

	// a good ticket, then the same nonce again, then a stale one, another
	// room's and a changed mac
	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3d", 1999999999, user, sizeof(user), nonce, &expiry, why, sizeof(why)) != 0
			|| strcmp(user, "joiner") != 0 || expiry != 2000000000ULL) {
		return 1;
	}

	// not spent until the join is accepted
	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3d", 1999999999, NULL, 0, NULL, NULL, why, sizeof(why)) != 0
			|| netTicketUse(nonce, expiry, 1999999999) != 0) {
		return 6;
	}

	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3d", 1999999999, NULL, 0, NULL, NULL, why, sizeof(why)) == 0) {
		return 2;
	}

	memset(s_NetNonces, 0, sizeof(s_NetNonces));

	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3d", 2000000000, NULL, 0, NULL, NULL, why, sizeof(why)) == 0) {
		return 3;
	}

	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3e", 1, NULL, 0, NULL, NULL, why, sizeof(why)) == 0) {
		return 4;
	}

	ticket[strlen(ticket) - 1] = '0';

	if (netTicketVerify(ticket, strlen(ticket), key, "0a1b2c3d", 1, NULL, 0, NULL, NULL, why, sizeof(why)) == 0) {
		return 5;
	}

	memset(s_NetNonces, 0, sizeof(s_NetNonces));

	return 0;
}
