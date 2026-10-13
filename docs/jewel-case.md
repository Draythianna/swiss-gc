# Jewel Case games view

The Games section can show your games one at a time as clear CD jewel cases,
a GameCube take on [bubbbly.com/cd-case](https://www.bubbbly.com/cd-case). Each
case floats over Swiss's backdrop with the game's disc spinning inside. Pressing
A opens the case, spins the disc up, runs the camera in to it and fades to black
as the game starts.

It is the default Games view. Cover Flow is still there: *Settings → Interface
Settings → Games View*.

## Controls

| Control                         | Action                                              |
| ------------------------------- | --------------------------------------------------- |
| Left / Right (or Control Stick) | Next or previous game, repeating while held         |
| Up / L, Down / R                | Jump five games                                     |
| C-Stick                         | Turn the case to look at it; it springs back        |
| A                               | Play: the case opens, the disc spins up, fade out   |
| B                               | Cancel a launch, or go back to the home menu        |
| Y                               | Stop or start the disc spinning                     |
| X                               | Slideshow: bring in the next case every 8.8 seconds |
| Start                           | Recent list                                         |

The dock along the bottom shows the discs around the chosen one (eight at a
time) and three buttons: the disc spin (Y), whether the case is open, and the
slideshow (X).

## The disc art library

Disc art isn't built into Swiss. You put it on the same device as your games,
one image per game:

```
<device>/swiss/discs/<GAMEID>.png
```

For example, on an SD card with the games in `/gc_games`:

```
/gc_games/Mario Kart - Double Dash!! (USA).iso
/gc_games/Super Smash Bros. Melee (v1.01)(USA).iso
/swiss/discs/GM4E01.png
/swiss/discs/GALE01.png
```

### The game ID

`<GAMEID>` is the six character ID from the disc's header: four for the game
and two for the publisher, such as `GM4E01` (Mario Kart: Double Dash!!, USA,
Nintendo) or `GZLE01` (The Legend of Zelda: The Wind Waker, USA). The jewel case
view shows it at the top right next to the game's place in the list
(`GM4E01 · 1 of 9`), so you can read it off there. It doesn't depend on the
file's name or folder: every copy and every region-named file of the same disc
uses the same image. A game with no image gets a plain silver disc.

### The image

| | |
|---|---|
| Format | PNG, 8 bits per channel, not interlaced (RGBA, RGB, grey or palette) |
| Shape | Square, with the disc filling it edge to edge and centred |
| Outside the disc | Transparent |
| Size | 256 x 256 is best; 160 x 160 (GameTDB's) works as it is; at most 1024 x 1024 |
| File size | Under 4 MB |

The whole image is laid over a full size disc, so a disc that doesn't fill its
image, or sits off centre, comes out small or wobbles as it spins. The middle
of the image (the hole and the clear ring round it) is covered by the case's
silver hub, so whatever is drawn there doesn't matter.

Swiss draws discs at up to 256 x 256: anything bigger only costs memory and
loading time on the GameCube, and images over 1024 pixels aren't read at all.

### Where to get images

GameTDB has disc images for most games, named by game ID already:

```
https://art.gametdb.com/wii/disc/US/<GAMEID>.png
```

(swap `US` for `EN`, `JA`, `FR` and so on for other regions). They are 160 x 160
and need no changes.

For larger or hand made images, `pc/jewelcase-preview/fitdisc.py` finds the disc
in an image, crops to it and shrinks it to 256 x 256:

```sh
python3 pc/jewelcase-preview/fitdisc.py big-scan.png GM4E01.png
```

It needs Python 3 with Pillow and NumPy.

### How Swiss loads them

Images are read as they're needed rather than all at once: the chosen game's
first, then the rest of the discs the dock shows, nearest first, one per frame.
Up to sixteen are kept in memory, and the one drawn longest ago makes way for a
new one. Each image becomes a texture for the disc and a 64 x 64 one for its
dock thumbnail.

## Settings

| Setting    | Where     | What it does                                               |
| ---------- | --------- | ---------------------------------------------------------- |
| Games View | Interface | *Jewel Case* (default) or *Cover Flow*; saved as `GamesView` |
| Menu Overscan | Interface | Pulls the whole interface in, the jewel cases included, for TVs that crop the edges |

## Working on it

`cube/swiss/source/gui/jewelcase.c` is the view; `pngdec.c` is the PNG reader.
`pc/jewelcase-preview/` builds the same `jewelcase.c` on a PC against a small
software GX, to render frames without a GameCube (see its README). It is not a
GameCube: check changes on hardware, or in Dolphin where Swiss runs.
