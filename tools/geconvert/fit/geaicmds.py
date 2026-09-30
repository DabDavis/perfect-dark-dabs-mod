"""GoldenEye's AI command table: opcode -> (name, length, [(arg, width)]).

The opcode is the command's place in src/aicommands.def, which bondconstants.h's
AI_CMD enum is built from; cmdbuilder's output src/aicommands2.h carries each
command's macro (the arguments in order, a CharArrayFromN wrapper meaning N/8
bytes) and its _LENGTH.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths

H = os.path.join(paths.GE_DECOMP, 'src/aicommands2.h')


def table():
    txt = open(H).read()
    joined = re.sub(r'\\\n', ' ', txt)     # the macros are written over several lines
    out = []
    for m in re.finditer(r'^#define (\w+)\(([^)]*)\)(.*)$', joined, re.M):
        name, decl, body = m.group(1), m.group(2), m.group(3)
        bm = re.match(r'\s*AI_(\w+)\s*(?:,|$)', body)
        if not bm or bm.group(1) != name:
            continue                      # an alias (BREAK, LOOP, ...)
        if out and out[-1][0] == name:
            continue                      # the #ifdef __sgi / #else pair of one command
        args = []
        for part in body.split(',')[1:]:
            part = part.strip().rstrip('\\').strip()
            if not part or part.startswith('AI_'):
                continue
            w = re.match(r'CharArrayFrom(\d+)\((\w+)\)', part)
            if w:
                args.append((w.group(2), int(w.group(1)) // 8))
            elif re.match(r'^\w+$', part):
                args.append((part, 1))
        lm = re.search(r'^#define AI_%s_LENGTH\s+\(AICMDSIZE([^)]*)\)' % re.escape(name), txt, re.M)
        ln = 1 + sum(int(x) for x in re.findall(r'\+\s*(\d+)', lm.group(1))) if lm else None
        out.append((name, ln, args))
    return out


if __name__ == '__main__':
    t = table()
    bad = [(i, n, l, a) for i, (n, l, a) in enumerate(t) if l is None or l != 1 + sum(w for _, w in a)]
    print('%d commands, %d whose argument widths do not add up to the length' % (len(t), len(bad)))
    for i, n, l, a in bad:
        print('   %02x %-32s len %s args %s' % (i, n, l, a))
