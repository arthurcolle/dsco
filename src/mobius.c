#include "mobius.h"
#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846
typedef struct { double x,y,z; } vec;
typedef struct { vec p,n; } vertex;
static vec sub(vec a,vec b){return (vec){a.x-b.x,a.y-b.y,a.z-b.z};}
static vec cross(vec a,vec b){return (vec){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static vec unit(vec a){double l=sqrt(a.x*a.x+a.y*a.y+a.z*a.z);return (vec){a.x/l,a.y/l,a.z/l};}
void mobius_point(double u,double v,double out[3]){
    double r=1+v*cos(u/2);
    out[0]=r*cos(u); out[1]=r*sin(u); out[2]=v*sin(u/2);
}
static vec rotate(vec p,double a){
    double x=cos(a)*p.x-sin(a)*p.y,y=sin(a)*p.x+cos(a)*p.y;
    return (vec){x,0.60*y-0.80*p.z,0.80*y+0.60*p.z};
}
static vertex sample(double u,double v,double a,int w,int h){
    double p[3],c=cos(u),s=sin(u),ch=cos(u/2),sh=sin(u/2),r=1+v*ch;
    mobius_point(u,v,p);
    vec du={-r*s-v*sh*c/2,r*c-v*sh*s/2,v*ch/2};
    vec dv={ch*c,ch*s,sh};
    vec n=rotate(unit(cross(du,dv)),a),q=rotate((vec){p[0],p[1],p[2]},a);
    double scale=0.25*(w<h?w:h);
    return (vertex){{w/2.0+scale*q.x,h/2.0-scale*q.y,q.z},n};
}
static double edge(vec a,vec b,double x,double y){return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);}
static void triangle(uint8_t *rgb,double *depth,int w,int h,vertex a,vertex b,vertex c){
    double area=edge(a.p,b.p,c.p.x,c.p.y);
    if(fabs(area)<1e-10)return;
    int x0=(int)fmax(0,floor(fmin(a.p.x,fmin(b.p.x,c.p.x))));
    int x1=(int)fmin(w-1,ceil(fmax(a.p.x,fmax(b.p.x,c.p.x))));
    int y0=(int)fmax(0,floor(fmin(a.p.y,fmin(b.p.y,c.p.y))));
    int y1=(int)fmin(h-1,ceil(fmax(a.p.y,fmax(b.p.y,c.p.y))));
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
        double wa=edge(b.p,c.p,x+0.5,y+0.5)/area;
        double wb=edge(c.p,a.p,x+0.5,y+0.5)/area,wc=1-wa-wb;
        if(wa< -1e-9||wb< -1e-9||wc< -1e-9)continue;
        double z=wa*a.p.z+wb*b.p.z+wc*c.p.z;
        size_t i=(size_t)y*w+x;
        if(z<=depth[i])continue;
        depth[i]=z;
        vec n=unit((vec){wa*a.n.x+wb*b.n.x+wc*c.n.x,
                        wa*a.n.y+wb*b.n.y+wc*c.n.y,
                        wa*a.n.z+wb*b.n.z+wc*c.n.z});
        /* No globally consistent outward normal exists: shade both sides. */
        if(n.z<0)n=(vec){-n.x,-n.y,-n.z};
        double diffuse=fmax(0,-0.36*n.x+0.48*n.y+0.80*n.z);
        double spec=pow(fmax(0,-0.19*n.x+0.25*n.y+0.949*n.z),48)*0.30;
        const double base[3]={0.20,0.53,0.70};
        for(int k=0;k<3;k++){
            double color=base[k]*(0.22+0.78*diffuse)+spec;
            rgb[3*i+k]=(uint8_t)(255*sqrt(fmin(1,color)));
        }
    }
}
bool mobius_render(uint8_t *rgb,int w,int h,double half_width,double angle){
    if(!rgb||w<32||h<32||w>2048||h>2048||!isfinite(half_width)||
       half_width<0.05||half_width>0.8||!isfinite(angle))return false;
    size_t count=(size_t)w*h;
    double *depth=malloc(count*sizeof(*depth));
    if(!depth)return false;
    for(size_t i=0;i<count;i++){depth[i]=-INFINITY;rgb[3*i]=rgb[3*i+1]=rgb[3*i+2]=255;}
    angle=remainder(angle,2*PI);
    /* Duplicate seam endpoints with reversed transverse coordinate naturally. */
    for(int i=0;i<256;i++)for(int j=0;j<32;j++){
        double u=2*PI*i/256,un=2*PI*(i+1)/256;
        double v=half_width*(2.0*j/32-1),vn=half_width*(2.0*(j+1)/32-1);
        vertex a=sample(u,v,angle,w,h),b=sample(un,v,angle,w,h);
        vertex c=sample(un,vn,angle,w,h),d=sample(u,vn,angle,w,h);
        triangle(rgb,depth,w,h,a,b,c);triangle(rgb,depth,w,h,a,c,d);
    }
    free(depth);return true;
}
