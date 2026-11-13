#!/usr/bin/env python3
# rawsweep: per sensor, let libcamera wire the media graph once, then capture raw frames
# straight from the VFE video node with exposure and gain set by hand on the sensor subdev
# (no IPA, no auto-exposure), and print the mean and spread of the frame's high bytes (RAW10 packed:
# every 5th byte holds the low bits and is skipped). A working path moves the mean.
#   usage: rawsweep.py CAMERA_PATH...   (libcamera camera-name, e.g. /base/soc@0/.../camera@3c)
import os, re, subprocess, sys

def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw).stdout

def graph():
    """entity name -> (device node, [enabled sink entity names])"""
    ents, cur = {}, None
    for line in sh('media-ctl -d /dev/media0 -p').splitlines():
        m = re.match(r'- entity \d+: (.+?) \(', line)
        if m:
            cur = m.group(1)
            ents[cur] = [None, []]
            continue
        if cur is None:
            continue
        m = re.search(r'device node name (\S+)', line)
        if m:
            ents[cur][0] = m.group(1)
        m = re.match(r'\s+-> "(.+?)":\d+ \[.*ENABLED', line)
        if m:
            ents[cur][1].append(m.group(1))
    return ents

def path_to_video(ents, start):
    seen, todo = set(), [start]
    while todo:
        e = todo.pop()
        if e in seen:
            continue
        seen.add(e)
        if '_video' in e:
            return ents[e][0]
        todo += ents[e][1]
    return None

def ctrl_range(sd, name):
    out = sh('v4l2-ctl -d %s -l' % sd)
    m = re.search(r'%s .*min=(-?\d+) max=(\d+)' % name, out)
    return (int(m.group(1)), int(m.group(2))) if m else None

def mean_last_frame(path, w, h, bpl):
    """mean and spread of the high 8 bits of a sparse pixel grid; pixel x of a RAW10
    line is byte (x // 4) * 5 + x % 4, and each line is bpl bytes (with padding)"""
    with open(path, 'rb') as f:
        f.seek(-bpl * h, 2)
        b = f.read(bpl * h)
    px = [b[y * bpl + (x // 4) * 5 + x % 4] for y in range(1, h, 16) for x in range(1, w, 16)]
    m = sum(px) / len(px)
    return m, (sum((v - m) ** 2 for v in px) / len(px)) ** 0.5

tmp = os.environ.get('RAWSWEEP_TMP', os.path.expanduser('~/.cache/rawsweep.raw'))
for cam in sys.argv[1:]:
    print('==', cam)
    sh('timeout 4 gst-launch-1.0 -q libcamerasrc camera-name=%s ! fakesink' % cam)
    ents = graph()
    # the camera path ends in the sensor's I2C address: match the entity "name BUS-ADDR"
    cands = [e for e in ents if e.split()[0] in ('ov02b1b', 'hi259', 'hi1337', 's5k3t2', 'imx682')]
    want = cam.rsplit('@', 1)[1]
    sensor = next((e for e in cands if e.endswith('-%04x' % int(want, 16))), None)
    sd, vid = ents[sensor][0], path_to_video(ents, sensor)
    fmt = sh('v4l2-ctl -d %s --get-fmt-video' % vid)
    size = int(re.search(r'Size Image\s*:\s*(\d+)', fmt).group(1))
    w, h = map(int, re.search(r'Width/Height\s*:\s*(\d+)/(\d+)', fmt).groups())
    bpl = int(re.search(r'Bytes per Line\s*:\s*(\d+)', fmt).group(1))
    print('  %s subdev=%s video=%s %s' % (sensor, sd, vid,
          ' '.join(re.findall(r'Width/Height\s*:\s*(\S+)|Pixel Format\s*:\s*(\S+)', fmt)[0])))
    er, gr = ctrl_range(sd, 'exposure'), ctrl_range(sd, 'analogue_gain')
    print('  exposure %s gain %s' % (er, gr))
    if not er or not gr:
        continue
    for e, g in ((er[1] // 16, gr[0]), (er[1] // 2, gr[0]), (er[1] // 2, gr[0] + (gr[1] - gr[0]) // 4),
                 (er[1] // 2, gr[1])):
        sh('v4l2-ctl -d %s --set-ctrl exposure=%d,analogue_gain=%d' % (sd, e, g))
        if os.path.exists(tmp):
            os.remove(tmp)
        r = subprocess.run('timeout 15 v4l2-ctl -d %s --stream-mmap --stream-skip=6 --stream-count=2 '
                           '--stream-to=%s' % (vid, tmp), shell=True, capture_output=True, text=True)
        got = os.path.getsize(tmp) if os.path.exists(tmp) else 0
        if got < size:
            print('  exp=%-5d gain=%-4d no frame (%d bytes) %s' % (e, g, got, r.stderr.strip()[-80:]))
            continue
        print('  exp=%-5d gain=%-4d mean=%6.1f std=%5.2f' % ((e, g) + mean_last_frame(tmp, w, h, bpl)))
    if os.path.exists(tmp):
        os.remove(tmp)
