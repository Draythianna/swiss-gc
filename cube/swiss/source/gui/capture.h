/* -----------------------------------------------------------
      capture.h - Screenshots and screen recording

      Tap L+R+Z+D-Pad Down (or flick the C-Stick down) to save a
      PNG screenshot; hold the combo for a second (or flick the
      C-Stick up) to start or stop recording an MJPEG AVI. Files go to
      swiss/screenshots and swiss/recordings on the device being
      browsed, written from a background thread.
   ----------------------------------------------------------- */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <gctypes.h>
#include <stdbool.h>

void Capture_Init(void);
void Capture_Shutdown(void);
// Finishes any screenshot or recording in progress, call before unmounting the current device
void Capture_Stop(void);

// Video thread: read the hotkeys once per frame
void Capture_Poll(void);
// Turns the C-Stick flicks off (and back on) for views that use the C-Stick
// themselves; the button combo always works
void Capture_SetStickShortcuts(bool enabled);
// Video thread: true when the frame about to be copied out should be handed to Capture_Frame
bool Capture_WantsFrame(void);
// Video thread: grab the YUY2 frame just copied to the XFB
void Capture_Frame(const void *xfb, u16 fbWidth, u16 height);

// Overlay state: returns false when there is nothing to show
bool Capture_Overlay(char *text, int textLen, bool *recording, float *alpha);

#endif
