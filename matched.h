/* zc_matched.h — diseñador analog-matched (matched-Z + corrección FIR HF-pesada) para Dynamo.
 * Reemplaza el par bilineal+oversampling del EQ por una forma cerrada SIN OS:
 *   1. raíces del analógico H(s)=B(s)/A(s)  (Durand-Kerner)
 *   2. matched-Z: polo/cero digital = exp(s/fs)  -> den D(z) grado nd, num N0(z) grado nz
 *   3. escala de ganancia DC exacta
 *   4. corrección FIR C(z) grado ZC_MZ_CORR (=5), |C|^2 ajustada por LS PESADO A AGUDOS a
 *      |Han|^2/|Hdig|^2, factorizada a mínima-fase por raíces del poli de autocorrelación
 *   -> H(z) = N0(z)·C(z) / D(z) : numerador grado (nz+ZC_MZ_CORR), denominador grado nd.
 * Los polos vienen SOLO de D(z) (matched-Z de un analógico estable) => SIEMPRE ESTABLE.
 * Portable: solo <math.h> y <complex.h>. Sin estado, sin asignaciones.  */
#ifndef ZC_MATCHED_H
#define ZC_MATCHED_H
#include <math.h>
#include <complex.h>

#ifndef ZC_MZ_CORR
#define ZC_MZ_CORR   5      /* orden de la corrección FIR (overridable: -DZC_MZ_CORR=N para barrer).
                             * 5 = robusto y medido: halva el peor caso vs 3 (HISHELF 0.87->0.45 dB,
                             * toda la rejilla mejora, 0 bolsas de mal-fit a 44.1/48/96k). NC>=8 el LS
                             * se vuelve mal-condicionado (mal-fit); 5-6 es la meseta segura. Nota: la
                             * corrección solo añade CEROS -> estable a cualquier NC (polos = matched-Z). */
#endif
#ifndef ZC_MZ_WPOW
#define ZC_MZ_WPOW   14.0   /* peso ~ (f/fnyq)^WPOW en el LS de la corrección (overridable) */
#endif
/* margen de arrays: el poli de autocorrelación de |C|^2 tiene grado 2*NC (raíces via Durand-Kerner
 * necesitan tamaño 2*NC+1), y el numerador final grado nz(<=2)+NC. 2*NC+2 cubre ambos. */
#define ZC_MZ_MAXNUM (2 * ZC_MZ_CORR + 2)

/* raíces de un polinomio real (coefs alto->bajo, grado n) por Durand-Kerner. */
static inline void zc_mz_roots(const double *p, int n, double _Complex *r) {
  if (n <= 0) return;
  if (n == 1) { r[0] = -p[1] / p[0]; return; }
  if (n == 2) {                                  /* cerrado: robusto ante raíz doble (disc=0) */
    double _Complex a1 = p[1] / p[0], a2 = p[2] / p[0];
    double _Complex disc = csqrt(a1 * a1 - 4.0 * a2);
    r[0] = (-a1 + disc) * 0.5; r[1] = (-a1 - disc) * 0.5; return;
  }
  double _Complex a[ZC_MZ_MAXNUM + 1];
  for (int k = 0; k <= n; k++) a[k] = p[k] / p[0];
  double _Complex seed = 0.4 + 0.9 * _Complex_I;
  for (int i = 0; i < n; i++) { double _Complex z = 1; for (int j = 0; j < i; j++) z *= seed; r[i] = z; }
  for (int it = 0; it < 8000; it++) {
    double mx = 0;
    for (int i = 0; i < n; i++) {
      double _Complex num = a[0];
      for (int k = 1; k <= n; k++) num = num * r[i] + a[k];
      double _Complex d = 1;
      for (int j = 0; j < n; j++) if (j != i) d *= (r[i] - r[j]);
      double _Complex dl = num / d; r[i] -= dl;
      double m = cabs(dl); if (m > mx) mx = m;
    }
    if (mx < 1e-15) break;
  }
}
/* polinomio (coefs asc: c[0]+c[1]z^-1+...) mónico a partir de nr raíces. */
static inline void zc_mz_poly(const double _Complex *r, int nr, double *c) {
  double _Complex cc[ZC_MZ_MAXNUM + 1];
  cc[0] = 1; for (int i = 1; i <= nr; i++) cc[i] = 0;
  for (int i = 0; i < nr; i++)
    for (int k = i + 1; k > 0; k--) cc[k] = cc[k] - r[i] * cc[k - 1];
  for (int k = 0; k <= nr; k++) c[k] = creal(cc[k]);
}
/* |H(e^jw)| en dB de una respuesta digital num[0..nb]/den[0..na] (coefs asc). */
static inline double zc_mz_digdb(const double *B, int nb, const double *A, int na, double w) {
  double _Complex z = cexp(-_Complex_I * w), n = 0, d = 0, zp = 1;
  for (int k = 0; k <= nb; k++) { n += B[k] * zp; zp *= z; }
  zp = 1; for (int k = 0; k <= na; k++) { d += A[k] * zp; zp *= z; }
  return 20.0 * log10(cabs(n / d));
}

/* Diseña el filtro digital matched a partir del analógico b/a (coefs DESC en s, longitudes nb_s/na_s).
 * Escribe B[0..*ndB] (num) y A[0..*ndA] (den) en coefs ASCENDENTES en z^-1, A[0]=1.
 * fs = sample rate.  */
static inline void zc_matched_design(const double *b_s, int nb_s, const double *a_s, int na_s,
                                     double fs, double *B, int *ndB, double *A, int *ndA) {
  /* strip de ceros líder en el numerador analógico (LP: b=[0,0,w0^2] -> ceros en el infinito).
   * Los ceros en s=inf no se mapean por matched-Z; se dejan fuera y la corrección FIR ajusta la
   * magnitud (incl. Nyquist) en su lugar. */
  int off = 0; while (off < nb_s - 1 && b_s[off] == 0.0) off++;
  b_s += off; nb_s -= off;
  int nz = nb_s - 1, np = na_s - 1;
  /* |Han(jw_digital)| en dB: s = j*w*fs  (w en rad/muestra) */
  #define ZC_MZ_ANDB(W) ({ double _Complex s = _Complex_I * (W) * fs, nu = b_s[0], de = a_s[0]; \
      for (int _k = 1; _k < nb_s; _k++) nu = nu * s + b_s[_k]; \
      for (int _k = 1; _k < na_s; _k++) de = de * s + a_s[_k]; 20.0 * log10(cabs(nu / de)); })
  /* 1-2. matched-Z de polos y ceros */
  double _Complex zr[ZC_MZ_MAXNUM], pr[ZC_MZ_MAXNUM], dz[ZC_MZ_MAXNUM], dp[ZC_MZ_MAXNUM];
  zc_mz_roots(b_s, nz, zr); zc_mz_roots(a_s, np, pr);
  /* refleja ceros del semiplano DERECHO (analógico no-mínima-fase) a LHP: preserva |H(jw)| EXACTO
   * (|jw-(σ+jω0)| = |jw-(-σ+jω0)|) y evita el overflow de exp(+σ/fs) en matched-Z. */
  for (int i = 0; i < nz; i++) if (creal(zr[i]) > 0.0) zr[i] = -creal(zr[i]) + cimag(zr[i]) * _Complex_I;
  for (int i = 0; i < nz; i++) dz[i] = cexp(zr[i] / fs);
  for (int i = 0; i < np; i++) dp[i] = cexp(pr[i] / fs);
  zc_mz_poly(dz, nz, B); zc_mz_poly(dp, np, A);
  int nb2 = nz, na2 = np;
  /* frecuencia de referencia para el anclaje de ganancia: DC si su ganancia NO es despreciable
   * (bells/shelves/LP → ancla en DC, plano y limpio, lejos de la acción HF de la corrección);
   * si la ganancia DC es ~0 (HP: cero en DC) → ancla en la frecuencia de máx |Han| (pasabanda). */
  double wdc = 1e-6 * 2.0 * M_PI / fs, gdc_db = ZC_MZ_ANDB(wdc);
  double wmax = wdc, gmax_db = -1e9;
  for (int m = 0; m < 400; m++) { double f = 20.0 * pow((fs * 0.5) / 20.0, (double)m / 399.0), w = 2.0 * M_PI * f / fs;
    double g = ZC_MZ_ANDB(w); if (g > gmax_db) { gmax_db = g; wmax = w; } }
  double wref = (gdc_db > gmax_db - 40.0) ? wdc : wmax;
  double gref = pow(10.0, ZC_MZ_ANDB(wref) / 20.0);   /* |Han(wref)| lineal */
  /* 3. escala para casar |Hdig(wref)| = gref */
  { double cur = pow(10.0, zc_mz_digdb(B, nb2, A, na2, wref) / 20.0);
    double sc = gref / cur; for (int k = 0; k <= nb2; k++) B[k] *= sc; }
  /* 4. corrección FIR grado NC: |C|^2 = sum_{m} rm cos(mw), LS pesado a HF sobre |Han|^2/|Hdig|^2 */
  const int NC = ZC_MZ_CORR, P = NC + 1;
  double S[ZC_MZ_CORR + 1][ZC_MZ_CORR + 1], T[ZC_MZ_CORR + 1];
  for (int i = 0; i < P; i++) { T[i] = 0; for (int j = 0; j < P; j++) S[i][j] = 0; }
  for (int m = 0; m < 500; m++) {
    double f = 20.0 * pow((fs * 0.5) / 20.0, (double)m / 499.0), w = 2.0 * M_PI * f / fs;
    double cur = zc_mz_digdb(B, nb2, A, na2, w), tg = ZC_MZ_ANDB(w);
    if (cur < -120.0 || tg < -120.0) continue;          /* evita 0/0 en los ceros (HP@DC, etc.) */
    double D2 = pow(10.0, (tg - cur) / 10.0);           /* (|Han|/|Hdig|)^2 */
    double wt = pow(f / (fs * 0.5), ZC_MZ_WPOW) + 0.02;
    double g[ZC_MZ_CORR + 1]; for (int aa = 0; aa < P; aa++) g[aa] = cos(aa * w);
    for (int aa = 0; aa < P; aa++) { for (int bb = 0; bb < P; bb++) S[aa][bb] += wt * g[aa] * g[bb]; T[aa] += wt * D2 * g[aa]; }
  }
  /* resuelve S rm = T (Gauss) */
  double rm[ZC_MZ_CORR + 1];
  for (int i = 0; i < P; i++) {
    int pv = i; for (int r = i + 1; r < P; r++) if (fabs(S[r][i]) > fabs(S[pv][i])) pv = r;
    for (int k = 0; k < P; k++) { double t = S[i][k]; S[i][k] = S[pv][k]; S[pv][k] = t; }
    double t = T[i]; T[i] = T[pv]; T[pv] = t;
    for (int r = 0; r < P; r++) if (r != i) { double fct = S[r][i] / S[i][i];
      for (int k = 0; k < P; k++) S[r][k] -= fct * S[i][k]; T[r] -= fct * T[i]; }
  }
  for (int i = 0; i < P; i++) rm[i] = T[i] / S[i][i];
  /* factoriza |C|^2 -> C mínima-fase: poli simétrico grado 2NC, toma raíces dentro del círculo */
  double lc[2 * ZC_MZ_CORR + 1]; for (int k = 0; k <= 2 * NC; k++) lc[k] = 0;
  lc[NC] = rm[0]; for (int mm = 1; mm <= NC; mm++) { lc[NC - mm] += rm[mm] / 2; lc[NC + mm] += rm[mm] / 2; }
  double hp[2 * ZC_MZ_CORR + 1]; for (int k = 0; k <= 2 * NC; k++) hp[k] = lc[2 * NC - k]; /* alto->bajo */
  double _Complex rr[2 * ZC_MZ_CORR]; zc_mz_roots(hp, 2 * NC, rr);
  double _Complex cin[ZC_MZ_CORR]; int ci = 0;
  for (int i = 0; i < 2 * NC && ci < NC; i++) if (cabs(rr[i]) < 1.0) cin[ci++] = rr[i];
  double cc[ZC_MZ_CORR + 1]; zc_mz_poly(cin, ci, cc);
  /* aplica C(z) al numerador: B <- B * C */
  double nn[ZC_MZ_MAXNUM + 1]; for (int k = 0; k <= nb2 + ci; k++) nn[k] = 0;
  for (int k = 0; k <= nb2; k++) for (int aa = 0; aa <= ci; aa++) nn[k + aa] += B[k] * cc[aa];
  for (int k = 0; k <= nb2 + ci; k++) B[k] = nn[k]; nb2 += ci;
  /* re-normaliza en wref tras la corrección */
  { double cur = pow(10.0, zc_mz_digdb(B, nb2, A, na2, wref) / 20.0);
    double sc = gref / cur; for (int k = 0; k <= nb2; k++) B[k] *= sc; }
  *ndB = nb2; *ndA = na2;
  #undef ZC_MZ_ANDB
}
#endif
