#!/usr/bin/env python3
"""
Check that picking a recording encoder actually changes what encodes.

    python3 tools/recordcodec/test_recordcodec.py

The menu row writes one string and the next recording reads it, with a probe, a
fallback search and a config file in between. Every part of that can look right
while the recording is made by something else: the log line naming an encoder is
printed from the same variable the menu set, so on its own it proves only that
the game remembered what it was told.

So the check is the file. ffmpeg writes the encoder into the video stream's
metadata - `Lavc61.19.101 h264_vaapi` - which is written by the thing that did
the encoding and cannot be faked by a mistake anywhere in this code.

What it does, headlessly and in a temporary directory:

  1. boots the game on its own X display, to its menu - the recorder runs off
     the pre-swap callback, so there is nothing a match would add
  2. sets the encoder the way the menu does, through recordSetCodecIndex()
  3. presses the real record key and waits for a real mp4
  4. asks the file which encoder wrote it, and fails if it is not the one picked
  5. quits properly, and checks the choice came back as the default

Most of the list is unusable on any given machine - an AMD card cannot use NVENC
and an Nvidia one cannot use VAAPI - so opening the dropdown probes them and
marks the ones that will not work. Two cases cover that: the marking has to
agree with what actually happens, and an encoder it marked unavailable, if
picked anyway, still has to produce a recording on whatever the search finds
instead, with the row correcting itself to name it. Falling back silently while
the menu goes on claiming the encoder the player chose is the failure this is
really looking for.

Nothing but the standard library. Needs Xvfb, xdotool, gdb and ffprobe, a build
in build/ and its ROM; it says which is missing and skips rather than failing if
the machine cannot run it.
"""

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

GAME = os.path.join(ROOT, "build", "pd.x86_64")

# Small and slow: the picture is not being looked at, and every frame of it is
# one some encoder has to get through. 640x480 at 24fps records in real time on
# libx264 as well as on the hardware encoders, which is what makes the software
# case quick enough to be worth having.
WIDTH, HEIGHT, FPS = 640, 480, 24

RECORD_SECONDS = 3.0
BOOT_TIMEOUT = 90.0
RECORD_TIMEOUT = 60.0

# A display of our own, away from :0 and from the :99 someone may be using by
# hand. Xvfb refuses one that is taken, which is the check that it is ours.
DISPLAY = os.environ.get("PDRECORD_TEST_DISPLAY", ":97")


class Skip(Exception):
    pass


class Failure(Exception):
    pass


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, **kw)


# ----------------------------------------------------------------- the harness


class Game:
    """The game on its own display, with a log that can be waited on."""

    def __init__(self, savedir, logpath):
        self.savedir = savedir
        self.logpath = logpath
        self.proc = None
        self.window = None
        self.env = dict(os.environ, DISPLAY=DISPLAY)

    def start(self):
        log = open(self.logpath, "ab", buffering=0)
        self.proc = subprocess.Popen(
            # stdbuf, because stdout to a file is block buffered and the NOTE
            # lines this reads would not appear until the game exited.
            # No --boot-stage: the recorder runs off the pre-swap callback, so
            # it records the menu as readily as a match, and a fresh save
            # directory has no eeprom to enter a match with anyway - the game
            # sits on a black screen waiting for one that is never coming.
            ["stdbuf", "-oL", GAME, "--savedir", self.savedir],
            cwd=os.path.dirname(GAME),
            stdout=log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, env=self.env,
            start_new_session=True,
        )
        self.window = self._await_window()

    def _await_window(self):
        deadline = time.time() + BOOT_TIMEOUT
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise Failure("the game exited while starting up:\n" + self.tail())
            r = run(["xdotool", "search", "--pid", str(self.proc.pid)], env=self.env)
            ids = [x for x in r.stdout.split() if x.strip()]
            if ids:
                # The window exists well before the game is drawing into it.
                # Nothing here needs more than frames going past, and the record
                # key retries anyway, so a settle is enough.
                time.sleep(8.0)
                return ids[-1]
            time.sleep(0.5)
        raise Failure("no window after %gs" % BOOT_TIMEOUT)

    def read(self):
        with open(self.logpath, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def tail(self, n=25):
        return "\n".join(self.read().splitlines()[-n:])

    def await_log(self, pattern, timeout=30.0):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.read().splitlines():
                m = rx.search(line)
                if m:
                    return m
            if self.proc.poll() is not None:
                raise Failure("the game exited waiting for /%s/:\n%s" % (pattern, self.tail()))
            time.sleep(0.25)
        raise Failure("no /%s/ within %gs:\n%s" % (pattern, timeout, self.tail()))

    def press_record(self):
        """
        The real key, through the real input path.

        SDL drops `xdotool key --window`, so the window is focused once and the
        press goes to whatever holds focus. A press and release inside a single
        frame is missed by the edge detection, hence the hold.
        """
        run(["xdotool", "windowfocus", "--sync", self.window], env=self.env)
        run(["xdotool", "keydown", "F11"], env=self.env)
        time.sleep(0.25)
        run(["xdotool", "keyup", "F11"], env=self.env)

    def gdb(self, *commands):
        argv = ["gdb", "-p", str(self.proc.pid), "-batch", "-nx"]
        # Thread 1 owns the GL context and everything the recorder touches.
        for c in ("thread 1",) + commands:
            argv += ["-ex", c]
        r = run(argv)
        if "ptrace:" in r.stderr or "Operation not permitted" in r.stderr:
            raise Skip("gdb cannot attach - ptrace_scope, run as root or set it to 0")
        return r.stdout

    # gdb prints the frame it stopped in before it prints anything asked for,
    # and a frame in game code carries its arguments: "pos=pos@entry=0x7ffd...".
    # Anything looking for an "=" finds one of those first and reads the 0 out
    # of an address - which is a valid-looking answer, and index 0 is Auto, so
    # it would have passed. Only gdb's own "$1 = " lines count.
    VALUE = re.compile(r"^\$\d+ += +(-?\d+)", re.M)
    STRING = re.compile(r'^\$\d+ += +.*?"([^"]*)"', re.M)

    def value(self, expr):
        out = self.gdb("p %s" % expr)
        found = self.VALUE.findall(out)
        if not found:
            raise Failure("could not read %s from the game:\n%s" % (expr, out))
        return int(found[-1])

    def string(self, expr):
        found = self.STRING.findall(self.gdb("p %s" % expr))
        return found[-1] if found else ""

    def stop(self, clean):
        if self.proc is None or self.proc.poll() is not None:
            return
        if clean:
            # atexit(cleanup) in main.c is what writes pd.ini. Nothing else
            # saves the config, so nothing else would prove the default stuck.
            self.gdb("call (void)exit(0)")
            try:
                self.proc.wait(timeout=30)
                return
            except subprocess.TimeoutExpired:
                pass
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.proc.wait(timeout=10)


def write_ini(savedir):
    """
    The smallest pd.ini that records.

    Windowed and sized deliberately: with no window manager a fullscreen request
    is not honoured, so the window would stay at its default size while the game
    laid everything out for the screen.
    """
    with open(os.path.join(savedir, "pd.ini"), "w") as f:
        f.write("[Video]\n"
                "DefaultFullscreen=0\n"
                "DefaultWidth=%d\n"
                "DefaultHeight=%d\n"
                "\n"
                "[Mod]\n"
                "RecordKey=F11\n"
                "RecordFps=%d\n" % (WIDTH, HEIGHT, FPS))


def ini_value(savedir, key):
    path = os.path.join(savedir, "pd.ini")
    if not os.path.exists(path):
        return None
    # configSave() writes a section header whenever the section changes, so
    # [Mod] can appear more than once. Only the key matters.
    for line in open(path):
        line = line.strip()
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None


def file_encoder(path):
    """
    Which encoder wrote the file, according to the file.

    ffmpeg puts it in the video stream's metadata as "Lavc<version> <encoder>",
    written by whatever did the encoding. This is the only statement in the whole
    test that does not come from the code being tested.
    """
    r = run(["ffprobe", "-hide_banner", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=codec_name,width,height",
             "-show_entries", "stream_tags=encoder",
             "-of", "default=noprint_wrappers=1", path])
    if r.returncode != 0:
        raise Failure("ffprobe would not read %s:\n%s" % (path, r.stderr.strip()))

    out = {}
    for line in r.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out.setdefault(k.strip(), v.strip())

    if out.get("codec_name") != "h264":
        raise Failure("%s is not h264 but %r" % (path, out.get("codec_name")))

    tag = out.get("TAG:encoder", "")
    if not tag:
        raise Failure("%s carries no encoder tag, so nothing can be checked" % path)
    return tag


# ------------------------------------------------------------------- the cases


def await_labels(game, count, timeout=60.0):
    """
    The dropdown's text once the probe behind it has finished.

    It runs on a thread so the menu does not stop dead for it, which means the
    labels say "checking" for about a second first. Reading them before then
    would be reading the wrong thing.
    """
    deadline = time.time() + timeout
    labels = []
    while time.time() < deadline:
        labels = [game.string("(char *)recordGetCodecLabel(%d)" % i) for i in range(count)]
        if not any("(checking)" in x for x in labels):
            return labels
        time.sleep(1.0)
    return labels


def select(game, index):
    """Set the encoder the way the menu's MENUOP_SET does, and read it back."""
    game.gdb("call (void)recordSetCodecIndex(%d)" % index)
    got = game.value("(int)recordGetCodecIndex()")
    if got != index:
        raise Failure("selected index %d and the menu reads back %d" % (index, got))
    return game.string("codecName")


def press_until(game, pattern, what):
    """
    Press the record key until the log shows it landed.

    A press and release has to span a frame to be seen at all, and even held it
    is missed now and then - so the press is not evidence, the log line is. Both
    ends of a recording are driven this way.
    """
    for attempt in range(6):
        mark = len(game.read())
        game.press_record()
        deadline = time.time() + 5.0
        while time.time() < deadline:
            m = re.compile(pattern).search(game.read()[mark:])
            if m:
                return m
            if game.proc.poll() is not None:
                raise Failure("the game exited while trying to %s:\n%s" % (what, game.tail()))
            time.sleep(0.4)
    raise Failure("six presses and the game never %s:\n%s" % (what, game.tail()))


def record_once(game, savedir):
    """Press record, wait for a finished file, and report what the log claimed."""
    started = press_until(game, r"record: \d+x\d+ at \d+fps, (\S+), to (\S+)",
                          "started recording")
    claimed, path = started.group(1), started.group(2)

    time.sleep(RECORD_SECONDS)
    press_until(game, r"record: (?:\d+ frames to|gave up on)", "stopped recording")

    # The stop line only appears once the file has been finished properly.
    done = game.await_log(re.escape(path) + r" \(capture ([\d.]+)ms, "
                          r"encoder wait ([\d.]+)ms\)", timeout=RECORD_TIMEOUT)
    frames = game.await_log(r"record: (\d+) frames to " + re.escape(path)).group(1)

    return {"claimed": claimed, "path": path, "frames": int(frames),
            "capture_ms": float(done.group(1)), "wait_ms": float(done.group(2))}


def check(game, savedir, want, note, failures):
    """One recording, checked against the file rather than against the log."""
    try:
        got = record_once(game, savedir)
    except Failure as e:
        failures.append("%s: %s" % (note, e))
        return None

    tag = file_encoder(got["path"])

    if got["claimed"] != want:
        failures.append("%s: asked for %s, the game started %s" % (note, want, got["claimed"]))
    if want not in tag:
        failures.append("%s: asked for %s, the file says it was written by %r"
                        % (note, want, tag))
    if got["frames"] < FPS:
        failures.append("%s: only %d frames, which is under a second" % (note, got["frames"]))

    print("      %-14s %3d frames  capture %5.2fms  wait %5.2fms  file says: %s"
          % (want, got["frames"], got["capture_ms"], got["wait_ms"], tag))
    return got


def main(failures):
    for tool in ("Xvfb", "xdotool", "gdb", "ffprobe", "stdbuf"):
        if not shutil.which(tool):
            raise Skip("%s is not installed" % tool)
    if not os.path.exists(GAME):
        raise Skip("no build at %s" % GAME)

    data = os.path.join(ROOT, "build", "data")
    if not os.path.isdir(data) or not any(f.endswith(".z64") for f in os.listdir(data)):
        raise Skip("no ROM in build/data")

    xvfb = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "%dx%dx24" % (WIDTH, HEIGHT)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp = tempfile.mkdtemp(prefix="pd-recordcodec-")
    game = None

    try:
        time.sleep(1.5)
        if xvfb.poll() is not None:
            raise Skip("Xvfb would not start on %s - already in use?" % DISPLAY)

        write_ini(tmp)
        print("  game on %s, saving into %s" % (DISPLAY, tmp))

        game = Game(tmp, os.path.join(tmp, "game.log"))
        game.start()

        count = game.value("(int)recordGetCodecCount()")
        labels = [game.string("(char *)recordGetCodecLabel(%d)" % i) for i in range(count)]
        print("  the dropdown reads: %s" % ", ".join(labels))

        if labels[0] != "Auto":
            failures.append("index 0 is %r, not Auto" % labels[0])
        software = count - 1
        if "Software" not in labels[software]:
            failures.append("the last option is %r, not the software one" % labels[software])

        # --- Auto, which is what a fresh config does -------------------------
        print("  [1] Auto picks an encoder and records with it")
        select(game, 0)
        got = record_once(game, tmp)
        auto_encoder = got["claimed"]
        tag = file_encoder(got["path"])
        if auto_encoder not in tag:
            failures.append("auto: started %s and the file says %r" % (auto_encoder, tag))
        print("      chose %-9s %3d frames  file says: %s"
              % (auto_encoder, got["frames"], tag))

        # Auto does not stick: recordResolveCodec() writes what it found back,
        # so from here the row names a real encoder. That is the behaviour.
        resolved_index = game.value("(int)recordGetCodecIndex()")
        if resolved_index == 0:
            failures.append("after recording, the row still says Auto")

        # --- the same one, now asked for by name -----------------------------
        print("  [2] picking that encoder by hand uses it, with no search")
        select(game, resolved_index)
        check(game, tmp, auto_encoder, "explicit", failures)
        if not re.search(r"record: \S+, as the config asks for", game.read()):
            failures.append("explicit: the log does not say the named codec was taken as given")

        # --- the dropdown says what works, before anything is picked ----------
        # Opening the list starts the probe. Until it finishes the names say
        # "checking", and the player is not being told anything that could be
        # wrong; after it, the ones this machine cannot use say so.
        print("  [3] opening the dropdown marks what this machine cannot use")
        game.gdb("call (void)recordProbeCodecs()")
        probed = await_labels(game, count)
        for i in range(1, count):
            print("      %s" % probed[i])
        if any("(checking)" in x for x in probed):
            failures.append("the probe never finished")
        if "(unavailable)" in probed[resolved_index]:
            failures.append("%s recorded a moment ago and the dropdown calls it unavailable"
                            % probed[resolved_index])

        # --- one this machine does not have ----------------------------------
        # A pick that cannot work must not quietly record with something else
        # while claiming it ran: the log has to say so, the search has to take
        # over, and the row has to correct itself to what did the encoding.
        print("  [4] an encoder marked unavailable falls back, and says so")
        unusable = [i for i in range(1, count) if "(unavailable)" in probed[i]]

        if not unusable:
            print("      (every encoder on the list works here, so nothing to fall back from)")
        else:
            i = unusable[0]
            name = select(game, i)
            mark = len(game.read())
            got = record_once(game, tmp)
            tail = game.read()[mark:]
            tag = file_encoder(got["path"])

            if not re.search(r"record: %s does not work here" % re.escape(name), tail):
                failures.append("fallback: %s is marked unavailable but the log did not say so"
                                % name)
            if got["claimed"] not in tag:
                failures.append("fallback: started %s and the file says %r" % (got["claimed"], tag))
            if game.value("(int)recordGetCodecIndex()") == i:
                failures.append("fallback: %s did not work and the row still shows it" % name)
            if got["frames"] < FPS:
                failures.append("fallback: %s failed and took the recording with it" % name)

            print("      %-14s unusable, fell back to %s  file says: %s"
                  % (name, got["claimed"], tag))

        # --- software, which nothing would ever choose on its own -------------
        # The discriminating case: if selection did nothing at all, this would
        # still be encoded by whatever Auto found, and the file would say so.
        print("  [5] picking Software really does encode on the CPU")
        select(game, software)
        check(game, tmp, "libx264", "software", failures)

        # --- and it is still the default next time ---------------------------
        print("  [6] the choice survives a restart, and is used without searching")
        game.stop(clean=True)

        saved = ini_value(tmp, "RecordCodec")
        if saved != "software":
            failures.append("pd.ini kept RecordCodec=%r, not 'software'" % saved)

        game = Game(tmp, os.path.join(tmp, "game2.log"))
        game.start()
        if game.value("(int)recordGetCodecIndex()") != software:
            failures.append("after restarting, the row does not show Software")
        check(game, tmp, "libx264", "restarted", failures)
        if not re.search(r"record: libx264, as the config asks for", game.read()):
            failures.append("restarted: libx264 was searched for rather than taken as given")

    finally:
        try:
            if game is not None:
                game.stop(clean=False)
        except Exception:
            pass
        xvfb.terminate()
        if failures:
            print("\n  logs and recordings left in %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    print("recording encoder selection")
    problems = []
    try:
        main(problems)
    except Skip as e:
        print("  SKIP: %s" % e)
        sys.exit(0)
    except Failure as e:
        print("  FAIL: %s" % e)
        sys.exit(1)

    if problems:
        print("\n  %d failed:" % len(problems))
        for p in problems:
            print("    - %s" % p)
        sys.exit(1)

    print("\n  all good")
