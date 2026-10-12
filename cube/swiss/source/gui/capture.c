/* -----------------------------------------------------------
      capture.c - Screenshots and screen recording

      The video thread copies the finished frame out of the XFB
      (before the capture overlay is drawn on top) and hands it to
      a worker thread, which encodes and writes it to the device
      being browsed. Recordings are paced by the clock rather than
      by rendered frames: if the UI or the device falls behind, an
      empty chunk is written so players repeat the last frame and
      the video keeps real time.

      The video thread must never wait on the worker: shared state
      is guarded by short interrupts-off sections rather than a
      mutex, since a low priority worker holding a lock while the
      menus scan the SD card would stall the screen. For the same
      reason the worker runs above the main thread while it talks
      to the device (so it never sits on the FatFs volume lock) and
      only drops below it to encode.

      Swiss calls Capture_Stop() before it mounts, unmounts or
      probes a device. That doesn't end a recording: the worker
      finishes the file (so a complete video is on the card) and
      closes it, then keeps encoding frames into memory while
      Swiss works. Once the devices have been left alone for a
      moment it reopens the same file, writes out the held frames
      and carries on, so the video has no gap.
   ----------------------------------------------------------- */

#include <gccore.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>
#include "capture.h"
#include "capture_enc.h"
#include "deviceHandler.h"
#include "files.h"
#include "input.h"
#include "uisound.h"
#include "video.h"

#define SHOT_DIR        "swiss/screenshots"
#define REC_DIR         "swiss/recordings"
#define REC_FPS         30
#define REC_QUALITY     80
#define REC_SLOTS       6
#define REC_MAX_BYTES   (1000u << 20)	// well clear of the 2 GiB RIFF limit
#define HOLD_MAX_BYTES  (4u << 20)		// encoded frames kept in memory while Swiss uses the devices
#define HOLD_QUIET_MS   1500			// how long Swiss must leave the devices alone before writing resumes
#define STICK_PRESS     64
#define STICK_RELEASE   32
#define STATUS_SECS     2.5f
#define COMBO_HOLD_MS   1000
#define PRIO_IO         (LWP_PRIO_NORMAL + 1)	// above the main thread
#define PRIO_ENCODE     (LWP_PRIO_NORMAL - 2)	// below the main and metadata threads

// Interrupts-off critical section. Nothing inside may block or wake a thread.
static u32 lockLevel;
#define LOCK()   (lockLevel = IRQ_Disable())
#define UNLOCK() IRQ_Restore(lockLevel)

enum { REC_IDLE, REC_STARTING, REC_RUNNING, REC_STOPPING };

static lwp_t thread = LWP_THREAD_NULL;
static sem_t wake = LWP_SEM_NULL;
static bool quit;

// Screenshot hand-off
static bool shotRequested;
static bool shotReady;
static u8 *shotBuf;
static int shotW, shotH, shotStride;

// Recording ring, filled by the video thread and drained by the worker
static int recState;
static u8 *slotBuf[REC_SLOTS];
static u32 slotDrops[REC_SLOTS];
static int slotHead, slotCount;
static bool slotFilling;	// the video thread is writing the slot past the end of the ring
static bool recFailed;
static int recSrcW, recSrcH, recW, recH;
static bool recHalveV;
static u64 recStart;
static u32 recNextFrame, recPendingDrops;
static bool frameDue;

// Device hold, set by Capture_Stop()
static bool devHold;		// no device I/O until Swiss has left the devices alone for HOLD_QUIET_MS
static bool drainReq;		// Capture_Stop() is waiting for the worker to let go of the device
static u64 lastStopTime;

// Worker-only recording state
static DEVICEHANDLER_INTERFACE *recDev;
static file_handle *recFile;	// NULL while the file is closed for a hold
static AviInfo recInfo;
static u8 *recJpeg;
static u32 recJpegCap;
static u8 *recIndex;
static u32 recIndexCap;
static u32 recPos;
static char recName[64];
static char recStamp[32];

// Frames encoded while the file is closed for a hold
typedef struct HeldFrame {
	struct HeldFrame *next;
	u32 drops;	// repeats before this frame
	u32 size;
	u8 data[];
} HeldFrame;
static HeldFrame *heldHead, *heldTail;
static u32 heldBytes, heldDrops;

// Overlay
static char statusText[96];
static u64 statusTime;
static int stickState;
static bool stickShortcuts = true;
static bool comboHeld, comboFired;
static u64 comboStart;

static void setStatus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setStatus(const char *fmt, ...)
{
	char text[sizeof(statusText)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	LOCK();
	memcpy(statusText, text, sizeof(statusText));
	statusTime = gettime();
	UNLOCK();
}

static void encodePriority(bool encoding)
{
	LWP_SetThreadPriority(LWP_GetSelf(), encoding ? PRIO_ENCODE : PRIO_IO);
}

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

static DEVICEHANDLER_INTERFACE *captureDevice(void)
{
	DEVICEHANDLER_INTERFACE *dev = devices[DEVICE_CUR];
	if(dev && (dev->features & (FEAT_WRITE | FEAT_THREAD_SAFE)) == (FEAT_WRITE | FEAT_THREAD_SAFE)
		&& dev->writeFile && dev->makeDir && dev->statFile) {
		return dev;
	}
	return NULL;
}

// Picks an unused swiss/<dir>/<stamp>.<ext> name, stamping it with the current time unless one is given
// (stampOut receives the stamp used). name receives the path relative to the device.
static file_handle *createCaptureFile(DEVICEHANDLER_INTERFACE *dev, const char *dir, const char *ext, const char *useStamp, char *stampOut, char *name, int nameLen)
{
	file_handle *file = calloc(1, sizeof(file_handle));
	if(!file) return NULL;
	file->device = dev;
	concat_path(file->name, dev->initial->name, "swiss");
	dev->makeDir(file);
	concat_path(file->name, dev->initial->name, dir);
	dev->makeDir(file);

	char stamp[32];
	if(useStamp) {
		snprintf(stamp, sizeof(stamp), "%s", useStamp);
	}
	else {
		time_t now = time(NULL);
		struct tm *tm = now != (time_t)-1 ? localtime(&now) : NULL;
		if(tm) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
		else snprintf(stamp, sizeof(stamp), "%08X", (u32)gettime());
	}
	if(stampOut) strcpy(stampOut, stamp);

	for(int n = 1; n < 100; n++) {
		if(n == 1) snprintf(name, nameLen, "%s/%s.%s", dir, stamp, ext);
		else snprintf(name, nameLen, "%s/%s-%d.%s", dir, stamp, n, ext);
		memset(file, 0, sizeof(file_handle));
		file->device = dev;
		concat_path(file->name, dev->initial->name, name);
		if(dev->statFile(file)) {
			memset(file, 0, sizeof(file_handle));
			file->device = dev;
			concat_path(file->name, dev->initial->name, name);
			return file;
		}
	}
	free(file);
	return NULL;
}

static void saveScreenshot(const u8 *frame, int width, int height, int stride)
{
	DEVICEHANDLER_INTERFACE *dev = captureDevice();
	if(!dev) {
		setStatus("Screenshot failed: device is not writable");
		UISound_Play(SND_ERROR);
		return;
	}
	// Progressive 240p/288p frames are line doubled to keep the 4:3 shape
	u8 *png = NULL;
	encodePriority(true);
	size_t len = Enc_PNG(frame, width, height, stride, height <= 288 ? 2 : 1, &png);
	encodePriority(false);
	if(!len) {
		setStatus("Screenshot failed: out of memory");
		UISound_Play(SND_ERROR);
		return;
	}
	char name[64];
	file_handle *file = createCaptureFile(dev, SHOT_DIR, "png", NULL, NULL, name, sizeof(name));
	bool ok = file && dev->writeFile(file, png, len) == len;
	if(file) {
		if(dev->closeFile(file)) ok = false;
		free(file);
	}
	free(png);
	if(ok) {
		setStatus("Saved %s", name);
		UISound_Play(SND_INFO);
	}
	else {
		setStatus("Screenshot failed: could not write file");
		UISound_Play(SND_ERROR);
	}
}

/* ---------------------------------------------------------------------------
 * Recording (worker side)
 * ------------------------------------------------------------------------- */

static void recFreeHeld(void)
{
	while(heldHead) {
		HeldFrame *next = heldHead->next;
		free(heldHead);
		heldHead = next;
	}
	heldTail = NULL;
	heldBytes = heldDrops = 0;
}

static void recFreeAll(void)
{
	for(int i = 0; i < REC_SLOTS; i++) {
		free(slotBuf[i]);
		slotBuf[i] = NULL;
	}
	free(recJpeg);
	free(recIndex);
	recJpeg = recIndex = NULL;
	recIndexCap = 0;
	recFreeHeld();
}

static bool recAlloc(void)
{
	u32 frameBytes = recW * recH * 2;
	recJpegCap = 8 + frameBytes + 2;
	recJpeg = memalign(32, recJpegCap);
	recIndexCap = 1024;
	recIndex = malloc(recIndexCap * 16);
	bool ok = recJpeg && recIndex;
	for(int i = 0; i < REC_SLOTS && ok; i++) {
		slotBuf[i] = memalign(32, frameBytes);
		ok = slotBuf[i] != NULL;
	}
	if(!ok) {
		recFreeAll();
		setStatus("Recording failed: out of memory");
	}
	return ok;
}

// Starts a new file. continuation keeps the recording's name stem (<stamp>-2.avi ...).
static bool recCreateFile(DEVICEHANDLER_INTERFACE *dev, bool continuation)
{
	recDev = dev;
	if(!dev) {
		setStatus("Recording failed: device is not writable");
		return false;
	}
	recFile = createCaptureFile(dev, REC_DIR, "avi", continuation ? recStamp : NULL, recStamp, recName, sizeof(recName));
	memset(&recInfo, 0, sizeof(recInfo));
	recInfo.width = recW;
	recInfo.height = recH;
	recInfo.rate = REC_FPS;
	recInfo.scale = 1;
	u8 header[AVI_HEADER_SIZE];
	Avi_Header(header, &recInfo);
	if(!recFile || dev->writeFile(recFile, header, sizeof(header)) != sizeof(header)) {
		if(recFile) {
			dev->closeFile(recFile);
			if(dev->deleteFile) dev->deleteFile(recFile);
			free(recFile);
			recFile = NULL;
		}
		setStatus("Recording failed: could not create file");
		return false;
	}
	recPos = AVI_HEADER_SIZE;
	return true;
}

// After a hold: reopen the same file and append, or carry on in <stamp>-2.avi if that isn't possible
static bool recReopen(void)
{
	DEVICEHANDLER_INTERFACE *dev = captureDevice();
	if(dev && dev == recDev && dev->writeFile == deviceHandler_FAT_writeFile) {
		file_handle *file = calloc(1, sizeof(file_handle));
		if(file) {
			file->device = dev;
			concat_path(file->name, dev->initial->name, recName);
			if(!deviceHandler_FAT_openForWrite(file)) {
				// The old index gets overwritten by the next frames and rewritten when the file is closed
				dev->seekFile(file, recPos, DEVICE_HANDLER_SEEK_SET);
				recFile = file;
				return true;
			}
			free(file);
		}
	}
	return recCreateFile(dev, true);
}

static bool recIndexAdd(u32 size)
{
	if(recInfo.frames == recIndexCap) {
		u8 *grown = realloc(recIndex, recIndexCap * 2 * 16);
		if(!grown) return false;
		recIndex = grown;
		recIndexCap *= 2;
	}
	Avi_IndexEntry(recIndex + recInfo.frames * 16, recPos - AVI_MOVI_OFFSET, size);
	recInfo.frames++;
	return true;
}

// Writes one chunk from recJpeg + 8 (empty for a repeated frame). Returns false when recording has to end.
static bool recWriteChunk(u32 size)
{
	u32 total = Avi_ChunkHeader(recJpeg, size);
	if(recPos + total + 8 + (recInfo.frames + 1) * 16 > REC_MAX_BYTES) {
		setStatus("Recording stopped: file size limit reached");
		return false;
	}
	if(size & 1) recJpeg[8 + size] = 0;
	if(!recIndexAdd(size)) {
		setStatus("Recording stopped: out of memory");
		return false;
	}
	if(recDev->writeFile(recFile, recJpeg, total) != total) {
		recInfo.frames--;
		setStatus("Recording stopped: write failed (device full?)");
		return false;
	}
	recPos += total;
	if(size > recInfo.maxChunk) recInfo.maxChunk = size;
	return true;
}

static bool recWriteFrame(u32 drops, u32 size)
{
	for(u32 i = 0; i < drops; i++) {
		if(!recWriteChunk(0)) return false;
	}
	return recWriteChunk(size);
}

// Keeps a frame encoded at recJpeg + 8 in memory until the file can be written again
static void recHoldFrame(u32 drops, u32 size)
{
	HeldFrame *frame = heldBytes + size <= HOLD_MAX_BYTES ? malloc(sizeof(HeldFrame) + size) : NULL;
	if(!frame) {
		// Out of room: the frame becomes a repeat, which still keeps the timing
		heldDrops += drops + 1;
		return;
	}
	frame->next = NULL;
	frame->drops = heldDrops + drops;
	frame->size = size;
	memcpy(frame->data, recJpeg + 8, size);
	heldDrops = 0;
	heldBytes += size;
	if(heldTail) heldTail->next = frame;
	else heldHead = frame;
	heldTail = frame;
}

// Writes the oldest held frame (or the repeats left after the last one)
static bool recFlushHeld(void)
{
	if(!heldHead) {
		u32 drops = heldDrops;
		heldDrops = 0;
		for(u32 i = 0; i < drops; i++) {
			if(!recWriteChunk(0)) return false;
		}
		return true;
	}
	HeldFrame *frame = heldHead;
	heldHead = frame->next;
	if(!heldHead) heldTail = NULL;
	heldBytes -= frame->size;
	memcpy(recJpeg + 8, frame->data, frame->size);
	bool ok = recWriteFrame(frame->drops, frame->size);
	free(frame);
	return ok;
}

// Writes the index and final header and closes the file, leaving a complete video behind
static bool recFinishFile(void)
{
	recInfo.moviSize = recPos - AVI_HEADER_SIZE;
	recInfo.idxSize = recInfo.frames * 16;
	u8 header[AVI_HEADER_SIZE];
	Avi_IndexHeader(header, recInfo.frames);
	bool ok = recDev->writeFile(recFile, header, 8) == 8
		&& recDev->writeFile(recFile, recIndex, recInfo.idxSize) == recInfo.idxSize;
	Avi_Header(header, &recInfo);
	recDev->seekFile(recFile, 0, DEVICE_HANDLER_SEEK_SET);
	ok = recDev->writeFile(recFile, header, sizeof(header)) == sizeof(header) && ok;
	ok = !recDev->closeFile(recFile) && ok;
	free(recFile);
	recFile = NULL;
	return ok;
}

/* ---------------------------------------------------------------------------
 * Worker thread
 * ------------------------------------------------------------------------- */

// Call with the lock held: the recording can't go on, finish what's on the card
static void recFail(void)
{
	recFailed = true;
	slotCount = 0;
	recState = REC_STOPPING;
}

static void *captureThread(void *arg)
{
	static const struct timespec holdPoll = {0, 100 * 1000 * 1000};
	LOCK();
	while(!quit) {
		if(drainReq) {
			// Swiss is about to mount, unmount or probe a device: let go of it now.
			// The device is still usable at this point, so a pending screenshot is saved first.
			bool shot = shotReady;
			u8 *frame = shotBuf;
			int w = shotW, h = shotH, stride = shotStride;
			UNLOCK();
			if(shot) {
				saveScreenshot(frame, w, h, stride);
				free(frame);
			}
			bool ok = !recFile || recFinishFile();
			LOCK();
			if(shot) {
				shotBuf = NULL;
				shotReady = false;
			}
			if(!ok) {
				setStatus("Recording may be incomplete: %s", recName);
				recFail();
			}
			drainReq = false;
			continue;
		}

		bool ioOK = !devHold;
		if(devHold && ticks_to_millisecs(gettime() - lastStopTime) >= HOLD_QUIET_MS) {
			// Swiss has left the devices alone for a while. Keep holding while there's
			// nothing writable to go back to (e.g. the device picker is open).
			UNLOCK();
			bool writable = captureDevice() != NULL;
			LOCK();
			if(writable && !drainReq) devHold = false, ioOK = true;
		}
		bool active = recState == REC_RUNNING || recState == REC_STOPPING;
		bool held = heldHead || heldDrops;

		if(shotReady && ioOK) {
			u8 *frame = shotBuf;
			int w = shotW, h = shotH, stride = shotStride;
			UNLOCK();
			saveScreenshot(frame, w, h, stride);
			free(frame);
			LOCK();
			shotBuf = NULL;
			shotReady = false;
		}
		else if(recState == REC_STARTING && ioOK) {
			UNLOCK();
			bool ok = recAlloc();
			if(ok && !recCreateFile(captureDevice(), false)) {
				recFreeAll();
				ok = false;
			}
			if(!ok) UISound_Play(SND_ERROR);
			LOCK();
			if(ok) {
				slotHead = slotCount = 0;
				slotFilling = recFailed = false;
				recNextFrame = recPendingDrops = 0;
				recStart = gettime();
				recState = REC_RUNNING;
				statusTime = 0;
			}
			else {
				recState = REC_IDLE;
			}
		}
		else if(active && !recFailed && !recFile && ioOK && (held || slotCount || recState == REC_RUNNING)) {
			// Back from a hold
			UNLOCK();
			bool ok = recReopen();
			LOCK();
			if(!ok) recFail();
		}
		else if(active && !recFailed && recFile && held) {
			UNLOCK();
			bool ok = recFlushHeld();
			LOCK();
			if(!ok) recFail();
		}
		else if(slotCount) {
			int slot = slotHead;
			u32 drops = slotDrops[slot];
			bool direct = recFile && !held && !drainReq;
			UNLOCK();
			encodePriority(true);
			u32 size = Enc_JPEG(slotBuf[slot], recW, recH, recW * 2, REC_QUALITY, recJpeg + 8, recJpegCap - 10);
			encodePriority(false);
			bool ok = true;
			if(direct) ok = recWriteFrame(drops, size);
			else recHoldFrame(drops, size);
			LOCK();
			slotHead = (slotHead + 1) % REC_SLOTS;
			slotCount--;
			if(!ok) recFail();
		}
		else if(recState == REC_STOPPING && !slotFilling && (recFailed || !held)) {
			bool failed = recFailed;
			UNLOCK();
			bool ok = !recFile || recFinishFile();
			u32 secs = recInfo.frames / REC_FPS;
			if(!failed) {
				if(ok) setStatus("Saved %s (%u:%02u)", recName, secs / 60, secs % 60);
				else setStatus("Recording may be incomplete: %s", recName);
			}
			UISound_Play(ok && !failed ? SND_INFO : SND_ERROR);
			recFreeAll();
			LOCK();
			recState = REC_IDLE;
			recFailed = false;
		}
		else {
			bool polling = devHold;
			UNLOCK();
			if(polling) LWP_SemTimedWait(wake, &holdPoll);
			else LWP_SemWait(wake);
			LOCK();
		}
	}
	UNLOCK();
	return NULL;
}

void Capture_Init(void)
{
	if(thread != LWP_THREAD_NULL) return;
	quit = false;
	shotRequested = shotReady = false;
	recState = REC_IDLE;
	devHold = drainReq = false;
	statusTime = 0;
	stickState = 0;
	comboHeld = comboFired = false;
	LWP_SemInit(&wake, 0, 0x7FFFFFFF);
	LWP_CreateThread(&thread, captureThread, NULL, NULL, 32*1024, PRIO_IO);
}

void Capture_Stop(void)
{
	if(thread == LWP_THREAD_NULL || LWP_GetSelf() == thread) return;
	LOCK();
	lastStopTime = gettime();
	devHold = drainReq = true;
	UNLOCK();
	for(;;) {
		LWP_SemPost(wake);
		LOCK();
		bool waiting = drainReq;
		UNLOCK();
		if(!waiting) break;
		usleep(5000);
	}
	// lastStopTime again: the quiet period counts from when Swiss actually gets the device
	LOCK();
	lastStopTime = gettime();
	UNLOCK();
}

void Capture_Shutdown(void)
{
	if(thread == LWP_THREAD_NULL) return;
	// Finishes the file; frames still held in memory can't be written any more
	Capture_Stop();
	LOCK();
	quit = true;
	UNLOCK();
	LWP_SemPost(wake);
	LWP_JoinThread(thread, NULL);
	thread = LWP_THREAD_NULL;
	LWP_SemDestroy(wake);
	if(shotReady) free(shotBuf);
	shotBuf = NULL;
	shotReady = false;
	recFreeAll();
	recState = REC_IDLE;
}

/* ---------------------------------------------------------------------------
 * Video thread side
 * ------------------------------------------------------------------------- */

static s8 subStickY(void)
{
	s8 best = 0;
	for(int chan = PAD_CHAN0; chan < PAD_CHANMAX; chan++) {
		s8 y = PAD_SubStickY(chan);
		if(abs(y) > abs(best)) best = y;
	}
	return best;
}

static void requestScreenshot(void)
{
	LOCK();
	if(!shotRequested && !shotReady) shotRequested = true;
	UNLOCK();
}

static void toggleRecording(void)
{
	GXRModeObj *vmode = getVideoMode();
	LOCK();
	if(recState == REC_IDLE) {
		recSrcW = vmode->fbWidth;
		recSrcH = vmode->xfbHeight;
		recHalveV = recSrcH > 288;
		recW = recSrcW / 2;
		recH = recHalveV ? recSrcH / 2 : recSrcH;
		recState = REC_STARTING;
	}
	else if(recState == REC_RUNNING) {
		recState = REC_STOPPING;
	}
	UNLOCK();
	LWP_SemPost(wake);
}

// L+R+Z+D-Pad Down: tap for a screenshot, hold to start or stop recording
static void pollCombo(void)
{
	bool held = false;
	for(int chan = PAD_CHAN0; chan < PAD_CHANMAX; chan++) {
		held |= padsCaptureCombo(PAD_ButtonsHeld(chan));
	}
	if(held) {
		if(!comboHeld) {
			comboHeld = true;
			comboFired = false;
			comboStart = gettime();
		}
		else if(!comboFired && ticks_to_millisecs(gettime() - comboStart) >= COMBO_HOLD_MS) {
			comboFired = true;
			toggleRecording();
		}
	}
	else if(comboHeld) {
		comboHeld = false;
		if(!comboFired) requestScreenshot();
	}
}

void Capture_Poll(void)
{
	if(thread == LWP_THREAD_NULL) return;
	pollCombo();
	if(!stickShortcuts) return;

	s8 y = subStickY();
	if(stickState) {
		if(abs(y) < STICK_RELEASE) stickState = 0;
		return;
	}
	if(abs(y) < STICK_PRESS) return;
	stickState = y > 0 ? 1 : -1;
	if(stickState < 0) requestScreenshot();
	else toggleRecording();
}

void Capture_SetStickShortcuts(bool enabled)
{
	stickShortcuts = enabled;
	// a stick already held over doesn't fire when they come back
	stickState = enabled ? 1 : 0;
}

bool Capture_WantsFrame(void)
{
	if(thread == LWP_THREAD_NULL) return false;
	LOCK();
	frameDue = false;
	if(recState == REC_RUNNING) {
		u32 frame = (u32)(ticks_to_microsecs(gettime() - recStart) * REC_FPS / 1000000);
		frameDue = frame >= recNextFrame;
	}
	bool want = shotRequested || frameDue;
	UNLOCK();
	return want;
}

void Capture_Frame(const void *xfb, u16 fbWidth, u16 height)
{
	int stride = VIDEO_PadFramebufferWidth(fbWidth) * 2;
	DCInvalidateRange((void *)xfb, stride * height);

	LOCK();
	bool shot = shotRequested && !shotReady;
	shotRequested = false;
	UNLOCK();
	if(shot) {
		u8 *buf = malloc(stride * height);
		if(buf) {
			memcpy(buf, xfb, stride * height);
			LOCK();
			shotBuf = buf;
			shotW = fbWidth;
			shotH = height;
			shotStride = stride;
			shotReady = true;
			UNLOCK();
			LWP_SemPost(wake);
		}
		else {
			setStatus("Screenshot failed: out of memory");
			UISound_Play(SND_ERROR);
		}
	}

	LOCK();
	if(!frameDue || recState != REC_RUNNING) {
		UNLOCK();
		return;
	}
	frameDue = false;
	if(fbWidth != recSrcW || height < (recHalveV ? recH * 2 : recH)) {
		// The video mode changed underneath us
		recState = REC_STOPPING;
		UNLOCK();
		LWP_SemPost(wake);
		return;
	}
	u32 frame = (u32)(ticks_to_microsecs(gettime() - recStart) * REC_FPS / 1000000);
	u32 skipped = frame - recNextFrame;
	recNextFrame = frame + 1;
	if(slotCount == REC_SLOTS) {
		// The device can't keep up, this frame becomes a repeat
		recPendingDrops += skipped + 1;
		UNLOCK();
		return;
	}
	int slot = (slotHead + slotCount) % REC_SLOTS;
	slotDrops[slot] = recPendingDrops + skipped;
	recPendingDrops = 0;
	slotFilling = true;
	UNLOCK();

	// The worker won't free the slots or read this one until slotFilling is cleared
	Enc_HalveYUY2(xfb, recSrcW, recHalveV ? recH * 2 : recH, stride, recHalveV, slotBuf[slot]);

	LOCK();
	slotFilling = false;
	// Dropped if recording was stopped meanwhile
	if(recState == REC_RUNNING) slotCount++;
	UNLOCK();
	LWP_SemPost(wake);
}

bool Capture_Overlay(char *text, int textLen, bool *recording, float *alpha)
{
	if(thread == LWP_THREAD_NULL) return false;
	bool show = false;
	char status[sizeof(statusText)];
	LOCK();
	*recording = recState == REC_RUNNING;
	*alpha = 1.0f;
	if(statusTime) {
		float age = ticks_to_millisecs(gettime() - statusTime) / 1000.0f;
		if(age < STATUS_SECS) {
			memcpy(status, statusText, sizeof(status));
			*alpha = age > STATUS_SECS - 0.5f ? (STATUS_SECS - age) / 0.5f : 1.0f;
			show = true;
		}
		else {
			statusTime = 0;
		}
	}
	u32 secs = ticks_to_secs(gettime() - recStart);
	UNLOCK();
	if(show) {
		snprintf(text, textLen, "%s", status);
	}
	else if(*recording) {
		snprintf(text, textLen, "REC %u:%02u", secs / 60, secs % 60);
		show = true;
	}
	return show;
}
