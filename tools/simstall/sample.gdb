set pagination off
set confirm off
python
import gdb
n = int(gdb.parse_and_eval("g_BotCount"))
f = int(gdb.parse_and_eval("g_Vars.lvframe60"))
out = ["F %d d%d t%d" % (f, int(gdb.parse_and_eval("g_BotConfigsArray[0].difficulty")), int(gdb.parse_and_eval("g_BotConfigsArray[0].type")))]
for i in range(n):
    c = gdb.parse_and_eval("g_MpBotChrPtrs[%d]" % i)
    if int(c) == 0: continue
    p = c['prop']['pos']
    out.append("b%d act=%d my=%d pos=%.0f,%.0f,%.0f" % (i, int(c['actiontype']), int(c['myaction']), float(p['x']), float(p['y']), float(p['z'])))
print(" | ".join(out))
end
detach
quit
