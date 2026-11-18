#include "regiondec.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include <jerror.h>
#include <jpeglib.h>

struct rdec_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
    int     code;
};

static void
rdec_error_exit(j_common_ptr cinfo)
{
    struct rdec_error_mgr *err = (struct rdec_error_mgr *)cinfo->err;

    switch (err->pub.msg_code) {
    case JERR_OUT_OF_MEMORY:
        err->code = RDEC_ERR_MEMORY;
        break;
    case JERR_NO_SOI:
    case JERR_EMPTY_IMAGE:
    case JERR_NO_IMAGE:
    case JERR_UNKNOWN_MARKER:
        err->code = RDEC_ERR_FORMAT;
        break;
    case JERR_WIDTH_OVERFLOW:
    case JERR_BAD_CROP_SPEC:
        err->code = RDEC_ERR_REGION;
        break;
    default:
        err->code = RDEC_ERR_INTERNAL;
        break;
    }
    longjmp(err->jmp, 1);
}

const char *
rdec_strerror(int code)
{
    switch (code) {
    case RDEC_OK:           return "ok";
    case RDEC_ERR_ARGUMENT: return "invalid argument";
    case RDEC_ERR_FORMAT:   return "unrecognized or malformed JPEG data";
    case RDEC_ERR_SCALE:    return "unsupported DCT scaling factor";
    case RDEC_ERR_REGION:   return "crop rectangle outside scaled image";
    case RDEC_ERR_MEMORY:   return "out of memory";
    default:                return "internal decompression error";
    }
}

int
rdec_scaled_dimensions(unsigned image_width, unsigned image_height,
                       int scale_num, int scale_denom,
                       unsigned *scaled_width, unsigned *scaled_height)
{
    if (!scaled_width || !scaled_height || scale_denom <= 0 ||
        scale_num <= 0)
        return RDEC_ERR_ARGUMENT;

    *scaled_width =
        (unsigned)(((unsigned long long)image_width * scale_num +
                    (unsigned)scale_denom - 1) / (unsigned)scale_denom);
    *scaled_height =
        (unsigned)(((unsigned long long)image_height * scale_num +
                    (unsigned)scale_denom - 1) / (unsigned)scale_denom);
    return RDEC_OK;
}

static int
validate_request(const rdec_request *req)
{
    if (!req || req->w == 0 || req->h == 0)
        return RDEC_ERR_ARGUMENT;
    if (req->scale_denom != 8 || req->scale_num < 1 || req->scale_num > 8)
        return RDEC_ERR_SCALE;
    if (req->format != RDEC_FMT_RGB && req->format != RDEC_FMT_GRAY)
        return RDEC_ERR_ARGUMENT;
    return RDEC_OK;
}

static int
decode_internal(struct jpeg_decompress_struct *cinfo,
                const rdec_request *req,
                unsigned char **out_pixels, size_t *out_size,
                rdec_info *info)
{
    struct rdec_error_mgr *err = (struct rdec_error_mgr *)cinfo->err;
    JDIMENSION crop_x, crop_w, crop_y, crop_h;
    JDIMENSION strip_x, strip_w;
    unsigned components, out_stride;
    JSAMPARRAY strip_buf = NULL;
    unsigned char * volatile result_v = NULL;
    unsigned char *result;
    JDIMENSION row;
    int rc;

    if (setjmp(err->jmp)) {
        /* A libjpeg error longjmps here, bypassing the done: label. */
        free((unsigned char *)result_v);
        jpeg_abort_decompress(cinfo);
        return err->code;
    }

    jpeg_read_header(cinfo, TRUE);

    cinfo->scale_num = (unsigned int)req->scale_num;
    cinfo->scale_denom = (unsigned int)req->scale_denom;
    cinfo->do_fancy_upsampling = req->fancy ? TRUE : FALSE;
    cinfo->out_color_space =
        req->format == RDEC_FMT_GRAY ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_start_decompress(cinfo);

    components = (unsigned)cinfo->output_components;

    if (info) {
        info->scaled_width = (unsigned)cinfo->output_width;
        info->scaled_height = (unsigned)cinfo->output_height;
        info->components = (int)components;
    }

    /* All rectangle coordinates are in the scaled image coordinate system. */
    if ((unsigned long long)req->x + req->w > cinfo->output_width ||
        (unsigned long long)req->y + req->h > cinfo->output_height) {
        rc = RDEC_ERR_REGION;
        goto done;
    }

    crop_x = (JDIMENSION)req->x;
    crop_w = (JDIMENSION)req->w;
    crop_y = (JDIMENSION)req->y;
    crop_h = (JDIMENSION)req->h;

    /* jpeg_crop_scanline() moves crop_x left to the nearest iMCU column and
     * widens crop_w accordingly.  The values it returns describe the strip
     * that the hardware/SIMD pipeline can actually decode.
     */
    jpeg_crop_scanline(cinfo, &crop_x, &crop_w);
    strip_x = crop_x;
    strip_w = crop_w;

    strip_buf = (*cinfo->mem->alloc_sarray)
        ((j_common_ptr)cinfo, JPOOL_IMAGE,
         (JDIMENSION)strip_w * components, (JDIMENSION)1);

    out_stride = (unsigned)req->w * components;
    result_v = (unsigned char *)malloc((size_t)out_stride * req->h);
    if (!result_v) {
        rc = RDEC_ERR_MEMORY;
        goto done;
    }
    result = result_v;

    if (crop_y > 0 && jpeg_skip_scanlines(cinfo, crop_y) != crop_y) {
        rc = RDEC_ERR_INTERNAL;
        goto done;
    }

    /* Read exactly crop_h rows of the wider strip, then copy only the
     * requested window [req->x, req->x + req->w).  Rows are copied into a
     * tightly packed destination, so strip alignment padding can never
     * serialize into the caller's output.
     */
    for (row = 0; row < crop_h; row++) {
        JDIMENSION read = jpeg_read_scanlines(cinfo, strip_buf, 1);
        unsigned char *dst;
        if (read != 1) {
            rc = RDEC_ERR_INTERNAL;
            goto done;
        }
        dst = result + (size_t)row * out_stride;
        memmove(dst,
                strip_buf[0] + ((JDIMENSION)req->x - strip_x) * components,
                out_stride);
    }

    if (info) {
        info->actual_x = (unsigned)strip_x;
        info->actual_w = (unsigned)strip_w;
    }
    if (out_size)
        *out_size = (size_t)out_stride * req->h;
    *out_pixels = (unsigned char *)result_v;
    result_v = NULL;
    rc = RDEC_OK;

done:
    free((unsigned char *)result_v);
    jpeg_abort_decompress(cinfo);
    return rc;
}

int
rdec_decode_mem(const unsigned char *jpeg, size_t jpeg_len,
                const rdec_request *req,
                unsigned char **out_pixels, size_t *out_size,
                rdec_info *info)
{
    struct jpeg_decompress_struct cinfo;
    struct rdec_error_mgr jerr;
    int rc;

    if (out_pixels)
        *out_pixels = NULL;
    if (!jpeg || jpeg_len == 0 || !out_pixels)
        return RDEC_ERR_ARGUMENT;

    rc = validate_request(req);
    if (rc != RDEC_OK)
        return rc;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = rdec_error_exit;
    jerr.code = RDEC_OK;
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg, (unsigned long)jpeg_len);

    rc = decode_internal(&cinfo, req, out_pixels, out_size, info);
    jpeg_destroy_decompress(&cinfo);
    return rc;
}

int
rdec_decode_file(const char *path, const rdec_request *req,
                 unsigned char **out_pixels, size_t *out_size,
                 rdec_info *info)
{
    FILE *fp;
    long file_len;
    unsigned char *buffer;
    size_t got;
    int rc;

    if (!path)
        return RDEC_ERR_ARGUMENT;

    fp = fopen(path, "rb");
    if (!fp)
        return RDEC_ERR_FORMAT;

    if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RDEC_ERR_FORMAT;
    }

    buffer = (unsigned char *)malloc((size_t)file_len);
    if (!buffer) {
        fclose(fp);
        return RDEC_ERR_MEMORY;
    }
    got = fread(buffer, 1, (size_t)file_len, fp);
    fclose(fp);
    if (got != (size_t)file_len) {
        free(buffer);
        return RDEC_ERR_FORMAT;
    }

    rc = rdec_decode_mem(buffer, (size_t)file_len, req, out_pixels,
                         out_size, info);
    free(buffer);
    return rc;
}
