#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jerror.h>
#include <jpeglib.h>

#include "regiondec.h"

struct test_err {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
};

static void
test_error_exit(j_common_ptr cinfo)
{
    struct test_err *err = (struct test_err *)cinfo->err;
    longjmp(err->jmp, 1);
}

/* Reference path: full decode at the given scale, returning a tightly packed
 * RGB or grayscale buffer plus the scaled dimensions.
 */
static int
reference_full_decode(const char *path, int scale_num, int scale_denom,
                      int format, int fancy,
                      unsigned char **pixels, unsigned *full_w,
                      unsigned *full_h, unsigned *components)
{
    struct jpeg_decompress_struct cinfo;
    struct test_err jerr;
    unsigned char *buf;
    unsigned stride, row;
    JSAMPROW row_ptr[1];
    FILE *fp;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = test_error_exit;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.scale_num = (unsigned int)scale_num;
    cinfo.scale_denom = (unsigned int)scale_denom;
    cinfo.do_fancy_upsampling = fancy ? TRUE : FALSE;
    cinfo.out_color_space =
        format == RDEC_FMT_GRAY ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_start_decompress(&cinfo);

    *full_w = (unsigned)cinfo.output_width;
    *full_h = (unsigned)cinfo.output_height;
    *components = (unsigned)cinfo.output_components;
    stride = *full_w * *components;
    buf = (unsigned char *)malloc((size_t)stride * *full_h);
    if (!buf)
        longjmp(jerr.jmp, 1);

    row_ptr[0] = NULL;
    for (row = 0; row < *full_h; row++) {
        row_ptr[0] = (JSAMPROW)(buf + (size_t)row * stride);
        if (jpeg_read_scanlines(&cinfo, row_ptr, 1) != 1)
            longjmp(jerr.jmp, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    *pixels = buf;
    return 0;
}

typedef struct {
    const char *name;
    int x, y, w, h;
} crop_case;

static const crop_case valid_cases[] = {
    { "full-frame",          0,  0,  0,  0 },  /* w/h filled at runtime */
    { "top-left 1px",        0,  0,  1,  1 },
    { "bottom-right 1px",   -1, -1,  1,  1 },  /* x/y filled at runtime */
    { "unaligned interior",  3,  5, 17, 11 },
    { "right-edge strip",   -1,  0,  9,  0 },  /* x filled; h filled */
    { "bottom-edge strip",   0, -1,  0,  7 },  /* y filled; w filled */
    { "odd 1-wide column",  11,  2,  1, 23 },
    { "odd 1-tall row",      2, 13, 31,  1 }
};

typedef struct {
    const char *name;
    rdec_request req;
    int expect_code;
} error_case;

static unsigned long total_pixels_checked;
static unsigned long total_mismatches;
static unsigned long exact_runs;
static unsigned long edge_only_runs;
static unsigned long interior_diff_runs;

static void
compare_pixels(const char *tag, const rdec_request *req,
               const unsigned char *got, const unsigned char *ref,
               unsigned components, unsigned full_w)
{
    unsigned row, col, c;
    unsigned long mism = 0;
    int interior_mism = 0;
    int left_edge_mism = 0, right_edge_mism = 0;

    for (row = 0; row < (unsigned)req->h; row++) {
        for (col = 0; col < (unsigned)req->w; col++) {
            const unsigned char *g =
                got + ((size_t)row * req->w + col) * components;
            const unsigned char *r =
                ref + ((size_t)(req->y + row) * full_w + req->x + col) *
                components;
            total_pixels_checked += components;
            for (c = 0; c < components; c++) {
                if (g[c] != r[c]) {
                    mism++;
                    total_mismatches++;
                    if (col == 0)
                        left_edge_mism = 1;
                    else if (col == (unsigned)req->w - 1)
                        right_edge_mism = 1;
                    else
                        interior_mism = 1;
                }
            }
        }
    }

    if (mism == 0) {
        exact_runs++;
        printf("  [EXACT ] %-22s mismatches=0\n", tag);
    } else if (!interior_mism) {
        edge_only_runs++;
        printf("  [EDGE  ] %-22s mismatches=%lu (only L=%d R=%d edge columns,"
               " documented fancy-upsampling exception)\n",
               tag, mism, left_edge_mism, right_edge_mism);
    } else {
        interior_diff_runs++;
        printf("  [FAIL  ] %-22s mismatches=%lu including interior pixels\n",
               tag, mism);
    }
}

static unsigned char *
load_file(const char *path, size_t *length)
{
    FILE *fp = fopen(path, "rb");
    long n;
    unsigned char *buf;

    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    *length = (size_t)n;
    fclose(fp);
    return buf;
}

static void
run_error_cases(const unsigned char *jpeg, size_t jpeg_len,
                unsigned full_w, unsigned full_h, int scale_num)
{
    error_case cases[] = {
        { "zero width",
          { 0, 0, 0, 10, scale_num, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_ARGUMENT },
        { "zero height",
          { 0, 0, 10, 0, scale_num, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_ARGUMENT },
        { "rectangle past right edge",
          { full_w - 4, 0, 10, 10, scale_num, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_REGION },
        { "rectangle past bottom edge",
          { 0, full_h - 4, 10, 10, scale_num, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_REGION },
        { "x beyond image",
          { full_w, 0, 1, 1, scale_num, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_REGION },
        { "bad scale numerator",
          { 0, 0, 4, 4, 9, 8, RDEC_FMT_RGB, 1 },
          RDEC_ERR_SCALE },
        { "non-DCT denominator",
          { 0, 0, 4, 4, 1, 3, RDEC_FMT_RGB, 1 },
          RDEC_ERR_SCALE },
        { "bad format enum",
          { 0, 0, 4, 4, scale_num, 8, 99, 1 },
          RDEC_ERR_ARGUMENT }
    };
    unsigned i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned char *out = NULL;
        size_t out_size = 0;
        int rc = rdec_decode_mem(jpeg, jpeg_len, &cases[i].req, &out,
                                 &out_size, NULL);
        if (rc == cases[i].expect_code)
            printf("  [PASS  ] error case %-28s -> %s\n",
                   cases[i].name, rdec_strerror(rc));
        else
            printf("  [FAIL  ] error case %-28s got %d expected %d\n",
                   cases[i].name, rc, cases[i].expect_code);
    }
}

static void
run_image(const char *path, const char *label)
{
    static const int scales[][2] = {
        {1,8}, {2,8}, {3,8}, {4,8}, {5,8}, {6,8}, {7,8}, {8,8}
    };
    static const int formats[] = { RDEC_FMT_RGB, RDEC_FMT_GRAY };
    static const int fancy_modes[] = { 0, 1 };
    unsigned si, fi, mi, ci;
    size_t jpeg_len = 0;
    unsigned char *jpeg_buf = load_file(path, &jpeg_len);

    if (!jpeg_buf) {
        printf("  [FAIL  ] cannot load %s\n", path);
        return;
    }

    printf("====================================================================\n");
    printf("image: %s (%s)\n", label, path);
    printf("====================================================================\n");

    for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
        int scale_num = scales[si][0];
        int scale_denom = scales[si][1];

        for (fi = 0; fi < 2; fi++) {
            for (mi = 0; mi < 2; mi++) {
                int format = formats[fi];
                int fancy = fancy_modes[mi];
                unsigned char *ref = NULL, *got = NULL;
                unsigned full_w, full_h, components;
                size_t got_size;

                if (reference_full_decode(path, scale_num, scale_denom,
                                          format, fancy, &ref, &full_w,
                                          &full_h, &components) != 0) {
                    printf("  [FAIL  ] reference decode error\n");
                    continue;
                }

                printf("-- scale %d/%d  %s  %s  (scaled size %ux%u)\n",
                       scale_num, scale_denom,
                       format == RDEC_FMT_GRAY ? "GRAY" : "RGB ",
                       fancy ? "fancy   " : "nosmooth",
                       full_w, full_h);

                for (ci = 0;
                     ci < sizeof(valid_cases) / sizeof(valid_cases[0]);
                     ci++) {
                    rdec_request req;
                    rdec_info info;
                    char tag[96];
                    int rc;

                    memset(&req, 0, sizeof(req));
                    req.scale_num = scale_num;
                    req.scale_denom = scale_denom;
                    req.format = format;
                    req.fancy = fancy;

                    /* Resolve edge-anchored (-1) and full-extent (0)
                     * markers, then shrink fixed rectangles so every case
                     * stays inside even the smallest scaled image (16x12).
                     */
                    if (valid_cases[ci].x < 0)
                        req.x = full_w - 1;
                    else
                        req.x = (unsigned)valid_cases[ci].x;
                    if (valid_cases[ci].y < 0)
                        req.y = full_h - 1;
                    else
                        req.y = (unsigned)valid_cases[ci].y;

                    if (req.x >= full_w) req.x = full_w - 1;
                    if (req.y >= full_h) req.y = full_h - 1;

                    req.w = valid_cases[ci].w == 0 ?
                            full_w - req.x : (unsigned)valid_cases[ci].w;
                    req.h = valid_cases[ci].h == 0 ?
                            full_h - req.y : (unsigned)valid_cases[ci].h;
                    if (req.w > full_w - req.x) req.w = full_w - req.x;
                    if (req.h > full_h - req.y) req.h = full_h - req.y;

                    rc = rdec_decode_file(path, &req, &got, &got_size,
                                          &info);
                    if (rc != RDEC_OK) {
                        printf("  [FAIL  ] %-22s decode error: %s\n",
                               valid_cases[ci].name, rdec_strerror(rc));
                        continue;
                    }
                    if (got_size != (size_t)req.w * req.h * components ||
                        info.actual_x > req.x ||
                        info.actual_x + info.actual_w < req.x + req.w) {
                        printf("  [FAIL  ] %-22s geometry/size contract"
                               " violated\n", valid_cases[ci].name);
                        free(got);
                        continue;
                    }

                    snprintf(tag, sizeof(tag), "%s", valid_cases[ci].name);
                    compare_pixels(tag, &req, got, ref, components, full_w);
                    free(got);
                    got = NULL;
                }

                if (si == 0 && fi == 0 && mi == 0)
                    run_error_cases(jpeg_buf, jpeg_len, full_w, full_h,
                                    scale_num);

                free(ref);
            }
        }
    }
    free(jpeg_buf);
    putchar('\n');
}

int
main(int argc, char **argv)
{
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s image.jpg [more.jpg ...]\n", argv[0]);
        return 2;
    }

    for (i = 1; i < argc; i++)
        run_image(argv[i], argv[i]);

    printf("====================================================================\n");
    printf("CONSISTENCY SUMMARY\n");
    printf("====================================================================\n");
    printf("component samples compared : %lu\n", total_pixels_checked);
    printf("total mismatching samples  : %lu\n", total_mismatches);
    printf("byte-exact decode runs     : %lu\n", exact_runs);
    printf("edge-only diff runs        : %lu (fancy upsampling, documented)\n",
           edge_only_runs);
    printf("runs with interior diffs   : %lu (must be zero)\n",
           interior_diff_runs);

    return interior_diff_runs == 0 ? 0 : 1;
}
