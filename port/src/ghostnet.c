#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <PR/ultratypes.h>
#include <SDL2/SDL.h>
#include "platform.h"
#include "types.h"
#include "game/modghost.h"
#include "bss.h"
#include "fs.h"
#include "system.h"
#include "ghostnet.h"

/**
 * Which transport this build talks HTTPS over.
 *
 * Windows has one in the box. WinHTTP has shipped with every version since XP,
 * verifies against the certificate store Windows already keeps and updates,
 * and adds nothing to the package - where libcurl on Windows means shipping
 * libcurl and the dozen DLLs underneath it (OpenSSL, nghttp2, brotli, idn2,
 * psl, zstd) and then answering for a CA bundle that OpenSSL looks for at a
 * path no player's machine has. That is a lot of moving parts for four
 * requests, and every one of them is a thing that can be missing from a zip.
 *
 * Everywhere else libcurl is the answer, because everywhere else there is no
 * system HTTP API to use instead: macOS carries libcurl in the SDK so there is
 * nothing to bundle, and on Linux libcurl is on every desktop already.
 *
 * WinHTTP wins where both are available - a build on Windows that happens to
 * find libcurl still uses the one with nothing to ship.
 */
#if defined(PLATFORM_WIN32)
#define PD_GHOST_WINHTTP 1
#endif

#if defined(PD_GHOST_WINHTTP) || defined(PD_HAVE_CURL)
#define PD_GHOST_NET 1
#endif

#ifdef PD_GHOST_WINHTTP
#include <windows.h>
#include <winhttp.h>
#elif defined(PD_HAVE_CURL)
#include <curl/curl.h>
#endif

/**
 * See ghostnet.h for what this is and why it is asynchronous.
 *
 * The threading here is deliberately the smallest thing that works: one
 * worker, one job, one result, one mutex. A menu asks one question at a time
 * and waits for the answer before it can ask another, so a queue would be
 * machinery with no second user. SDL's threads are used because SDL2 is
 * already linked and its mutexes work the same on all three platforms.
 *
 * Everything above ghostnetSend() is transport agnostic: the four things this
 * speaks - post a JSON credential pair, post a file, get a board, get a blob -
 * are the same requests whichever backend carries them.
 */

/**
 * The account in use, and the ones this machine remembers.
 *
 * Slot zero is the active account and is what every request sends, which is
 * why it keeps the plain Mod.GhostUser and Mod.GhostPin names in pd.ini: a
 * config file written by an older build still signs the same person in. The
 * rest are accounts that were signed into before and can be switched back to
 * without typing a PIN again, the way the game remembers agents rather than
 * making you name one every time you sit down.
 *
 * Switching swaps rather than copies, so nothing is lost by choosing: the
 * account you were using goes into the slot the one you chose came out of.
 */
char g_GhostNetUser[GHOSTNET_MAXUSER + 2] = { 0 };
char g_GhostNetPin[GHOSTNET_MAXPIN + 2] = { 0 };
char g_GhostNetSavedUser[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXUSER + 2] = { 0 };
char g_GhostNetSavedPin[GHOSTNET_MAXACCOUNTS - 1][GHOSTNET_MAXPIN + 2] = { 0 };
char g_GhostNetUrl[256] = "https://texturepacks.art/pdghosts";

static s32 g_State = GHOSTNET_IDLE;
static char g_Message[128] = { 0 };

static struct ghostboardentry g_Board[GHOSTNET_MAXBOARD];
static s32 g_BoardCount = 0;
static s32 g_BoardStage = -1;
static s32 g_BoardDiff = -1;

#ifdef PD_GHOST_NET

#define JOB_NONE     0
#define JOB_REGISTER 1
#define JOB_LOGIN    2
#define JOB_UPLOAD   3
#define JOB_BOARD    4
#define JOB_DOWNLOAD 5

static SDL_mutex *g_Lock = NULL;
static SDL_Thread *g_Thread = NULL;
static s32 g_Job = JOB_NONE;
static s32 g_JobStage = 0;
static s32 g_JobDiff = 0;
static s32 g_JobId = 0;
static char g_JobFile[FS_MAXPATH + 1];

/**
 * Everything the worker reads, copied here before it starts.
 *
 * The rule this file now keeps is that a job is decided on the main thread and
 * carried out on the worker, and the worker touches nothing the menu owns.
 * Uploading used to break it twice over: it scanned the ghosts directory and
 * rebuilt the catalogue from the worker, while the page that started it was
 * still drawing rows out of that same array - modGhostScanCatalogue() sets the
 * count to zero before it refills, so a My Ghosts open during an upload reads
 * a list that is being emptied underneath it, and a delete from that page
 * writes to it.
 *
 * The credentials are copied for a smaller version of the same reason: Ghost
 * Share can reach the account pages while a request is in flight, so the name
 * and PIN a request was started with are not necessarily the ones still in the
 * boxes when it sends them.
 *
 * The queue is sized to the catalogue because the catalogue is what fills it.
 * Best-of-mine gives one run per stage and difficulty, which is sixty three
 * for the missions that exist, but stagenum is a byte out of a file that may
 * have been written anywhere and the ceiling should come from this end.
 */
#define GHOSTNET_MAXUPLOAD MODGHOST_MAXCATALOGUE

static char g_JobUser[GHOSTNET_MAXUSER + 2];
static char g_JobPin[GHOSTNET_MAXPIN + 2];
static char g_JobUploads[GHOSTNET_MAXUPLOAD][64];
static s32 g_JobUploadCount = 0;
static s32 g_JobUploadSkipped = 0;

// What a request may return before it is treated as a broken or hostile
// server. A leaderboard of a hundred rows is a few kilobytes; a ghost is a
// megabyte or two.
#define GHOSTNET_MAXREPLY (8 * 1024 * 1024)
#define GHOSTNET_TIMEOUT  20L

struct ghostnetbuf {
	char *data;
	size_t len;
};

/**
 * Add to the reply, refusing to grow past what a reply may be.
 *
 * Always leaves a terminator one past the end, so a reply can be handed to the
 * JSON scanner as a string without the length having to travel with it - and a
 * body that arrives with a zero byte in the middle of it, which a ghost blob
 * legitimately does, still has its true length in buf->len.
 */
static bool ghostnetBufAppend(struct ghostnetbuf *buf, const void *ptr, size_t add)
{
	char *grown;

	if (add == 0) {
		return true;
	}

	if (buf->len + add > GHOSTNET_MAXREPLY) {
		return false;
	}

	grown = realloc(buf->data, buf->len + add + 1);

	if (grown == NULL) {
		return false;
	}

	buf->data = grown;
	memcpy(buf->data + buf->len, ptr, add);
	buf->len += add;
	buf->data[buf->len] = '\0';

	return true;
}

/**
 * One request, in the terms both backends can answer.
 *
 * Four shapes are all this ever needs: a GET, a GET whose reply is a file, a
 * POST of a small JSON body, and a POST of a ghost. So the request is a URL, an
 * optional body with a type, and whether the account headers go on it.
 */
struct ghostnetreq {
	const char *url;
	const void *body;   // NULL for a GET
	u32 bodylen;
	const char *type;   // Content-Type, when there is a body
	bool auth;          // send X-Ghost-User and X-Ghost-Pin
};

/**
 * Carry out one request. Defined once per backend, below.
 *
 * Returns false only when the exchange did not happen at all - no route, no
 * name, a refused certificate - with why in err. A server that answered is a
 * true return whatever it said, and what it said is in status and buf, because
 * "the board is empty" and "the network is down" are different things to tell
 * a player and only the caller knows which reply means which.
 */
static bool ghostnetSend(const struct ghostnetreq *req, struct ghostnetbuf *buf,
		s32 *status, char *err, u32 errsize);

static void ghostnetSetResult(s32 state, const char *msg)
{
	SDL_LockMutex(g_Lock);
	g_State = state;
	snprintf(g_Message, sizeof(g_Message), "%s", msg ? msg : "");
	SDL_UnlockMutex(g_Lock);
}

/**
 * Pull one value out of a flat JSON object.
 *
 * The replies this speaks to are small, flat and written by the server on the
 * other end of this file, so a scanner is enough and a parser would be a
 * dependency. It is still written to survive nonsense: nothing is copied
 * without a length, and a key that is not there simply is not found.
 */
static bool ghostnetJsonField(const char *json, const char *key, char *out, u32 outsize)
{
	char pattern[64];
	const char *at;
	u32 i = 0;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	at = strstr(json, pattern);

	if (at == NULL) {
		return false;
	}

	at += strlen(pattern);

	while (*at == ' ' || *at == ':') {
		at++;
	}

	if (*at == '"') {
		at++;

		while (*at && *at != '"' && i + 1 < outsize) {
			out[i++] = *at++;
		}
	} else {
		while (*at && *at != ',' && *at != '}' && *at != ' ' && i + 1 < outsize) {
			out[i++] = *at++;
		}
	}

	out[i] = '\0';

	return true;
}

#define GHOSTNET_AGENT "pd-dabs-mod-ghost/1"

#ifdef PD_GHOST_WINHTTP

/**
 * The WinHTTP backend.
 *
 * WinHTTP wants wide strings and a URL taken apart into its pieces, which is
 * most of what is below. The rest is the shape every WinHTTP exchange has:
 * open a session, connect to a host, open a request on it, send, receive, then
 * read the body in whatever sized pieces it is willing to hand over.
 *
 * Certificates are the operating system's business here, which is the reason
 * this backend exists: there is no bundle to ship and none to keep current.
 */
static bool ghostnetWide(const char *src, wchar_t *dst, s32 dstchars)
{
	return MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dstchars) > 0;
}

/**
 * What went wrong, in words rather than a number where it is worth it.
 *
 * The handful named here are the ones a player can act on - the machine is
 * offline, the address is wrong, the server did not answer, its certificate
 * did not check out. Everything else is rare enough that the code is more use
 * than a sentence would be.
 */
static void ghostnetWinError(DWORD code, char *err, u32 errsize)
{
	const char *text = NULL;

	switch (code) {
	case ERROR_WINHTTP_CANNOT_CONNECT:        text = "could not connect to the server"; break;
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:     text = "could not find the server"; break;
	case ERROR_WINHTTP_TIMEOUT:               text = "the server did not answer in time"; break;
	case ERROR_WINHTTP_CONNECTION_ERROR:      text = "the connection was lost"; break;
	case ERROR_WINHTTP_SECURE_FAILURE:        text = "the server's certificate was refused"; break;
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:   text = "the server address is not http or https"; break;
	case ERROR_WINHTTP_INVALID_URL:           text = "the server address is not a URL"; break;
	default:                                  break;
	}

	if (text) {
		snprintf(err, errsize, "%s", text);
	} else {
		snprintf(err, errsize, "network error %lu", (unsigned long)code);
	}
}

static bool ghostnetSend(const struct ghostnetreq *req, struct ghostnetbuf *buf,
		s32 *status, char *err, u32 errsize)
{
	wchar_t wurl[512];
	wchar_t whost[256];
	wchar_t wpath[512];
	wchar_t wheaders[512];
	char headers[512];
	URL_COMPONENTS parts;
	HINTERNET session = NULL;
	HINTERNET connect = NULL;
	HINTERNET request = NULL;
	DWORD statuscode = 0;
	DWORD statussize = sizeof(statuscode);
	DWORD flags = 0;
	DWORD option;
	bool ok = false;
	s32 at = 0;

	*status = 0;

	if (!ghostnetWide(req->url, wurl, ARRAYCOUNT(wurl))) {
		snprintf(err, errsize, "the server address is too long");
		return false;
	}

	memset(&parts, 0, sizeof(parts));
	parts.dwStructSize = sizeof(parts);
	parts.lpszHostName = whost;
	parts.dwHostNameLength = ARRAYCOUNT(whost);
	parts.lpszUrlPath = wpath;
	parts.dwUrlPathLength = ARRAYCOUNT(wpath);

	// The query string is part of the path as far as this is concerned - the
	// board fetch puts its stage and difficulty there, and WinHttpOpenRequest
	// takes the two together.
	parts.lpszExtraInfo = NULL;
	parts.dwExtraInfoLength = 0;

	if (!WinHttpCrackUrl(wurl, 0, ICU_ESCAPE, &parts)) {
		ghostnetWinError(GetLastError(), err, errsize);
		return false;
	}

	session = WinHttpOpen(L"" GHOSTNET_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

	if (session == NULL) {
		ghostnetWinError(GetLastError(), err, errsize);
		return false;
	}

	// Milliseconds, and the same budget the curl backend is given: ten seconds
	// to connect and twenty for the whole exchange.
	WinHttpSetTimeouts(session, 10000, 10000, (int)(GHOSTNET_TIMEOUT * 1000),
			(int)(GHOSTNET_TIMEOUT * 1000));

	connect = WinHttpConnect(session, whost, parts.nPort, 0);

	if (connect == NULL) {
		ghostnetWinError(GetLastError(), err, errsize);
		goto done;
	}

	if (parts.nScheme == INTERNET_SCHEME_HTTPS) {
		flags |= WINHTTP_FLAG_SECURE;
	}

	request = WinHttpOpenRequest(connect, req->body ? L"POST" : L"GET", wpath,
			NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

	if (request == NULL) {
		ghostnetWinError(GetLastError(), err, errsize);
		goto done;
	}

	// Redirects are not part of this protocol and following one would carry
	// the account headers to wherever it pointed. See the same decision in the
	// curl backend.
	option = WINHTTP_DISABLE_REDIRECTS;
	WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option));

	headers[0] = '\0';

	if (req->type) {
		at += snprintf(headers + at, sizeof(headers) - at, "Content-Type: %s\r\n", req->type);
	}

	if (req->auth) {
		at += snprintf(headers + at, sizeof(headers) - at, "X-Ghost-User: %s\r\n", g_JobUser);
		at += snprintf(headers + at, sizeof(headers) - at, "X-Ghost-Pin: %s\r\n", g_JobPin);
	}

	if (headers[0] && !ghostnetWide(headers, wheaders, ARRAYCOUNT(wheaders))) {
		snprintf(err, errsize, "could not build the request");
		goto done;
	}

	if (!WinHttpSendRequest(request,
			headers[0] ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS,
			headers[0] ? (DWORD)-1 : 0,
			(LPVOID)req->body, req->bodylen, req->bodylen, 0)) {
		ghostnetWinError(GetLastError(), err, errsize);
		goto done;
	}

	if (!WinHttpReceiveResponse(request, NULL)) {
		ghostnetWinError(GetLastError(), err, errsize);
		goto done;
	}

	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statuscode, &statussize, WINHTTP_NO_HEADER_INDEX)) {
		*status = (s32)statuscode;
	}

	for (;;) {
		DWORD avail = 0;
		DWORD got = 0;
		char chunk[8192];

		if (!WinHttpQueryDataAvailable(request, &avail)) {
			ghostnetWinError(GetLastError(), err, errsize);
			goto done;
		}

		if (avail == 0) {
			break;
		}

		if (avail > sizeof(chunk)) {
			avail = sizeof(chunk);
		}

		if (!WinHttpReadData(request, chunk, avail, &got)) {
			ghostnetWinError(GetLastError(), err, errsize);
			goto done;
		}

		if (got == 0) {
			break;
		}

		// The only way this fails is a reply bigger than one may be, which is
		// a broken or hostile server rather than a network problem.
		if (!ghostnetBufAppend(buf, chunk, got)) {
			snprintf(err, errsize, "the server sent more than a reply may be");
			goto done;
		}
	}

	ok = true;

done:
	if (request) {
		WinHttpCloseHandle(request);
	}

	if (connect) {
		WinHttpCloseHandle(connect);
	}

	if (session) {
		WinHttpCloseHandle(session);
	}

	return ok;
}

#else // PD_GHOST_WINHTTP

/**
 * The libcurl backend, for everywhere that has no system HTTP of its own.
 */
static size_t ghostnetCurlWrite(void *ptr, size_t size, size_t nmemb, void *arg)
{
	size_t add = size * nmemb;

	// Returning short is how a write callback tells curl to abort the
	// transfer, which is what a reply past the cap should do.
	return ghostnetBufAppend(arg, ptr, add) ? add : 0;
}

static bool ghostnetSend(const struct ghostnetreq *req, struct ghostnetbuf *buf,
		s32 *status, char *err, u32 errsize)
{
	CURL *curl = curl_easy_init();
	struct curl_slist *headers = NULL;
	char header[128];
	CURLcode res;
	long code = 0;

	*status = 0;

	if (curl == NULL) {
		snprintf(err, errsize, "could not start request");
		return false;
	}

	curl_easy_setopt(curl, CURLOPT_URL, req->url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ghostnetCurlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, GHOSTNET_TIMEOUT);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, GHOSTNET_AGENT);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// Redirects are not followed. The endpoints are exact paths on a server
	// this file was written against, so a redirect is not part of the protocol
	// and there is nothing to gain by chasing one - while there is something
	// to lose, because curl carries a custom header across hosts even though
	// it strips Authorization, and the PIN travels as a custom header.
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

	if (req->type) {
		snprintf(header, sizeof(header), "Content-Type: %s", req->type);
		headers = curl_slist_append(headers, header);
	}

	if (req->auth) {
		snprintf(header, sizeof(header), "X-Ghost-User: %s", g_JobUser);
		headers = curl_slist_append(headers, header);
		snprintf(header, sizeof(header), "X-Ghost-Pin: %s", g_JobPin);
		headers = curl_slist_append(headers, header);
	}

	if (headers) {
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	}

	if (req->body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->bodylen);
	}

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	*status = (s32)code;

	if (res != CURLE_OK) {
		snprintf(err, errsize, "%s", curl_easy_strerror(res));
	}

	if (headers) {
		curl_slist_free_all(headers);
	}

	curl_easy_cleanup(curl);

	return res == CURLE_OK;
}

#endif // PD_GHOST_WINHTTP

/**
 * Post a username and PIN as JSON, for register and login.
 *
 * The PIN goes over TLS, which is the whole reason the endpoint is https. It
 * is a four digit number and the server treats the rate limiter rather than
 * the PIN as the actual control, but sending it in clear would still be
 * handing it to anyone on the same network.
 */
/**
 * Copy a string into a JSON string literal, escaping what would end it early.
 *
 * The body below is built by hand rather than by a serialiser, which is right
 * for two fields and wrong the moment one of them can contain a quote. The
 * name is typed on the game's own keyboard and the server's rules for it are
 * the server's, not this end's: a name with a quote in it should be refused by
 * the server having read a well formed request, not turned into a request that
 * says something else.
 */
static void ghostnetJsonEscape(const char *src, char *dst, u32 dstsize)
{
	u32 i = 0;

	while (*src) {
		unsigned char c = (unsigned char)*src++;

		if (c == '"' || c == '\\') {
			if (i + 2 >= dstsize) {
				break;
			}

			dst[i++] = '\\';
			dst[i++] = c;
		} else if (c < 0x20) {
			if (i + 6 >= dstsize) {
				break;
			}

			i += snprintf(dst + i, dstsize - i, "\\u%04x", c);
		} else {
			if (i + 1 >= dstsize) {
				break;
			}

			dst[i++] = c;
		}
	}

	dst[i] = '\0';
}

static bool ghostnetPostCredentials(const char *endpoint, char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	struct ghostnetreq req;
	char url[320];
	char body[256];
	char user[GHOSTNET_MAXUSER * 6 + 2];
	char pin[GHOSTNET_MAXPIN * 6 + 2];
	s32 status = 0;
	bool ok = false;

	snprintf(url, sizeof(url), "%s/%s", g_GhostNetUrl, endpoint);

	ghostnetJsonEscape(g_JobUser, user, sizeof(user));
	ghostnetJsonEscape(g_JobPin, pin, sizeof(pin));
	snprintf(body, sizeof(body), "{\"username\":\"%s\",\"pin\":\"%s\"}", user, pin);

	memset(&req, 0, sizeof(req));
	req.url = url;
	req.body = body;
	req.bodylen = strlen(body);
	req.type = "application/json";

	if (!ghostnetSend(&req, &buf, &status, msg, msgsize)) {
		free(buf.data);
		return false;
	}

	if (buf.data && strstr(buf.data, "\"ok\": true")) {
		ok = true;
	} else {
		char err[96];

		if (buf.data && ghostnetJsonField(buf.data, "error", err, sizeof(err))) {
			snprintf(msg, msgsize, "%s", err);
		} else {
			snprintf(msg, msgsize, "server said %d", status);
		}
	}

	free(buf.data);

	return ok;
}

static bool ghostnetUploadFile(const char *rel, char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	struct ghostnetreq req;
	char url[320];
	void *data;
	u32 size = 0;
	s32 status = 0;
	bool ok = false;

	data = fsFileLoad(rel, &size);

	if (data == NULL || size == 0) {
		snprintf(msg, msgsize, "could not read %s", rel);
		free(data);
		return false;
	}

	snprintf(url, sizeof(url), "%s/upload", g_GhostNetUrl);

	memset(&req, 0, sizeof(req));
	req.url = url;
	req.body = data;
	req.bodylen = size;
	req.type = "application/octet-stream";
	req.auth = true;

	if (!ghostnetSend(&req, &buf, &status, msg, msgsize)) {
		free(buf.data);
		free(data);
		return false;
	}

	if (buf.data && strstr(buf.data, "\"ok\": true")) {
		ok = true;
	} else {
		char err[96];

		if (buf.data && ghostnetJsonField(buf.data, "error", err, sizeof(err))) {
			snprintf(msg, msgsize, "%s", err);
		} else {
			snprintf(msg, msgsize, "upload refused (%d)", status);
		}
	}

	free(buf.data);
	free(data);

	return ok;
}

/**
 * Upload the runs this agent set.
 *
 * A run recorded since ghosts carried an owner is yours to send if that owner
 * is your account. The agent in the header says who ran it, which is a
 * different question - two people can both play as Joanna, and an agent name
 * was never a claim on anything.
 *
 * Runs from before the owner field, and runs recorded while signed out, name
 * nobody and are sent by nobody. The server refuses them too: what the client
 * sends is a proposal, and the file that arrives is what decides.
 *
 * One button that means "publish my times".
 *
 * Which runs those are is decided by ghostnetQueueUploads() below, on the main
 * thread, before the worker is started - see the note on the job state above
 * for why it cannot be decided from the worker. What crosses over to the
 * worker is a list of filenames and nothing else.
 */

/**
 * Whether this account may publish this run.
 *
 * Case insensitively, because the server holds usernames that way and Dab and
 * dab are one account there: a comparison here that disagreed would offer to
 * upload a file the server then refused.
 */
static bool ghostnetOwns(const struct modghostentry *entry)
{
	// A run that cannot say whose it is is nobody's to publish. There was a
	// fallback here comparing the agent instead, and it was a hole rather than
	// a kindness: an agent is a save file and does not change when the ghost
	// account does, so signing in as somebody new and pressing Upload
	// published every unowned run on the machine under the new name. That is
	// the theft the owner field exists to stop, arrived at by being helpful
	// about old files. They still race and still list locally; they just do
	// not go on a board.
	if (entry->owner[0] == '\0') {
		return false;
	}

	return strcasecmp(entry->owner, g_GhostNetUser) == 0;
}

static bool ghostnetIsBestOfMine(const struct modghostentry *entry)
{
	s32 count = modGhostGetCatalogueCount();
	s32 i;

	for (i = 0; i < count; i++) {
		const struct modghostentry *other = modGhostGetCatalogueEntry(i);

		// Only runs this account may publish get a say in which of them is
		// the best. Without that, a downloaded ghost sitting in the directory
		// with a quicker time would be found first and would answer no for a
		// run of your own - the stolen file cannot be uploaded and would
		// silently stop yours from being uploaded either.
		if (other == NULL
				|| other->stagenum != entry->stagenum
				|| other->difficulty != entry->difficulty
				|| !ghostnetOwns(other)) {
			continue;
		}

		return strcmp(other->filename, entry->filename) == 0;
	}

	return true;
}

/**
 * Work out what to send, on the thread that owns the catalogue.
 *
 * The catalogue is read here rather than in the worker, and the worker is
 * handed filenames. It used to be built from the worker with fsScanDir() over
 * the ghosts directory and a lookup into the catalogue per file, which was
 * both the race described above and a scan of a list that already was the
 * directory listing.
 *
 * The filter is set to everything first: it is left wherever the last page to
 * read the catalogue put it, so a Choose Ghosts visit before pressing Upload
 * would otherwise publish one mission's runs and call that all of them. Both
 * pages that care rescan when they open, so nothing needs it put back.
 */
static void ghostnetQueueUploads(void)
{
	s32 count;
	s32 i;

	g_JobUploadCount = 0;
	g_JobUploadSkipped = 0;

	modGhostSetCatalogueFilter(-1, -1);
	count = modGhostScanCatalogue();

	for (i = 0; i < count && g_JobUploadCount < GHOSTNET_MAXUPLOAD; i++) {
		const struct modghostentry *entry = modGhostGetCatalogueEntry(i);

		if (entry == NULL) {
			continue;
		}

		if (!ghostnetOwns(entry)) {
			g_JobUploadSkipped++;
			continue;
		}

		// Only your quickest run of a stage and difficulty is sent, because
		// only your quickest is a place on the board: the server keeps one row
		// per player per level and answers "you already have a faster time" to
		// the rest. Deciding that here rather than letting the server decide
		// it is the difference between one upload and an evening of them.
		//
		// The catalogue is sorted by stage, then difficulty, then time, so the
		// first entry that matches this one's level is the run to send and
		// anything else matching is slower.
		if (!ghostnetIsBestOfMine(entry)) {
			g_JobUploadSkipped++;
			continue;
		}

		snprintf(g_JobUploads[g_JobUploadCount], sizeof(g_JobUploads[0]), "%s", entry->filename);
		g_JobUploadCount++;
	}
}

/**
 * Send the queued runs, one at a time, and say how it went.
 *
 * A failure does not stop the rest: the runs are independent and one server
 * refusal - a name taken, a board full - says nothing about the next file.
 * The last failure's message is what the page shows, because a player with
 * one thing wrong wants to read what it was.
 */
static bool ghostnetUploadQueued(char *msg, u32 msgsize)
{
	char last[128] = { 0 };
	char rel[FS_MAXPATH + 1];
	s32 sent = 0;
	s32 failed = 0;
	s32 i;

	for (i = 0; i < g_JobUploadCount; i++) {
		snprintf(rel, sizeof(rel), MODGHOST_DIR "/%s", g_JobUploads[i]);

		if (ghostnetUploadFile(rel, last, sizeof(last))) {
			sent++;
		} else {
			failed++;
		}
	}

	if (failed) {
		snprintf(msg, msgsize, "sent %d, %d failed: %s", sent, failed, last);
		return false;
	}

	if (sent == 0) {
		// Naming the account matters here: an empty result almost always
		// means the runs on disk were set by a different one, and a message
		// that does not say whose runs it was looking for gives the player
		// nothing to go on.
		snprintf(msg, msgsize, "no runs by %s here - %d are not yours",
				g_JobUser, g_JobUploadSkipped);
		return true;
	}

	snprintf(msg, msgsize, "uploaded %d of your ghosts", sent);

	return true;
}

static bool ghostnetFetchBoardNow(char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	struct ghostnetreq req;
	char url[320];
	const char *at;
	s32 status = 0;
	s32 count = 0;

	snprintf(url, sizeof(url), "%s/leaderboard?stage=%d&diff=%d&limit=%d",
			g_GhostNetUrl, g_JobStage, g_JobDiff, GHOSTNET_MAXBOARD);

	memset(&req, 0, sizeof(req));
	req.url = url;

	if (!ghostnetSend(&req, &buf, &status, msg, msgsize)) {
		free(buf.data);
		return false;
	}

	if (buf.data == NULL) {
		snprintf(msg, msgsize, "no reply (%d)", status);
		return false;
	}

	// One object per row, each with the same three keys. Walking from one
	// "id" to the next is enough structure for a reply this shape, and every
	// copy out of it is bounded.
	at = buf.data;

	while (count < GHOSTNET_MAXBOARD) {
		char num[24];
		const char *next = strstr(at, "{\"id\"");

		if (next == NULL) {
			break;
		}

		at = next + 1;

		if (!ghostnetJsonField(at, "id", num, sizeof(num))) {
			break;
		}

		g_Board[count].id = atoi(num);

		if (ghostnetJsonField(at, "time60", num, sizeof(num))) {
			g_Board[count].time60 = (u32)atoi(num);
		} else {
			g_Board[count].time60 = 0;
		}

		if (!ghostnetJsonField(at, "user", g_Board[count].user, sizeof(g_Board[count].user))) {
			g_Board[count].user[0] = '\0';
		}

		// Sent by the server for every row. Nothing without it can be stored
		// there, so this is a belt on top of braces - but a row that arrived
		// from a server with a looser policy should be readable as what it is
		// rather than quietly ranked beside runs it cannot be compared with.
		if (ghostnetJsonField(at, "trialrules", num, sizeof(num))) {
			g_Board[count].trialrules = atoi(num) != 0;
		} else {
			g_Board[count].trialrules = false;
		}

		g_Board[count].have = false;
		count++;
	}

	free(buf.data);

	SDL_LockMutex(g_Lock);
	g_BoardCount = count;
	g_BoardStage = g_JobStage;
	g_BoardDiff = g_JobDiff;
	SDL_UnlockMutex(g_Lock);

	snprintf(msg, msgsize, "%d %s on the board", count, count == 1 ? "time" : "times");

	return true;
}

static bool ghostnetDownloadNow(char *msg, u32 msgsize)
{
	struct ghostnetbuf buf = { NULL, 0 };
	struct ghostnetreq req;
	char url[320];
	FILE *f;
	s32 status = 0;

	snprintf(url, sizeof(url), "%s/download?id=%d", g_GhostNetUrl, g_JobId);

	memset(&req, 0, sizeof(req));
	req.url = url;

	if (!ghostnetSend(&req, &buf, &status, msg, msgsize)) {
		free(buf.data);
		return false;
	}

	// A ghost, not a JSON error: the server sends one or the other and the
	// magic is what tells them apart without trusting the status code.
	if (buf.len < 128 || memcmp(buf.data, MODGHOST_MAGIC, 8) != 0) {
		snprintf(msg, msgsize, "server did not send a ghost (%d)", status);
		free(buf.data);
		return false;
	}

	if (fsFileSize(MODGHOST_DIR) < 0 && fsCreateDir(MODGHOST_DIR) != 0) {
		snprintf(msg, msgsize, "could not create the ghosts folder");
		free(buf.data);
		return false;
	}

	f = fsFileOpenWrite(g_JobFile);

	if (f == NULL) {
		snprintf(msg, msgsize, "could not write %s", g_JobFile);
		free(buf.data);
		return false;
	}

	if (fwrite(buf.data, 1, buf.len, f) != buf.len) {
		snprintf(msg, msgsize, "only part of the ghost was written");
		fclose(f);
		free(buf.data);
		return false;
	}

	fclose(f);
	free(buf.data);

	snprintf(msg, msgsize, "downloaded, it is in Choose Ghosts now");

	return true;
}

static int ghostnetWorker(void *arg)
{
	char msg[128] = { 0 };
	bool ok = false;

	switch (g_Job) {
	case JOB_REGISTER:
		ok = ghostnetPostCredentials("register", msg, sizeof(msg));

		if (ok) {
			snprintf(msg, sizeof(msg), "account created, you are signed in");
		}
		break;
	case JOB_LOGIN:
		ok = ghostnetPostCredentials("login", msg, sizeof(msg));

		if (ok) {
			snprintf(msg, sizeof(msg), "signed in as %s", g_JobUser);
		}
		break;
	case JOB_UPLOAD:
		// The queue was built by ghostnetUploadMine() before this thread
		// existed. Nothing here reads the catalogue.
		ok = ghostnetUploadQueued(msg, sizeof(msg));
		break;
	case JOB_BOARD:
		ok = ghostnetFetchBoardNow(msg, sizeof(msg));
		break;
	case JOB_DOWNLOAD:
		ok = ghostnetDownloadNow(msg, sizeof(msg));
		break;
	}

	ghostnetSetResult(ok ? GHOSTNET_OK : GHOSTNET_ERROR, msg);

	return 0;
}

static bool ghostnetStart(s32 job)
{
	if (g_State == GHOSTNET_BUSY) {
		return false;
	}

	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	// The account as it stands now. Both account pages are reachable while a
	// request is in flight, so the name and PIN in the boxes are not
	// necessarily the ones this job was started with.
	snprintf(g_JobUser, sizeof(g_JobUser), "%s", g_GhostNetUser);
	snprintf(g_JobPin, sizeof(g_JobPin), "%s", g_GhostNetPin);

	g_Job = job;
	ghostnetSetResult(GHOSTNET_BUSY, "talking to the server...");

	g_Thread = SDL_CreateThread(ghostnetWorker, "pd-ghostnet", NULL);

	if (g_Thread == NULL) {
		ghostnetSetResult(GHOSTNET_ERROR, "could not start the network thread");
		return false;
	}

	return true;
}

bool ghostnetIsAvailable(void)
{
	return true;
}

const char *ghostnetGetAccountName(void)
{
	return g_GhostNetUser;
}

static char *ghostnetSlotUser(s32 index)
{
	return index == 0 ? g_GhostNetUser : g_GhostNetSavedUser[index - 1];
}

static char *ghostnetSlotPin(s32 index)
{
	return index == 0 ? g_GhostNetPin : g_GhostNetSavedPin[index - 1];
}

/**
 * How many slots the chooser lists: the ones with a name in them, plus the
 * first empty one if there is room, which is the row that means "another".
 */
s32 ghostnetGetNumAccounts(void)
{
	s32 i;

	for (i = 0; i < GHOSTNET_MAXACCOUNTS; i++) {
		if (ghostnetSlotUser(i)[0] == '\0') {
			return i;
		}
	}

	return GHOSTNET_MAXACCOUNTS;
}

const char *ghostnetGetAccountAt(s32 index)
{
	if (index < 0 || index >= GHOSTNET_MAXACCOUNTS) {
		return "";
	}

	return ghostnetSlotUser(index);
}

/**
 * Make a remembered account the active one and prove it against the server.
 *
 * The swap is the whole of the local work; the login that follows is what says
 * whether the PIN still opens that account. Choosing an account offline leaves
 * it selected with an error on screen, which is the right outcome: recording
 * carries on and stamps runs with the account the player chose, and the
 * uploading those runs will need is the thing that was never going to work
 * without a network anyway.
 */
void ghostnetSelectAccount(s32 index)
{
	char user[GHOSTNET_MAXUSER + 2];
	char pin[GHOSTNET_MAXPIN + 2];

	if (index <= 0 || index >= GHOSTNET_MAXACCOUNTS || ghostnetSlotUser(index)[0] == '\0') {
		return;
	}

	snprintf(user, sizeof(user), "%s", g_GhostNetUser);
	snprintf(pin, sizeof(pin), "%s", g_GhostNetPin);

	snprintf(g_GhostNetUser, sizeof(g_GhostNetUser), "%s", ghostnetSlotUser(index));
	snprintf(g_GhostNetPin, sizeof(g_GhostNetPin), "%s", ghostnetSlotPin(index));

	snprintf(ghostnetSlotUser(index), GHOSTNET_MAXUSER + 2, "%s", user);
	snprintf(ghostnetSlotPin(index), GHOSTNET_MAXPIN + 2, "%s", pin);

	if (ghostnetIsAvailable()) {
		ghostnetLogin();
	}
}

/**
 * Put the active account aside and clear the fields for a new one.
 *
 * Without this, making a second account would type over the first and the
 * player would find they had signed out of something they never left. If every
 * slot is full the oldest remembered one goes, which is the only slot that can
 * go without losing what is in front of the player.
 */
void ghostnetBeginNewAccount(void)
{
	s32 i;

	if (g_GhostNetUser[0]) {
		for (i = GHOSTNET_MAXACCOUNTS - 1; i > 1; i--) {
			snprintf(ghostnetSlotUser(i), GHOSTNET_MAXUSER + 2, "%s", ghostnetSlotUser(i - 1));
			snprintf(ghostnetSlotPin(i), GHOSTNET_MAXPIN + 2, "%s", ghostnetSlotPin(i - 1));
		}

		snprintf(ghostnetSlotUser(1), GHOSTNET_MAXUSER + 2, "%s", g_GhostNetUser);
		snprintf(ghostnetSlotPin(1), GHOSTNET_MAXPIN + 2, "%s", g_GhostNetPin);
	}

	g_GhostNetUser[0] = '\0';
	g_GhostNetPin[0] = '\0';
}

/**
 * Say so when the PIN is about to travel in the clear.
 *
 * Mod.GhostServer is there so a build can be pointed at a local copy for
 * testing, which means it can also be pointed at a plain http server by
 * accident - and the PIN goes in a header. Loopback is the testing case and is
 * left alone; anything else gets a line in the log, because refusing outright
 * would break the one legitimate reason the setting exists.
 */
static void ghostnetCheckUrl(void)
{
	if (strncmp(g_GhostNetUrl, "https://", 8) == 0) {
		return;
	}

	if (strncmp(g_GhostNetUrl, "http://127.0.0.1", 16) == 0
			|| strncmp(g_GhostNetUrl, "http://localhost", 16) == 0
			|| strncmp(g_GhostNetUrl, "http://[::1]", 12) == 0) {
		return;
	}

	sysLogPrintf(LOG_WARNING,
			"ghost: Mod.GhostServer is %s - the account PIN will be sent unencrypted",
			g_GhostNetUrl);
}

void ghostnetInit(void)
{
	g_Lock = SDL_CreateMutex();
	ghostnetCheckUrl();

#ifndef PD_GHOST_WINHTTP
	curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

void ghostnetShutdown(void)
{
	if (g_Thread) {
		SDL_WaitThread(g_Thread, NULL);
		g_Thread = NULL;
	}

	if (g_Lock) {
		SDL_DestroyMutex(g_Lock);
		g_Lock = NULL;
	}

#ifndef PD_GHOST_WINHTTP
	curl_global_cleanup();
#endif
}

void ghostnetRegister(void)
{
	ghostnetStart(JOB_REGISTER);
}

void ghostnetLogin(void)
{
	ghostnetStart(JOB_LOGIN);
}

void ghostnetUploadMine(void)
{
	// Reading the directory is the slow half of this and it happens here, on
	// the main thread, because the catalogue it fills belongs to the menu.
	// It is one header read per ghost - the same work opening My Ghosts does,
	// and the same one frame it costs there.
	if (ghostnetGetState() == GHOSTNET_BUSY) {
		return;
	}

	ghostnetQueueUploads();
	ghostnetStart(JOB_UPLOAD);
}

void ghostnetFetchBoard(s32 stagenum, s32 difficulty)
{
	g_JobStage = stagenum;
	g_JobDiff = difficulty;
	ghostnetStart(JOB_BOARD);
}

/**
 * Reduce a name from the board to something safe to put in a path.
 *
 * The same treatment modGhostSafeName() gives a local run's filename, and for
 * a stronger reason: that one starts from a name the player typed on this
 * machine, and this one starts from a string the server sent. The server this
 * was written against will not send anything strange, but "the file is written
 * where the reply says" is not a sentence that should be true - a name is
 * fifteen characters of anything but a quote, which is enough for slashes,
 * backslashes and dots, and it lands in a path that is then created.
 *
 * Letters and digits survive and everything else becomes an underscore, so a
 * name that means nothing to a filesystem still produces a file, and the name
 * shown in the chooser comes out of the downloaded ghost's header rather than
 * out of this.
 */
static void ghostnetSafeName(const char *src, char *dst, u32 dstsize)
{
	u32 i;

	for (i = 0; i + 1 < dstsize && src[i]; i++) {
		char c = src[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
			dst[i] = c;
		} else {
			dst[i] = '_';
		}
	}

	dst[i] = '\0';

	if (i == 0) {
		snprintf(dst, dstsize, "%s", "player");
	}
}

void ghostnetDownload(s32 index)
{
	struct ghostboardentry *entry = ghostnetGetBoardEntry(index);
	char safe[GHOSTNET_MAXUSER + 2];

	if (entry == NULL) {
		return;
	}

	// Nothing may be written into the job while the worker is reading it. The
	// start below would refuse anyway, but the filename and the id are set
	// before that refusal and would be a download in flight having its target
	// changed underneath it.
	if (ghostnetGetState() == GHOSTNET_BUSY) {
		return;
	}

	g_JobId = entry->id;

	ghostnetSafeName(entry->user, safe, sizeof(safe));

	// Named the way a locally recorded ghost is named, time and all, so the
	// chooser lists it beside everything else. The time matters here because a
	// board holds a hundred rows without caring how many of them one player
	// owns: two of somebody's runs downloaded from the same board are two
	// files, and without the time the second would land on the first and the
	// list would show one row where the player asked for two.
	snprintf(g_JobFile, sizeof(g_JobFile), MODGHOST_DIR "/pd-s%02d-d%d-%s-%06u" MODGHOST_EXT,
			g_BoardStage, g_BoardDiff, safe, entry->time60);

	ghostnetStart(JOB_DOWNLOAD);
}

s32 ghostnetGetState(void)
{
	s32 state;

	SDL_LockMutex(g_Lock);
	state = g_State;
	SDL_UnlockMutex(g_Lock);

	return state;
}

/**
 * The last thing a request said, taken under the lock.
 *
 * A copy rather than the buffer itself: the worker writes g_Message while the
 * menu is drawing, so handing out a pointer into it was handing out a string
 * that could change halfway through being read. The copy is only ever written
 * from the calling thread, and the pages that read this immediately print it.
 */
const char *ghostnetGetMessage(void)
{
	static char copy[sizeof(g_Message)];

	SDL_LockMutex(g_Lock);
	memcpy(copy, g_Message, sizeof(copy));
	SDL_UnlockMutex(g_Lock);

	copy[sizeof(copy) - 1] = '\0';

	return copy;
}

#else // PD_GHOST_NET

bool ghostnetIsAvailable(void) { return false; }
const char *ghostnetGetAccountName(void) { return ""; }
void ghostnetInit(void) {}
void ghostnetShutdown(void) {}
void ghostnetRegister(void) {}
void ghostnetLogin(void) {}
void ghostnetUploadMine(void) {}
void ghostnetFetchBoard(s32 stagenum, s32 difficulty) {}
void ghostnetDownload(s32 index) {}
// ghostnetClearBoard() is not stubbed here. It only resets the counters below
// the #endif, needs no transport to do it, and is defined unconditionally
// there - a stub as well was a redefinition, which nothing noticed for as long
// as nothing built this branch.
s32 ghostnetGetNumAccounts(void) { return g_GhostNetUser[0] ? 1 : 0; }
const char *ghostnetGetAccountAt(s32 index) { return index == 0 ? g_GhostNetUser : ""; }
void ghostnetSelectAccount(s32 index) {}
void ghostnetBeginNewAccount(void) { g_GhostNetUser[0] = '\0'; g_GhostNetPin[0] = '\0'; }
s32 ghostnetGetState(void) { return GHOSTNET_IDLE; }

const char *ghostnetGetMessage(void)
{
	return "this build has no network support";
}

#endif // PD_GHOST_NET

bool ghostnetHasAccount(void)
{
	return g_GhostNetUser[0] != '\0' && g_GhostNetPin[0] != '\0';
}

/**
 * Whether the name and PIN in the boxes are ones the server would accept.
 *
 * The same rule the server applies at registration, checked here so that a
 * name it will refuse is refused before a request goes out. The page said
 * "3-15 characters: letters, digits, _ . -" on a label and then let anything
 * through, so a name with a space in it got as far as the server and came back
 * as an error message about a rule the player had already read and thought
 * they were following.
 *
 * Both ends keep the rule rather than one end trusting the other: this one is
 * for the player, and the server's is the one that means anything.
 */
bool ghostnetAccountIsValid(void)
{
	u32 len = strlen(g_GhostNetUser);
	u32 i;

	if (len < 3 || len > GHOSTNET_MAXUSER) {
		return false;
	}

	for (i = 0; i < len; i++) {
		char c = g_GhostNetUser[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
			return false;
		}
	}

	len = strlen(g_GhostNetPin);

	if (len < 4 || len > GHOSTNET_MAXPIN) {
		return false;
	}

	for (i = 0; i < len; i++) {
		if (g_GhostNetPin[i] < '0' || g_GhostNetPin[i] > '9') {
			return false;
		}
	}

	return true;
}

void ghostnetClearState(void)
{
	g_State = GHOSTNET_IDLE;
	g_Message[0] = '\0';
}

/**
 * Forget the board that is held.
 *
 * Called when the mission or difficulty selection changes, because the rows on
 * screen belong to whatever was last fetched and showing Villa's times under
 * the word Chicago is worse than showing none. The alternative - refetching on
 * every change - turns scrolling a dropdown into twenty requests.
 */
void ghostnetClearBoard(void)
{
	g_BoardCount = 0;
	g_BoardStage = -1;
	g_BoardDiff = -1;
}

s32 ghostnetGetBoardCount(void)
{
	return g_BoardCount;
}

s32 ghostnetGetBoardStage(void)
{
	return g_BoardStage;
}

s32 ghostnetGetBoardDifficulty(void)
{
	return g_BoardDiff;
}

struct ghostboardentry *ghostnetGetBoardEntry(s32 index)
{
	if (index < 0 || index >= g_BoardCount) {
		return NULL;
	}

	return &g_Board[index];
}
