#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "regiondec.h"

static void
usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options] input.jpg output.ppm\n"
            "  -g            grayscale output (default RGB)\n"
            "  -n            nosmooth upsampling (byte-exact edges)\n"
            "  -r WxH+X+Y    rectangle in scaled-image coordinates\n"
            "  -s M          DCT scale M/8, M = 1..8 (default 8 = full)\n",
            prog);
}

static int
parse_rect(const char *spec, rdec_request *req)
{
    int w, h, x, y;

    if (sscanf(spec, "%dx%d+%d+%d", &w, &h, &x, &y) != 4 ||
        w <= 0 || h <= 0 || x < 0 || y < 0)
        return -1;
    req->w = (unsigned)w;
    req->h = (unsigned)h;
    req->x = (unsigned)x;
    req->y = (unsigned)y;
    return 0;
}

static void
write_ppm(const char *path, const unsigned char *pixels, unsigned w,
          unsigned h, int components)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    if (components == 1)
        fprintf(fp, "P5\n%u %u\n255\n", w, h);
    else
        fprintf(fp, "P6\n%u %u\n255\n", w, h);
    fwrite(pixels, 1, (size_t)w * h * (size_t)components, fp);
    fclose(fp);
}

int
main(int argc, char **argv)
{
    rdec_request req;
    rdec_info info;
    unsigned char *pixels = NULL;
    size_t size = 0;
    int opt, rc, have_rect = 0;

    memset(&req, 0, sizeof(req));
    req.scale_num = 8;
    req.scale_denom = 8;
    req.format = RDEC_FMT_RGB;
    req.fancy = 1;

    while ((opt = getopt(argc, argv, "gnr:s:")) != -1) {
        switch (opt) {
        case 'g':
            req.format = RDEC_FMT_GRAY;
            break;
        case 'n':
            req.fancy = 0;
            break;
        case 'r':
            if (parse_rect(optarg, &req) != 0) {
                usage(argv[0]);
                return 2;
            }
            have_rect = 1;
            break;
        case 's':
            req.scale_num = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (optind + 2 != argc || !have_rect) {
        usage(argv[0]);
        return 2;
    }

    rc = rdec_decode_file(argv[optind], &req, &pixels, &size, &info);
    if (rc != RDEC_OK) {
        fprintf(stderr, "decode failed: %s\n", rdec_strerror(rc));
        return 1;
    }

    fprintf(stderr,
            "scaled image %ux%u; requested %ux%u+%u+%u; "
            "internal strip %ux%u+%u+%u; %u bytes\n",
            info.scaled_width, info.scaled_height, (unsigned)req.w,
            (unsigned)req.h, (unsigned)req.x, (unsigned)req.y,
            info.actual_w, info.actual_x, (unsigned)req.y,
            (unsigned)req.y, (unsigned)size);

    write_ppm(argv[optind + 1], pixels, req.w, req.h, info.components);
    free(pixels);
    return 0;
}
