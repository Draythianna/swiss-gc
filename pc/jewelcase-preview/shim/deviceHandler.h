/* Host shim: just enough of Swiss's device layer for jewelcase.c */
#ifndef SHIM_DEVICEHANDLER_H
#define SHIM_DEVICEHANDLER_H
#include <gccore.h>
#define PATHNAME_MAX 1024
typedef struct { dvddiskid diskId; const char *displayName; } file_meta;
typedef struct { char name[PATHNAME_MAX]; u32 size; void *fp; file_meta *meta; } file_handle;
typedef struct {
	const char *deviceName;
	file_handle *initial;
	s32 (*statFile)(file_handle *);
	s64 (*seekFile)(file_handle *, s64, u32);
	s32 (*readFile)(file_handle *, void *, u32);
	s32 (*closeFile)(file_handle *);
} DEVICEHANDLER_INTERFACE;
#define DEVICE_CUR 0
#define DEVICE_HANDLER_SEEK_SET 0
extern DEVICEHANDLER_INTERFACE *devices[8];
#endif
