/* n64twin: the real GoldenEye cartridge in ares, steered line by line.
 *
 * The GoldenEye side of tools/gefidelity's twin driver (common/aresge.py) runs
 * this on the oracle host and talks to it over a pipe, the way gdbge.py talks to
 * gdb: one command per line on stdin, every answer ends with a line starting
 * "ok" or "err". Nothing in it knows GoldenEye; addresses come from the driver.
 *
 *   frames N                      run N video frames
 *   frame                         ok <currentFrameCounter> <video frames run>
 *   until-word ADDR OP VAL [MAX]  run frames until the big-endian s32 at ADDR
 *                                 OP (>= == != <) VAL, at most MAX frames
 *   until-pc PC [COUNT] [MAX]     run frames until the function entry PC has
 *                                 fired COUNT times (counted from this command);
 *                                 stops at the end of that frame
 *   on-pc PC [if REG VAL] set-gpr REG VAL
 *   on-pc PC [if REG VAL] poke ADDR HEX
 *   until-fired N [MAX]           run frames until N armed actions have fired
 *                                 in all since power on (`fired` reports it)
 *                                 one-shot action at the next entry of PC
 *                                 (whose GPR REG equals VAL, if given)
 *   peek ADDR LEN                 ok <hex>, bytes as the game sees them
 *   poke ADDR HEX                 write bytes as the game would
 *   pad P BUTTONS [SX SY]         hold pad P from now on (overrides the script);
 *                                 `pad P script` hands it back to the cues
 *   cue FRAME P BUTTONS [SX SY]   add a pad-script cue at game frame FRAME
 *   shot PATH                     the next framebuffer the game hands to
 *                                 osViSwapBuffer, as a PPM; ok <w> <h>
 *   trace PC NAME [REG...] [stack N]
 *                                 log every entry of PC without stopping: the
 *                                 clock word, the frame, ra, the GPRs named and
 *                                 N words from sp (for callers further up)
 *   watch PC NAME ADDR LEN [ADDR LEN...]
 *                                 at every entry of PC, log those bytes
 *   trace-clock ADDR              the word every trace/watch line carries
 *                                 (GoldenEye's g_GlobalTimer)
 *   trace-dump                    the lines logged since the last dump, each
 *                                 "T name clock frame pc ra regs... s=stack" or
 *                                 "W name clock frame hex:hex...", then ok N
 *   trace-off                     drop every trace and watch
 *   pad-when ADDR OP VAL P BUTTONS [SX SY]
 *                                 hold pad P at BUTTONS from the first poll
 *                                 at which the s32 at ADDR OP VAL holds (one
 *                                 shot, applied in the order given): input
 *                                 keyed on the game's own clock, not frames
 *   pad-when-clear                drop the pending pad-when cues
 *   readwatch-arm                 log every CPU data read into a per-byte map of
 *                                 physical RDRAM (8 MB), cleared now; tools/
 *                                 gefidelity/census/ares. Off until armed: the
 *                                 core's cpuReadHook stays null
 *   readwatch-arm-at PC [if REG VAL]
 *                                 arm (and clear) at the next entry of PC
 *   readwatch-exclude LO HI       reads made from code in [LO, HI) (the last
 *                                 block entry's PC) go to bit 3 only
 *   readwatch-copy LO HI          ... to bit 2 only (copy routines)
 *   readwatch-phase N             other reads go to bit N (0 load, 1 play)
 *   readwatch-stats               ok <armed> <reads> <phase>
 *   readwatch-dump PATH ADDR LEN  LEN flag bytes from ADDR (physical, or a
 *                                 KSEG0 address), then LEN u32 first-reader
 *                                 PCs and LEN u32 last-reader PCs (host
 *                                 order, bits 0-1 reads only); ok LEN
 *   readwatch-off                 disarm (the map is kept for a dump)
 *   quit
 *
 * Traps carried over from oracle.cpp, each of which cost a run there:
 *   - ares fires the instruction prologue only at the entry of a recompiled
 *     block, so a PC is only ever a function entry; an interior PC never fires
 *     and reads as "the game never got there".
 *   - every read and write goes through the CPU's data cache (readDebug /
 *     writeDebug): the dcache is write-back, so raw RDRAM can hold a value
 *     the game has already overwritten.
 *   - ares RDRAM is in its "lsb" layout; going through the cache handles it.
 *   - the pad script's clock is the game's currentFrameCounter.
 */
#include <nall/main.hpp>
#include <ares/ares.hpp>
#include <mia/mia.hpp>
#include <n64/n64.hpp>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <iostream>

namespace {

std::shared_ptr<mia::Pak> systemPak, gamePak;
constexpr u32 ADDR_FRAME_COUNTER = 0x8004'8494;   /* currentFrameCounter */
constexpr u32 PC_VI_SWAP         = 0x7000'e490;   /* osViSwapBuffer */

auto paddr(u32 a) -> u32 { return a & 0x1fff'ffff; }
auto rb(u32 a) -> u32 {
  u32 p = paddr(a);
  return (u32)ares::Nintendo64::cpu.dcache.readDebug<ares::Nintendo64::Byte>(p | 0x8000'0000, p) & 0xff;
}
auto rw(u32 a) -> u32 { return (rb(a) << 24) | (rb(a + 1) << 16) | (rb(a + 2) << 8) | rb(a + 3); }
auto wb(u32 a, u8 v) -> void {
  u32 p = paddr(a);
  ares::Nintendo64::cpu.dcache.writeDebug<ares::Nintendo64::Byte>(p | 0x8000'0000, p, v);
}

/* ------------------------------------------------------------- pad input */
struct PadCue { u64 frame; int pad; u16 buttons; s32 sx, sy; };
std::vector<PadCue> padCues;
bool padScripted[4] = {};
bool padForced[4] = {};
int padScriptMaxPad = -1;
u64 padClock = 0;
size_t padCueNext = 0;
struct PadState { u16 buttons; s32 sx, sy; };
PadState padState[4] = {};

enum : u16 {
  PADB_A = 0x8000, PADB_B = 0x4000, PADB_Z = 0x2000, PADB_START = 0x1000,
  PADB_DUP = 0x0800, PADB_DDOWN = 0x0400, PADB_DLEFT = 0x0200, PADB_DRIGHT = 0x0100,
  PADB_L = 0x0020, PADB_R = 0x0010,
  PADB_CUP = 0x0008, PADB_CDOWN = 0x0004, PADB_CLEFT = 0x0002, PADB_CRIGHT = 0x0001,
};
struct PadName { const char* name; u16 bit; };
const PadName padNames[] = {
  {"A", PADB_A}, {"B", PADB_B}, {"Z", PADB_Z}, {"START", PADB_START},
  {"DUP", PADB_DUP}, {"DDOWN", PADB_DDOWN}, {"DLEFT", PADB_DLEFT}, {"DRIGHT", PADB_DRIGHT},
  {"L", PADB_L}, {"R", PADB_R},
  {"CUP", PADB_CUP}, {"CDOWN", PADB_CDOWN}, {"CLEFT", PADB_CLEFT}, {"CRIGHT", PADB_CRIGHT},
};
auto padBitForNode(const std::string& n) -> u16 {
  if(n == "A") return PADB_A;   if(n == "B") return PADB_B;   if(n == "Z") return PADB_Z;
  if(n == "Start") return PADB_START;
  if(n == "Up") return PADB_DUP; if(n == "Down") return PADB_DDOWN;
  if(n == "Left") return PADB_DLEFT; if(n == "Right") return PADB_DRIGHT;
  if(n == "L") return PADB_L;   if(n == "R") return PADB_R;
  if(n == "C-Up") return PADB_CUP; if(n == "C-Down") return PADB_CDOWN;
  if(n == "C-Left") return PADB_CLEFT; if(n == "C-Right") return PADB_CRIGHT;
  return 0;
}
auto padParseButtons(std::string s) -> u16 {
  u16 mask = 0;
  if(s.empty() || s == "-" || s == "0") return 0;
  if(s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (u16)strtoul(s.c_str(), nullptr, 16);
  size_t i = 0;
  while(i <= s.size()) {
    size_t plus = s.find('+', i);
    std::string tok = s.substr(i, plus == std::string::npos ? std::string::npos : plus - i);
    i = (plus == std::string::npos) ? s.size() + 1 : plus + 1;
    for(auto& c : tok) c = (char)toupper((unsigned char)c);
    for(auto& pn : padNames) if(tok == pn.name) { mask |= pn.bit; break; }
  }
  return mask;
}
auto padScriptLoad(const char* path) -> bool {
  FILE* f = fopen(path, "r");
  if(!f) return false;
  char line[256];
  while(fgets(line, sizeof(line), f)) {
    if(char* h = strchr(line, '#')) *h = 0;
    unsigned long long frame; int pad, sx = 0, sy = 0; char btn[64];
    int n = sscanf(line, "%llu %d %63s %d %d", &frame, &pad, btn, &sx, &sy);
    if(n < 3 || pad < 0 || pad > 3) continue;
    padCues.push_back({(u64)frame, pad, padParseButtons(btn), sx, sy});
    padScripted[pad] = true;
    if(pad > padScriptMaxPad) padScriptMaxPad = pad;
  }
  fclose(f);
  std::stable_sort(padCues.begin(), padCues.end(), [](auto& a, auto& b) { return a.frame < b.frame; });
  return true;
}
auto padIndexOfNode(ares::Node::Input::Input node) -> int {
  auto peripheral = node->parent().lock();
  if(!peripheral) return 0;
  auto port = peripheral->parent().lock();
  if(!port) return 0;
  std::string n{(const char*)port->name()};
  for(int i = 0; i < 4; i++) if(n == std::string("Controller Port ") + char('1' + i)) return i;
  return 0;
}

/* ------------------------------------------------------------- the hook */
struct Action {
  u32 pc; int condReg = -1; u64 condVal = 0;
  enum Kind { SetGpr, Poke } kind;
  int reg = 0; u64 val = 0; u32 addr = 0; std::vector<u8> bytes;
  bool done = false;
};
std::vector<Action> actions;
u32 countPc = 0; u64 countHits = 0; bool counting = false;
bool shotPending = false, shotDone = false;
std::string shotPath; u32 shotW = 0, shotH = 0;
u64 actionsFired = 0;

auto writeShot() -> void {
  u32 fb = (u32)ares::Nintendo64::cpu.ipu.r[4].u64;
  u32 w = (u32)ares::Nintendo64::vi.io.width;
  u32 depth = (u32)ares::Nintendo64::vi.io.colorDepth;
  if(!w || w > 1024 || depth != 2) return;     /* not a 16-bit frame; wait for the next */
  u32 h = (w > 320) ? 330 : 240;
  FILE* f = fopen(shotPath.c_str(), "wb");
  if(!f) { shotPending = false; shotDone = true; shotW = 0; return; }
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  for(u32 i = 0; i < w * h; i++) {
    u16 p = (u16)((rb(fb + i * 2) << 8) | rb(fb + i * 2 + 1));
    /* the port's fast3dRGBA5551ToRGB888, so pictures compare without a bias */
    u8 rgb[3] = {(u8)(((p >> 11) & 0x1f) * 255 / 31), (u8)(((p >> 6) & 0x1f) * 255 / 31),
                 (u8)(((p >> 1) & 0x1f) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  shotW = w; shotH = h; shotPending = false; shotDone = true;
}

/* non-stopping traces and watches: the gun diff's record-and-carry-on breakpoints */
struct Trace { u32 pc; std::string name; std::vector<int> regs; u32 stackWords = 0; };
struct Watch { u32 pc; std::string name; std::vector<std::pair<u32, u32>> regions; };
std::vector<Trace> traces;
std::vector<Watch> watches;
std::vector<u32> tracePcs;          /* every PC either kind listens on, for the fast reject */
u32 traceClock = 0;
std::vector<std::string> traceLog;
auto rebuildTracePcs() -> void {
  tracePcs.clear();
  for(auto& t : traces) tracePcs.push_back(t.pc);
  for(auto& w : watches) tracePcs.push_back(w.pc);
}
auto onTracePc(u32 pc) -> void {
  auto& r = ares::Nintendo64::cpu.ipu.r;
  u32 clock = traceClock ? rw(traceClock) : 0;
  s32 frame = (s32)rw(ADDR_FRAME_COUNTER);
  char buf[96];
  for(auto& t : traces) {
    if(t.pc != pc) continue;
    std::string l = "T " + t.name;
    snprintf(buf, sizeof buf, " %d %d %08x %08x", (s32)clock, frame, pc, (u32)r[31].u64); l += buf;
    for(int reg : t.regs) { snprintf(buf, sizeof buf, " r%d=%08x", reg, (u32)r[reg].u64); l += buf; }
    if(t.stackWords) {
      u32 sp = (u32)r[29].u64; l += " s=";
      for(u32 k = 0; k < t.stackWords; k++) { snprintf(buf, sizeof buf, k ? ",%08x" : "%08x", rw(sp + 4 * k)); l += buf; }
    }
    traceLog.push_back(l);
  }
  for(auto& w : watches) {
    if(w.pc != pc) continue;
    std::string l = "W " + w.name;
    snprintf(buf, sizeof buf, " %d %d ", (s32)clock, frame); l += buf;
    bool first = true;
    for(auto& rg : w.regions) {
      if(!first) l += ':';
      first = false;
      for(u32 k = 0; k < rg.second; k++) { snprintf(buf, sizeof buf, "%02x", rb(rg.first + k)); l += buf; }
    }
    traceLog.push_back(l);
  }
}

/* pad input keyed on a word of the game's memory (pad-when) */
struct WhenCue { u32 addr; std::string op; s64 val; int pad; u16 buttons; s32 sx, sy; bool done = false; };
std::vector<WhenCue> whenCues;
auto applyWhenCues() -> void {
  for(auto& c : whenCues) {
    if(c.done) continue;
    s64 w = (s32)rw(c.addr);
    bool met = c.op == ">=" ? w >= c.val : c.op == "==" ? w == c.val : c.op == ">" ? w > c.val :
               c.op == "<" ? w < c.val : c.op == "!=" ? w != c.val : false;
    if(!met) break;                 /* in order: a later cue waits for the earlier */
    padForced[c.pad] = true;
    padState[c.pad] = {c.buttons, c.sx, c.sy};
    c.done = true;
  }
}

/* the read watch (readwatch-*): which bytes of RDRAM the game's CPU reads, and
 * from which code - the census's "read by the cartridge" column. The PC is the
 * last recompiled block's entry, always inside the function doing the read. */
constexpr u32 READWATCH_RAM = 0x80'0000;
std::vector<u8> readFlags;
std::vector<u32> readFirstPc, readLastPc;
struct PcRange { u32 lo, hi; };
std::vector<PcRange> readExclude, readCopy;
u32 lastBlockPc = 0;
int readPhase = 0;
bool readArmed = false;
u64 readCount = 0;
u32 readArmPc = 0; int readArmReg = -1; u64 readArmVal = 0;
auto twinReadHook(u64, u32 paddr, u32 size) -> void {
  if(paddr >= READWATCH_RAM) return;
  readCount++;
  int cls = readPhase ? 1 : 0;
  for(auto& rg : readExclude) if(lastBlockPc >= rg.lo && lastBlockPc < rg.hi) { cls = 3; break; }
  if(cls < 2) for(auto& rg : readCopy) if(lastBlockPc >= rg.lo && lastBlockPc < rg.hi) { cls = 2; break; }
  u8 bit = (u8)(1 << cls);
  u32 end = std::min<u32>(paddr + size, READWATCH_RAM);
  for(u32 p = paddr; p < end; p++) {
    readFlags[p] |= bit;
    if(cls < 2) { if(!readFirstPc[p]) readFirstPc[p] = lastBlockPc; readLastPc[p] = lastBlockPc; }
  }
}
auto readWatchArm() -> void {
  readFlags.assign(READWATCH_RAM, 0); readFirstPc.assign(READWATCH_RAM, 0); readLastPc.assign(READWATCH_RAM, 0);
  readCount = 0; readArmed = true;
  ares::Nintendo64::cpuReadHook = twinReadHook;
}

extern "C" void twinTraceHook(u64 address, u32) {
  u32 pc = (u32)address;
  lastBlockPc = pc;
  if(readArmPc && pc == readArmPc &&
     (readArmReg < 0 || (u32)ares::Nintendo64::cpu.ipu.r[readArmReg].u64 == (u32)readArmVal)) {
    readWatchArm(); readArmPc = 0;
  }
  if(shotPending && pc == PC_VI_SWAP) { writeShot(); return; }
  if(counting && pc == countPc) countHits++;
  for(u32 tp : tracePcs) if(tp == pc) { onTracePc(pc); break; }
  if(actions.empty()) return;
  auto& r = ares::Nintendo64::cpu.ipu.r;
  for(auto& a : actions) {
    if(a.done || a.pc != pc) continue;
    if(a.condReg >= 0 && (u32)r[a.condReg].u64 != (u32)a.condVal) continue;
    if(a.kind == Action::SetGpr) r[a.reg].u64 = (u64)(s64)(s32)(u32)a.val;
    else for(size_t k = 0; k < a.bytes.size(); k++) wb(a.addr + (u32)k, a.bytes[k]);
    a.done = true;
    actionsFired++;
  }
  actions.erase(std::remove_if(actions.begin(), actions.end(), [](auto& a) { return a.done; }), actions.end());
}

/* ------------------------------------------------------------- platform */
u64 videoFrames = 0;
struct TwinPlatform : ares::Platform {
  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "Nintendo 64") return systemPak->pak;
    if(node->name() == "Nintendo 64 Cartridge") return gamePak->pak;
    return {};
  }
  auto audio(ares::Node::Audio::Stream) -> void override {}
  auto video(ares::Node::Video::Screen, const u32*, u32, u32, u32) -> void override { videoFrames++; }
  auto input(ares::Node::Input::Input node) -> void override {
    int idx = padIndexOfNode(node);
    if(!whenCues.empty()) applyWhenCues();
    if(!padCues.empty()) {
      u64 now = rw(ADDR_FRAME_COUNTER);
      if(now < padClock) padCueNext = 0;
      padClock = now;
      while(padCueNext < padCues.size() && padCues[padCueNext].frame <= padClock) {
        auto& c = padCues[padCueNext++];
        if(!padForced[c.pad]) padState[c.pad] = {c.buttons, c.sx, c.sy};
      }
    }
    std::string n{(const char*)node->name()};
    if(idx < 0 || idx > 3) idx = 0;
    if(auto bit = padBitForNode(n)) {
      if(auto b = node->cast<ares::Node::Input::Button>()) b->setValue((padState[idx].buttons & bit) != 0);
      return;
    }
    auto scale = [](s32 v) -> s32 { s64 r = (s64)v * 32767 / 85; return (s32)std::max<s64>(-32767, std::min<s64>(32767, r)); };
    if(auto axis = node->cast<ares::Node::Input::Axis>()) {
      if(n == "X-Axis") axis->setValue(scale(padState[idx].sx));
      else if(n == "Y-Axis") axis->setValue(scale(padState[idx].sy));
      else axis->setValue(0);
    }
  }
};
TwinPlatform twinPlatform;

auto hex2bytes(const std::string& h) -> std::vector<u8> {
  std::vector<u8> out;
  for(size_t i = 0; i + 1 < h.size(); i += 2) out.push_back((u8)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  return out;
}
auto num(const std::string& s) -> u64 { return strtoull(s.c_str(), nullptr, 0); }
auto snum(const std::string& s) -> s64 { return strtoll(s.c_str(), nullptr, 0); }

auto say(const char* fmt, ...) -> void {
  va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
  fputc('\n', stdout); fflush(stdout);
}

}

auto nall::main(Arguments arguments) -> void {
  string romPath, padScript;
  if(!arguments.take("--rom", romPath)) { say("err usage: n64twin --rom ROM [--pad-script F] [--pads N]"); return; }
  int pads = 1;
  { string t; if(arguments.take("--pads", t)) pads = (int)t.natural(); }
  if(arguments.take("--pad-script", padScript)) {
    if(!padScriptLoad((const char*)padScript)) { say("err cannot read pad script %s", (const char*)padScript); return; }
  }

  string home = {Path::userData(), "ares-oracle/"};
  directory::create(home);
  mia::setHomeLocation([home]() -> string { return home; });
  mia::setSaveLocation([home]() -> string { return home; });
  mia::construct();
  ares::platform = &twinPlatform;
  gamePak = mia::Medium::create("Nintendo 64");
  if(!gamePak || gamePak->load(romPath) != successful) { say("err ROM load failed"); return; }
  systemPak = mia::System::create("Nintendo 64");
  if(systemPak->load() != successful) { say("err system load failed"); return; }
  ares::Nintendo64::option("Quality", "SD");
  ares::Nintendo64::option("Enable GPU acceleration", "true");
  ares::Nintendo64::option("Recompiler", "true");
  ares::Nintendo64::option("Expansion Pak", "false");
  ares::Node::System root;
  if(!ares::Nintendo64::load(root, "[Nintendo] Nintendo 64 (NTSC)")) { say("err core load failed"); return; }
  if(auto port = root->find<ares::Node::Port>("Cartridge Slot")) { port->allocate(); port->connect(); }
  if(padScriptMaxPad + 1 > pads) pads = padScriptMaxPad + 1;
  pads = std::max(1, std::min(4, pads));
  for(int i = 0; i < pads; i++) {
    string name = {"Controller Port ", i + 1};
    if(auto port = root->find<ares::Node::Port>(name)) { port->allocate("Gamepad"); port->connect(); }
  }
  root->power();
  /* see oracle.cpp: stop the screen thread while the Screen is still owned */
  struct ScreenQuitter { ~ScreenQuitter() { if(auto& s = ares::Nintendo64::vi.screen) s->quit(); } } quitter;
  ares::Nintendo64::cpuTraceHook = twinTraceHook;
  ares::Nintendo64::cpu.recompiler.reset();
  ares::Nintendo64::cpu.recompiler.callInstructionPrologue = true;
  say("ok n64twin ready: %s, %d pad(s), %u cues", gamePak->pak->attribute("title").data(), pads, (u32)padCues.size());

  std::string line;
  while(std::getline(std::cin, line)) {
    std::vector<std::string> t;
    { size_t i = 0; while(i < line.size()) { while(i < line.size() && isspace((unsigned char)line[i])) i++;
        size_t j = i; while(j < line.size() && !isspace((unsigned char)line[j])) j++;
        if(j > i) t.push_back(line.substr(i, j - i)); i = j; } }
    if(t.empty()) continue;
    auto& c = t[0];
    if(c == "quit") { say("ok bye"); break; }
    else if(c == "frame") { say("ok %d %llu", (s32)rw(ADDR_FRAME_COUNTER), (unsigned long long)videoFrames); }
    else if(c == "frames" && t.size() >= 2) {
      u64 n = num(t[1]); for(u64 i = 0; i < n; i++) root->run();
      say("ok %d", (s32)rw(ADDR_FRAME_COUNTER));
    }
    else if(c == "until-word" && t.size() >= 4) {
      u32 a = (u32)num(t[1]); std::string op = t[2]; s64 v = snum(t[3]);
      u64 max = t.size() >= 5 ? num(t[4]) : 100000, i = 0;
      auto met = [&]() { s64 w = (s32)rw(a); return op == ">=" ? w >= v : op == "==" ? w == v : op == "!=" ? w != v : op == "<" ? w < v : op == ">" ? w > v : false; };
      while(!met() && i < max) { root->run(); i++; }
      if(met()) say("ok %d %llu", (s32)rw(a), (unsigned long long)i); else say("err timeout %d", (s32)rw(a));
    }
    else if(c == "until-pc" && t.size() >= 2) {
      countPc = (u32)num(t[1]); u64 want = t.size() >= 3 ? num(t[2]) : 1, max = t.size() >= 4 ? num(t[3]) : 100000, i = 0;
      countHits = 0; counting = true;
      while(countHits < want && i < max) { root->run(); i++; }
      counting = false;
      if(countHits >= want) say("ok %llu %llu", (unsigned long long)countHits, (unsigned long long)i);
      else say("err timeout %llu", (unsigned long long)countHits);
    }
    else if(c == "on-pc" && t.size() >= 4) {
      Action a; a.pc = (u32)num(t[1]); size_t k = 2;
      if(t[k] == "if" && t.size() >= k + 3) { a.condReg = (int)num(t[k + 1]); a.condVal = num(t[k + 2]); k += 3; }
      if(k < t.size() && t[k] == "set-gpr" && t.size() >= k + 3) { a.kind = Action::SetGpr; a.reg = (int)num(t[k + 1]); a.val = (u64)snum(t[k + 2]); }
      else if(k < t.size() && t[k] == "poke" && t.size() >= k + 3) { a.kind = Action::Poke; a.addr = (u32)num(t[k + 1]); a.bytes = hex2bytes(t[k + 2]); }
      else { say("err on-pc: want set-gpr REG VAL or poke ADDR HEX"); continue; }
      actions.push_back(a);
      say("ok armed %zu", actions.size());
    }
    else if(c == "fired") { say("ok %llu %zu", (unsigned long long)actionsFired, actions.size()); }
    else if(c == "until-fired" && t.size() >= 2) {
      /* run until the armed actions have fired N times in all (counted since power on) */
      u64 want = num(t[1]), max = t.size() >= 3 ? num(t[2]) : 100000, i = 0;
      while(actionsFired < want && i < max) { root->run(); i++; }
      if(actionsFired >= want) say("ok %llu %llu %d", (unsigned long long)actionsFired, (unsigned long long)i, (s32)rw(ADDR_FRAME_COUNTER));
      else say("err timeout %llu", (unsigned long long)actionsFired);
    }
    else if(c == "peek" && t.size() >= 3) {
      u32 a = (u32)num(t[1]), n = (u32)num(t[2]);
      std::string out; out.reserve(n * 2); char b[3];
      for(u32 k = 0; k < n; k++) { snprintf(b, 3, "%02x", rb(a + k)); out += b; }
      say("ok %s", out.c_str());
    }
    else if(c == "poke" && t.size() >= 3) {
      u32 a = (u32)num(t[1]); auto bytes = hex2bytes(t[2]);
      for(size_t k = 0; k < bytes.size(); k++) wb(a + (u32)k, bytes[k]);
      say("ok %zu", bytes.size());
    }
    else if(c == "cue" && t.size() >= 4) {
      /* a pad-script line added at run time: FRAME (currentFrameCounter) PAD BUTTONS [SX SY] */
      int p = (int)num(t[2]); if(p < 0 || p > 3) { say("err pad"); continue; }
      PadCue q{num(t[1]), p, padParseButtons(t[3]), t.size() >= 5 ? (s32)snum(t[4]) : 0, t.size() >= 6 ? (s32)snum(t[5]) : 0};
      auto at = std::upper_bound(padCues.begin() + (long)padCueNext, padCues.end(), q,
                                 [](auto& a, auto& b) { return a.frame < b.frame; });
      padCues.insert(at, q);
      say("ok %zu", padCues.size());
    }
    else if(c == "pad" && t.size() >= 3) {
      int p = (int)num(t[1]); if(p < 0 || p > 3) { say("err pad"); continue; }
      if(t[2] == "script") { padForced[p] = false; say("ok"); continue; }
      padForced[p] = true;
      padState[p] = {padParseButtons(t[2]), t.size() >= 4 ? (s32)snum(t[3]) : 0, t.size() >= 5 ? (s32)snum(t[4]) : 0};
      say("ok");
    }
    else if(c == "shot" && t.size() >= 2) {
      shotPath = t[1]; shotPending = true; shotDone = false; u64 i = 0;
      while(!shotDone && i < 600) { root->run(); i++; }
      if(shotDone && shotW) say("ok %u %u", shotW, shotH); else { shotPending = false; say("err no frame"); }
    }
    else if(c == "trace" && t.size() >= 3) {
      Trace tr; tr.pc = (u32)num(t[1]); tr.name = t[2];
      for(size_t k = 3; k < t.size(); k++) {
        if(t[k] == "stack" && k + 1 < t.size()) { tr.stackWords = (u32)std::min<u64>(num(t[k + 1]), 64); k++; }
        else { int reg = (int)num(t[k][0] == 'r' ? t[k].substr(1) : t[k]); if(reg >= 0 && reg < 32) tr.regs.push_back(reg); }
      }
      traces.push_back(tr); rebuildTracePcs();
      say("ok %zu", traces.size());
    }
    else if(c == "watch" && t.size() >= 5 && (t.size() - 3) % 2 == 0) {
      Watch w; w.pc = (u32)num(t[1]); w.name = t[2];
      for(size_t k = 3; k + 1 < t.size(); k += 2) w.regions.push_back({(u32)num(t[k]), (u32)std::min<u64>(num(t[k + 1]), 4096)});
      watches.push_back(w); rebuildTracePcs();
      say("ok %zu", watches.size());
    }
    else if(c == "trace-clock" && t.size() >= 2) { traceClock = (u32)num(t[1]); say("ok"); }
    else if(c == "trace-dump") {
      for(auto& l : traceLog) { fputs(l.c_str(), stdout); fputc('\n', stdout); }
      size_t n = traceLog.size(); traceLog.clear();
      say("ok %zu", n);
    }
    else if(c == "trace-off") { traces.clear(); watches.clear(); rebuildTracePcs(); traceLog.clear(); say("ok"); }
    else if(c == "pad-when" && t.size() >= 6) {
      int p = (int)num(t[4]); if(p < 0 || p > 3) { say("err pad"); continue; }
      WhenCue q{(u32)num(t[1]), t[2], snum(t[3]), p, padParseButtons(t[5]),
                t.size() >= 7 ? (s32)snum(t[6]) : 0, t.size() >= 8 ? (s32)snum(t[7]) : 0};
      whenCues.push_back(q);
      say("ok %zu", whenCues.size());
    }
    else if(c == "pad-when-clear") { whenCues.clear(); say("ok"); }
    else if(c == "readwatch-arm") { readWatchArm(); say("ok"); }
    else if(c == "readwatch-arm-at" && t.size() >= 2) {
      readArmPc = (u32)num(t[1]); readArmReg = -1;
      if(t.size() >= 5 && t[2] == "if") { readArmReg = (int)num(t[3]); readArmVal = num(t[4]); }
      say("ok");
    }
    else if((c == "readwatch-exclude" || c == "readwatch-copy") && t.size() >= 3) {
      (c == "readwatch-exclude" ? readExclude : readCopy).push_back({(u32)num(t[1]), (u32)num(t[2])});
      say("ok");
    }
    else if(c == "readwatch-phase" && t.size() >= 2) { readPhase = (int)num(t[1]) ? 1 : 0; say("ok %d", readPhase); }
    else if(c == "readwatch-stats") { say("ok %d %llu %d", readArmed ? 1 : 0, (unsigned long long)readCount, readPhase); }
    else if(c == "readwatch-off") { ares::Nintendo64::cpuReadHook = nullptr; readArmed = false; readArmPc = 0; say("ok"); }
    else if(c == "readwatch-dump" && t.size() >= 4) {
      u32 a = (u32)num(t[2]) & 0x1fff'ffff, n = (u32)num(t[3]);
      if(readFlags.empty() || a >= READWATCH_RAM || n > READWATCH_RAM - a) { say("err readwatch-dump: not armed or out of range"); continue; }
      FILE* f = fopen(t[1].c_str(), "wb");
      if(!f) { say("err cannot write %s", t[1].c_str()); continue; }
      fwrite(readFlags.data() + a, 1, n, f);
      fwrite(readFirstPc.data() + a, 4, n, f);
      fwrite(readLastPc.data() + a, 4, n, f);
      fclose(f);
      say("ok %u", n);
    }
    else say("err unknown: %s", line.c_str());
  }
}
