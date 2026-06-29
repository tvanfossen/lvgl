/**
 * @file lv_lottie.h
 *
 */

#ifndef LV_LOTTIE_H
#define LV_LOTTIE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../../misc/lv_types.h"
#if LV_USE_LOTTIE

/*Testing of dependencies*/
#if LV_USE_CANVAS == 0
#error "lv_lottie: lv_canvas is required. Enable it in lv_conf.h (LV_USE_CANVAS 1)"
#endif

#if LV_USE_THORVG == 0
#error "lv_lottie: ThorVG is required. Enable it in lv_conf.h (LV_USE_THORVG_INTERNAL/EXTERNAL 1)"
#endif

#include "../../draw/lv_draw_buf.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create a lottie animation
 * @param parent    pointer to the parent widget
 * @return          pointer to the created Lottie animation widget
 */
lv_obj_t  * lv_lottie_create(lv_obj_t * parent);

/**
 * Set a buffer for the animation. It also defines the size of the animation
 * @param obj       pointer to a lottie widget
 * @param w         width of the animation and buffer
 * @param h         height of the animation and buffer
 * @param buf       a static buffer with `width x height x 4` byte size
 */
void lv_lottie_set_buffer(lv_obj_t * obj, int32_t w, int32_t h, void * buf);

/**
 * Set a draw buffer for the animation. It also defines the size of the animation
 * @param obj       pointer to a lottie widget
 * @param draw_buf  an initialized draw buffer with ARGB8888 color format
 */
void lv_lottie_set_draw_buf(lv_obj_t * obj, lv_draw_buf_t * draw_buf);

/**
 * Set the source for the animation as an array
 * @param obj       pointer to a lottie widget
 * @param src       the lottie animation converted to an nul terminated array
 * @param src_size  size of the source array in bytes
 */
void lv_lottie_set_src_data(lv_obj_t * obj, const void * src, size_t src_size);

/**
 * Parse the animation data WITHOUT the LVGL lock (the heavy ThorVG load). Pair
 * with lv_lottie_finalize_src (which must be called under the LVGL lock). This
 * keeps the ~hundreds-of-ms parse off the LVGL lock so it doesn't freeze
 * rendering. A ThorVG mutex serializes the parse vs the render task internally.
 * @param obj       pointer to a lottie widget
 * @param src       the lottie animation as a nul-terminated array
 * @param src_size  size of the source array in bytes
 */
void lv_lottie_parse_src_data(lv_obj_t * obj, const void * src, size_t src_size);

/**
 * Attach a previously lv_lottie_parse_src_data'd animation. MUST be called under
 * the LVGL lock (touches the canvas + does the inline first render).
 * @param obj   pointer to a lottie widget
 */
void lv_lottie_finalize_src(lv_obj_t * obj);

/**
 * Set the source for the animation as a path.
 * Lottie doesn't use LVGL's File System API.
 * @param obj       pointer to a lottie widget
 * @param src       path to a json file, e.g. "path/to/file.json"
 */
void lv_lottie_set_src_file(lv_obj_t * obj, const char * src);

/**
 * Get the LVGL animation which controls the lottie animation
 * @param obj       pointer to a lottie widget
 * @return          the LVGL animation
 */
lv_anim_t * lv_lottie_get_anim(lv_obj_t * obj);

/**
 * Check whether the background render task failed to start (e.g. insufficient PSRAM).
 * When true the widget will remain blank; callers should substitute a text label.
 * @param obj   pointer to a lottie widget
 * @return      true if the render task could not be created
 */
bool lv_lottie_render_failed(lv_obj_t * obj);

/**
 * Pause the Lottie animation (delete lv_anim, keep ThorVG state).
 * Used to cache the widget across state transitions without re-parsing JSON.
 * @param obj   pointer to a lottie widget
 */
void lv_lottie_pause(lv_obj_t * obj);

/**
 * Resume a paused Lottie animation by creating a new lv_anim.
 * No-op if src is not loaded or animation is already running.
 * @param obj        pointer to a lottie widget
 * @param target_fps desired frames per second
 */
void lv_lottie_resume(lv_obj_t * obj, uint32_t target_fps);

/**********************
 * GLOBAL VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_LOTTIE*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_LOTTIE_H*/
