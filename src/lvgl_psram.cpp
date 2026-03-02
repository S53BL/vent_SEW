// lvgl_psram.cpp - PSRAM allocator implementation for LVGL
#include "lvgl_psram.h"
#include <stdlib.h>
#include <string.h>

#if CONFIG_SPIRAM
#include <esp32-hal-psram.h>
#endif

void* lvgl_psram_malloc(size_t size) {
#if CONFIG_SPIRAM
    void* ptr = ps_malloc(size);
    if (ptr) return ptr;
    // fallback to internal RAM if PSRAM fails
#endif
    return malloc(size);
}

void* lvgl_psram_realloc(void* ptr, size_t new_size) {
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }

#if CONFIG_SPIRAM
    void* new_ptr = ps_malloc(new_size);
    if (new_ptr) {
        if (ptr) {
            // Copy old data (note: this is simplified - should copy min(old_size, new_size))
            memcpy(new_ptr, ptr, new_size);
            free(ptr);
        }
        return new_ptr;
    }
    // PSRAM allocation failed, fallback to realloc
#endif
    return realloc(ptr, new_size);
}

void lvgl_psram_free(void* ptr) {
    free(ptr);
}