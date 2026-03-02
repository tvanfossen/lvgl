/**
 * @file lv_lottie.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_lottie_private.h"
#include "../../lv_conf_internal.h"
#if LV_USE_LOTTIE

/* Diagnostic: checkpoint logging to narrow crash location */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
/* idf_additions.h intentionally not included: task created via standard xTaskCreate */
static const char * LV_LOTTIE_TAG = "lv_lottie";

#if LV_USE_THORVG_EXTERNAL
    #include <thorvg_capi.h>
#else
    #include "../../libs/thorvg/thorvg_capi.h"
#endif

#include "../../misc/lv_timer.h"
#include "../../core/lv_obj_class_private.h"
#include "../../misc/cache/lv_cache.h"

/*********************
 *      DEFINES
 *********************/
#define MY_CLASS (&lv_lottie_class)

/** Stack for the permanent ThorVG render task, allocated from SPIRAM.
 *  ThorVG rasterisation uses ~30 KB; 96 KB gives comfortable headroom for
 *  complex animations.  SPIRAM is plentiful so size generously. */
#define LOTTIE_RENDER_TASK_STACK    (96 * 1024)

/** Warn when render task stack headroom falls below this threshold. */
#define LOTTIE_RENDER_MIN_HW        (8 * 1024)

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_lottie_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_lottie_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void anim_exec_cb(void * var, int32_t v);
static void lottie_update(lv_lottie_t * lottie, int32_t v);
static void lottie_render_task(void * arg);

/**********************
 *  STATIC VARIABLES
 **********************/
const lv_obj_class_t lv_lottie_class = {
    .constructor_cb = lv_lottie_constructor,
    .destructor_cb = lv_lottie_destructor,
    .width_def = LV_DPI_DEF,
    .height_def = LV_DPI_DEF,
    .instance_size = sizeof(lv_lottie_t),
    .base_class = &lv_canvas_class,
    .name = "lv_lottie",
};

/**********************
 *  GLOBAL VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lv_lottie_create(lv_obj_t * parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void lv_lottie_set_buffer(lv_obj_t * obj, int32_t w, int32_t h, void * buf)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    int32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED);
    buf = lv_draw_buf_align(buf, LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED);

    tvg_swcanvas_set_target(lottie->tvg_canvas, buf, stride / 4, w, h, TVG_COLORSPACE_ARGB8888);
    tvg_canvas_push(lottie->tvg_canvas, lottie->tvg_paint);
    lv_canvas_set_buffer(obj, buf, w, h, LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED);
    tvg_picture_set_size(lottie->tvg_paint, w, h);

    /* Rendered output images are premultiplied */
    lv_draw_buf_t * draw_buf = lv_canvas_get_draw_buf(obj);
    lv_draw_buf_set_flag(draw_buf, LV_IMAGE_FLAGS_PREMULTIPLIED);

    /*Force updating when the buffer changes*/
    float f_current;
    tvg_animation_get_frame(lottie->tvg_anim, &f_current);
    anim_exec_cb(obj, (int32_t) f_current);
}

void lv_lottie_set_draw_buf(lv_obj_t * obj, lv_draw_buf_t * draw_buf)
{
    if(draw_buf->header.cf != LV_COLOR_FORMAT_ARGB8888 && draw_buf->header.cf != LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED) {
        LV_LOG_WARN("The draw buf needs to have ARGB8888 or ARGB8888_PREMULTIPLIED color format");
        return;
    }

    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    tvg_swcanvas_set_target(lottie->tvg_canvas, (void *)draw_buf->data, draw_buf->header.stride / 4,
                            draw_buf->header.w, draw_buf->header.h, TVG_COLORSPACE_ARGB8888);
    tvg_canvas_push(lottie->tvg_canvas, lottie->tvg_paint);
    lv_canvas_set_draw_buf(obj, draw_buf);
    tvg_picture_set_size(lottie->tvg_paint, draw_buf->header.w, draw_buf->header.h);

    /* Rendered output images are premultiplied */
    lv_draw_buf_set_flag(draw_buf, LV_IMAGE_FLAGS_PREMULTIPLIED);

    /*Force updating when the buffer changes*/
    float f_current;
    tvg_animation_get_frame(lottie->tvg_anim, &f_current);
    anim_exec_cb(obj, (int32_t) f_current);
}

void lv_lottie_set_src_data(lv_obj_t * obj, const void * src, size_t src_size)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    tvg_picture_load_data(lottie->tvg_paint, src, src_size, "lottie", true);
    lottie->src_loaded = true; /* allow lottie_update to call ThorVG */
    lv_draw_buf_t * canvas_draw_buf = lv_canvas_get_draw_buf(obj);
    if(canvas_draw_buf) {
        tvg_picture_set_size(lottie->tvg_paint, canvas_draw_buf->header.w, canvas_draw_buf->header.h);
    }

    float f_total;
    tvg_animation_get_total_frame(lottie->tvg_anim, &f_total);
    lv_anim_set_duration(lottie->anim, (int32_t)f_total * 1000 / 60); /*60 FPS*/
    lottie->anim->act_time = 0;
    lottie->anim->end_value = (int32_t)f_total;
    lottie->anim->reverse_play_in_progress = false;
    lottie_update(lottie, 0);   /*Render immediately*/
}

void lv_lottie_set_src_file(lv_obj_t * obj, const char * src)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    ESP_LOGE(LV_LOTTIE_TAG, "CP1: tvg_picture_load start src=%s", src);
    Tvg_Result load_res = tvg_picture_load(lottie->tvg_paint, src);
    ESP_LOGE(LV_LOTTIE_TAG, "CP2: tvg_picture_load done res=%d", (int)load_res);
    if(load_res == TVG_RESULT_SUCCESS) {
        lottie->src_loaded = true; /* allow lottie_update to call ThorVG */
    }
    lv_draw_buf_t * canvas_draw_buf = lv_canvas_get_draw_buf(obj);
    if(canvas_draw_buf) {
        ESP_LOGE(LV_LOTTIE_TAG, "CP3: tvg_picture_set_size %dx%d",
                 (int)canvas_draw_buf->header.w, (int)canvas_draw_buf->header.h);
        tvg_picture_set_size(lottie->tvg_paint, canvas_draw_buf->header.w, canvas_draw_buf->header.h);
        ESP_LOGE(LV_LOTTIE_TAG, "CP4: tvg_picture_set_size done");
    }

    float f_total = 0;
    ESP_LOGE(LV_LOTTIE_TAG, "CP5: tvg_animation_get_total_frame start");
    tvg_animation_get_total_frame(lottie->tvg_anim, &f_total);
    ESP_LOGE(LV_LOTTIE_TAG, "CP6: tvg_animation_get_total_frame done f_total=%d", (int)f_total);
    lv_anim_set_duration(lottie->anim, (int32_t)f_total * 1000 / 60); /*60 FPS*/
    lottie->anim->act_time = 0;
    lottie->anim->end_value = (int32_t)f_total;
    lottie->anim->reverse_play_in_progress = false;
    ESP_LOGE(LV_LOTTIE_TAG, "CP7: calling lottie_update (initial, inline on load-task stack)");
    lottie_update(lottie, 0);   /*Render immediately*/
    ESP_LOGE(LV_LOTTIE_TAG, "CP8: lottie_update done");
}


lv_anim_t * lv_lottie_get_anim(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    return lottie->anim;
}

bool lv_lottie_render_failed(lv_obj_t * obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    return lottie->render_failed;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_lottie_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    LV_TRACE_OBJ_CREATE("begin");
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: ENTRY obj=%p", obj);

    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: set_size done");

    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: tvg_animation_new");
    lottie->tvg_anim = tvg_animation_new();

    ESP_LOGE(LV_LOTTIE_TAG, "ctor: tvg_animation_get_picture");
    lottie->tvg_paint = tvg_animation_get_picture(lottie->tvg_anim);
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: tvg_swcanvas_create");
    lottie->tvg_canvas = tvg_swcanvas_create();
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: tvg init done, creating semaphores");

    lottie->src_loaded        = false;
    lottie->initial_render_done = false;
    lottie->destroying        = false;
    lottie->render_failed     = false;
    lottie->render_frame      = 0;
    lottie->render_task       = NULL;
    lottie->render_start_sem  = NULL;
    lottie->render_done_sem   = NULL;
    lottie->render_exit_sem   = NULL;
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: fields init done, internal_free=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Create semaphores */
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: xSemaphoreCreateBinary #1");
    lottie->render_start_sem = xSemaphoreCreateBinary();
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: start_sem=%p", lottie->render_start_sem);
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: xSemaphoreCreateBinary #2");
    lottie->render_done_sem  = xSemaphoreCreateBinary();
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: done_sem=%p", lottie->render_done_sem);
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: xSemaphoreCreateBinary #3");
    lottie->render_exit_sem  = xSemaphoreCreateBinary();
    ESP_LOGE(LV_LOTTIE_TAG, "ctor: exit_sem=%p", lottie->render_exit_sem);

    if(!lottie->render_start_sem || !lottie->render_done_sem || !lottie->render_exit_sem) {
        ESP_LOGE(LV_LOTTIE_TAG, "lottie: semaphore creation failed");
        lottie->render_failed = true;
    }
    else {
        /* Create permanent render task with stack in SPIRAM.
         * LVGL task blocks on render_done_sem during each render, so there
         * is no race between ThorVG writing the canvas buffer and LVGL
         * flushing it to the display. */
        /* Use standard xTaskCreate so pvPortMalloc routes the 96 KB stack
         * automatically to SPIRAM (96 KB > ALWAYSINTERNAL=4096 threshold).
         * TCB (~350 B, ≤4096) stays in DRAM — scheduler-safe.
         * xTaskCreateWithCaps(SPIRAM) asserts/aborts internally on ESP32-P4. */
        ESP_LOGE(LV_LOTTIE_TAG, "ctor: xTaskCreate stack=%u", LOTTIE_RENDER_TASK_STACK);
        BaseType_t ret = xTaskCreate(
            lottie_render_task, "lottie_rndr",
            LOTTIE_RENDER_TASK_STACK / sizeof(StackType_t), lottie,
            4,                                   /* priority: below LVGL (5), above idle */
            &lottie->render_task);
        ESP_LOGE(LV_LOTTIE_TAG, "ctor: xTaskCreate ret=%d task=%p", (int)ret, lottie->render_task);

        if(ret != pdPASS) {
            ESP_LOGE(LV_LOTTIE_TAG,
                     "lottie: render task creation failed — SPIRAM free=%lu B",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            lottie->render_failed = true;
        }
        else {
            ESP_LOGI(LV_LOTTIE_TAG,
                     "lottie: render task started (stack=%u B, SPIRAM free=%lu B)",
                     LOTTIE_RENDER_TASK_STACK,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_exec_cb);
    lv_anim_set_var(&a, obj);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lottie->anim = lv_anim_start(&a);

    LV_TRACE_OBJ_CREATE("finished");
}

static void lv_lottie_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    lv_lottie_t * lottie = (lv_lottie_t *)obj;

    /* Ask render task to exit, then wait for it to acknowledge */
    if(lottie->render_task) {
        lottie->destroying = true;
        if(lottie->render_start_sem) xSemaphoreGive(lottie->render_start_sem);
        if(lottie->render_exit_sem)
            xSemaphoreTake(lottie->render_exit_sem, pdMS_TO_TICKS(500));
        lottie->render_task = NULL;
    }

    if(lottie->render_start_sem) { vSemaphoreDelete(lottie->render_start_sem); lottie->render_start_sem = NULL; }
    if(lottie->render_done_sem)  { vSemaphoreDelete(lottie->render_done_sem);  lottie->render_done_sem  = NULL; }
    if(lottie->render_exit_sem)  { vSemaphoreDelete(lottie->render_exit_sem);  lottie->render_exit_sem  = NULL; }

    tvg_animation_del(lottie->tvg_anim);
    tvg_canvas_destroy(lottie->tvg_canvas);
}

static void anim_exec_cb(void * var, int32_t v)
{
    lv_lottie_t * lottie = var;

    /*Do not render not visible animations.*/
    if(lv_obj_is_visible(var)) {
        lottie_update(lottie, v);
        if(lottie->anim) {
            lottie->last_rendered_time = lottie->anim->act_time;
        }
    }
    else {
        /*Artificially keep the animation on the last rendered frame's time
         *To avoid a jump when the widget becomes visible*/
        if(lottie->anim) {
            lottie->anim->act_time = lottie->last_rendered_time;
        }
    }
}

/**
 * Permanent background render task.  Stack lives in SPIRAM so the LVGL
 * task stack (DRAM) is not consumed by ThorVG rasterisation.
 *
 * Protocol with lottie_update():
 *   1. LVGL task sets render_frame, gives render_start_sem, then blocks on render_done_sem.
 *   2. This task wakes, clears the draw buffer, runs the full ThorVG pipeline,
 *      then gives render_done_sem to unblock the LVGL task.
 *   3. Because the LVGL task is blocked (holding the LVGL mutex), there is no
 *      concurrent flush racing against the ThorVG write.
 */
static void lottie_render_task(void * arg)
{
    lv_lottie_t * lottie = (lv_lottie_t *)arg;
    lv_obj_t * obj = (lv_obj_t *)lottie;
    volatile uint8_t _probe = 0;
    ESP_LOGI(LV_LOTTIE_TAG, "render_task: started, stack_addr=%p (SPIRAM=0x48xxxxxx)",
             (void *)&_probe);

    while(!lottie->destroying) {
        xSemaphoreTake(lottie->render_start_sem, portMAX_DELAY);
        if(lottie->destroying) break;

        int32_t frame = lottie->render_frame;

        /* Clear the draw buffer before each frame: ThorVG only writes
         * non-transparent pixels so stale data must be zeroed first. */
        lv_draw_buf_t * draw_buf = lv_canvas_get_draw_buf(obj);
        if(draw_buf) lv_draw_buf_clear(draw_buf, NULL);

        tvg_animation_set_frame(lottie->tvg_anim, frame);
        tvg_canvas_update(lottie->tvg_canvas);
        tvg_canvas_draw(lottie->tvg_canvas);
        tvg_canvas_sync(lottie->tvg_canvas);

        /* Unblock LVGL task — frame is ready */
        xSemaphoreGive(lottie->render_done_sem);

        /* Monitor headroom; complex animations may need a larger stack */
        uint32_t hw = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        if(hw < LOTTIE_RENDER_MIN_HW) {
            ESP_LOGE(LV_LOTTIE_TAG,
                     "render_task: stack nearly exhausted! hw=%lu B — increase LOTTIE_RENDER_TASK_STACK",
                     (unsigned long)hw);
        }
    }

    ESP_LOGI(LV_LOTTIE_TAG, "render_task: exiting, final hw=%lu B",
             (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    xSemaphoreGive(lottie->render_exit_sem);
    vTaskDelete(NULL);  /* matches xTaskCreate; idle task frees TCB+stack */
}

static void lottie_update(lv_lottie_t * lottie, int32_t v)
{
    /* Guard: calling tvg_canvas_update on an empty (unloaded) canvas corrupts the heap. */
    if(!lottie->src_loaded) return;
    if(lottie->render_failed) return;

    lv_obj_t * obj = (lv_obj_t *) lottie;

    if(!lottie->initial_render_done) {
        /* First render: run inline on the calling stack (the load task's 64 KB DRAM
         * stack).  The render_task is not used here to avoid a cross-task semaphore
         * deadlock while the LVGL lock is held by the load task. */
        lv_draw_buf_t * draw_buf = lv_canvas_get_draw_buf(obj);
        if(draw_buf) {
            lv_draw_buf_clear(draw_buf, NULL);
            lv_image_cache_drop(lv_image_get_src(obj));
        }

        ESP_LOGE(LV_LOTTIE_TAG, "LU1: tvg_animation_set_frame %d (initial, inline)", (int)v);
        tvg_animation_set_frame(lottie->tvg_anim, v);
        ESP_LOGE(LV_LOTTIE_TAG, "LU2: tvg_canvas_update");
        tvg_canvas_update(lottie->tvg_canvas);
        ESP_LOGE(LV_LOTTIE_TAG, "LU3: tvg_canvas_draw");
        tvg_canvas_draw(lottie->tvg_canvas);
        ESP_LOGE(LV_LOTTIE_TAG, "LU4: tvg_canvas_sync");
        tvg_canvas_sync(lottie->tvg_canvas);
        ESP_LOGE(LV_LOTTIE_TAG, "LU5: initial render done — load task stack_hw=%lu B",
                 (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

        lottie->initial_render_done = true;
        lv_obj_invalidate(obj);
        return;
    }

    /* Subsequent frames: dispatch to render_task (96 KB SPIRAM stack).
     *
     * The LVGL task blocks on render_done_sem so LVGL cannot flush the canvas
     * buffer while ThorVG is writing it — no tearing, no race condition.
     * FreeRTOS can still schedule other non-LVGL tasks (WiFi, SD, etc.) during
     * this wait because the block is at the FreeRTOS level, not a busy-wait. */
    lottie->render_frame = v;
    xSemaphoreGive(lottie->render_start_sem);
    xSemaphoreTake(lottie->render_done_sem, portMAX_DELAY);

    lv_image_cache_drop(lv_image_get_src(obj));
    lv_obj_invalidate(obj);
}

#endif /*LV_USE_LOTTIE*/
