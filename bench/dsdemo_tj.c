#include <stdio.h>
#include <stdlib.h>
#include <turbojpeg.h>

/*
 * TurboJPEG high-level API: direct downsampled decode at 1/2, 1/4, 1/8.
 * Usage: dsdemo_tj image.jpg
 */
static unsigned char *read_file(const char *path, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = (unsigned char *)malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) exit(1);
  fclose(f);
  *len = (size_t)n;
  return buf;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s image.jpg\n", argv[0]); return 1; }

  /* 1. enumerate all hardware-supported scaling factors */
  int nfactors = 0;
  tjscalingfactor *factors = tj3GetScalingFactors(&nfactors);
  printf("supported scaling factors (%d):", nfactors);
  for (int i = 0; i < nfactors; i++)
    printf(" %d/%d", factors[i].num, factors[i].denom);
  printf("\n");

  size_t jlen;
  unsigned char *jpeg = read_file(argv[1], &jlen);

  static const tjscalingfactor wanted[3] = {
    {1, 2}, {1, 4}, {1, 8}
  };
  for (int k = 0; k < 3; k++) {
    tjhandle h = tj3Init(TJINIT_DECOMPRESS);
    if (tj3DecompressHeader(h, jpeg, jlen) != 0) {
      fprintf(stderr, "header: %s\n", tj3GetErrorStr(h)); return 1;
    }
    int jw = tj3Get(h, TJPARAM_JPEGWIDTH);
    int jh = tj3Get(h, TJPARAM_JPEGHEIGHT);
    if (tj3SetScalingFactor(h, wanted[k]) != 0) {
      fprintf(stderr, "scale: %s\n", tj3GetErrorStr(h)); return 1;
    }
    int dw = TJSCALED(jw, wanted[k]);
    int dh = TJSCALED(jh, wanted[k]);
    size_t pitch = (size_t)dw * tjPixelSize[TJPF_RGB];
    unsigned char *dst = (unsigned char *)malloc(pitch * (size_t)dh);
    if (tj3Decompress8(h, jpeg, jlen, dst, (int)pitch, TJPF_RGB) != 0) {
      fprintf(stderr, "decode: %s\n", tj3GetErrorStr(h)); return 1;
    }
    printf("factor %d/%d: %dx%d -> %dx%d, output=%zuKB\n",
           wanted[k].num, wanted[k].denom, jw, jh, dw, dh,
           pitch * (size_t)dh / 1024);
    free(dst);
    tj3Destroy(h);
  }
  free(jpeg);
  return 0;
}
