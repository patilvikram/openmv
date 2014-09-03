#include <ff.h>
#include <stdio.h>
#include "xalloc.h"
#include "imlib.h"
#include <arm_math.h>

/* Viola-Jones face detector implementation
 * Original Author:   Francesco Comaschi (f.comaschi@tue.nl)
 */
static int evalWeakClassifier(struct cascade *cascade, int std, int p_offset, int tree_index, int w_index, int r_index )
{
    int i, sumw=0;
    struct rectangle tr;
    struct integral_image *sum = cascade->sum;

    /* the node threshold is multiplied by the standard deviation of the image */
    int t = cascade->tree_thresh_array[tree_index] * std;

    for (i=0; i<cascade->num_rectangles_array[tree_index]; i++) {
        tr.x = cascade->rectangles_array[r_index + (i<<2) + 0];
        tr.y = cascade->rectangles_array[r_index + (i<<2) + 1];
        tr.w = cascade->rectangles_array[r_index + (i<<2) + 2];
        tr.h = cascade->rectangles_array[r_index + (i<<2) + 3];

        sumw += ( sum->data[sum->w*tr.y + tr.x + p_offset]
                - sum->data[sum->w*tr.y + tr.x + tr.w + p_offset]
                - sum->data[sum->w*(tr.y  + tr.h) + tr.x + p_offset]
                + sum->data[sum->w*(tr.y  + tr.h) + tr.x  + tr.w + p_offset])
                * (cascade->weights_array[w_index + i]<<12);
    }

    if (sumw >= t) {
        return cascade->alpha2_array[tree_index];
    }

    return cascade->alpha1_array[tree_index];
}

static int runCascadeClassifier(struct cascade* cascade, struct point pt, int start_stage)
{
    int w_index = 0;
    int r_index = 0;
    int tree_index = 0;

    uint32_t sumsq = 0;
    int32_t std, mean;

    for (int y=pt.y; y<cascade->window.h; y++) {
          int offset = y*cascade->img->w;
      for (int x=pt.x; x<cascade->window.w; x+=2) {
          uint32_t v0 = __PKHBT(cascade->img->pixels[offset+x+0],
                                cascade->img->pixels[offset+x+1], 16);
          sumsq = __SMLAD(v0, v0, sumsq);
      }
    }

    /* Image normalization */
    i_image_t *sum = cascade->sum;
    int win_w = cascade->window.w - 1;
    int win_h = cascade->window.h - 1;
    int p_offset = pt.y * (sum->w) + pt.x;

    mean = sum->data[p_offset]
         - sum->data[win_w + p_offset]
         - sum->data[sum->w * win_h + p_offset]
         + sum->data[sum->w * win_h + win_w + p_offset];

    std = fast_sqrtf(sumsq * cascade->window.w * cascade->window.h - mean * mean);

    for (int i=start_stage; i<cascade->n_stages; i++) {
        int stage_sum = 0;
        for (int j=0; j<cascade->stages_array[i]; j++, tree_index++) {
            /* send the shifted window to a haar filter */
              stage_sum += evalWeakClassifier(cascade, std, p_offset, tree_index, w_index, r_index);
              w_index+=cascade->num_rectangles_array[tree_index];
              r_index+=4 * cascade->num_rectangles_array[tree_index];
        }

        /* If the sum is below the stage threshold, no faces are detected */
        if (stage_sum < 0.4*cascade->stages_thresh_array[i]) {
            return -i;
        }
    }

    return 1;
}

static void ScaleImageInvoker(struct cascade *cascade, float factor, int sum_row, int sum_col, struct array *vec)
{
    int result;
    int x, y, x2, y2;

    struct point p;
    struct size win_size;

    win_size.w =  fast_roundf(cascade->window.w*factor);
    win_size.h =  fast_roundf(cascade->window.h*factor);

    /* When filter window shifts to image boarder, some margin need to be kept */
    y2 = sum_row - win_size.h;
    x2 = sum_col - win_size.w;

    /* Shift the filter window over the image. */
    for (x=0; x<=x2; x+=cascade->step) {
        for (y=0; y<=y2; y+=cascade->step) {
            p.x = x;
            p.y = y;

            result = runCascadeClassifier(cascade, p, 0);

            /* If a face is detected, record the coordinates of the filter window */
            if (result > 0) {
                struct rectangle *r = xalloc(sizeof(struct rectangle));
                r->x = fast_roundf(x*factor);
                r->y = fast_roundf(y*factor);
                r->w = win_size.w;
                r->h = win_size.h;
                array_push_back(vec, r);
            }
        }
    }
}

#include "framebuffer.h"
struct array *imlib_detect_objects(struct image *image, struct cascade *cascade)
{
    /* scaling factor */
    float factor;

    struct array *objects;

    struct image img;
    struct integral_image sum;

    /* allocate buffer for scaled image */
    img.w = image->w;
    img.h = image->h;
    img.bpp = image->bpp;
    //sum.data   = xalloc(image->w *image->h*sizeof(*sum.data));
    /* use the second half of the framebuffer */
    img.pixels = fb->pixels+(fb->w * fb->h);

    /* allocate buffer for integral image */
    sum.w = image->w;
    sum.h = image->h;
    //sum.data   = xalloc(image->w *image->h*sizeof(*sum.data));
    sum.data = (uint32_t*) (fb->pixels+(fb->w * fb->h * 2));

    /* allocate the detections array */
    array_alloc(&objects, xfree);

    /* set cascade image pointer */
    cascade->img = &img;

    /* sets cascade integral image */
    cascade->sum = &sum;

    /* iterate over the image pyramid */
    for(factor=1.0f; ; factor*=cascade->scale_factor) {
        /* size of the scaled image */
        struct size sz = {
            (image->w*factor),
            (image->h*factor)
        };

        /* if scaled image is smaller than the original detection window, break */
        if ((sz.w  - cascade->window.w)  <= 0 ||
            (sz.h - cascade->window.h) <= 0) {
            break;
        }

        /* Set the width and height of the images */
        img.w = sz.w;
        img.h = sz.h;

        sum.w = sz.w;
        sum.h = sz.h;

        /* downsample using nearest neighbor */
        imlib_scale(image, &img, INTERP_NEAREST);

        /* compute a new integral image */
        imlib_integral_image(&img, &sum);

        /* process the current scale with the cascaded fitler. */
        ScaleImageInvoker(cascade, factor, sum.h, sum.w, objects);
    }

    //xfree(sum.data);

    objects = rectangle_merge(objects);
    return objects;
}

int imlib_load_cascade(struct cascade *cascade, const char *path)
{
    int i;
    UINT n_out;

    FIL fp;
    FRESULT res=FR_OK;

    res = f_open(&fp, path, FA_READ|FA_OPEN_EXISTING);
    if (res != FR_OK) {
        return res;
    }

    /* read detection window size */
    res = f_read(&fp, &cascade->window, sizeof(cascade->window), &n_out);
    if (res != FR_OK || n_out != sizeof(cascade->window)) {
        goto error;
    }

    /* read num stages */
    res = f_read(&fp, &cascade->n_stages, sizeof(cascade->n_stages), &n_out);
    if (res != FR_OK || n_out != sizeof(cascade->n_stages)) {
        goto error;
    }

    cascade->stages_array = xalloc (sizeof(*cascade->stages_array) * cascade->n_stages);
    cascade->stages_thresh_array = xalloc (sizeof(*cascade->stages_thresh_array) * cascade->n_stages);

    if (cascade->stages_array == NULL ||
        cascade->stages_thresh_array == NULL) {
        res = 20;
        goto error;
    }

    /* read num features in each stages */
    res = f_read(&fp, cascade->stages_array, sizeof(uint8_t) * cascade->n_stages, &n_out);
    if (res != FR_OK || n_out != sizeof(uint8_t) * cascade->n_stages) {
        goto error;
    }

    /* sum num of features in each stages*/
    for (i=0, cascade->n_features=0; i<cascade->n_stages; i++) {
        cascade->n_features += cascade->stages_array[i];
    }

    /* alloc features thresh array, alpha1, alpha 2,rects weights and rects*/
    cascade->tree_thresh_array = xalloc (sizeof(*cascade->tree_thresh_array) * cascade->n_features);
    cascade->alpha1_array = xalloc (sizeof(*cascade->alpha1_array) * cascade->n_features);
    cascade->alpha2_array = xalloc (sizeof(*cascade->alpha2_array) * cascade->n_features);
    cascade->num_rectangles_array = xalloc (sizeof(*cascade->num_rectangles_array) * cascade->n_features);

    if (cascade->tree_thresh_array == NULL ||
        cascade->alpha1_array   == NULL ||
        cascade->alpha2_array   == NULL ||
        cascade->num_rectangles_array == NULL) {
        res = 20;
        goto error;
    }

    /* read stages thresholds */
    res = f_read(&fp, cascade->stages_thresh_array, sizeof(int16_t)*cascade->n_stages, &n_out);
    if (res != FR_OK || n_out != sizeof(int16_t)*cascade->n_stages) {
        goto error;
    }

    /* read features thresholds */
    res = f_read(&fp, cascade->tree_thresh_array, sizeof(*cascade->tree_thresh_array)*cascade->n_features, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->tree_thresh_array)*cascade->n_features) {
        goto error;
    }

    /* read alpha 1 */
    res = f_read(&fp, cascade->alpha1_array, sizeof(*cascade->alpha1_array)*cascade->n_features, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->alpha1_array)*cascade->n_features) {
        goto error;
    }

    /* read alpha 2 */
    res = f_read(&fp, cascade->alpha2_array, sizeof(*cascade->alpha2_array)*cascade->n_features, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->alpha2_array)*cascade->n_features) {
        goto error;
    }

    /* read num rectangles per feature*/
    res = f_read(&fp, cascade->num_rectangles_array, sizeof(*cascade->num_rectangles_array)*cascade->n_features, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->num_rectangles_array)*cascade->n_features) {
        goto error;
    }

    /* sum num of recatngles per feature*/
    for (i=0, cascade->n_rectangles=0; i<cascade->n_features; i++) {
        cascade->n_rectangles += cascade->num_rectangles_array[i];
    }

    cascade->weights_array = xalloc (sizeof(*cascade->weights_array) * cascade->n_rectangles);
    cascade->rectangles_array = xalloc (sizeof(*cascade->rectangles_array) * cascade->n_rectangles * 4);

    if (cascade->weights_array  == NULL ||
        cascade->rectangles_array == NULL) {
        res = 20;
        goto error;
    }

    /* read rectangles weights */
    res =f_read(&fp, cascade->weights_array, sizeof(*cascade->weights_array)*cascade->n_rectangles, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->weights_array)*cascade->n_rectangles) {
        goto error;
    }

    /* read rectangles num rectangles * 4 points */
    res = f_read(&fp, cascade->rectangles_array, sizeof(*cascade->rectangles_array)*cascade->n_rectangles *4, &n_out);
    if (res != FR_OK || n_out != sizeof(*cascade->rectangles_array)*cascade->n_rectangles *4) {
        goto error;
    }

error:
    f_close(&fp);
    return res;
}


