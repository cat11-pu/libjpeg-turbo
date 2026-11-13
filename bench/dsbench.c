#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <jpeglib.h>
#include <jerror.h>

/*
 * Benchmark: DCT downsample decode (scale_num/scale_denom) vs.
 * full-resolution decode followed by box-average downsampling.
 *
 * Output: dimensions, libjpeg internal allocation peak, wall time, and
 * per-channel pixel diff stats (max abs diff, mean abs diff, # of
 * differing pixels, PSNR).  True process-level malloc peak is measured
 * separately with mallocpeak.so via run_bench.sh.
 */

static size_t g_live = 0;       /* currently live bytes allocated by libjpeg */
static size_t g_peak = 0;       /* high-water mark of g_live */
static size_t g_total = 0;      /* cumulative allocation bytes */
static struct jpeg_memory_mgr g_default_mgr;

static void *count_alloc_small(j_common_ptr cinfo, int pool_id, size_t sz)
{
  void *p = g_default_mgr.alloc_small(cinfo, pool_id, sz);
  g_live += sz;
  g_total += sz;
  if (g_live > g_peak) g_peak = g_live;
  return p;
}

static void *count_alloc_large(j_common_ptr cinfo, int pool_id, size_t sz)
{
  void *p = g_default_mgr.alloc_large(cinfo, pool_id, sz);
  g_live += sz;
  g_total += sz;
  if (g_live > g_peak) g_peak = g_live;
  return p;
}

/* Wrap the default memory manager with allocation counters. */
static void install_counter(j_common_ptr cinfo)
{
  g_default_mgr = *cinfo->mem;
  cinfo->mem->alloc_small = count_alloc_small;
  cinfo->mem->alloc_large = count_alloc_large;
}

static void reset_counter(void)
{
  g_live = g_peak = g_total = 0;
}

static long ns_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static unsigned char *read_file(const char *path, size_t *len_out)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = (unsigned char *)malloc((size_t)len);
  if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "short read\n"); exit(2); }
  fclose(f);
  *len_out = (size_t)len;
  return buf;
}

/* Streaming decode via jpeg_stdio src (no whole-file input buffer). */
static void decode_stream(const char *path, int num, int den,
                          int *w_out, int *h_out)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  install_counter((j_common_ptr)&cinfo);
  reset_counter();
  jpeg_stdio_src(&cinfo, f);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  if (den > 1) { cinfo.scale_num = num; cinfo.scale_denom = den; }
  jpeg_start_decompress(&cinfo);
  int w = cinfo.output_width, h = cinfo.output_height;
  size_t stride = (size_t)w * 3;
  unsigned char *row = (unsigned char *)malloc(stride);
  while (cinfo.output_scanline < cinfo.output_height)
  {
    JSAMPROW r = row;
    jpeg_read_scanlines(&cinfo, &r, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(row);
  fclose(f);
  *w_out = w; *h_out = h;
}

/* Full-resolution decode to RGB (3 bytes/pixel, top-down). */
static unsigned char *decode_full(const unsigned char *jpeg, size_t jlen,
                                  int *w_out, int *h_out, size_t *outbuf_sz)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  install_counter((j_common_ptr)&cinfo);
  reset_counter();
  jpeg_mem_src(&cinfo, jpeg, jlen);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  int w = cinfo.output_width, h = cinfo.output_height;
  size_t row_stride = (size_t)w * 3;
  unsigned char *rgb = (unsigned char *)malloc(row_stride * (size_t)h);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = rgb + (size_t)cinfo.output_scanline * row_stride;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  *w_out = w; *h_out = h;
  *outbuf_sz = g_peak;
  jpeg_destroy_decompress(&cinfo);
  return rgb;
}

/* DCT-scaled decode (num/denom), RGB top-down. */
static unsigned char *decode_scaled(const unsigned char *jpeg, size_t jlen,
                                    int num, int denom,
                                    int *w_out, int *h_out, size_t *outbuf_sz)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  install_counter((j_common_ptr)&cinfo);
  reset_counter();
  jpeg_mem_src(&cinfo, jpeg, jlen);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  cinfo.scale_num = num;
  cinfo.scale_denom = denom;
  jpeg_start_decompress(&cinfo);

  int w = cinfo.output_width, h = cinfo.output_height;
  size_t row_stride = (size_t)w * 3;
  unsigned char *rgb = (unsigned char *)malloc(row_stride * (size_t)h);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = rgb + (size_t)cinfo.output_scanline * row_stride;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  *w_out = w; *h_out = h;
  *outbuf_sz = g_peak;
  jpeg_destroy_decompress(&cinfo);
  return rgb;
}

/*
 * Box-average downscale by integer factor f (width/height multiples of f).
 * Reference path: what "full decode then shrink" produces.
 */
static unsigned char *box_down(const unsigned char *src, int sw, int sh, int f)
{
  int dw = sw / f, dh = sh / f;
  unsigned char *dst = (unsigned char *)malloc((size_t)dw * 3 * (size_t)dh);
  for (int dy = 0; dy < dh; dy++) {
    for (int dx = 0; dx < dw; dx++) {
      unsigned sum[3] = {0, 0, 0};
      for (int yy = 0; yy < f; yy++) {
        const unsigned char *s = src + ((size_t)(dy * f + yy) * sw + dx * f) * 3;
        for (int xx = 0; xx < f; xx++) {
          sum[0] += s[xx * 3]; sum[1] += s[xx * 3 + 1]; sum[2] += s[xx * 3 + 2];
        }
      }
      unsigned char *d = dst + ((size_t)dy * dw + dx) * 3;
      d[0] = (unsigned char)((sum[0] + f * f / 2) / (f * f));
      d[1] = (unsigned char)((sum[1] + f * f / 2) / (f * f));
      d[2] = (unsigned char)((sum[2] + f * f / 2) / (f * f));
    }
  }
  return dst;
}

/* Nearest-neighbor (point) downscale by integer factor f. */
static unsigned char *point_down(const unsigned char *src, int sw, int sh, int f)
{
  int dw = sw / f, dh = sh / f;
  unsigned char *dst = (unsigned char *)malloc((size_t)dw * 3 * (size_t)dh);
  for (int dy = 0; dy < dh; dy++)
    for (int dx = 0; dx < dw; dx++)
      memcpy(dst + ((size_t)dy * dw + dx) * 3,
             src + ((size_t)(dy * f) * sw + dx * f) * 3, 3);
  return dst;
}

static void diff_stats(const unsigned char *a, const unsigned char *b,
                       int w, int h, double *maxd, double *meand,
                       unsigned long *ndiff, double *psnr)
{
  size_t n = (size_t)w * h * 3;
  unsigned long m = 0, diffs = 0;
  double sq = 0.0;
  int mx = 0;
  for (size_t i = 0; i < n; i++) {
    int d = (int)a[i] - (int)b[i];
    if (d < 0) d = -d;
    if (d) diffs++;
    if (d > mx) mx = d;
    m += (unsigned long)d;
    sq += (double)d * d;
  }
  *maxd = mx;
  *meand = (double)m / (double)n;
  *ndiff = diffs;
  *psnr = sq > 0 ? 10.0 * log10((double)n * 255.0 * 255.0 / sq) : 999.0;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s image.jpg [iters]\n", argv[0]); return 1; }
  int iters = argc > 2 ? atoi(argv[2]) : 5;

  /* streaming single-shot mode: "stream-full" | "stream-1/N" */
  if (argc > 3 && !strncmp(argv[3], "stream", 6)) {
    int den = 1;
    if (strstr(argv[3], "1/2")) den = 2;
    else if (strstr(argv[3], "1/4")) den = 4;
    else if (strstr(argv[3], "1/8")) den = 8;
    int w, h;
    decode_stream(argv[1], 1, den, &w, &h);
    printf("mode=%s out=%dx%d\n", argv[3], w, h);
    return 0;
  }

  size_t jlen;
  unsigned char *jpeg = read_file(argv[1], &jlen);

  /* single-shot mode for external malloc preload: "full" | "1/2" | "1/4" | "1/8" */
  if (argc > 3) {
    int num = 1, den = 1;
    if (!strcmp(argv[3], "full")) { num = 1; den = 1; }
    else if (!strcmp(argv[3], "1/2")) den = 2;
    else if (!strcmp(argv[3], "1/4")) den = 4;
    else if (!strcmp(argv[3], "1/8")) den = 8;
    else { fprintf(stderr, "bad mode %s\n", argv[3]); return 1; }
    int w, h; size_t pk;
    unsigned char *out;
    if (den == 1)
      out = decode_full(jpeg, jlen, &w, &h, &pk);
    else
      out = decode_scaled(jpeg, jlen, num, den, &w, &h, &pk);
    printf("mode=%s out=%dx%d output_bytes=%zu\n", argv[3], w, h, (size_t)w * h * 3);
    free(out);
    free(jpeg);
    return 0;
  }

  int fw, fh;
  size_t full_peak;
  long t0 = ns_now();
  unsigned char *full = decode_full(jpeg, jlen, &fw, &fh, &full_peak);
  long t_full = ns_now() - t0;
  size_t full_rgb_bytes = (size_t)fw * fh * 3;
  printf("image=%s jpeg=%zuKB full=%dx%d\n", argv[1], jlen / 1024, fw, fh);
  printf("full decode: time=%.2fms internal_peak=%zuKB output=%zuKB\n",
         t_full / 1e6, full_peak / 1024, full_rgb_bytes / 1024);

  static const int factors[][2] = {{1, 2}, {1, 4}, {1, 8}};
  for (int k = 0; k < 3; k++) {
    int num = factors[k][0], den = factors[k][1], f = den / num;
    int rw = fw / f, rh = fh / f;

    /* reference: full decode + box average (timed, iters) */
    long tref = 0;
    unsigned char *ref = NULL;
    for (int it = 0; it < iters; it++) {
      long a = ns_now();
      free(ref);
      int rw2, rh2; size_t pk;
      unsigned char *fd = decode_full(jpeg, jlen, &rw2, &rh2, &pk);
      ref = box_down(full, fw, fh, f);
      free(fd);
      tref += ns_now() - a;
    }

    /* DCT scaled decode (timed, iters) */
    int sw, sh; size_t sc_peak;
    long tsc = 0;
    unsigned char *sc = NULL;
    for (int it = 0; it < iters; it++) {
      free(sc);
      long a = ns_now();
      sc = decode_scaled(jpeg, jlen, num, den, &sw, &sh, &sc_peak);
      tsc += ns_now() - a;
    }

    double maxd, meand, psnr; unsigned long ndiff;
    if (sw == rw && sh == rh) {
      diff_stats(sc, ref, rw, rh, &maxd, &meand, &ndiff, &psnr);
    } else {
      /* dimensions can differ by rounding to iMCU boundaries */
      int cw = sw < rw ? sw : rw, ch = sh < rh ? sh : rh;
      diff_stats(sc, ref, cw, ch, &maxd, &meand, &ndiff, &psnr);
    }
    unsigned char *pt = point_down(full, fw, fh, f);
    double pmaxd, pmeand, ppsnr; unsigned long pndiff;
    diff_stats(sc, pt, rw, rh, &pmaxd, &pmeand, &pndiff, &ppsnr);
    free(pt);
    size_t sc_rgb_bytes = (size_t)sw * sh * 3;
    size_t full_total_kb = (full_peak + full_rgb_bytes) / 1024;
    size_t sc_total_kb = (sc_peak + sc_rgb_bytes) / 1024;
    printf("--- 1/%d  dct_out=%dx%d ref=%dx%d ---\n", f, sw, sh, rw, rh);
    printf("  time: dct=%.2fms  full+box=%.2fms (avg of %d)\n",
           tsc / 1e6 / iters, tref / 1e6 / iters, iters);
    printf("  memory total_peak: dct=%zuKB (internal %zu + output %zu) | "
           "full=%zuKB (internal %zu + output %zu) | ratio=%.1f%%\n",
           sc_total_kb, sc_peak / 1024, sc_rgb_bytes / 1024,
           full_total_kb, full_peak / 1024, full_rgb_bytes / 1024,
           100.0 * sc_total_kb / full_total_kb);
    printf("  pixel diff vs full+box: max=%.0f mean=%.3f ndiff=%lu/%zu psnr=%.2fdB\n",
           maxd, meand, ndiff, (size_t)rw * rh * 3, psnr);
    printf("  pixel diff vs full+point: max=%.0f mean=%.3f ndiff=%lu/%zu psnr=%.2fdB\n",
           pmaxd, pmeand, pndiff, (size_t)rw * rh * 3, ppsnr);
    free(ref); free(sc);
  }
  free(full); free(jpeg);
  return 0;
}
