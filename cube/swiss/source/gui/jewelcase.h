/* -----------------------------------------------------------
      jewelcase.h - games shown as clear CD jewel cases

      A GX take on bubbbly.com/cd-case: one clear jewel case at
      a time on a white page, the game's disc spinning inside.
      A plays the game: the case opens, the disc spins up, the
      camera runs in to it and the picture fades to black;
      left/right swaps to the next case, the C-stick turns it.

      Disc art is read from <device>/swiss/discs/<GAMEID>.png
      (GameTDB's 160x160 disc images fit as they are).
   ----------------------------------------------------------- */

#ifndef JEWELCASE_H
#define JEWELCASE_H

#include <gccore.h>
#include "deviceHandler.h"

#define JEWEL_ID_LEN 8

typedef struct {
	char id[JEWEL_ID_LEN];	// six character game ID, empty if unknown
} jewel_entry_t;

// Main thread
void JewelCase_Enter(int count, int selected);
void JewelCase_Step(int delta);
void JewelCase_GoTo(int index);
void JewelCase_SetOpen(bool open);
bool JewelCase_IsOpen(void);
bool JewelCase_Launch(void);
bool JewelCase_IsLaunching(void);
bool JewelCase_IsCovered(void);
bool JewelCase_LaunchDone(void);
int  JewelCase_Selected(void);
void JewelCase_SetDrag(float x, float y, bool active);
void JewelCase_ToggleSpin(void);
void JewelCase_TogglePause(void);
void JewelCase_Interact(void);
void JewelCase_GameId(file_handle *file, char *id);
bool JewelCase_NeedsArt(const char *id);
void JewelCase_LoadArt(const char *id);

// Video thread
void JewelCase_Draw(const jewel_entry_t *entries, int count);

#endif
