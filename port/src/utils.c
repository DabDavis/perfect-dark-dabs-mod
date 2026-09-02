#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "utils.h"

static inline bool isSingleCharToken(const s32 ch)
{
	switch (ch) {
		case '{':
		case '}':
		case '[':
		case ']':
		case '(':
		case ')':
		case ',':
		case '\'':
		case '=':
			return true;
		default:
			return false;
	}
}

char *strFmt(const char *fmt, ...)
{
	static char buf[4096];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return buf;
}

char *strRightTrim(char *str)
{
	if (!str) {
		return NULL;
	}

	const s32 len = strlen(str);
	for (s32 i = len - 1; i >= 0 && isspace(str[i]); --i) {
		str[i] = '\0';
	}

	return str;
}

char *strTrim(char *str)
{
	if (!str) {
		return NULL;
	}

	// left trim
	//
	// <= rather than <, so that an actual space is trimmed and not only the
	// control characters below it. The right trim below has always used
	// isspace(), which does include the space, and the disagreement was
	// invisible until a string setting was written the way an ini is usually
	// written: `Key = value` kept the space, and `Key = "value"` kept the
	// space and therefore never reached the unquoting below it either, so the
	// value came out with its quotes still attached. Mod.GhostServer set that
	// way pointed every request at a URL beginning with a space.
	while (*str && (u8)*str <= ' ') {
		++str;
	}

	// right trim
	const s32 len = strlen(str);
	for (s32 i = len - 1; i > 0 && isspace(str[i]); --i) {
		str[i] = '\0';
	}

	return str;
}

char *strUnquote(char *str)
{
	if (!str) {
		return NULL;
	}

	if (*str == '"') {
		++str;
	}

	char *end = strrchr(str, '"');
	if (end && end[1] == '\0') {
		*end = '\0';
	}

	return str;
}

char *strParseToken(char *str, char *out, s32 *outCount)
{
	if (outCount) {
		*outCount = 0;
	}

	if (!out) {
		return NULL;
	}

	out[0] = '\0';

	if (!str) {
		return NULL;
	}

	s32 cnt = 0;
	while (*str) {
		// skip whitespace and other garbage
		while (*str && (u8)*str <= ' ') {
			++str;
		}
		if (!*str) {
			// just whitespace
			return NULL;
		}

		// skip #, ; and // comments
		if (*str == ';' || *str == '#' || (str[0] == '/' && str[1] == '/')) {
			while (*str && *str != '\n') {
				++str;
			}
			continue;
		}

		if (*str == '"') {
			// quoted string; treat it as an entire token, including the quotes
			out[cnt++] = *str++;
			while (true) {
				if (*str == '\0') {
					// unterminated quoted string
					break;
				}

				if (str[0] == '\\' && str[1] == '"') {
					// escaped quote; add to token
					if (cnt + 1 < UTIL_MAX_TOKEN) {
						out[cnt++] = str[1];
					}
					str += 2;
					continue;
				}

				// add char to token
				const s32 ch = (u8)*str++;
				if (cnt + 1 < UTIL_MAX_TOKEN) {
					out[cnt++] = ch;
				}

				if (ch == '"') {
					// ch was the closing quote
					break;
				}
			}
			break;
		}

		if (isSingleCharToken(*str)) {
			// single char token
			out[cnt++] = *str++;
			break;
		}

		// regular single word
		do {
			if (cnt + 1 < UTIL_MAX_TOKEN) {
				out[cnt++] = *str++;
			}
			if (isSingleCharToken(*str)) {
				break;
			}
		} while ((u8)*str > ' ');
		break;
	}

	out[cnt] = '\0';
	if (outCount) {
		*outCount = cnt;
	}

	return str;
}

char *strDuplicate(const char *str)
{
	if (!str) {
		return NULL;
	}
	const u32 len = strlen(str);
	char *out = sysMemAlloc(len + 1);
	if (out) {
		memcpy(out, str, len + 1);
	}
	return out;
}