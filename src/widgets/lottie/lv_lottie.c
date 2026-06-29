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
#include "freertos/queue.h"
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

/** Stack for the shared ThorVG render task (singleton — all widgets share one task).
 *  ESP32-P4 FreeRTOS port rejects SPIRAM task stacks (portVALID_STACK_MEM assert),
 *  so the stack lives in static BSS (internal DRAM) — fixed cost regardless of
 *  instance count.
 *  32 KB = LV_DRAW_THREAD_STACK_SIZE minimum for ThorVG (lv_conf_internal.h:4954).
 *  + 8 KB overhead/margin = 40 KB total. */
#define LOTTIE_RENDER_TASK_STACK    (40 * 1024)

/** Warn when render task stack headroom falls below this threshold. */
#define LOTTIE_RENDER_MIN_HW        (8 * 1024)

/**********************
 *      TYPEDEFS
 **********************/

/** Job dispatched by lottie_update() to the shared render task via s_render_queue. */
typedef struct {
    lv_lottie_t * lottie;
    int32_t       frame;
} lottie_job_t;

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

/* Shared render task — created once on first Lottie widget, runs for device lifetime.
 * Stack and TCB in static BSS (internal DRAM); no heap allocation needed.
 * When no widgets exist the task blocks on the empty queue at zero CPU cost. */
static TaskHandle_t  s_render_task  = NULL;
static StaticTask_t  s_render_tcb;
static StackType_t   s_render_stack[LOTTIE_RENDER_TASK_STACK / sizeof(StackType_t)];
static QueueHandle_t s_render_queue = NULL;
static int           s_lottie_count = 0; /* live widget count — for logging only */

/* Serializes ALL ThorVG engine operations (parse + render) across tasks so the
 * off-LVGL-lock parse (lv_lottie_parse_src_data) cannot race the render task on
 * ThorVG global state. Separate from the LVGL lock — neither task holding this
 * holds the LVGL lock, so the parse no longer freezes rendering. */
static SemaphoreHandle_t s_tvg_lock = NULL;
static inline void tvg_lock(void)   { if(s_tvg_lock) xSemaphoreTake(s_tvg_lock, portMAX_DELAY); }
static inline void tvg_unlock(void) { if(s_tvg_lock) xSemaphoreGive(s_tvg_lock); }

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

/* Parse-only: the heavy ThorVG load, runnable WITHOUT the LVGL lock. Safe
 * because lv_malloc is thread-safe and the render task does not touch this
 * widget until lv_lottie_finalize_src sets src_loaded; the ThorVG mutex
 * serializes the load vs the render task's global engine state. */
void lv_lottie_parse_src_data(lv_obj_t * obj, const void * src, size_t src_size)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    tvg_lock();
    tvg_picture_load_data(lottie->tvg_paint, src, src_size, "lottie", true);
    tvg_unlock();
}

/* Attach the already-parsed animation. Caller MUST hold the LVGL lock (touches
 * the canvas + does the inline first render). Fast vs the parse. */
void lv_lottie_finalize_src(lv_obj_t * obj)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    lottie->src_loaded = true; /* allow lottie_update to call ThorVG */

    lv_draw_buf_t * canvas_draw_buf = lv_canvas_get_draw_buf(obj);
    float f_total = 0;
    tvg_lock();
    if(canvas_draw_buf) {
        tvg_picture_set_size(lottie->tvg_paint, canvas_draw_buf->header.w, canvas_draw_buf->header.h);
    }
    tvg_animation_get_total_frame(lottie->tvg_anim, &f_total);
    tvg_unlock();

    /* Skip the junk frame 0 that some Lottie exports render as noise: start
     * playback (and the inline first render) at frame 1. */
    int32_t start_frame = (f_total > 1) ? 1 : 0;
    if(lottie->anim) {
        lv_anim_set_duration(lottie->anim, (int32_t)f_total * 1000 / 60); /*60 FPS*/
        lottie->anim->act_time = 0;
        lottie->anim->start_value = start_frame;
        lottie->anim->end_value = (int32_t)f_total;
        lottie->anim->reverse_play_in_progress = false;
    }
    lottie_update(lottie, start_frame);   /* inline first render (takes the ThorVG mutex itself) */
}

void lv_lottie_set_src_data(lv_obj_t * obj, const void * src, size_t src_size)
{
    lv_lottie_parse_src_data(obj, src, src_size);
    lv_lottie_finalize_src(obj);
}

void lv_lottie_set_src_file(lv_obj_t * obj, const char * src)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    ESP_LOGD(LV_LOTTIE_TAG, "CP1: tvg_picture_load start src=%s", src);
    Tvg_Result load_res = tvg_picture_load(lottie->tvg_paint, src);
    ESP_LOGD(LV_LOTTIE_TAG, "CP2: tvg_picture_load done res=%d", (int)load_res);
    if(load_res == TVG_RESULT_SUCCESS) {
        lottie->src_loaded = true; /* allow lottie_update to call ThorVG */
    }
    lv_draw_buf_t * canvas_draw_buf = lv_canvas_get_draw_buf(obj);
    if(canvas_draw_buf) {
        ESP_LOGD(LV_LOTTIE_TAG, "CP3: tvg_picture_set_size %dx%d",
                 (int)canvas_draw_buf->header.w, (int)canvas_draw_buf->header.h);
        tvg_picture_set_size(lottie->tvg_paint, canvas_draw_buf->header.w, canvas_draw_buf->header.h);
        ESP_LOGD(LV_LOTTIE_TAG, "CP4: tvg_picture_set_size done");
    }

    float f_total = 0;
    ESP_LOGD(LV_LOTTIE_TAG, "CP5: tvg_animation_get_total_frame start");
    tvg_animation_get_total_frame(lottie->tvg_anim, &f_total);
    ESP_LOGD(LV_LOTTIE_TAG, "CP6: tvg_animation_get_total_frame done f_total=%d", (int)f_total);
    int32_t start_frame = (f_total > 1) ? 1 : 0;   /* skip junk frame 0 */
    if(lottie->anim) {
        lv_anim_set_duration(lottie->anim, (int32_t)f_total * 1000 / 60); /*60 FPS*/
        lottie->anim->act_time = 0;
        lottie->anim->start_value = start_frame;
        lottie->anim->end_value = (int32_t)f_total;
        lottie->anim->reverse_play_in_progress = false;
    }
    ESP_LOGD(LV_LOTTIE_TAG, "CP7: calling lottie_update (initial, inline on load-task stack)");
    lottie_update(lottie, start_frame);   /*Render immediately*/
    ESP_LOGD(LV_LOTTIE_TAG, "CP8: lottie_update done");
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

void lv_lottie_pause(lv_obj_t * obj)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    if(lottie->anim) {
        lv_anim_delete(obj, NULL);
        lottie->anim = NULL;
    }
}

void lv_lottie_resume(lv_obj_t * obj, uint32_t target_fps)
{
    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    if(!lottie->src_loaded || lottie->anim) return;

    float f_total;
    tvg_animation_get_total_frame(lottie->tvg_anim, &f_total);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_exec_cb);
    lv_anim_set_var(&a, obj);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_duration(&a, (int32_t)f_total * 1000 / target_fps);
    a.start_value = (f_total > 1) ? 1 : 0;   /* skip junk frame 0 */
    a.end_value = (int32_t)f_total;
    lottie->anim = lv_anim_start(&a);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_lottie_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj)
{
    LV_UNUSED(class_p);
    LV_TRACE_OBJ_CREATE("begin");
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: ENTRY obj=%p count=%d", obj, s_lottie_count);

    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: set_size done");

    lv_lottie_t * lottie = (lv_lottie_t *)obj;
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: tvg_animation_new");
    lottie->tvg_anim = tvg_animation_new();
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: tvg_animation_get_picture");
    lottie->tvg_paint = tvg_animation_get_picture(lottie->tvg_anim);
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: tvg_swcanvas_create");
    lottie->tvg_canvas = tvg_swcanvas_create();

    lottie->src_loaded          = false;
    lottie->initial_render_done = false;
    lottie->last_frame          = -1;
    lottie->destroying          = false;
    lottie->render_failed       = false;
    lottie->render_done_sem     = NULL;
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: fields init done, internal_free=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    lottie->render_done_sem = xSemaphoreCreateBinary();
    ESP_LOGD(LV_LOTTIE_TAG, "ctor: done_sem=%p", lottie->render_done_sem);

    if(!lottie->render_done_sem) {
        ESP_LOGE(LV_LOTTIE_TAG, "lottie: done_sem creation failed");
        lottie->render_failed = true;
    }
    else if(s_render_task == NULL) {
        /* First instance: create shared queue and render task.
         * Task uses static BSS stack + TCB (internal DRAM).
         * ESP32-P4 FreeRTOS port aborts on SPIRAM stacks (portVALID_STACK_MEM).
         * Static allocation = fixed 40 KB DRAM cost for ALL instances combined. */
        if(!s_tvg_lock) s_tvg_lock = xSemaphoreCreateMutex();
        s_render_queue = xQueueCreate(1, sizeof(lottie_job_t));
        if(!s_render_queue) {
            ESP_LOGE(LV_LOTTIE_TAG, "lottie: render queue creation failed");
            lottie->render_failed = true;
        }
        else {
            s_render_task = xTaskCreateStatic(
                lottie_render_task, "lottie_rndr",
                LOTTIE_RENDER_TASK_STACK / sizeof(StackType_t),
                NULL, 4, s_render_stack, &s_render_tcb);

            if(!s_render_task) {
                ESP_LOGE(LV_LOTTIE_TAG, "lottie: shared render task creation failed");
                vQueueDelete(s_render_queue);
                s_render_queue = NULL;
                lottie->render_failed = true;
            }
            else {
                ESP_LOGI(LV_LOTTIE_TAG,
                         "lottie: shared render task started (stack=%u B DRAM, DRAM free=%lu B)",
                         LOTTIE_RENDER_TASK_STACK,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
        }
    }
    else {
        ESP_LOGI(LV_LOTTIE_TAG, "lottie: reusing shared render task (count=%d)", s_lottie_count);
    }

    if(!lottie->render_failed) {
        s_lottie_count++;
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

    /* Mark widget as dead so render_task skips any queued job for it.
     * Safe: LVGL is single-threaded — no lottie_update() can be in flight
     * when the destructor runs (they run on the same LVGL task). */
    lottie->destroying = true;

    if(!lottie->render_failed) {
        s_lottie_count--;
        ESP_LOGI(LV_LOTTIE_TAG, "lottie: destroyed, count now=%d", s_lottie_count);
    }

    if(lottie->render_done_sem) {
        vSemaphoreDelete(lottie->render_done_sem);
        lottie->render_done_sem = NULL;
    }

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
 * Shared background render task (singleton — one task for all Lottie widgets).
 * Stack: 40 KB static DRAM BSS. ESP32-P4 FreeRTOS port rejects SPIRAM stacks.
 *
 * Dispatch protocol with lottie_update():
 *   1. LVGL task (single-threaded) sends job {lottie, frame} to s_render_queue.
 *   2. LVGL task blocks on per-widget lottie->render_done_sem.
 *   3. This task dequeues the job, clears buffer, runs ThorVG pipeline,
 *      then gives render_done_sem to unblock the LVGL task.
 *   4. The LVGL task is blocked during rendering — no concurrent canvas flush.
 *
 * Multiple widgets serialize naturally: LVGL blocks per-widget until each
 * frame completes before dispatching the next widget's frame.
 */
static void lottie_render_task(void * arg)
{
    (void)arg;
    volatile uint8_t _probe = 0;
    ESP_LOGI(LV_LOTTIE_TAG, "render_task: started stack_addr=%p DRAM free=%lu B",
             (void *)&_probe,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    lottie_job_t job;
    while(1) {
        xQueueReceive(s_render_queue, &job, portMAX_DELAY);

        lv_lottie_t * lottie = job.lottie;

        /* Widget was destroyed between dispatch and dequeue (belt-and-suspenders) */
        if(lottie->destroying) {
            xSemaphoreGive(lottie->render_done_sem);
            continue;
        }

        /* Clear draw buffer before each frame: ThorVG only writes non-transparent
         * pixels so stale data must be zeroed first. */
        lv_draw_buf_t * draw_buf = lv_canvas_get_draw_buf((lv_obj_t *)lottie);
        if(draw_buf) lv_draw_buf_clear(draw_buf, NULL);

        uint32_t t0 = lv_tick_get();
        tvg_lock();   /* serialize vs the off-lock parse */
        tvg_animation_set_frame(lottie->tvg_anim, job.frame);
        tvg_canvas_update(lottie->tvg_canvas);
        tvg_canvas_draw(lottie->tvg_canvas);
        tvg_canvas_sync(lottie->tvg_canvas);
        tvg_unlock();
        uint32_t render_ms = lv_tick_elaps(t0);

        xSemaphoreGive(lottie->render_done_sem);

        /* Log every 10th frame at DEBUG to avoid log flood */
        if(job.frame % 10 == 0) {
            ESP_LOGD(LV_LOTTIE_TAG, "render: frame=%d %lums", (int)job.frame, (unsigned long)render_ms);
        }

        uint32_t hw = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        if(hw < LOTTIE_RENDER_MIN_HW) {
            ESP_LOGE(LV_LOTTIE_TAG,
                     "render_task: stack nearly exhausted! hw=%lu B — increase LOTTIE_RENDER_TASK_STACK",
                     (unsigned long)hw);
        }
    }
}

static void lottie_update(lv_lottie_t * lottie, int32_t v)
{
    /* Guard: calling tvg_canvas_update on an empty (unloaded) canvas corrupts the heap. */
    if(!lottie->src_loaded) return;
    if(lottie->render_failed) return;

    /* Frame dedup: the LVGL anim engine calls this on every refresh tick (~60Hz),
     * but the integer frame only changes target_fps times/sec. Re-rendering an
     * identical frame blocks the LVGL task for ~render_ms (full ThorVG composite)
     * with no visual change and pins the CPU at 100%, starving transitions. Skip
     * when the frame is unchanged. The first render always runs (last_frame=-1). */
    if(v == lottie->last_frame) return;
    lottie->last_frame = v;

    lv_obj_t * obj = (lv_obj_t *) lottie;

    if(!lottie->initial_render_done) {
        /* First render: run inline on the calling stack (load task's DRAM stack).
         * The shared render_task is not used here because the load task holds the
         * LVGL lock — dispatching to another task would deadlock. */
        lv_draw_buf_t * draw_buf = lv_canvas_get_draw_buf(obj);
        if(draw_buf) {
            lv_draw_buf_clear(draw_buf, NULL);
            lv_image_cache_drop(lv_image_get_src(obj));
        }

        ESP_LOGD(LV_LOTTIE_TAG, "LU1: tvg_animation_set_frame %d (initial, inline)", (int)v);
        tvg_lock();   /* serialize vs the off-lock parse / render task */
        tvg_animation_set_frame(lottie->tvg_anim, v);
        ESP_LOGD(LV_LOTTIE_TAG, "LU2: tvg_canvas_update");
        tvg_canvas_update(lottie->tvg_canvas);
        ESP_LOGD(LV_LOTTIE_TAG, "LU3: tvg_canvas_draw");
        tvg_canvas_draw(lottie->tvg_canvas);
        ESP_LOGD(LV_LOTTIE_TAG, "LU4: tvg_canvas_sync");
        tvg_canvas_sync(lottie->tvg_canvas);
        tvg_unlock();
        ESP_LOGD(LV_LOTTIE_TAG, "LU5: initial render done — load task stack_hw=%lu B",
                 (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

        lottie->initial_render_done = true;
        lv_obj_invalidate(obj);
        return;
    }

    /* Subsequent frames: dispatch to shared render task.
     *
     * LVGL task blocks on render_done_sem — ThorVG cannot race with display flush.
     * FreeRTOS schedules other tasks (WiFi, SD, etc.) during this wait.
     *
     * Bounded timeout (500ms): if render_task can't complete (e.g. DRAM starvation
     * from concurrent TLS handshakes), skip this frame instead of hanging the
     * entire LVGL task forever. */
    lottie_job_t job = {lottie, v};
    if(xQueueSend(s_render_queue, &job, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(LV_LOTTIE_TAG, "render queue full, skipping frame %d", (int)v);
        return;
    }
    if(xSemaphoreTake(lottie->render_done_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(LV_LOTTIE_TAG, "render timeout frame=%d — skipping (DRAM pressure?)", (int)v);
        return;
    }

    lv_image_cache_drop(lv_image_get_src(obj));
    lv_obj_invalidate(obj);
}

#endif /*LV_USE_LOTTIE*/
