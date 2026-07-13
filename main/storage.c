#include "storage.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

// Tanmatsu wires the SD card slot to the 4-bit SDIO bus on these pins. The
// badge-bsp component does not expose these as a public API (they only exist
// in its private per-target hardware header), so they are mirrored here.
// See: esp32-component-badge-bsp/targets/tanmatsu/tanmatsu_hardware.h
#define TANMATSU_SDCARD_CLK   43
#define TANMATSU_SDCARD_CMD   44
#define TANMATSU_SDCARD_D0    39
#define TANMATSU_SDCARD_D1    40
#define TANMATSU_SDCARD_D2    41
#define TANMATSU_SDCARD_D3    42
#define TANMATSU_SDCARD_WIDTH 4

static char const   TAG[]    = "storage";
static sdmmc_card_t *s_card  = NULL;
static bool          s_mounted = false;

bool storage_mount_sdcard(void) {
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width               = TANMATSU_SDCARD_WIDTH;
    slot_config.clk                 = TANMATSU_SDCARD_CLK;
    slot_config.cmd                 = TANMATSU_SDCARD_CMD;
    slot_config.d0                  = TANMATSU_SDCARD_D0;
    slot_config.d1                  = TANMATSU_SDCARD_D1;
    slot_config.d2                  = TANMATSU_SDCARD_D2;
    slot_config.d3                  = TANMATSU_SDCARD_D3;
    slot_config.flags              |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t res = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(res));
        s_card    = NULL;
        s_mounted = false;
        return false;
    }

    s_mounted = true;
    return true;
}

void storage_unmount_sdcard(void) {
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT_POINT, s_card);
    s_card    = NULL;
    s_mounted = false;
}

bool storage_sdcard_mounted(void) {
    return s_mounted;
}
