# Third-party licences

What this source release contains or builds against, and the licence of each.

| Component | Where | Licence | Text |
|---|---|---|---|
| ps3recomp (runtime, translators, system libraries) | `ps3kit/` | MIT | `ps3recomp-MIT.txt` |
| AMD FidelityFX SDK headers (FSR 3) | `ps3kit/libs/video/third_party/ffx/` | MIT | `FidelityFX-MIT.md` |
| SMAA | `ps3kit/libs/video/third_party/smaa/`, `rsx_smaa_embed.inc` | MIT | `SMAA-MIT.txt` |
| ATRAC3 / ATRAC3+ decoder (from PPSSPP, originally FFmpeg) | `ps3kit/third_party/at3_standalone/` | LGPL-2.1 | `LGPL-2.1.txt` |
| stb_image, stb_image_write | `ps3kit/libs/codec/` | public domain / MIT | in the headers |
| RPCS3 (`Crypto/unself.cpp`, ported) | `ascension/launcher/app/unself.py` | GPL-2.0 | `GPL-2.0.txt` |
| decrypt_self.py (Twisted Metal port) | `ascension/tools/decrypt_self.py` | MIT | in the file header |
| Cinzel font | `ascension/launcher/Cinzel-Black.ttf` | SIL OFL 1.1 | `Cinzel-OFL-1.1.txt` |
| SDL2 (not in this repository; you download it, its DLL is copied beside the game) | build dependency | zlib | `SDL2-zlib.txt` |
| FFmpeg (not in this repository; you download a shared LGPL build, its DLLs are copied beside the game) | build dependency | LGPL (the BtbN "lgpl" builds are LGPL-3.0) | `FFmpeg-LGPL-3.0.txt` |
| Python, Pillow (not in this repository; the launcher runs on them) | run-time dependency | PSF / HPND | python.org, python-pillow.org |
