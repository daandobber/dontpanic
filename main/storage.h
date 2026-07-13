#pragma once

#include <stdbool.h>

// Mount point where the SD card is exposed in the VFS once mounted.
#define STORAGE_MOUNT_POINT "/sdcard"

// Mounts the SD card over the 4-bit SDIO bus wired on Tanmatsu.
// Returns true on success. Safe to call again after storage_unmount_sdcard().
bool storage_mount_sdcard(void);

// Unmounts the SD card, if it is currently mounted.
void storage_unmount_sdcard(void);

// Whether the SD card is currently mounted.
bool storage_sdcard_mounted(void);
