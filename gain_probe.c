/* gain_probe.c -- measures the REAL gain curve (norm -> dB of peak deviation) of each
 * GAIN knob of the plugin, with a chirp + FFT (takes the maximum |dB| deviation with its sign).
 * Output: tables ready to paste into the GUI (exact read-out + honest ticks). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include "ports.h"
#include "uris.h"

#define SR 48000.0
#define N  65536

static float ports[ZC_N_PORTS];
static float inL[N], inR[N], outL[N], outR[N];

/* single-bin DFT on a log frequency grid; enough to read the filter curve */
static void band_response(double *fout, double *gout, int *nout) {
  /* 60 log points from 20 to 22000 Hz */
  int n = 60; double lo = 20, hi = 22000;
  for (int k = 0; k < n; k++) {
    double f = lo * pow(hi/lo, (double)k/(n-1));
    double w = 2*M_PI*f/SR;
    double ri=0,ii=0,ro=0,io=0;
    for (int i=0;i<N;i++){ double c=cos(w*i),s=sin(w*i);
      ri+=inL[i]*c; ii-=inL[i]*s; ro+=outL[i]*c; io-=outL[i]*s; }
    double mi=sqrt(ri*ri+ii*ii), mo=sqrt(ro*ro+io*io);
    fout[k]=f; gout[k]=20*log10((mo+1e-15)/(mi+1e-15));
  }
  *nout=n;
}

int main(int argc, char **argv) {
  const char *so = argc>1?argv[1]:"build/Dynamo-0C.so";
  void *h=dlopen(so,RTLD_NOW); if(!h){fprintf(stderr,"%s\n",dlerror());return 1;}
  LV2_Descriptor_Function df=(LV2_Descriptor_Function)dlsym(h,"lv2_descriptor");
  if(!df){fprintf(stderr,"dlsym lv2_descriptor: %s\n",dlerror());return 1;}
  const LV2_Descriptor *d=df(0);
  if(!d){fprintf(stderr,"null descriptor\n");return 1;}
  LV2_Handle inst=d->instantiate(d,SR,"",NULL);
  if(!inst){fprintf(stderr,"instantiate failed\n");return 1;}
  for(int i=0;i<ZC_N_PORTS;i++) ports[i]=0;
  ports[P_POWER]=1; ports[P_EQ_ON]=1; ports[P_OVERSAMPLE]=2; ports[P_LP_FREQ]=1.0f;
  ports[P_HI_FREQ]=ports[P_HIMID_FREQ]=ports[P_LOMID_FREQ]=ports[P_LO_FREQ]=0.5f;
  ports[P_HI_GAIN]=ports[P_HIMID_GAIN]=ports[P_LOMID_GAIN]=ports[P_LO_GAIN]=0.5f;
  ports[P_HP_ON]=0; ports[P_LP_ON]=0;
  for(int i=0;i<ZC_N_PORTS;i++) d->connect_port(inst,i,&ports[i]);
  d->connect_port(inst,P_IN_L,inL); d->connect_port(inst,P_IN_R,inR);
  d->connect_port(inst,P_OUT_L,outL); d->connect_port(inst,P_OUT_R,outR);
  d->activate(inst);

  /* chirp log 20->22k */
  for(int i=0;i<N;i++){ double t=(double)i/N; double f=20*pow(22000.0/20.0,t);
    inL[i]=0.2f*sinf((float)(2*M_PI*f*i/SR)); }
  memcpy(inR,inL,sizeof inL);

  struct { const char*name; int gport; int shelfport; } K[]={
    {"HI",   P_HI_GAIN,    P_HF_SHELF},
    {"HIMID",P_HIMID_GAIN, -1},
    {"LOMID",P_LOMID_GAIN, -1},
    {"LO",   P_LO_GAIN,    P_LF_SHELF},
  };
  double norms[11]; for(int k=0;k<11;k++) norms[k]=k/10.0;
  double fr[60],gr[60]; int nr;

  for(int mode=0; mode<2; mode++){   /* 0=bell 1=shelf (HI/LO only) */
    printf("\n=== %s ===\n", mode? "SHELVES (HI/LO)":"BELLS");
    for(int b=0;b<4;b++){
      if(mode==1 && K[b].shelfport<0) continue;
      if(K[b].shelfport>=0) ports[K[b].shelfport]= mode?1:0;
      printf("%-6s", K[b].name);
      for(int k=0;k<11;k++){
        ports[K[b].gport]=(float)norms[k];
        d->run(inst,N);
        band_response(fr,gr,&nr);
        /* signed peak deviation (the largest |dB|) */
        double pk=0; for(int j=0;j<nr;j++) if(fabs(gr[j])>fabs(pk)) pk=gr[j];
        printf(" %+6.2f", pk);
        ports[K[b].gport]=0.5f;
      }
      printf("\n");
    }
  }
  printf("\n(columns: norm = 0.0 0.1 ... 1.0)\n");
  d->cleanup(inst); dlclose(h);
  return 0;
}
