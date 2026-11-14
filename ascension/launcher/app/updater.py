"""Updates from the port's GitHub releases.

The launcher asks GitHub for the latest release (at start, unless switched off
under Advanced → Updates). When it is newer than this copy and carries an update
zip (GodOfWarAscension-<version>-update.zip: the game exe and the launcher at
their paths in the release folder), the launcher offers it, downloads it into
updates\\ and writes its files over the release folder. What it replaces is kept
in updates\\backup-<old version>\\. Saves (gamedata\\), the prepared disc
(work\\), the settings (%APPDATA%) and the bundled Python (runtime\\) are never
written.

The repository is private, so GitHub serves its releases only to an account
with access: the token is GH_TOKEN / GITHUB_TOKEN, the launcher's update_token
setting, or the GitHub CLI's (`gh auth token`).
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from .config import ROOT, VERSION

# The repository's current name: the old one (GoW-Ascension-PC-Port) only redirects, and the redirect
# loses the token, so a private repository answered "not found" (found 2026-11-11).
REPO = 'bc160201983/Ascension-Recomp'
API = f'https://api.github.com/repos/{REPO}'
RELEASES_PAGE = f'https://github.com/{REPO}/releases/latest'
INSTALL = ROOT.parent.parent       # the release folder: the launcher exe, runtime\, ascension-recomp\
UPDATES = INSTALL / 'updates'
# Never written by an update (paths in the zip, from the release folder).
KEEP = ('ascension-recomp/ascension/gamedata/', 'ascension-recomp/work/', 'runtime/', 'updates/')


class UpdateError(Exception):
    """A step failed; the text is for the player."""


def version_tuple(text):
    """'v0.1.4' -> (0, 1, 4); '0.1.3-test1' -> (0, 1, 3)."""
    return tuple(int(n) for n in re.findall(r'\d+', str(text).lstrip('vV').split('-')[0]))


def newer(info):
    return version_tuple(info['version']) > version_tuple(VERSION)


def can_install():
    """Updates are written only into a release folder (the launcher exe and its
    Python beside ascension-recomp\\); a source checkout is updated from the source."""
    return (INSTALL / 'runtime' / 'python').is_dir()


def token(setting=''):
    for t in (os.environ.get('GH_TOKEN'), os.environ.get('GITHUB_TOKEN'), setting):
        if t and t.strip():
            return t.strip()
    gh = shutil.which('gh')
    if gh:
        try:
            r = subprocess.run([gh, 'auth', 'token'], capture_output=True, text=True, errors='replace', timeout=10,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if r.returncode == 0 and r.stdout.strip():
                return r.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return ''


def _open(url, tok, accept='application/vnd.github+json', timeout=20):
    req = urllib.request.Request(url, headers={'Accept': accept, 'User-Agent': 'ascension-launcher',
                                               'X-GitHub-Api-Version': '2022-11-28'})
    if tok:   # not sent on after a redirect: GitHub's file storage refuses a second credential
        req.add_unredirected_header('Authorization', f'Bearer {tok}')
    return urllib.request.urlopen(req, timeout=timeout)


def _http_error(e, tok):
    if e.code == 404:
        return UpdateError('The release was not found. The repository is private: GitHub shows it only to an '
                           'account with access (Advanced → Updates → GitHub token).' if not tok else
                           'The release was not found with this GitHub token (no access to the repository?).')
    if e.code == 401:
        return UpdateError('GitHub refused the token (expired or mistyped?).')
    if e.code in (403, 429):
        return UpdateError('GitHub refused for now (too many requests); try again later.')
    return UpdateError(f'GitHub answered {e.code} {e.reason}.')


def latest(setting_token=''):
    """The latest release: {'version', 'tag', 'notes', 'url', 'asset'}; asset is
    its update zip ({'name', 'size', 'api_url', 'digest'}) or None."""
    tok = token(setting_token)
    try:
        with _open(f'{API}/releases/latest', tok) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        raise _http_error(e, tok) from None
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise UpdateError(f'Could not reach GitHub ({getattr(e, "reason", e)}).') from None
    tag = str(data.get('tag_name') or '')
    zips = [a for a in data.get('assets') or [] if str(a.get('name', '')).lower().endswith('-update.zip')]
    asset = zips[0] if zips else None
    return {'version': tag.lstrip('vV'), 'tag': tag, 'notes': str(data.get('body') or ''),
            'url': data.get('html_url') or RELEASES_PAGE,
            'asset': asset and {'name': str(asset['name']), 'size': int(asset.get('size') or 0),
                                'api_url': str(asset['url']), 'digest': str(asset.get('digest') or '')}}


def _sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def _good(path, asset):
    if asset['size'] and path.stat().st_size != asset['size']:
        return False
    digest = asset['digest']
    return not digest.startswith('sha256:') or digest[7:].lower() == _sha256(path)


def download(asset, setting_token='', progress=None):
    """The update zip into updates\\ (kept until it is installed: a second try
    does not download it again). progress(done, total) is called per MB."""
    UPDATES.mkdir(parents=True, exist_ok=True)
    out = UPDATES / Path(asset['name']).name
    if out.is_file() and _good(out, asset):
        return out
    part = out.with_name(out.name + '.part')
    done = 0
    try:
        with _open(asset['api_url'], token(setting_token), accept='application/octet-stream', timeout=60) as r, \
                open(part, 'wb') as f:
            for block in iter(lambda: r.read(1 << 20), b''):
                f.write(block)
                done += len(block)
                if progress:
                    progress(done, asset['size'])
    except urllib.error.HTTPError as e:
        part.unlink(missing_ok=True)
        raise _http_error(e, token(setting_token)) from None
    except (urllib.error.URLError, OSError) as e:
        part.unlink(missing_ok=True)
        raise UpdateError(f'The download stopped ({getattr(e, "reason", e)}).') from None
    if not _good(part, asset):
        part.unlink(missing_ok=True)
        raise UpdateError('The download is damaged (its size or checksum does not match); try again.')
    os.replace(part, out)
    return out


def install(zip_path):
    """Writes the update zip's files over the release folder; returns how many.
    Every file is first unpacked beside its target (*.update-new). Only when all
    of them are there are the old files moved to the backup and the new ones put
    in their place, and a failure then puts the old ones back."""
    if not can_install():
        raise UpdateError('This is not a release folder (no runtime\\python): update it from the source.')
    items = []
    try:
        with zipfile.ZipFile(zip_path) as z:
            bad = z.testzip()
            if bad:
                raise UpdateError(f'The update zip is damaged ({bad}).')
            for info in z.infolist():
                if info.is_dir():
                    continue
                name = info.filename.replace('\\', '/')
                parts = name.split('/')
                if name.startswith('/') or ':' in name or any(p in ('', '.', '..') for p in parts):
                    raise UpdateError(f'The update zip has a path outside the folder: {name}')
                if not name.startswith(KEEP):
                    items.append((info, INSTALL.joinpath(*parts)))
            if not items:
                raise UpdateError('The update zip has nothing to install.')
            for info, target in items:
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(target.with_name(target.name + '.update-new'), 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    except (OSError, zipfile.BadZipFile) as e:
        for _, target in items:
            target.with_name(target.name + '.update-new').unlink(missing_ok=True)
        raise UpdateError(f'Could not unpack the update ({e}). Nothing was changed.') from None
    backup = UPDATES / f'backup-{VERSION}'
    shutil.rmtree(backup, ignore_errors=True)
    moved = []        # (target, its old file in the backup or None)
    try:
        for _, target in items:
            old = None
            if target.exists():
                old = backup / target.relative_to(INSTALL)
                old.parent.mkdir(parents=True, exist_ok=True)
                os.replace(target, old)
            moved.append((target, old))
            os.replace(target.with_name(target.name + '.update-new'), target)
    except OSError as e:
        failed = target.name
        for done, old in reversed(moved):
            try:
                if old:
                    os.replace(old, done)
                else:
                    done.unlink(missing_ok=True)
            except OSError:
                pass
        for _, item in items:
            item.with_name(item.name + '.update-new').unlink(missing_ok=True)
        raise UpdateError(f'Could not replace {failed} ({e.strerror or e}). Nothing was changed; '
                          'close the game and try again.') from None
    return len(items)
