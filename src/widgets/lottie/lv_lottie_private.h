/**
 * @file lv_lottie_private.h
 *
 */

#ifndef LV_LOTTIE_PRIVATE_H
#define LV_LOTTIE_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../lv_conf_internal.h"
#if LV_USE_LOTTIE

#include "lv_lottie.h"
#include "../canvas/lv_canvas_private.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

#if LV_USE_THORVG_EXTERNAL
#include <thorvg_capi.h>
#else
#include "../../libs/thorvg/thorvg_capi.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    lv_canvas_t canvas;
    Tvg_Paint * tvg_paint;
    Tvg_Canvas * tvg_canvas;
    Tvg_Animation * tvg_anim;
    lv_anim_t * anim;
    int32_t last_rendered_time;
    int32_t last_frame;           /**< Last integer frame actually rendered; -1 = none yet (frame-dedup) */

    /* --- shared render task sync --- */
    bool src_loaded;              /**< True once source loaded; guards lottie_update */
    bool initial_render_done;     /**< First render inline on load-task stack; later via shared render_task */
    bool destroying;              /**< Tells render_task to skip jobs from this widget */
    bool render_failed;           /**< Shared task creation failed; widget stays blank */
    SemaphoreHandle_t render_done_sem;  /**< Binary: shared render_task signals this widget on frame complete */
} lv_lottie_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#endif /*LV_LOTTIE_H*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_LOTTIE_PRIVATE_H*/
