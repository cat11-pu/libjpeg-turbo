#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>
#include <jerror.h>

/*
 * Low-level libjpeg API: DCT downsampled decode.
 * Set scale_num/scale_denom between jpeg_read_header() and
 * jpeg_start_decompress().  Legal denominators: 1, 2, 4, 8.
 * Usage: dsdemo_jpeg image.jpg 1|2|4|8
 */
int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: %s image.jpg denom\n", argv[0]); return 1; }
  int denom = atoi(argv[2]);

  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, f);
  jpeg_read_header(&cinfo, TRUE);

  cinfo.out_color_space = JCS_RGB;
  cinfo.scale_num = 1;
  cinfo.scale_denom = denom;          /* 1, 2, 4 or 8 */
  jpeg_start_decompress(&cinfo);

  int w = cinfo.output_width, h = cinfo.output_height;
  size_t stride = (size_t)w * cinfo.output_components;
  unsigned char *row = (unsigned char *)malloc(stride);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW r = row;
    jpeg_read_scanlines(&cinfo, &r, 1);
  }
  printf("scale 1/%d -> %dx%d, %zu bytes/row\n", denom, w, h, stride);

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(row);
  fclose(f);
  return 0;
}
