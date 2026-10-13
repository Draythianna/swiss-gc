#ifndef __INPUT_H
#define __INPUT_H

#include <ogc/pad.h>

#define BUTTON_LEFT  (PAD_BUTTON_LEFT)
#define BUTTON_RIGHT (PAD_BUTTON_RIGHT)
#define BUTTON_DOWN  (PAD_BUTTON_DOWN)
#define BUTTON_UP    (PAD_BUTTON_UP)
#define BUTTON_Z     (PAD_TRIGGER_Z)
#define BUTTON_R     (PADEX_TRIGGER_R)
#define BUTTON_L     (PADEX_TRIGGER_L)
#define BUTTON_CLAP  (PADEX_BARREL_MIC)
#define BUTTON_A     (PAD_BUTTON_A)
#define BUTTON_B     (PAD_BUTTON_B)
#define BUTTON_X     (PAD_BUTTON_X | PADEX_BUTTON_C_RIGHT | PADEX_BUTTON_C_DOWN)
#define BUTTON_Y     (PAD_BUTTON_Y | PADEX_BUTTON_C_LEFT  | PADEX_BUTTON_C_UP)
#define BUTTON_START (PAD_BUTTON_START)

// L+R+Z+D-Pad Down, the screen capture combo (either trigger click or analog press counts)
#define CAPTURE_COMBO_L  (PAD_TRIGGER_L | PADEX_TRIGGER_L)
#define CAPTURE_COMBO_R  (PAD_TRIGGER_R | PADEX_TRIGGER_R)
#define padsCaptureCombo(b) (((b) & CAPTURE_COMBO_L) && ((b) & CAPTURE_COMBO_R) && ((b) & PAD_TRIGGER_Z) && ((b) & PAD_BUTTON_DOWN))

void padsInit();
u32 padsButtonsHeld();
s8 padsStickX();
s8 padsStickY();
s8 padsSubStickX();
s8 padsSubStickY();

#endif
