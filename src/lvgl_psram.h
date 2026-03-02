// lvgl_psram.h - PSRAM allocator functions for LVGL
#ifndef LVGL_PSRAM_H
#define LVGL_PSRAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

void* lvgl_psram_malloc(size_t size);
void* lvgl_psram_realloc(void* ptr, size_t new_size);
void lvgl_psram_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif // LVGL_PSRAM_H