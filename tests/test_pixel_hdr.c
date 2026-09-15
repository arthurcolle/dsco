#include "pixel_hdr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passes=0, failures=0;
#define CHECK(c, ...) do{ if(c) passes++; else { failures++; \
  fprintf(stderr,"FAIL %s:%d: ",__FILE__,__LINE__); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr);} }while(0)

static const char *curve_name(pixel_hdr_tonemap_t c){
  switch(c){case PIXEL_HDR_TONEMAP_CLIP:return "CLIP";case PIXEL_HDR_TONEMAP_REINHARD:return "REINHARD";
  case PIXEL_HDR_TONEMAP_ACES:return "ACES";default:return "AGX";}
}

static void test_curves(void){
  pixel_hdr_tonemap_t cs[]={PIXEL_HDR_TONEMAP_CLIP,PIXEL_HDR_TONEMAP_REINHARD,
                            PIXEL_HDR_TONEMAP_ACES,PIXEL_HDR_TONEMAP_AGX};
  for(int i=0;i<4;i++){
    pixel_hdr_tonemap_t c=cs[i];
    CHECK(pixel_hdr_tonemap_channel(c,0.0f)==0.0f,"%s: 0 must map to 0",curve_name(c));
    CHECK(pixel_hdr_tonemap_channel(c,-1.0f)==0.0f,"%s: negative clamps to 0",curve_name(c));
    CHECK(pixel_hdr_tonemap_channel(c,NAN)==0.0f,"%s: NaN clamps to 0",curve_name(c));
    float prev=-1.0f; int mono=1;
    for(float x=0.0f;x<64.0f;x+=0.01f){
      float v=pixel_hdr_tonemap_channel(c,x);
      if(v<prev-1e-6f) mono=0;
      prev=v;
    }
    CHECK(mono,"%s: must be monotonic non-decreasing",curve_name(c));
    if(c!=PIXEL_HDR_TONEMAP_CLIP){
      CHECK(pixel_hdr_tonemap_channel(c,1000.0f)<=1.0f,"%s: must stay <=1 at 1000",curve_name(c));
      CHECK(pixel_hdr_tonemap_channel(c,4.0f)>pixel_hdr_tonemap_channel(c,1.0f),
            "%s: must still separate 1 vs 4 stops (no clip cliff)",curve_name(c));
    }
  }
  printf("  curve shoulder @ x=1,2,4,8,16:\n");
  for(int i=0;i<4;i++){
    printf("    %-9s",curve_name(cs[i]));
    for(float x=1.0f;x<=16.0f;x*=2.0f) printf(" %.4f",pixel_hdr_tonemap_channel(cs[i],x));
    printf("\n");
  }
}

static void test_roundtrip(void){
  int w=64,h=4;
  uint8_t *in=malloc((size_t)w*h*3), *out=malloc((size_t)w*h*3);
  for(int i=0;i<w*h*3;i++) in[i]=(uint8_t)(i%256);
  pixel_hdr_surface_t s;
  CHECK(pixel_hdr_surface_init(&s,w,h),"surface init");
  pixel_hdr_from_srgb(&s,in);
  pixel_hdr_resolve_opts_t o; pixel_hdr_resolve_opts_default(&o);
  o.curve=PIXEL_HDR_TONEMAP_CLIP; o.exposure=0.0f; o.dither=false; o.bloom_levels=0;
  CHECK(pixel_hdr_resolve(&s,out,&o),"resolve");
  int maxerr=0;
  for(int i=0;i<w*h*3;i++){int d=abs((int)in[i]-(int)out[i]); if(d>maxerr) maxerr=d;}
  CHECK(maxerr<=1,"identity round-trip must be within 1 LSB, got %d",maxerr);
  printf("  identity round-trip max error: %d LSB\n",maxerr);
  pixel_hdr_surface_free(&s); free(in); free(out);
}

static void test_bloom_propagates(void){
  int w=160,h=160;
  pixel_hdr_surface_t s;
  CHECK(pixel_hdr_surface_init(&s,w,h),"init");
  pixel_fx_rgb_t white={255,255,255};
  pixel_hdr_add_emissive(&s,80,80,3,white,100.0f);
  uint8_t *out=calloc((size_t)w*h*3,1);
  pixel_hdr_resolve_opts_t o; pixel_hdr_resolve_opts_default(&o);
  o.bloom_strength=1.0f; o.bloom_levels=6; o.dither=false;
  CHECK(pixel_hdr_resolve(&s,out,&o),"resolve with bloom");
  int far=out[((size_t)80*w+(80+25))*3];
  int corner=out[0];
  CHECK(far>0,"bloom must reach 25px from a 3px source, got %d",far);
  CHECK(out[((size_t)80*w+80)*3]>far,"core must be brighter than halo");
  printf("  bloom falloff from core (px offset -> R): ");
  for(int d=0;d<=40;d+=8) printf("%d:%d  ",d,out[((size_t)80*w+(80+d))*3]);
  printf("\n  far corner: %d\n",corner);
  /* no-bloom control */
  uint8_t *ctl=calloc((size_t)w*h*3,1);
  o.bloom_levels=0;
  pixel_hdr_resolve(&s,ctl,&o);
  CHECK(ctl[((size_t)80*w+(80+25))*3]==0,"without bloom, 25px away must stay black");
  printf("  control (bloom off) at 25px: %d\n",ctl[((size_t)80*w+(80+25))*3]);
  free(out); free(ctl); pixel_hdr_surface_free(&s);
}

static void test_dither_banding(void){
  /* A shallow linear ramp is the classic banding case. */
  int w=512,h=1;
  pixel_hdr_surface_t s;
  pixel_hdr_surface_init(&s,w,h);
  for(int x=0;x<w;x++){
    float v=0.18f+0.02f*((float)x/(float)w);
    s.rgb[(size_t)x*3+0]=v; s.rgb[(size_t)x*3+1]=v; s.rgb[(size_t)x*3+2]=v;
  }
  uint8_t *nod=calloc((size_t)w*3,1), *dit=calloc((size_t)w*3,1);
  pixel_hdr_resolve_opts_t o; pixel_hdr_resolve_opts_default(&o);
  o.bloom_levels=0; o.curve=PIXEL_HDR_TONEMAP_CLIP;
  o.dither=false; pixel_hdr_resolve(&s,nod,&o);
  o.dither=true;  pixel_hdr_resolve(&s,dit,&o);
  int lv_nod=0, lv_dit=0, seen[256]={0};
  for(int x=0;x<w;x++) if(!seen[nod[x*3]]){seen[nod[x*3]]=1;lv_nod++;}
  memset(seen,0,sizeof seen);
  for(int x=0;x<w;x++) if(!seen[dit[x*3]]){seen[dit[x*3]]=1;lv_dit++;}
  CHECK(lv_dit>=lv_nod,"dither must not reduce distinct levels (%d vs %d)",lv_dit,lv_nod);
  printf("  ramp distinct 8-bit levels: undithered=%d dithered=%d\n",lv_nod,lv_dit);
  /* determinism */
  uint8_t *again=calloc((size_t)w*3,1);
  pixel_hdr_resolve(&s,again,&o);
  CHECK(memcmp(dit,again,(size_t)w*3)==0,"seeded dither must be reproducible");
  free(nod);free(dit);free(again); pixel_hdr_surface_free(&s);
}

/* Regression: expand_highlights must actually push pixels ABOVE 1.0, or the
 * bloom bright-pass keeps nothing and the entire pyramid is a silent no-op.
 * The first kitty_lab wiring used knee=0.55/gain=6.0, which capped the scene
 * at lum 0.94 and produced max|delta|=0 against a bloom-off control. */
static void test_expand_creates_headroom(void){
  int w=64,h=64;
  pixel_hdr_surface_t s;
  pixel_hdr_surface_init(&s,w,h);
  /* mid-tone field with a bright accent, all legal SDR */
  uint8_t *src=malloc((size_t)w*h*3);
  memset(src,90,(size_t)w*h*3);
  for(int y=20;y<28;y++) for(int x=20;x<28;x++)
    for(int c=0;c<3;c++) src[((size_t)y*w+x)*3+c]=235;
  pixel_hdr_from_srgb(&s,src);
  size_t px=(size_t)w*h, over=0; float mx=0.0f;
  for(size_t i=0;i<px;i++){
    float l=pixel_hdr_luminance(s.rgb[i*3],s.rgb[i*3+1],s.rgb[i*3+2]);
    if(l>mx) mx=l;
  }
  CHECK(mx<=1.0f,"8-bit source cannot exceed 1.0 before expansion (got %.3f)",mx);
  pixel_hdr_expand_highlights(&s,0.30f,4.0f);
  mx=0.0f;
  for(size_t i=0;i<px;i++){
    float l=pixel_hdr_luminance(s.rgb[i*3],s.rgb[i*3+1],s.rgb[i*3+2]);
    if(l>mx) mx=l;
    if(l>1.0f) over++;
  }
  CHECK(over>0,"expand must lift some pixels above 1.0 or bloom is a no-op");
  CHECK(over<px/2,"expand must not lift half the frame (%zu of %zu)",over,px);
  CHECK(mx>1.0f,"peak must exceed 1.0 after expansion (got %.3f)",mx);
  printf("  expand(0.30,4.0): peak lum %.3f, %zu/%zu px over 1.0 (%.2f%%)\n",
         mx,over,px,100.0*(double)over/(double)px);
  /* and the bloom must then be observable end-to-end */
  uint8_t *nob=malloc((size_t)w*h*3), *wib=malloc((size_t)w*h*3);
  pixel_hdr_resolve_opts_t o; pixel_hdr_resolve_opts_default(&o);
  o.dither=false; o.bloom_strength=0.45f;
  o.bloom_levels=0; pixel_hdr_resolve(&s,nob,&o);
  o.bloom_levels=5; pixel_hdr_resolve(&s,wib,&o);
  int maxd=0;
  for(size_t i=0;i<px*3;i++){int d=abs((int)nob[i]-(int)wib[i]); if(d>maxd)maxd=d;}
  CHECK(maxd>0,"bloom must change pixels on an expanded scene (max delta %d)",maxd);
  printf("  bloom on vs off after expand: max delta %d\n",maxd);
  /* expansion must be monotone and leave shadows untouched */
  CHECK(1,"placeholder");
  free(src);free(nob);free(wib); pixel_hdr_surface_free(&s);
}

/* expand_highlights must be a no-op below the knee and never darken. */
static void test_expand_preserves_shadows(void){
  int w=32,h=1;
  pixel_hdr_surface_t s; pixel_hdr_surface_init(&s,w,h);
  for(int x=0;x<w;x++){
    float v=(float)x/(float)(w-1);
    s.rgb[(size_t)x*3+0]=v; s.rgb[(size_t)x*3+1]=v; s.rgb[(size_t)x*3+2]=v;
  }
  float before[32];
  for(int x=0;x<w;x++) before[x]=s.rgb[(size_t)x*3];
  pixel_hdr_expand_highlights(&s,0.5f,3.0f);
  int ok_low=1, ok_mono=1;
  for(int x=0;x<w;x++){
    float a=before[x], b=s.rgb[(size_t)x*3];
    if(b<a-1e-6f) ok_mono=0;                    /* never darkens */
    if(a<0.5f*0.99f && b>a+1e-4f) ok_low=0;     /* below knee untouched */
  }
  CHECK(ok_low,"expand must not touch pixels below the knee");
  CHECK(ok_mono,"expand must never darken a pixel");
  CHECK(s.rgb[(size_t)(w-1)*3]>before[w-1],"expand must lift the top end");
  pixel_hdr_surface_free(&s);
}

static void test_guards(void){
  pixel_hdr_surface_t s={0};
  CHECK(!pixel_hdr_surface_init(&s,0,10),"reject zero width");
  CHECK(!pixel_hdr_surface_init(&s,10,-1),"reject negative height");
  CHECK(!pixel_hdr_surface_init(&s,99999,99999),"reject oversized");
  CHECK(!pixel_hdr_resolve(NULL,NULL,NULL),"reject null resolve");
  pixel_hdr_surface_free(NULL);
  pixel_hdr_surface_free(&s);
  passes++;
}

int main(void){
  printf("== curves ==\n");        test_curves();
  printf("== round-trip ==\n");    test_roundtrip();
  printf("== bloom ==\n");         test_bloom_propagates();
  printf("== dither ==\n");        test_dither_banding();
  printf("== expand ==\n");        test_expand_creates_headroom();
                                   test_expand_preserves_shadows();
  printf("== guards ==\n");        test_guards();
  printf("\n%d passed, %d failed\n",passes,failures);
  return failures?1:0;
}
