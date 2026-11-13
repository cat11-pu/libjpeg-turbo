#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
static unsigned char *rf(const char*p,size_t*l){FILE*f=fopen(p,"rb");fseek(f,0,2);long n=ftell(f);fseek(f,0,0);unsigned char*b=malloc(n);fread(b,1,n,f);fclose(f);*l=n;return b;}
static unsigned char*dec(const unsigned char*j,size_t jl,int den,int fast,int*w,int*h){
  struct jpeg_decompress_struct c;struct jpeg_error_mgr e;c.err=jpeg_std_error(&e);
  jpeg_create_decompress(&c);jpeg_mem_src(&c,j,jl);jpeg_read_header(&c,1);
  c.out_color_space=JCS_RGB;c.scale_num=1;c.scale_denom=den;
  if(fast)c.dct_method=JDCT_IFAST;else c.dct_method=JDCT_ISLOW;
  jpeg_start_decompress(&c);*w=c.output_width;*h=c.output_height;
  unsigned char*o=malloc((size_t)*w*3**h);
  while(c.output_scanline<c.output_height){JSAMPROW r=o+(size_t)c.output_scanline**w*3;jpeg_read_scanlines(&c,&r,1);}
  jpeg_finish_decompress(&c);jpeg_destroy_decompress(&c);return o;
}
int main(int ac,char**av){
  size_t jl;unsigned char*j=rf(av[1],&jl);
  int den=atoi(av[2]);
  int w1,h1,w2,h2;unsigned char*a=dec(j,jl,den,0,&w1,&h1),*b=dec(j,jl,den,1,&w2,&h2);
  long n=(long)w1*h1*3,nd=0,mx=0,sum=0;
  for(long i=0;i<n;i++){int d=abs((int)a[i]-(int)b[i]);if(d)nd++;if(d>mx)mx=d;sum+=d;}
  printf("%s 1/%d ISLOW vs IFAST: max=%ld mean=%.3f ndiff=%ld/%ld\n",av[1],den,mx,(double)sum/n,nd,n);
  return 0;
}
