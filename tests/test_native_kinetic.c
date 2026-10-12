#include "native_kinetic.h"
#include <assert.h>
#include <math.h>
int main(void) {
 double delta=0;
 for(int i=0;i<1000;i++) {
  double x=i/1000.0,a=native_kinetic_energy(x,.1),b=native_kinetic_energy(x,.4);
  assert(a>=0 && a<=1); assert(fabs(a-native_kinetic_energy(x,1.1))<1e-12);
  delta+=fabs(a-b);
 }
 assert(delta>10); assert(native_kinetic_energy(NAN,0)==0);
 for (int i=0;i<10000;i++) {
  double x=(i-5000)/137.0, p=i/71.0;
  native_kinetic_sample_t a=native_kinetic_sample(x,p,.01);
  native_kinetic_sample_t b=native_kinetic_sample(x,p+1,.01);
  assert(isfinite(a.energy) && a.energy>=0 && a.energy<=1);
  assert(a.filament>=0 && a.filament<=1);
  assert(a.spectral>=0 && a.spectral<=1);
  assert(fabs(a.energy-b.energy)<1e-10);
 }
 assert(native_kinetic_sample(0,INFINITY,.1).energy==0);
 assert(native_kinetic_sample(0,0,NAN).energy==0);
 return 0;
}
