#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

settings_t settings = {
    .gain = 0.016f,
    .accel = 1.0f,
    .tv_sleep = 0,
    .pc_sleep = 10,
    .smooth = 15,
    .dead = 12.0f,
};

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READONLY, &h) != ESP_OK) {
        return;  // nothing stored yet: defaults
    }
    settings_t s = settings;  // a blob from older firmware is shorter: the fields it lacks keep their defaults
    size_t len = sizeof(s);
    if (nvs_get_blob(h, "settings", &s, &len) == ESP_OK && len <= sizeof(s)) {
        settings = s;
    }
    nvs_close(h);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("mrb", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_blob(h, "settings", &settings, sizeof(settings));
    nvs_commit(h);
    nvs_close(h);
}

int settings_set(const char *name, float value)
{
    if (!strcmp(name, "gain")) {
        settings.gain = value;
    } else if (!strcmp(name, "accel")) {
        settings.accel = value;
    } else if (!strcmp(name, "tvsleep")) {
        settings.tv_sleep = value < 0 ? 0 : (uint32_t)value;
    } else if (!strcmp(name, "pcsleep")) {
        settings.pc_sleep = value < 0 ? 0 : (uint32_t)value;
    } else if (!strcmp(name, "dead")) {
        settings.dead = value < 0 ? 0 : value;
    } else if (!strcmp(name, "smooth")) {
        settings.smooth = value < 0 ? 0 : value > 100 ? 100 : (uint32_t)value;
    } else {
        return -1;
    }
    save();
    settings_print();
    return 0;
}

void settings_print(void)
{
    ESP_LOGI(TAG, "gain=%.4f accel=%.2f tvsleep=%lus%s pcsleep=%lus%s smooth=%lums dead=%.1f", settings.gain, settings.accel,
             (unsigned long)settings.tv_sleep, settings.tv_sleep ? "" : " (TV decides)",
             (unsigned long)settings.pc_sleep, settings.pc_sleep ? "" : " (never)", (unsigned long)settings.smooth, settings.dead);
}
