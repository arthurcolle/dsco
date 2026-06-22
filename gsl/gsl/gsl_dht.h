/* gsl_dht.h — Discrete Hankel Transform */
#ifndef __GSL_DHT_H__
#define __GSL_DHT_H__

typedef struct {
    size_t size;
    double nu;
    double xmax;
    double *j;
    double *Jjj;
    double *J2;
} gsl_dht;

gsl_dht *gsl_dht_alloc(size_t size);
gsl_dht *gsl_dht_new(size_t size, double nu, double xmax);
int gsl_dht_init(gsl_dht *t, double nu, double xmax);
void gsl_dht_free(gsl_dht *t);
double gsl_dht_x_sample(const gsl_dht *t, size_t n);
double gsl_dht_k_sample(const gsl_dht *t, size_t n);
int gsl_dht_apply(const gsl_dht *t, double *f_in, double *f_out);
int gsl_dht_radial(const gsl_dht *t, double *f_in, double *f_out, double r, double k);

#endif /* __GSL_DHT_H__ */