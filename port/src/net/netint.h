#ifndef _IN_NET_NETINT_H
#define _IN_NET_NETINT_H

#include <PR/ultratypes.h>
#include "net/netbuf.h"
#include "net/netproto.h"

/**
 * What port/src/net/'s files share among themselves and nothing else sees:
 * the session (netsession.c), the content hashes (nethash.c), the rules
 * (netrules.c) and the lobby ticket (netticket.c).
 */

struct nethashcomp {
	char name[NET_MAXCOMPNAME + 1];
	u64 hash;
};

// netbuf helpers for the session's messages
void netWriteU64(struct netbuf *b, u64 v);
u64 netReadU64(struct netbuf *b);
void netWriteStr(struct netbuf *b, const char *s, s32 maxlen); // cut to maxlen, never an error
u64 netDigestU64(const u8 *digest); // the first eight bytes of a SHA-256, big-endian

// netsession.c
struct nethost;
struct netevent;
extern struct nethost *g_NetHostSocket;
void netSessionArgs(void);
void netSessionEvent(const struct netevent *ev);
void netSessionTick(void);
void netSessionTickBegin(void);
s32 netSessionMatchActive(void);
s32 netSessionBarrierHeld(void);

// nethash.c: the session hash (computed once) and the stage hash
s32 netSessionHash(struct nethashcomp *comps, s32 max);
void netStageHashReset(void);
s32 netStageHashComponents(struct nethashcomp *comps, s32 max);
s32 netStageHashOpen(void);
void netStageHashOpenWindow(void);
void netStageHashCloseWindow(void);
u32 netStageHashCount(s32 comp);

// netrules.c
struct netkeyvalue {
	char key[NET_MAXKEY + 1];
	s32 type;
	s32 s;
	f32 f;
	u32 u;
	char str[NET_MAXSTRVAL + 1];
};

#define NETKEY_SYNC       0
#define NETKEY_MUST       1 // always
#define NETKEY_MUST_GE    2 // on GoldenEye stages only
#define NETKEY_REFUSE     3 // a net game only with the stock value

s32 netRulesReadKey(const char *key, struct netkeyvalue *out);
void netRulesWriteValue(struct netbuf *b, const struct netkeyvalue *kv);
void netRulesReadValue(struct netbuf *b, struct netkeyvalue *kv);
s32 netRulesValuesEqual(const struct netkeyvalue *a, const struct netkeyvalue *b);
void netRulesValueString(const struct netkeyvalue *kv, char *buf, s32 size);
s32 netRulesWriteClientKeys(struct netbuf *b); // CONNECT's MUST and REFUSE keys
// The first MUST/REFUSE key the client's set fails against this host's, or
// 0: the class and key are written out for the refusal
s32 netRulesCheckClientKeys(const struct netkeyvalue *keys, s32 nkeys, s32 gestage, s32 *code, char *key, s32 keysize, char *text, s32 textsize);
void netRulesWrite(struct netbuf *b, u32 matchid);
u32 netRulesMatchId(void);
s32 netRulesRead(struct netbuf *b); // a client keeps the blob until STAGE_LOAD
void netRulesApply(void);           // a client: the stored blob over its own state
void netRulesSaveHost(void);        // the host: what it changes for remote slots
void netRulesRestore(void);         // H12
void netRulesSetLocked(s32 locked);

// netticket.c: tools/pdlobbyd/README.md "Join ticket", checks 1-4 and 6;
// 5 (the roster) is the lobby client's, phase 6. 0 if good, else why not;
// nonce (char[33]) and expiry out for netTicketUse, which spends it
s32 netTicketVerify(const char *ticket, s32 len, const u8 *secret32, const char *roomid, u64 lobbynow, char *user, s32 usersize, char *nonce, u64 *expiry, char *why, s32 whysize);
s32 netTicketUse(const char *nonce, u64 expiry, u64 lobbynow);
s32 netTicketSelfTest(void);

#endif
