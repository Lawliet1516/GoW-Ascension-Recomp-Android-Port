"""The launcher's artwork and music, made from the player's own disc.

Nothing from the game is in the repository. The first time the launcher opens
with the disc in place (or with `ascension_launcher.py --make-assets`) it reads
the disc's PS3_GAME folder and writes, to work/data/launcher/:

  background.jpg  PIC1.PNG, the cover art (1920 x 1080)
  logo.png        the logo on ICON0.PNG, cut from its dark brush stroke
  icon-2.png      Kratos' head from PIC1.PNG, for the window and taskbar icon
  music.mp3       SND0.AT3, the menu music (ATRAC3plus), through FFmpeg when
                  it can be found (PATH, FFMPEG_BIN_DIR in ascension/.env, or
                  tools/ffmpeg/*/bin beside the project); skipped otherwise

A file already there is kept, so the player's own artwork wins.
"""

import glob
import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from .config import ASSET_DIRS, ROOT, WINDOWS, asset

OUT_DIR = ASSET_DIRS[-1]
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
FILES = ('background.jpg', 'logo.png', 'icon-2.png', 'music.mp3')


def missing():
    """The artwork files no asset folder has yet."""
    have = {n for n in FILES if asset(n)}
    if asset('background.png') or asset('background-2.jpg'):
        have.add('background.jpg')
    return [n for n in FILES if n not in have]


def find_ffmpeg():
    exe = 'ffmpeg.exe' if WINDOWS else 'ffmpeg'
    found = shutil.which('ffmpeg')
    if found:
        return found
    try:   # the build's own setting (scripts/windows/env.ps1 reads the same file)
        for line in (ROOT / '.env').read_text(encoding='utf-8').splitlines():
            if line.strip().startswith('FFMPEG_BIN_DIR='):
                folder = line.split('=', 1)[1].strip().strip('"\'')
                path = (ROOT / folder if not os.path.isabs(folder) else Path(folder)) / exe
                if path.is_file():
                    return str(path)
    except OSError:
        pass
    for base in (ROOT.parent.parent / 'tools' / 'ffmpeg', ROOT.parent / 'tools' / 'ffmpeg'):
        hits = sorted(glob.glob(str(base / '*' / 'bin' / exe)) + glob.glob(str(base / 'bin' / exe)))
        if hits:
            return hits[-1]
    return None


def _logo(icon0, out):
    """ICON0.PNG's logo (orange letters on a near-black brush) with the brush
    made transparent: opacity from brightness, colour un-premultiplied."""
    from PIL import Image, ImageChops
    with Image.open(icon0) as im:
        im = im.convert('RGBA')
    r, g, b, a = im.split()
    bright = ImageChops.lighter(ImageChops.lighter(r, g), b)
    lut = [0 if v < 28 else 255 if v > 90 else int(255 * ((v - 28) / 62) ** 1.2) for v in range(256)]
    alpha = ImageChops.multiply(bright.point(lut), a)
    try:
        import numpy as np
        rgb = np.asarray(im).astype('float32')[..., :3]
        al = np.asarray(alpha).astype('float32')[..., None] / 255.0
        rgb = np.clip(rgb / np.maximum(al, 1e-3), 0, 255).astype('uint8')
        logo = Image.fromarray(np.dstack([rgb, np.asarray(alpha)]), 'RGBA')
    except ImportError:
        logo = Image.merge('RGBA', (r, g, b, alpha))
    box = logo.getchannel('A').point(lambda v: 255 if v > 8 else 0).getbbox()
    if box:
        logo = logo.crop(box)
    logo.save(out)


def _icon(pic1, out):
    """A square of Kratos' head from the cover art, rounded, darker at the edges."""
    from PIL import Image, ImageDraw, ImageEnhance, ImageFilter
    with Image.open(pic1) as im:
        im = im.convert('RGB')
    sx, sy = im.width / 1920, im.height / 1080
    face = im.crop((int(700 * sx), int(170 * sy), int(1020 * sx), int(490 * sy))).resize((256, 256), Image.LANCZOS)
    face = ImageEnhance.Contrast(face).enhance(1.25)
    glow = Image.new('L', (256, 256), 0)
    ImageDraw.Draw(glow).ellipse((-40, -30, 296, 300), fill=255)
    face = Image.composite(face, Image.new('RGB', (256, 256), (20, 12, 10)), glow.filter(ImageFilter.GaussianBlur(40)))
    mask = Image.new('L', (256, 256), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 255, 255), radius=44, fill=255)
    icon = face.convert('RGBA')
    icon.putalpha(mask)
    icon.save(out)


def _music(snd0, out, ffmpeg):
    """SND0.AT3 to MP3, through a WAV: straight to MP3 the AT3's timestamps
    upset FFmpeg's muxer."""
    with tempfile.TemporaryDirectory(prefix='ascension-music-') as tmp:
        wav = Path(tmp) / 'snd0.wav'
        for args in ([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', str(snd0), '-c:a', 'pcm_s16le', str(wav)],
                     [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', str(wav), '-c:a', 'libmp3lame',
                      '-q:a', '2', str(out) + '.tmp.mp3']):
            r = subprocess.run(args, capture_output=True, text=True, errors='replace', timeout=300, creationflags=NO_WINDOW)
            if r.returncode != 0:
                Path(str(out) + '.tmp.mp3').unlink(missing_ok=True)
                raise OSError((r.stderr or r.stdout or 'ffmpeg failed').strip().splitlines()[-1])
        os.replace(str(out) + '.tmp.mp3', out)


def make_assets(ps3_game, log=lambda m: None, music=True):
    """Write the missing files from the disc's PS3_GAME folder. Returns the
    names written; what cannot be made is reported through `log`."""
    ps3_game = Path(ps3_game)
    want = missing()
    done = []
    if not want:
        return done
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    jobs = {
        'background.jpg': (ps3_game / 'PIC1.PNG', lambda src, out: _jpeg(src, out)),
        'logo.png': (ps3_game / 'ICON0.PNG', _logo),
        'icon-2.png': (ps3_game / 'PIC1.PNG', _icon),
    }
    for name, (src, make) in jobs.items():
        if name not in want:
            continue
        if not src.is_file():
            log(f'{name}: {src} not found')
            continue
        try:
            make(src, OUT_DIR / name)
            done.append(name)
        except (OSError, ValueError) as e:
            log(f'{name}: {e}')
    if music and 'music.mp3' in want:
        snd0 = ps3_game / 'SND0.AT3'
        ffmpeg = find_ffmpeg()
        if not snd0.is_file():
            log(f'music.mp3: {snd0} not found')
        elif not ffmpeg:
            log('music.mp3: FFmpeg not found (put ffmpeg on PATH, or FFMPEG_BIN_DIR in ascension/.env)')
        else:
            try:
                _music(snd0, OUT_DIR / 'music.mp3', ffmpeg)
                done.append('music.mp3')
            except (OSError, subprocess.SubprocessError) as e:
                log(f'music.mp3: {e}')
    return done


def _jpeg(src, out):
    from PIL import Image
    with Image.open(src) as im:
        im.convert('RGB').save(out, quality=92)
