#include "bsp/tanmatsu.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tanmatsu_coprocessor.h"

int hosted_reset_slave_callback(void) {
    tanmatsu_coprocessor_handle_t handle = NULL;
    esp_err_t res = bsp_tanmatsu_coprocessor_get_handle(&handle);
    if (res != ESP_OK) {
        return res;
    }

    res = tanmatsu_coprocessor_radio_disable(handle);
    if (res != ESP_OK) {
        return res;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    return tanmatsu_coprocessor_radio_enable_application(handle);
}
