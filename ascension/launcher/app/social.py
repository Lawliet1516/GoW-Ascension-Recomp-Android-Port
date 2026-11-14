"""The release's byline and link icons (YouTube, GitHub, Patreon).

The icons are drawn here with Pillow, so no image files ship with them.
"""

import webbrowser

POSTER = 'BilalReloaded'
POSTED_BY = f'Port by {POSTER}'
LINKS = (
    ('youtube', 'YouTube', 'https://www.youtube.com/@BilalReloaded'),
    ('github', 'GitHub', 'https://github.com/bc160201983'),
    ('patreon', 'Patreon', 'https://www.patreon.com/cw/bilalreloaded'),
)

SUPPORT_URL = 'https://www.patreon.com/cw/bilalreloaded'   # the launcher's Support page

SS = 4  # drawn this many times larger, then scaled down smoothly


def _youtube(d, s):
    d.rounded_rectangle((0, s * 0.16, s - 1, s * 0.84), radius=s * 0.2, fill='#ff0000')
    d.polygon([(s * 0.40, s * 0.32), (s * 0.40, s * 0.68), (s * 0.70, s * 0.50)], fill='white')


def _github(d, s):
    d.ellipse((0, 0, s - 1, s - 1), fill='#f0f0f0')
    ink = '#181717'
    # the cat: head with two ears, and a body reaching the circle's bottom edge
    d.ellipse((s * 0.27, s * 0.24, s * 0.73, s * 0.62), fill=ink)
    d.polygon([(s * 0.29, s * 0.36), (s * 0.30, s * 0.17), (s * 0.43, s * 0.27)], fill=ink)
    d.polygon([(s * 0.71, s * 0.36), (s * 0.70, s * 0.17), (s * 0.57, s * 0.27)], fill=ink)
    d.rounded_rectangle((s * 0.40, s * 0.52, s * 0.60, s * 0.98), radius=s * 0.06, fill=ink)
    # its tail curling to the left
    d.arc((s * 0.16, s * 0.56, s * 0.44, s * 0.80), start=60, end=200, fill=ink, width=max(1, int(s * 0.06)))


def _patreon(d, s):
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=s * 0.22, fill='#ff424d')
    d.ellipse((s * 0.38, s * 0.18, s * 0.82, s * 0.62), fill='white')
    d.rectangle((s * 0.18, s * 0.18, s * 0.30, s * 0.82), fill='#141518')


DRAW = {'youtube': _youtube, 'github': _github, 'patreon': _patreon}


def icon_image(kind, size):
    """A PhotoImage of one link icon, size x size pixels, or None without Pillow."""
    try:
        from PIL import Image, ImageDraw, ImageTk
    except ImportError:
        return None
    big = size * SS
    im = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    DRAW[kind](ImageDraw.Draw(im), big)
    return ImageTk.PhotoImage(im.resize((size, size), Image.LANCZOS))


def link_bar(tk, parent, bg, fg, font, size):
    """A row of clickable link icons. Each opens its page in the browser; the
    row's label shows which page while the mouse is over an icon."""
    frame = tk.Frame(parent, bg=bg)
    frame.images = []
    row = tk.Frame(frame, bg=bg)
    row.pack(anchor='w')
    hint = tk.Label(frame, text='', bg=bg, fg=fg, font=font)
    hint.pack(anchor='w')
    for kind, name, url in LINKS:
        image = icon_image(kind, size)
        if image is not None:
            frame.images.append(image)       # keep a reference, or Tk drops the picture
            icon = tk.Label(row, image=image, bg=bg, bd=0, cursor='hand2')
        else:
            icon = tk.Label(row, text=name, bg=bg, fg=fg, font=font, cursor='hand2')
        icon.pack(side='left', padx=(0, max(6, size // 3)))
        icon.bind('<Button-1>', lambda _e, u=url: webbrowser.open(u))
        icon.bind('<Enter>', lambda _e, n=name: hint.configure(text=n))
        icon.bind('<Leave>', lambda _e: hint.configure(text=''))
    return frame
