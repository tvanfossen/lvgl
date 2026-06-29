/**
 * @file lv_draw_ppa_img.c
 *
 */

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA

#include "../../lv_draw_image_private.h"
#include "../../lv_image_decoder_private.h"

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area);

static void lv_draw_img_ppa_scaled_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                        const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                        const lv_area_t * img_coords, const lv_area_t * clipped_img_area);


void lv_draw_ppa_img(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                     const lv_area_t * coords)
{
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN)
        return;
    bool scaled = (dsc->scale_x != LV_SCALE_NONE) || (dsc->scale_y != LV_SCALE_NONE);
    lv_draw_image_normal_helper(t, dsc, coords,
                                scaled ? lv_draw_img_ppa_scaled_core : lv_draw_img_ppa_core, NULL);
}

void lv_draw_ppa_layer(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                       const lv_area_t * coords)
{
    /* Mirror lv_draw_sw_layer: the source is the layer's draw_buf, not the layer. */
    lv_layer_t * layer_to_draw = (lv_layer_t *)dsc->src;
    if(layer_to_draw->draw_buf == NULL) return;
    if(dsc->opa <= (lv_opa_t)LV_OPA_MIN) return;

    lv_draw_image_dsc_t new_dsc = *dsc;
    new_dsc.src = layer_to_draw->draw_buf;

    bool scaled = (dsc->scale_x != LV_SCALE_NONE) || (dsc->scale_y != LV_SCALE_NONE);
    lv_draw_image_normal_helper(t, &new_dsc, coords,
                                scaled ? lv_draw_img_ppa_scaled_core : lv_draw_img_ppa_core, NULL);
}

static void lv_draw_img_ppa_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                 const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                 const lv_area_t * img_coords, const lv_area_t * clipped_img_area)
{
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;
    const lv_draw_buf_t * decoded = decoder_dsc->decoded;
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;

    lv_area_t rel_clip_area;
    lv_area_copy(&rel_clip_area, clipped_img_area);
    lv_area_move(&rel_clip_area, -img_coords->x1, -img_coords->y1);

    lv_area_t rel_img_coords;
    lv_area_copy(&rel_img_coords, img_coords);
    lv_area_move(&rel_img_coords, -img_coords->x1, -img_coords->y1);

    lv_area_t src_area;
    if(!lv_area_intersect(&src_area, &rel_clip_area, &rel_img_coords))
        return;

    lv_area_t dest_area;
    lv_area_copy(&dest_area, clipped_img_area);
    lv_area_move(&dest_area, -t->target_layer->buf_area.x1, -t->target_layer->buf_area.y1);

    const uint8_t * src_buf = decoded->data;
    lv_color_format_t src_cf = draw_dsc->header.cf;
    lv_color_format_t dest_cf = draw_buf->header.cf;
    uint8_t * dest_buf = draw_buf->data;

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer          = (void *)src_buf,
            .pic_w           = draw_dsc->header.w,
            .pic_h           = draw_dsc->header.h,
            .block_w         = lv_area_get_width(clipped_img_area),
            .block_h         = lv_area_get_height(clipped_img_area),
            .block_offset_x  = src_area.x1,
            .block_offset_y  = src_area.y1,
            .blend_cm        = lv_color_format_to_ppa_blend(src_cf),
        },
        .bg_rgb_swap           = false,
        .bg_byte_swap          = false,
        .bg_alpha_update_mode  = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val      = 0xFF,
        .bg_ck_en              = false,
        .in_fg = {
            .buffer          = (void *)dest_buf,
            .pic_w           = draw_dsc->header.w,
            .pic_h           = draw_dsc->header.h,
            .block_w         = lv_area_get_width(clipped_img_area),
            .block_h         = lv_area_get_height(clipped_img_area),
            .block_offset_x  = src_area.x1,
            .block_offset_y  = src_area.y1,
            .blend_cm        = PPA_BLEND_COLOR_MODE_A8,
        },
        .fg_fix_rgb_val = {
            .r = 0,
            .g = 0,
            .b = 0,
        },
        .fg_rgb_swap           = false,
        .fg_byte_swap          = false,
        .fg_alpha_update_mode  = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val      = 0,
        .fg_ck_en              = false,
        .out = {
            .buffer          = dest_buf,
            .buffer_size     = draw_buf->data_size,
            .pic_w           = draw_buf->header.w,
            .pic_h           = draw_buf->header.h,
            .block_offset_x  = dest_area.x1,
            .block_offset_y  = dest_area.y1,
            .blend_cm        = lv_color_format_to_ppa_blend(dest_cf),
        },
        .mode            = PPA_TRANS_MODE_BLOCKING,
        .user_data       = u,
    };

    esp_err_t ret = ppa_do_blend(u->blend_client, &cfg);
    if(ret != ESP_OK) {
        LV_LOG_WARN("PPA draw_img blend failed: %d", ret);
    }
}

/* Two-pass hardware composite of a scaled image/layer:
 *   pass 1  SRM:    src (RGB565/ARGB8888) --scale--> scratch (ARGB8888)
 *   pass 2  BLEND:  scratch (fg) over dest region (bg) --> dest
 * SRM cannot blend over a background, so the scale and the alpha-composite are
 * separate PPA transactions. Only rotation==0 is handled here (eval enforces). */
static void lv_draw_img_ppa_scaled_core(lv_draw_task_t * t, const lv_draw_image_dsc_t * draw_dsc,
                                        const lv_image_decoder_dsc_t * decoder_dsc, lv_draw_image_sup_t * sup,
                                        const lv_area_t * img_coords, const lv_area_t * clipped_img_area)
{
    LV_UNUSED(sup);
    lv_layer_t * layer       = t->target_layer;
    lv_draw_buf_t * dest      = layer->draw_buf;
    const lv_draw_buf_t * src = decoder_dsc->decoded;
    lv_draw_ppa_unit_t * u    = (lv_draw_ppa_unit_t *)t->draw_unit;

    if(u->buf == NULL) {
        u->buf = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, PPA_SCRATCH_SIZE,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(u->buf == NULL) {
            LV_LOG_WARN("PPA scaled: scratch alloc (%u B) failed", (unsigned)PPA_SCRATCH_SIZE);
            return;
        }
    }

    lv_color_format_t src_cf  = src->header.cf;
    lv_color_format_t dest_cf = dest->header.cf;
    int32_t src_w   = src->header.w;
    int32_t src_h   = src->header.h;
    uint32_t src_stride_px  = src->header.stride / lv_color_format_get_size(src_cf);
    uint32_t dst_stride_px  = dest->header.stride / lv_color_format_get_size(dest_cf);

    /* Full scaled output dimensions (LV_SCALE_NONE == 256 == 1.0x). */
    int32_t scaled_w = (int32_t)(((int64_t)src_w * draw_dsc->scale_x) / LV_SCALE_NONE);
    int32_t scaled_h = (int32_t)(((int64_t)src_h * draw_dsc->scale_y) / LV_SCALE_NONE);
    if(scaled_w <= 0 || scaled_h <= 0) return;
    if((size_t)scaled_w * scaled_h * 4 > PPA_SCRATCH_SIZE) {
        LV_LOG_WARN("PPA scaled: %" LV_PRId32 "x%" LV_PRId32 " exceeds scratch", scaled_w, scaled_h);
        return;
    }

    /* On-screen top-left of the scaled image (honor pivot; rotation==0). */
    lv_area_t tf;
    lv_image_buf_get_transformed_area(&tf, src_w, src_h, 0,
                                      (uint16_t)draw_dsc->scale_x, (uint16_t)draw_dsc->scale_y,
                                      &draw_dsc->pivot);
    int32_t tf_x1 = img_coords->x1 + tf.x1;
    int32_t tf_y1 = img_coords->y1 + tf.y1;

    /* ---- Pass 1: SRM scale src -> scratch (ARGB8888) ---- */
    lv_draw_buf_flush_cache(src, NULL);   /* CPU-rendered source -> writeback for PPA DMA */

    ppa_srm_oper_config_t srm = {
        .in = {
            .buffer         = (void *)src->data,
            .pic_w          = src_stride_px,
            .pic_h          = src_h,
            .block_w        = src_w,
            .block_h        = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm         = lv_color_format_to_ppa_srm(src_cf),
        },
        .out = {
            .buffer         = u->buf,
            .buffer_size    = PPA_SCRATCH_SIZE,
            .pic_w          = scaled_w,
            .pic_h          = scaled_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm         = PPA_SRM_COLOR_MODE_ARGB8888,
        },
        .rotation_angle   = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x          = (float)draw_dsc->scale_x / (float)LV_SCALE_NONE,
        .scale_y          = (float)draw_dsc->scale_y / (float)LV_SCALE_NONE,
        .mirror_x         = false,
        .mirror_y         = false,
        .rgb_swap         = false,
        .byte_swap        = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode             = PPA_TRANS_MODE_BLOCKING,
        .user_data        = u,
    };
    esp_err_t ret = ppa_do_scale_rotate_mirror(u->srm_client, &srm);
    if(ret != ESP_OK) {
        LV_LOG_WARN("PPA SRM scale failed: %d (%" LV_PRId32 "x%" LV_PRId32 " ->%" LV_PRId32 "x%" LV_PRId32 ")",
                    ret, src_w, src_h, scaled_w, scaled_h);
        return;
    }

    /* ---- Pass 2: blend scratch (fg, ARGB8888) over dest (bg) ---- */
    lv_area_t clip = *clipped_img_area;
    int32_t fg_off_x = clip.x1 - tf_x1;
    int32_t fg_off_y = clip.y1 - tf_y1;
    if(fg_off_x < 0) fg_off_x = 0;
    if(fg_off_y < 0) fg_off_y = 0;
    int32_t blk_w = lv_area_get_width(&clip);
    int32_t blk_h = lv_area_get_height(&clip);
    if(fg_off_x + blk_w > scaled_w) blk_w = scaled_w - fg_off_x;
    if(fg_off_y + blk_h > scaled_h) blk_h = scaled_h - fg_off_y;
    if(blk_w <= 0 || blk_h <= 0) return;

    int32_t dst_off_x = clip.x1 - layer->buf_area.x1;
    int32_t dst_off_y = clip.y1 - layer->buf_area.y1;

    lv_draw_buf_flush_cache(dest, &clip);   /* bg pixels -> writeback for PPA DMA read */

    ppa_blend_oper_config_t bl = {
        .in_bg = {
            .buffer         = dest->data,
            .pic_w          = dst_stride_px,
            .pic_h          = dest->header.h,
            .block_w        = blk_w,
            .block_h        = blk_h,
            .block_offset_x = dst_off_x,
            .block_offset_y = dst_off_y,
            .blend_cm       = lv_color_format_to_ppa_blend(dest_cf),
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val     = 0xFF,
        .in_fg = {
            .buffer         = u->buf,
            .pic_w          = scaled_w,
            .pic_h          = scaled_h,
            .block_w        = blk_w,
            .block_h        = blk_h,
            .block_offset_x = fg_off_x,
            .block_offset_y = fg_off_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_ARGB8888,
        },
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .out = {
            .buffer         = dest->data,
            .buffer_size    = dest->data_size,
            .pic_w          = dst_stride_px,
            .pic_h          = dest->header.h,
            .block_offset_x = dst_off_x,
            .block_offset_y = dst_off_y,
            .blend_cm       = lv_color_format_to_ppa_blend(dest_cf),
        },
        .mode      = PPA_TRANS_MODE_BLOCKING,
        .user_data = u,
    };
    ret = ppa_do_blend(u->blend_client, &bl);
    if(ret != ESP_OK) {
        LV_LOG_WARN("PPA scaled blend failed: %d", ret);
        return;
    }

    /* Invalidate the cache for the ACTUAL written region. When upscaling, this
     * region is larger than the caller's t->area-based invalidation, so the
     * extra scaled pixels (written by PPA DMA) would otherwise be read stale by
     * the CPU/flush → torn/garbage output. */
    lv_draw_buf_invalidate_cache(dest, clipped_img_area);
}

#endif /* LV_USE_PPA */
