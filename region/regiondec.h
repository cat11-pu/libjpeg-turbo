#ifndef REGIONDEC_H
#define REGIONDEC_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDEC_OK              0
#define RDEC_ERR_ARGUMENT    1
#define RDEC_ERR_FORMAT      2
#define RDEC_ERR_SCALE       3
#define RDEC_ERR_REGION      4
#define RDEC_ERR_MEMORY      5
#define RDEC_ERR_INTERNAL    6

#define RDEC_FMT_RGB  0
#define RDEC_FMT_GRAY 1

/*
 * Geometry of a decode request, expressed in the *scaled output image*
 * coordinate system (the same coordinate system documented for
 * jpeg_crop_scanline()).  scale_num/scale_denom select one of the
 * libjpeg-turbo DCT scaling factors M/8, M = 1..8.
 *
 * The rectangle [x, x + w) x [y, y + h) is guaranteed to be delivered.
 * Internally the library may decode a slightly wider, iMCU-aligned strip,
 * but the copy into out_pixels always produces exactly w * h pixels:
 * no alignment padding is ever visible to the caller and rows never bleed
 * into one another.
 */
typedef struct {
    unsigned x;             /* left column, scaled coordinates           */
    unsigned y;             /* top row, scaled coordinates               */
    unsigned w;             /* requested output width  in pixels         */
    unsigned h;             /* requested output height in pixels         */
    int      scale_num;     /* scaling numerator   (1..8)                */
    int      scale_denom;   /* scaling denominator (8)                   */
    int      format;        /* RDEC_FMT_RGB or RDEC_FMT_GRAY             */
    int      fancy;         /* nonzero = smooth/fancy upsampling (default)
                             * zero   = -nosmooth, guarantees byte-exact
                             *          edges even for 4:2:0/4:2:2       */
} rdec_request;

typedef struct {
    unsigned scaled_width;  /* full-image width  after DCT scaling       */
    unsigned scaled_height; /* full-image height after DCT scaling       */
    unsigned actual_x;      /* iMCU-aligned left column actually decoded */
    unsigned actual_w;      /* width of the internal decode strip        */
    int      components;    /* 3 for RGB, 1 for grayscale                */
} rdec_info;

/*
 * Decode exactly the requested rectangle at the requested scale.
 *
 * On success returns RDEC_OK and fills *out_pixels with a tightly packed
 * buffer (row-major, top-down, 3 bytes/pixel for RGB or 1 for grayscale,
 * no row padding) of exactly req->w * req->h * components bytes.  The
 * caller owns *out_pixels and must free() it.  *info may be NULL.
 *
 * The pixel content is identical to: fully decode the JPEG at the same
 * scale, then take rows [y, y+h) and columns [x, x+w).  See README.md for
 * the single documented exception (fancy chroma upsampling edges) and how
 * fancy=0 removes it.
 */
int rdec_decode_file(const char *path, const rdec_request *req,
                     unsigned char **out_pixels, size_t *out_size,
                     rdec_info *info);

/* Same contract, reading from an in-memory JPEG buffer. */
int rdec_decode_mem(const unsigned char *jpeg, size_t jpeg_len,
                    const rdec_request *req,
                    unsigned char **out_pixels, size_t *out_size,
                    rdec_info *info);

/* Human-readable description of an RDEC_ERR_* code. */
const char *rdec_strerror(int code);

/*
 * Dimensions the full image would have at a given DCT scale.  Returns
 * RDEC_OK on success.  Uses the library's jdiv_round_up formula.
 */
int rdec_scaled_dimensions(unsigned image_width, unsigned image_height,
                           int scale_num, int scale_denom,
                           unsigned *scaled_width, unsigned *scaled_height);

#ifdef __cplusplus
}
#endif

#endif /* REGIONDEC_H */
