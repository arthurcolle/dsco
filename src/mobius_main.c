#include "mobius.h"
#include "kitty_graphics.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int sig){(void)sig;stopped=1;}
static void usage(void){
    fprintf(stderr,"Usage: dsco-mobius [--width 640] [--height 640] [--band 0.42]\n"
                   "                   [--angle 25] [--animate] [--frames 180] [--fps 24]\n"
                   "                   [--ppm PATH | --protocol]\n"
                   "Angles are degrees. --protocol explicitly emits Kitty bytes even to a pipe.\n"
                   "Animation is bounded; Ctrl-C stops early. PPM exports one still frame.\n");
}
static int number(const char *s,double lo,double hi,double *out){
    char *end;errno=0;double n=strtod(s,&end);
    if(errno||end==s||*end||!isfinite(n)||n<lo||n>hi)return 0;
    *out=n;return 1;
}
int main(int argc,char **argv){
    int w=640,h=640,frames=180,fps=24,animate=0,protocol=0;
    double band=0.42,angle=25;
    const char *ppm=NULL;
    for(int i=1;i<argc;i++){
        const char *arg=argv[i];
        if(!strcmp(arg,"--help")){usage();return 0;}
        if(!strcmp(arg,"--animate")){animate=1;continue;}
        if(!strcmp(arg,"--protocol")){protocol=1;continue;}
        if(i+1>=argc){usage();return 2;}
        const char *value=argv[++i];double n;
        if(!strcmp(arg,"--ppm")){ppm=value;continue;}
        if(!strcmp(arg,"--band")){if(!number(value,0.05,0.8,&band))return 2;}
        else if(!strcmp(arg,"--angle")){if(!number(value,-36000,36000,&angle))return 2;}
        else {
            double lo,hi;
            if(!strcmp(arg,"--width")||!strcmp(arg,"--height")){lo=32;hi=2048;}
            else if(!strcmp(arg,"--frames")){lo=1;hi=3600;}
            else if(!strcmp(arg,"--fps")){lo=1;hi=60;}
            else {usage();return 2;}
            if(!number(value,lo,hi,&n)||n!=floor(n)){usage();return 2;}
            if(!strcmp(arg,"--width"))w=(int)n;
            else if(!strcmp(arg,"--height"))h=(int)n;
            else if(!strcmp(arg,"--frames"))frames=(int)n;
            else fps=(int)n;
        }
    }
    if(ppm&&(animate||protocol)){usage();return 2;}
    if(!ppm&&!protocol&&(!isatty(fileno(stdout))||!kitty_graphics_available(stdout))){
        fprintf(stderr,"Kitty graphics not detected; use --ppm PATH for an image or --protocol for explicit wire output.\n");return 1;
    }
    size_t bytes=(size_t)w*h*3;
    uint8_t *rgb=malloc(bytes);if(!rgb)return 1;
    signal(SIGINT,stop);signal(SIGTERM,stop);
    /* Broken output pipes must still follow our cleanup path. */
    signal(SIGPIPE,SIG_IGN);
    int result=0;
    if(animate)fputs("\033[?1049h\033[?25l",stdout);
    for(int f=0;f<(animate?frames:1)&&!stopped;f++){
        double theta=angle*3.14159265358979323846/180+f*2*3.14159265358979323846/frames;
        if(!mobius_render(rgb,w,h,band,theta)){result=1;break;}
        if(ppm){
            FILE *out=fopen(ppm,"wb");
            if(!out){perror(ppm);result=1;break;}
            if(fprintf(out,"P6\n%d %d\n255\n",w,h)<0||fwrite(rgb,1,bytes,out)!=bytes)result=1;
            if(fclose(out))result=1;
        }else{
            char control[128];
            /* Reuse a single image/placement rather than accumulating frames. */
            if(animate)fputs("\033[H",stdout);
            snprintf(control,sizeof(control),"a=T,t=d,f=24,s=%d,v=%d,i=73421,p=1,q=2",w,h);
            if(!kitty_graphics_send_pixels(stdout,control,rgb,bytes,NULL)||fflush(stdout)){result=1;break;}
        }
        if(animate&&!stopped){
            struct timespec delay={0,1000000000L/fps};
            while(nanosleep(&delay,&delay)<0&&errno==EINTR&&!stopped){}
        }
    }
    if(animate){
        fputs("\033_Ga=d,d=I,i=73421,q=2\033\\\033[?25h\033[?1049l",stdout);
        if(fflush(stdout))result=1;
    }else if(!ppm){fputc('\n',stdout);if(fflush(stdout))result=1;}
    free(rgb);return result;
}
