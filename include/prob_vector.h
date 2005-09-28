#ifndef PROB_VECTOR
#define PROB_VECTOR

#include <vector.h>

typedef enum {LOWER, UPPER} p_val_type;

void pv_stats(Vector *p, double *mean, double *var);
void pv_confidence_interval(Vector *p, double size, int *interval_min, 
                            int *interval_max);
int* pv_quantiles(Vector *p);
double pv_p_value(Vector *distrib, double x_0, p_val_type side);
void pv_normalize(Vector *p);
Vector *pv_convolve(Vector *p, int n, double epsilon);
Vector **pv_convolve_save(Vector *p, int n, double epsilon);
Vector *pv_convolve_many(Vector **p, int *counts, int n, double epsilon);
Vector *pv_poisson(double lambda, double epsilon);
Vector *pv_convolve_fast(Vector *p, int n, double epsilon);

#endif
