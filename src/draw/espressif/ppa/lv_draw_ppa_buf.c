/**
 * @file lv_draw_ppa_buf.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA
#include LV_STDINT_INCLUDE
#include "../../lv_draw_buf_private.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 *********************/

/**********************
 *  STATIC PROTOTYPES
 *********************/
static void invalidate_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area);

/**********************
 *   GLOBAL FUNCTIONS
 *********************/
void lv_draw_buf_ppa_init_handlers(void)
{
    lv_draw_buf_handlers_t * handlers = lv_draw_buf_get_handlers();
    handlers->invalidate_cache_cb = invalidate_cache;
}

/**********************
 *   STATIC FUNCTIONS
 *********************/

/* greenwood: sync only the rows covered by `area` (buffer-relative, per the LVGL API; NULL =
 * whole buffer). It used to ignore `area` and write back the entire draw buffer (1.2 MB for the
 * 1024x600 RGB565 frame) on every call — twice per PPA op, i.e. ~240x per frame for ~120 small
 * snowflake areas, which made rendering ~1.3 ms per area regardless of its size. */
static void invalidate_cache(const lv_draw_buf_t * draw_buf, const lv_area_t * area)
{
    uint8_t * data = (uint8_t *)draw_buf->data;
    size_t size = draw_buf->data_size;
    if(area) {
        int32_t y1 = LV_MAX(area->y1, 0);
        int32_t y2 = LV_MIN(area->y2, (int32_t)draw_buf->header.h - 1);
        if(y2 < y1) return;
        uint32_t stride = draw_buf->header.stride;
        data += (size_t)y1 * stride;
        size = (size_t)(y2 - y1 + 1) * stride;
    }
    esp_cache_msync(data, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
#endif /* LV_USE_PPA */
