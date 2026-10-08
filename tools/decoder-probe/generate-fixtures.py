#!/usr/bin/env python3
"""Generate small HEVC fixtures locally; never connects to the target."""
import array
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'tests/decoder-fixtures'
OUT.mkdir(parents=True, exist_ok=True)
W, H, FPS, FRAMES = 1920, 1080, 30, 60
commands = []

def run(args, **kw):
    commands.append(args)
    return subprocess.run(args, check=True, **kw)

common = ['ffmpeg', '-hide_banner', '-nostdin', '-y']
params = 'pools=1:frame-threads=1:wpp=0:bframes=0:keyint=30:min-keyint=30:scenecut=0:repeat-headers=1:aud=1'
for name, pix, profile in [('main', 'yuv420p', 'main'),
                            ('main10', 'yuv420p10le', 'main10')]:
    run(common + ['-f', 'lavfi', '-i', f'testsrc2=size={W}x{H}:rate={FPS}',
        '-frames:v', str(FRAMES), '-an', '-vf', f'format={pix}',
        '-c:v', 'libx265', '-preset', 'fast', '-crf', '18',
        '-profile:v', profile, '-x265-params', params,
        '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709',
        '-color_trc', 'bt709', '-f', 'hevc', str(OUT / f'{name}.hevc')])

# Construct native 10-bit values, not an upconverted 8-bit test pattern.
# Upper half: neutral luma ramp 64..940. Lower half: same luma plus chroma ramps.
yrow = [64 + (x * 876 // (W - 1)) for x in range(W)]
samples = array.array('H', yrow * H)
for reverse in (False, True):
    for y in range(H // 2):
        samples.extend([512] * (W // 2) if y < H // 4 else
                       [64 + ((W // 2 - 1 - x if reverse else x) * 896 // (W // 2 - 1))
                        for x in range(W // 2)])
if sys.byteorder != 'little':
    samples.byteswap()
source = OUT / 'gradient10-source.yuv'
source.write_bytes(samples.tobytes())
run(common + ['-stream_loop', '-1', '-f', 'rawvideo', '-pixel_format', 'yuv420p10le',
    '-video_size', f'{W}x{H}', '-framerate', str(FPS),
    '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709',
    '-color_trc', 'bt709', '-i', str(source),
    '-frames:v', str(FRAMES), '-an', '-c:v', 'libx265', '-preset', 'fast',
    '-profile:v', 'main10', '-crf', '4', '-x265-params', params + ':level-idc=4.1',
    '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709',
    '-color_trc', 'bt709', '-f', 'hevc', str(OUT / 'gradient10.hevc')])

manifest = {'width': W, 'height': H, 'fps': FPS, 'frames': FRAMES, 'files': {}}
for name in ('main', 'main10', 'gradient10'):
    path = OUT / f'{name}.hevc'
    info = json.loads(run(['ffprobe', '-v', 'error', '-count_frames', '-select_streams', 'v:0',
        '-show_streams', '-of', 'json', str(path)], capture_output=True).stdout)['streams'][0]
    assert info['width'] == W and info['height'] == H
    assert int(info['nb_read_frames']) == FRAMES
    assert info['profile'] == ('Main' if name == 'main' else 'Main 10'), info
    assert info['pix_fmt'] == ('yuv420p' if name == 'main' else 'yuv420p10le'), info
    ref = OUT / f'{name}-reference.yuv'
    run(common + ['-c:v', 'hevc', '-i', str(path), '-frames:v', '1',
                  '-pix_fmt', info['pix_fmt'], '-f', 'rawvideo', str(ref)])
    manifest['files'][name] = {'bytes': path.stat().st_size,
        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'stream': info,
        'reference_sha256': hashlib.sha256(ref.read_bytes()).hexdigest()}
ref_samples = array.array('H')
ref_samples.frombytes((OUT / 'gradient10-reference.yuv').read_bytes())
if sys.byteorder != 'little':
    ref_samples.byteswap()
luma = ref_samples[:W*H]
assert len(set(luma)) > 256 and any(v % 4 for v in luma)
manifest['gradient_reference_unique_luma_values'] = len(set(luma))
manifest['gradient_reference_low_two_bits_nonzero'] = sum(v % 4 != 0 for v in luma)
manifest['precision_reference'] = 'Software decode of the exact compressed stream, not the pre-encode source.'
manifest['ffmpeg_version'] = subprocess.check_output(['ffmpeg', '-version'], text=True)
manifest['commands'] = commands
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps({k: {'bytes': v['bytes'], 'profile': v['stream']['profile'],
    'pix_fmt': v['stream']['pix_fmt']} for k, v in manifest['files'].items()}, indent=2))
