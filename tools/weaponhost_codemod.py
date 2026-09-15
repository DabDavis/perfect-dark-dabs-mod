#!/usr/bin/env python3
"""Route weapon-number switches and ==/!= tests through weaponHost().

  weaponhost_codemod.py [--apply] FILE...

A switch whose own case labels (not a nested switch's) name WEAPON_ constants
gets its expression wrapped; `EXPR == WEAPON_X` / `EXPR != WEAPON_X` and the
mirrored forms get EXPR wrapped. Skipped: WEAPON_NONE, WEAPON_UNARMED and the
MP location markers (a host is never one of those, so the test is unchanged),
expressions already wrapped, preprocessor lines and comments. Prints every
change as file:line: before -> after.
"""
import re
import sys

SKIP_CONSTS = re.compile(r'WEAPON_(NONE|UNARMED|MPLOCATION\d\d|GE_\w+)\b')


def mask_comments_strings(src):
    """Same length as src, comments/strings/preprocessor lines blanked."""
    out = list(src)
    i = 0
    n = len(src)
    line_start = True
    while i < n:
        c = src[i]
        if line_start and c in ' \t':
            i += 1
            continue
        if line_start and c == '#':
            j = src.find('\n', i)
            j = n if j < 0 else j
            # continued lines
            while j > 0 and src[j - 1] == '\\':
                k = src.find('\n', j + 1)
                j = n if k < 0 else k
            for k in range(i, j):
                out[k] = ' '
            i = j
            continue
        line_start = False
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
            continue
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                out[k] = ' '
            i = j + 1
            continue
        if c == '\n':
            line_start = True
        i += 1
    return ''.join(out)


def match_forward(m, i, open_c, close_c):
    depth = 0
    while i < len(m):
        if m[i] == open_c:
            depth += 1
        elif m[i] == close_c:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def postfix_start(m, end):
    """Start of the postfix expression ending just before `end` (exclusive)."""
    i = end
    while i > 0 and m[i - 1] in ' \t':
        i -= 1
    stop = i
    while i > 0:
        c = m[i - 1]
        if c in ')]':
            open_c = '(' if c == ')' else '['
            depth = 0
            j = i - 1
            while j >= 0:
                if m[j] == c:
                    depth += 1
                elif m[j] == open_c:
                    depth -= 1
                    if depth == 0:
                        break
                j -= 1
            i = j
            continue
        if c.isalnum() or c == '_':
            i -= 1
            continue
        if c == '.':
            i -= 1
            continue
        if c == '>' and i > 1 and m[i - 2] == '-':
            i -= 2
            continue
        break
    # a leading parenthesised group is a cast or grouping; a bare '(' group
    # followed by nothing is the whole thing - both fine
    return i, stop


def postfix_end(m, start):
    i = start
    while i < len(m) and m[i] in ' \t':
        i += 1
    begin = i
    while i < len(m):
        c = m[i]
        if c.isalnum() or c in '_.':
            i += 1
        elif m.startswith('->', i):
            i += 2
        elif c in '([':
            close_c = ')' if c == '(' else ']'
            j = match_forward(m, i, c, close_c)
            if j < 0:
                break
            i = j + 1
        else:
            break
    return begin, i


def find_switch_edits(src, m):
    edits = []
    for sw in re.finditer(r'\bswitch\s*\(', m):
        po = sw.end() - 1
        pc = match_forward(m, po, '(', ')')
        if pc < 0:
            continue
        bo = m.find('{', pc)
        if bo < 0 or m[pc + 1:bo].strip():
            continue
        bc = match_forward(m, bo, '{', '}')
        body = m[bo + 1:bc]
        # blank nested switch bodies
        depth_body = list(body)
        for nsw in re.finditer(r'\bswitch\s*\(', body):
            npo = nsw.end() - 1
            npc = match_forward(body, npo, '(', ')')
            nbo = body.find('{', npc)
            nbc = match_forward(body, nbo, '{', '}')
            for k in range(nsw.start(), nbc + 1):
                depth_body[k] = ' '
        direct = ''.join(depth_body)
        labels = re.findall(r'\bcase\s+(WEAPON_\w+)', direct)
        if not labels:
            continue
        expr = src[po + 1:pc]
        if expr.strip().startswith('weaponHost('):
            continue
        edits.append((po + 1, pc, 'weaponHost(%s)' % expr.strip()))
    return edits


def find_compare_edits(src, m):
    edits = []
    for cm in re.finditer(r'(==|!=)', m):
        op_s, op_e = cm.start(), cm.end()
        # right side a constant?
        rb, re_ = postfix_end(m, op_e)
        rhs = m[rb:re_]
        lb, le = postfix_start(m, op_s)
        lhs = m[lb:le]
        if re.fullmatch(r'WEAPON_\w+', rhs) and lhs:
            if SKIP_CONSTS.fullmatch(rhs) or re.fullmatch(r'WEAPON_\w+', lhs):
                continue
            if lhs.startswith('weaponHost(') or lhs.startswith('('):
                # a parenthesised left side: wrap the whole group
                pass
            if lhs.startswith('weaponHost('):
                continue
            edits.append((lb, le, 'weaponHost(%s)' % src[lb:le]))
        else:
            lhs_const = re.fullmatch(r'WEAPON_\w+', lhs)
            if lhs_const and rhs and not re.fullmatch(r'WEAPON_\w+', rhs):
                if SKIP_CONSTS.fullmatch(lhs) or rhs.startswith('weaponHost('):
                    continue
                edits.append((rb, re_, 'weaponHost(%s)' % src[rb:re_]))
    return edits


def main():
    apply = '--apply' in sys.argv
    files = [a for a in sys.argv[1:] if a != '--apply']
    total = 0
    for path in files:
        src = open(path, encoding='utf-8', errors='surrogateescape').read()
        m = mask_comments_strings(src)
        edits = find_switch_edits(src, m) + find_compare_edits(src, m)
        edits.sort(key=lambda e: e[0])
        # drop overlapping
        kept = []
        last = -1
        for e in edits:
            if e[0] >= last:
                kept.append(e)
                last = e[1]
        out = src
        for s, e, rep in sorted(kept, key=lambda e: -e[0]):
            line = src.count('\n', 0, s) + 1
            print('%s:%d: %s -> %s' % (path, line, src[s:e].strip(), rep))
            out = out[:s] + rep + out[e:]
        total += len(kept)
        if apply and kept:
            open(path, 'w', encoding='utf-8', errors='surrogateescape').write(out)
    print('edits', total, file=sys.stderr)


if __name__ == '__main__':
    main()
