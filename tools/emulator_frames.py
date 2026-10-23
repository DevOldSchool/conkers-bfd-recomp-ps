#!/usr/bin/env python3
"""Screenshots from the decomp's software-rendered emulator, with scripted controller presses.

Ground truth for rendering questions: Mupen64Plus with Angrylion RDP and CXD4 RSP, run in the
decomp's `mupen-trace --software-renderer` container. Its input plugin is a dummy, so presses
are injected through the debugger: a breakpoint at 0x1501BC10, just after the game's
osContGetReadData(sp+0x38) call, writes the button word of pad 1. It is hit once per VI. The
breakpoint is removed after the last press, and the emulator then runs at full speed (about
20 VIs/s, 7 while the breakpoint is set). Mupen numbers its --testshots by VI.

Run `./conker mupen-trace --software-renderer ...` in the decomp once first, so the container
image exists. From a worktree, point --decomp or CONKER_DECOMP at the decomp checkout. The image
platform follows the host CPU (linux/arm64 or linux/amd64) unless CONKER_PLATFORM names one, as in
the decomp's ./conker. A new game (A at 3900, 4200, 4500, 4800 and 5100) shows the Panther King's
"MORE MILK, SIRE?" bubble around VI 12300 and Conker's "OH NO!" around VI 14160:

    tools/emulator_frames.py --out build/emu/newgame --press 3900:8:8000,4200:8:8000,4500:8:8000,4800:8:8000,5100:8:8000 --shots 12300,14160

Screenshots are 640x480 VI output; the 285x214 picture covers x 40-609, y 25-452.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

DRIVER = r'''
import argparse, sys, time
sys.path.insert(0, "/workspace/scripts")
from mupen_trace import MupenDebugger, parse_registers

ap = argparse.ArgumentParser()
ap.add_argument("--press", default="")
ap.add_argument("--shots", required=True)
ap.add_argument("--dir", required=True)
args = ap.parse_args()
presses = []
for item in filter(None, args.press.split(",")):
    start, length, buttons = item.split(":")
    presses.append((int(start), int(length), int(buttons, 16)))
last_shot = max(int(v) for v in args.shots.split(","))
stop_after = max((s + l for s, l, _ in presses), default=0) + 2
cmd = ["xvfb-run", "--auto-servernum", "--server-args=-screen 0 640x480x24 -nolisten tcp",
       "env", "LIBGL_ALWAYS_SOFTWARE=1", "/usr/local/bin/conker-mupen64plus", "--noosd",
       "--nospeedlimit", "--debug", "--emumode", "1", "--gfx", "mupen64plus-video-angrylion-plus",
       "--audio", "dummy", "--input", "dummy", "--rsp", "mupen64plus-rsp-cxd4-sse2",
       "--sshotdir", args.dir, "--testshots", args.shots, "roms/baserom.us.z64"]
log = open(args.dir + "/frames.log", "w")
def say(msg):
    log.write(time.strftime("%H:%M:%S ") + msg + "\n"); log.flush()
dbg = MupenDebugger(cmd, 7200)
dbg.wait_for_prompt(120)
dbg.command("bp add 0x1501BC10 1 8", 30)
dbg.resume()
say("started")
vi, pad = 0, None
while vi < stop_after:
    dbg.wait_for_breakpoint(600)
    vi += 1
    if pad is None:
        pad = (parse_registers(dbg.command("regs", 30))["sp"] & 0xFFFFFFFF) + 0x38
    buttons = 0
    for s, l, b in presses:
        if s <= vi < s + l:
            buttons |= b
    if buttons:
        dbg.command("write 0x%08X h %04X" % (pad, buttons), 30)
    if vi % 1000 == 0:
        say("VI %d" % vi)
    if vi < stop_after:
        dbg.resume()
dbg.command("bp rm 0x1501BC10", 30)
dbg.resume()
say("presses done at VI %d" % vi)
while dbg.process.poll() is None:
    data = dbg._read(5.0).decode("utf-8", "replace")
    for line in data.splitlines():
        if "Captured screenshot" in line:
            say(line.strip())
            if ("frame %d." % last_shot) in line:
                dbg.close()
say("done")
'''


IMAGE = "conkers-bfd-mupen-software"


def docker_platform() -> str:
    """The container platform, chosen like the decomp's ./conker: CONKER_PLATFORM, else the host CPU."""
    chosen = os.environ.get("CONKER_PLATFORM")
    if chosen is None:
        chosen = "linux/arm64" if platform.machine().lower() in ("arm64", "aarch64") else "linux/amd64"
    if chosen not in ("linux/amd64", "linux/arm64"):
        sys.exit("CONKER_PLATFORM must be linux/amd64 or linux/arm64")
    return chosen


def newest_image(wanted: str) -> str:
    """The most recently built software-renderer image for the platform; the tags are content hashes."""
    tags = subprocess.run(["docker", "images", IMAGE, "--format", "{{.Tag}}"],
                          check=True, capture_output=True, text=True).stdout.split()
    images = []
    for tag in tags:
        if tag == "<none>":
            continue
        info = subprocess.run(["docker", "image", "inspect", "--format", "{{.Os}}/{{.Architecture}} {{.Created}}",
                               f"{IMAGE}:{tag}"], check=True, capture_output=True, text=True).stdout.split()
        if info[0] == wanted:
            images.append((info[1], tag))
    if not images:
        sys.exit(f"no {wanted} {IMAGE} image: run ./conker mupen-trace --software-renderer in the decomp once")
    return f"{IMAGE}:{max(images)[1]}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", default=os.environ.get("CONKER_DECOMP", str(Path(__file__).resolve().parents[1].parent / "conkers-bfd-decomp")),
                    help="decomp checkout (default $CONKER_DECOMP, else ../conkers-bfd-decomp)")
    ap.add_argument("--out", required=True, help="output directory (inside the decomp's build/ or not)")
    ap.add_argument("--press", default="", help="<vi>:<length>:<buttons hex>,... (8000 A, 4000 B, 1000 Start)")
    ap.add_argument("--shots", required=True, help="VI numbers to capture, comma separated")
    args = ap.parse_args()

    decomp = Path(args.decomp).resolve()
    if not (decomp / "roms" / "baserom.us.z64").is_file():
        sys.exit(f"no decomp with a ROM at {decomp}: pass --decomp or set CONKER_DECOMP")
    container_platform = docker_platform()
    image = newest_image(container_platform)
    work = decomp / "build" / "emulator-frames"
    work.mkdir(parents=True, exist_ok=True)
    (work / "driver.py").write_text(DRIVER)
    shots = work / "shots"
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir()
    mounts = []
    for name, mode in (("scripts", ",readonly"), ("roms", ",readonly"), ("config", ",readonly"), ("build", "")):
        mounts += ["--mount", f"type=bind,source={decomp / name},target=/workspace/{name}{mode}"]
    # Run as the host user so the screenshots are ours; Windows has no uids, and Docker Desktop's
    # bind mounts don't need one.
    user = ["--user", f"{os.getuid()}:{os.getgid()}"] if hasattr(os, "getuid") else []
    subprocess.run(["docker", "run", "--rm", "--platform", container_platform, "--network", "none", "--read-only",
                    "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "512",
                    *user, "--tmpfs", "/tmp:rw,nosuid,nodev,size=1g",
                    "--tmpfs", "/workspace:rw,nosuid,nodev,size=256m,mode=1777", *mounts, "--workdir", "/workspace",
                    image, "python3", "build/emulator-frames/driver.py",
                    "--press", args.press, "--shots", args.shots, "--dir", "/workspace/build/emulator-frames/shots"],
                   check=True)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for f in sorted(shots.iterdir()):
        shutil.copy2(f, out / f.name)
        print(out / f.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
