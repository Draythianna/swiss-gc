# jewelcase-preview

Renders the jewel case games view (`cube/swiss/source/gui/jewelcase.c`) on a PC, so
changes to it can be looked at without a GameCube. `jewelcase.c` is compiled as it is
against `gx_soft.c`, a small software rasteriser for the part of GX it uses (TEV,
blending, depth, texture generation, display lists), and stub headers in `shim/` for
the rest of Swiss.

```sh
make
mkdir -p sd/swiss/discs && cp /path/to/GameTDB/disc/US/*.png sd/swiss/discs/
./preview out GZLE01,GALE01,GM4E01 1.0=shot:rest 3.6=shot:opening 9.5=shot:swap
```

Frames are written as PPM at 640x480. The view runs from t = 0 with the original's
autoplay paused (`pause` starts it: the next case comes in every 8.8 s); actions are
scheduled as `time=action`: `play` (A: open, spin up, zoom, fade), `done` (prints whether
the launch has finished), `open`, `close`, `next`, `prev`, `spin`, `pause`,
`drag:x:y`, `undrag`.

- `JC_RECORD=1` drops the dock and the 44 px lift, as bubbbly.com/cd-case?record does,
  for comparing frames with the original
- `JC_SD=dir/` reads disc art from somewhere other than `./sd/`
- `./preview matcap out.ppm` writes the rod material; `JC_COUNTS=1 ./preview x x` prints
  mesh sizes

The rasteriser is not a GameCube: it is there to check poses, layout, lighting and
blending, not exact output.

Disc art for the SD card (`swiss/discs/<GAMEID>.png`) is best at 256x256, the size the
view draws it at; larger images cost the GameCube memory and time to load, and over
1024 pixels aren't read at all. `python3 fitdisc.py big.png GAMEID.png` crops an image
to its disc (so it spins true) and shrinks it.
