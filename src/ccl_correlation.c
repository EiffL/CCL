#include "gsl/gsl_integration.h"
#include "ccl_cls.h"
#include "gsl/gsl_errno.h"
#include "gsl/gsl_roots.h"
#include "gsl/gsl_spline.h"
#include "gsl/gsl_sf_bessel.h"
#include "gsl/gsl_sf_legendre.h"
#include "ccl_error.h"
#include "ccl_utils.h"
#include "ccl_correlation.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
//#include "ccl_power.h"
#include "ccl.h"
#include "fftlog.h"
#include "jacobi.h"

/*--------ROUTINE: taper_cl ------
TASK:n Apply cosine tapering to Cls to reduce aliasing
INPUT: number of ell bins for Cl, ell vector, C_ell vector, limits for tapering
       e.g., ell_limits=[low_ell_limit_lower,low_ell_limit_upper,high_ell_limit_lower,high_ell_limit_upper]
*/
static int taper_cl(int n_ell,double *ell,double *cl, double *ell_limits)
{

  for(int i=0;i<n_ell;i++) {
    if(ell[i]<ell_limits[0] || ell[i]>ell_limits[3]) {
      cl[i]=0;//ell outside desirable range
      continue;
    }
    if(ell[i]>=ell_limits[1] && ell[i]<=ell_limits[2])
      continue;//ell within good ell range

    if(ell[i]<ell_limits[1])//tapering low ell
      cl[i]*=cos((ell[i]-ell_limits[1])/(ell_limits[1]-ell_limits[0])*M_PI/2.);

    if(ell[i]<ell_limits[1])//tapering low ell
      cl[i]*=cos((ell[i]-ell_limits[1])/(ell_limits[1]-ell_limits[0])*M_PI/2.);

    if(ell[i]>ell_limits[2])//tapering high ell
      cl[i]*=cos((ell[i]-ell_limits[2])/(ell_limits[3]-ell_limits[2])*M_PI/2.);
  }

  return 0;
}

static void interpolate_extrapolate_cl(ccl_cosmology *cosmo,double *l_arr, double *cl_arr,
                                        double *ell_inp, double *cl_inp, int n_ell_inp, int *status)
{
  int i;
  SplPar *cl_spl=ccl_spline_init(n_ell_inp,ell_inp,cl_inp,cl_inp[0],0);
  if(cl_spl==NULL) {
    //free(l_arr);
    //free(cl_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }

  double cl_tilt,l_edge,cl_edge;
  l_edge=ell_inp[n_ell_inp-1];
  if((cl_inp[n_ell_inp-1]*cl_inp[n_ell_inp-2]<0) || (cl_inp[n_ell_inp-2]==0)) {
    cl_tilt=0;
    cl_edge=0;
  }
  else {
    cl_tilt=log(cl_inp[n_ell_inp-1]/cl_inp[n_ell_inp-2])/log(ell_inp[n_ell_inp-1]/ell_inp[n_ell_inp-2]);
    cl_edge=cl_inp[n_ell_inp-1];
  }
  for(i=0;i<N_ELL_FFTLOG;i++) {
    if(l_arr[i]>=l_edge)
      cl_arr[i]=cl_edge*pow(l_arr[i]/l_edge,cl_tilt);
    else
      cl_arr[i]=ccl_spline_eval(l_arr[i],cl_spl);
  }
  ccl_spline_free(cl_spl);

return;
}

/*--------ROUTINE: ccl_tracer_corr_fftlog ------
TASK: For a given tracer, get the correlation function
      Following function takes a function to calculate angular cl as well.
      By default above function will call it using ccl_angular_cl
INPUT: type of tracer, number of theta values to evaluate = NL, theta vector
 */
static void ccl_tracer_corr_fftlog_projected(ccl_cosmology *cosmo,
				   int n_ell,double *ell,double *cls,
				   int n_theta,double *theta,double *wtheta,
				   int corr_type,int corr_space,
				   int do_taper_cl,double *taper_cl_limits,
				   int *status)
{
  int i;
  double *l_arr,*cl_arr,*th_arr,*wth_arr;
  /*if (corr_space == CCL_CORR_PHYS)
    {
      l_arr=ccl_log_spacing(k_MIN_FFTLOG,k_MAX_FFTLOG,N_ELL_FFTLOG);
    }*/
  //else if (corr_space == CCL_CORR_ANG){
      l_arr=ccl_log_spacing(ELL_MIN_FFTLOG,ELL_MAX_FFTLOG,N_ELL_FFTLOG);
    //}

  if(l_arr==NULL) {
    *status=CCL_ERROR_LINSPACE;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }
  cl_arr=malloc(N_ELL_FFTLOG*sizeof(double));
  if(cl_arr==NULL) {
    free(l_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }

  //Interpolate input Cl into array needed for FFTLog
  interpolate_extrapolate_cl(cosmo,l_arr,cl_arr,ell,cls,n_ell,status);
//exit if status is not good

  if (do_taper_cl)
    taper_cl(N_ELL_FFTLOG,l_arr,cl_arr,taper_cl_limits);

  th_arr=malloc(sizeof(double)*N_ELL_FFTLOG);
  if(th_arr==NULL) {
    free(l_arr);
    free(cl_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }
  wth_arr=(double *)malloc(sizeof(double)*N_ELL_FFTLOG);
  if(wth_arr==NULL) {
    free(l_arr); free(cl_arr); free(th_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }

  for(i=0;i<N_ELL_FFTLOG;i++)
    th_arr[i]=0;
  //Although set here to 0, theta is modified by FFTlog to obtain the correlation at ~1/l

  // FFTlog uses spherical bessel functions, j_n, but for projected
  // correlations we need bessel functions of first order, J_n.
  // To compensate for the difference, we use the relation
  // j_n(x) = sqrt(Pi/2x)J_{n+1/2}(x)
  // J_{m}(x) = sqrt(2x/Pi) j_{m-1/2}(x)
  int i_bessel=0;
  //if(corr_type==CCL_CORR_GG) i_bessel=0;
  if(corr_type==CCL_CORR_GL) i_bessel=2;
  //if(corr_type==CCL_CORR_LP) i_bessel=0;
  if(corr_type==CCL_CORR_GG) i_bessel=0;
  if(corr_type==CCL_CORR_GL) i_bessel=2;
  if(corr_type==CCL_CORR_LP) i_bessel=0;
  if(corr_type==CCL_CORR_LM) i_bessel=4;

  fftlog_ComputeXiLM(i_bessel-0.5,1.5,N_ELL_FFTLOG,l_arr,cl_arr,th_arr,wth_arr);
  for(i=0;i<N_ELL_FFTLOG;i++)
    wth_arr[i]*=sqrt(th_arr[i]*2.0*M_PI);

  // Interpolate to output values of theta
  SplPar *wth_spl=ccl_spline_init(N_ELL_FFTLOG,th_arr,wth_arr,wth_arr[0],0);
  for(i=0;i<n_theta;i++)
    wtheta[i]=ccl_spline_eval(theta[i]*M_PI/180.,wth_spl);
  ccl_spline_free(wth_spl);

  free(l_arr); free(cl_arr);
  free(th_arr); free(wth_arr);

  return;
}

static void ccl_tracer_corr_fftlog_3D(ccl_cosmology *cosmo,
				   int n_k,double *k,double *pk,
				   int n_r,double *r,double *xi,
				   int corr_type,int corr_space,
				   int do_taper_cl,double *taper_cl_limits,
				   int *status)
{
  int i;
  double *k_arr,*pk_arr,*r_arr,*xi_arr;
  if (corr_space == CCL_CORR_PHYS)
      k_arr=ccl_log_spacing(k_MIN_FFTLOG,k_MAX_FFTLOG,N_ELL_FFTLOG);
  else
    {
      *status=CCL_ERROR_LINSPACE;
      strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog_3D wrong corr_space\n");
      return;
    }
  if(k_arr==NULL) {
    *status=CCL_ERROR_LINSPACE;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog_3D ran out of memory\n");
    return;
  }
  pk_arr=malloc(N_ELL_FFTLOG*sizeof(double));
  if(pk_arr==NULL) {
    free(k_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog_3D ran out of memory\n");
    return;
  }

  //Interpolate input Cl into array needed for FFTLog
  interpolate_extrapolate_cl(cosmo,k_arr,pk_arr,k,pk,n_k,status);
//exit if status is not good

  if (do_taper_cl)
    taper_cl(N_ELL_FFTLOG,k_arr,pk_arr,taper_cl_limits);

  r_arr=malloc(sizeof(double)*N_ELL_FFTLOG);
  if(r_arr==NULL) {
    free(k_arr);
    free(pk_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog_3D ran out of memory\n");
    return;
  }
  xi_arr=(double *)malloc(sizeof(double)*N_ELL_FFTLOG);
  if(xi_arr==NULL) {
    free(k_arr); free(pk_arr); free(r_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_fftlog ran out of memory\n");
    return;
  }

  for(i=0;i<N_ELL_FFTLOG;i++)
    r_arr[i]=0;
  //Although set here to 0, theta is modified by FFTlog to obtain the correlation at ~1/l

  int i_bessel=0;
  //if(corr_type==CCL_CORR_GG) i_bessel=0;
  if(corr_type==CCL_CORR_GG_QUAD) i_bessel=2;

  fftlog_ComputeXiLM(i_bessel,2,N_ELL_FFTLOG,k_arr,pk_arr,r_arr,xi_arr);//check 2

  // Interpolate to output values of theta
  SplPar *wth_spl=ccl_spline_init(N_ELL_FFTLOG,r_arr,xi_arr,xi_arr[0],0);
  for(i=0;i<n_r;i++)
    xi[i]=ccl_spline_eval(r[i],wth_spl);
  ccl_spline_free(wth_spl);

  free(k_arr); free(pk_arr);
  free(r_arr); free(xi_arr);

  return;
}

typedef struct {
  int nell;
  double ell0;
  double ellf;
  double cl0;
  double clf;
  int extrapol_0;
  int extrapol_f;
  double tilt0;
  double tiltf;
  SplPar *cl_spl;
  int i_bessel;
  double th;
} corr_int_par;

static double corr_bessel_integrand(double l,void *params)
{
  double cl,jbes;
  corr_int_par *p=(corr_int_par *)params;
  double x=l*p->th;

  if(l<p->ell0) {
    if(p->extrapol_0)
      cl=p->cl0*pow(l/p->ell0,p->tilt0);
    else
      cl=0;
  }
  else if(l>p->ellf) {
    if(p->extrapol_f)
      cl=p->clf*pow(l/p->ellf,p->tiltf);
    else
      cl=0;
  }
  else
    cl=ccl_spline_eval(l,p->cl_spl);

  if(p->i_bessel)
    jbes=gsl_sf_bessel_Jn(p->i_bessel,x);
  else
    jbes=gsl_sf_bessel_J0(x);

  return l*jbes*cl;
}

static void ccl_tracer_corr_bessel(ccl_cosmology *cosmo,
				   int n_ell,double *ell,double *cls,
				   int n_theta,double *theta,double *wtheta,
				   int corr_type,int *status)
{
  corr_int_par *cp=malloc(sizeof(corr_int_par));
  if(cp==NULL) {
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_bessel ran out of memory\n");
    return;
  }

  cp->nell=n_ell;
  cp->ell0=ell[0];
  cp->ellf=ell[n_ell-1];
  cp->cl0=cls[0];
  cp->clf=cls[n_ell-1];
  cp->cl_spl=ccl_spline_init(n_ell,ell,cls,cls[0],0);
  if(cp->cl_spl==NULL) {
    free(cp);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_bessel ran out of memory\n");
    return;
  }
  if(corr_type==CCL_CORR_GG)
    cp->i_bessel=0;
  else if(corr_type==CCL_CORR_GL)
    cp->i_bessel=2;
  else if(corr_type==CCL_CORR_LP)
    cp->i_bessel=0;
  else if(corr_type==CCL_CORR_LM)
    cp->i_bessel=4;

  if(cls[0]*cls[1]<=0)
    cp->extrapol_0=0;
  else {
    cp->extrapol_0=1;
    cp->tilt0=log10(cls[1]/cls[0])/log10(ell[1]/ell[0]);
  }

  if(cls[n_ell-2]*cls[n_ell-1]<=0)
    cp->extrapol_f=0;
  else {
    cp->extrapol_f=1;
    cp->tiltf=log10(cls[n_ell-1]/cls[n_ell-2])/log10(ell[n_ell-1]/ell[n_ell-2]);
  }

  int ith;
  double result,eresult;
  gsl_function F;
  gsl_integration_workspace *w=gsl_integration_workspace_alloc(1000);
  for(ith=0;ith<n_theta;ith++) {
    cp->th=theta[ith]*M_PI/180;
    F.function=&corr_bessel_integrand;
    F.params=cp;
    *status=gsl_integration_qag(&F,0,ELL_MAX_FFTLOG,0,1E-4,1000,GSL_INTEG_GAUSS41,w,&result,&eresult);
    wtheta[ith]=result/(2*M_PI);
  }
  gsl_integration_workspace_free(w);
  ccl_spline_free(cp->cl_spl);
  free(cp);
}



/*--------ROUTINE: ccl_compute_legendre_polynomial ------
TASK: Compute input factor for ccl_tracer_corr_legendre
INPUT: tracer 1, tracer 2, i_bessel, theta array, n_theta, L_max, output Pl_theta
XXX Use only for GG or GL.
 */
static void ccl_compute_legendre_polynomial(int corr_type,int n_theta,double *theta,
					    int ell_max,double **Pl_theta)
{
  int i,j;
  double k=0;

  //Initialize Pl_theta
  for (i=0;i<n_theta;i++) {
    for (j=0;j<ell_max;j++)
      Pl_theta[i][j]=0.;
  }

  if(corr_type==CCL_CORR_GG) {
    for (i=0;i<n_theta;i++) {
      gsl_sf_legendre_Pl_array(ell_max,cos(theta[i]*M_PI/180),Pl_theta[i]);
      for (j=0;j<=ell_max;j++)
	Pl_theta[i][j]*=(2*j+1);
    }
  }
  else if(corr_type==CCL_CORR_GL) {
    for (i=0;i<n_theta;i++) {//https://arxiv.org/pdf/1007.4809.pdf
      for (j=2;j<=ell_max;j++) {
	Pl_theta[i][j]=gsl_sf_legendre_Plm(j,2,cos(theta[i]*M_PI/180));
	Pl_theta[i][j]*=(2*j+1.)/((j+0.)*(j+1.));// this assuming input is convergence power spectrum
      }
    }
  }
  // else if(corr_type==CCL_CORR_LP) {
  //   for (int i=0;i<n_theta;i++){
  //     gsl_sf_legendre_Pl_array(ell_max,cos(theta[i]*M_PI/180),Pl_theta[i]);
  //     for (int j=0;j<=ell_max;j++){
	//         Pl_theta[i][j]*=(2*j+1);
  //     }
  //   }
  // }
  //
  // else if(corr_type==CCL_CORR_LM) {
  //   for (int i=0;i<n_theta;i++) {
  //     for (int j=0;j<=ell_max;j++) {
	// if(j>1e4) {///////////Some theta points thrown away for speed
	//   Pl_theta[i][j]=0;
	//   continue;
	// }
	// if (j<4) {
	//   Pl_theta[i][j]=0;
	//   continue;
	// }
	// Pl_theta[i][j]=gsl_sf_legendre_Plm(j,4,cos(theta[i]*M_PI/180));
	// Pl_theta[i][j]*=(2*j+1)*pow(j,4);//approximate.. Using relation between bessel and legendre functions from Steibbens96.
	// //this again works only for matter correlation function
	// for (k=-3;k<=4;k++)
	//   Pl_theta[i][j]/=(j+k);
  //     }
  //   }
  // }
}


/*--------ROUTINE: ccl_compute_wigner_d_matrix ------
https://en.wikipedia.org/wiki/Wigner_D-matrix (small-d matrix)
 */

static void ccl_compute_wigner_3d_matrix(int corr_type,int n_theta,double *theta,
                                            int ell_max,double **Pl_theta)
{
  int i,j;
  double mF=0;
  int m1=2,m2=2;

  if(corr_type==CCL_CORR_LM)
    m2=-2;//FIXME check this
  if(corr_type==CCL_CORR_GG||corr_type==CCL_CORR_GL)//FIXME can also set m2. Check for speed.
    ccl_compute_legendre_polynomial(corr_type, n_theta,theta,
                ell_max,Pl_theta);

  int a=0,b=0,k_m=0,lambda=0,k=0;
  double *sin_theta2=malloc((n_theta)*sizeof(double));
  double *cos_theta2=malloc((n_theta)*sizeof(double));
  double *cos_theta=malloc((n_theta)*sizeof(double));
  //Initialize Pl_theta
  for (i=0;i<n_theta;i++) {
    sin_theta2[i]=sin(theta[i]/2.);
    cos_theta2[i]=cos(theta[i]/2.);
    cos_theta[i]=cos(theta[i]);
    for (j=0;j<ell_max;j++)
      Pl_theta[i][j]=0.;
  }
  //  gsl_sf_choose (unsigned int n, unsigned int m);
  //double jac_jacobi (double x, int n, double a, double b);

  if (m1>m2)
    {
      a=m1-m2;
      lambda=a;
    }
  else
    a=m2-m1;

  if (absolute(m1)>absolute(m2))
    k_m=absolute(m1);
  else
    k_m=absolute(m2);
  b=2*k_m-a;

  for (j=0;j<ell_max;j++)
    {
      k=j-k_m;
      mF=sqrt(gsl_sf_choose (2*j-k,k+a)/gsl_sf_choose (k+b,b));
      mF*=pow(-1,lambda);
      for (i=0;i<n_theta;i++) {
	       Pl_theta[i][j]=mF*pow(sin_theta2[i],a)*pow(cos_theta2[i],b)*jac_jacobi(cos_theta[i], j,
                                                                                  a, b);
      }
    }
}


/*--------ROUTINE: ccl_tracer_corr_legendre ------
TASK: Compute correlation function via Legendre polynomials
INPUT: cosmology, number of theta bins, theta array, tracer 1, tracer 2, i_bessel, boolean
       for tapering, vector of tapering limits, correlation vector, angular_cl function.
 */
static void ccl_tracer_corr_legendre(ccl_cosmology *cosmo,
				     int n_ell,double *ell,double *cls,
				     int n_theta,double *theta,double *wtheta,
				     int corr_type,int do_taper_cl,double *taper_cl_limits,
				     int *status)
{
  int i;
  double *l_arr,*cl_arr;

  l_arr=malloc((ELL_MAX_FFTLOG+1)*sizeof(double));
  if(l_arr==NULL) {
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_legendre ran out of memory\n");
    return;
  }
  cl_arr=malloc((ELL_MAX_FFTLOG+1)*sizeof(double));
  if(cl_arr==NULL) {
    free(l_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_legendre ran out of memory\n");
    return;
  }

  if(corr_type==CCL_CORR_LM)
    printf("WARNING: legendre sum for xi- is still not correctly implemented.\n");

  //Interpolate input Cl into
  interpolate_extrapolate_cl(cosmo,l_arr,cl_arr,ell,cls,n_ell,status);
  //Interpolate input Cl into
  SplPar *cl_spl=ccl_spline_init(n_ell,ell,cls,cls[0],0);
  if(cl_spl==NULL) {
    free(cl_arr);
    free(l_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_legendre ran out of memory\n");
    return;
  }

  double cl_tilt,l_edge,cl_edge;
  l_edge=ell[n_ell-1];
  if((cls[n_ell-1]*cls[n_ell-2]<0) || (cls[n_ell-2]==0)) {
    cl_tilt=0;
    cl_edge=0;
  }
  else {
    cl_tilt=log(cls[n_ell-1]/cls[n_ell-2])/log(ell[n_ell-1]/ell[n_ell-2]);
    cl_edge=cls[n_ell-1];
  }
  for(i=0;i<=ELL_MAX_FFTLOG;i++) {
    double l=(double)i;
    l_arr[i]=l;
    if(l>=l_edge)
      cl_arr[i]=cl_edge*pow(l/l_edge,cl_tilt);
    else
      cl_arr[i]=ccl_spline_eval(l,cl_spl);
  }
  ccl_spline_free(cl_spl);

  if (do_taper_cl)
    *status=taper_cl(ELL_MAX_FFTLOG+1,l_arr,cl_arr,taper_cl_limits);

  double **Pl_theta;
  Pl_theta=malloc(n_theta*sizeof(double *));
  if(Pl_theta==NULL) {
    free(cl_arr);
    free(l_arr);
    *status=CCL_ERROR_MEMORY;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_legendre ran out of memory\n");
    return;
  }
  for (int i=0;i<n_theta;i++) {
    Pl_theta[i]=malloc(sizeof(double)*(ELL_MAX_FFTLOG+1));
    if(Pl_theta[i]==NULL) {
      int j;
      free(cl_arr);
      free(l_arr);
      for(j=0;j<i;j++)
	     free(Pl_theta[j]);
      free(Pl_theta);
      *status=CCL_ERROR_MEMORY;
      strcpy(cosmo->status_message,"ccl_correlation.c: ccl_tracer_corr_legendre ran out of memory\n");
      return;
    }
  }
  ccl_compute_wigner_3d_matrix(corr_type,n_theta,theta,ELL_MAX_FFTLOG,Pl_theta);

  for (int i=0;i<n_theta;i++) {
    wtheta[i]=0;
    for(int i_L=1;i_L<ELL_MAX_FFTLOG;i_L+=1)
      wtheta[i]+=cl_arr[i_L]*Pl_theta[i][i_L];
    wtheta[i]/=(M_PI*4);
  }

  for (int i=0;i<n_theta;i++)
    free(Pl_theta[i]);
  free(Pl_theta);
  free(l_arr);
  free(cl_arr);
}

/*--------ROUTINE: ccl_tracer_corr ------
TASK: For a given tracer, get the correlation function. Do so by running
      ccl_angular_cls. If you already have Cls calculated, go to the next
      function to pass them directly.
INPUT: cosmology, number of theta values to evaluate = NL, theta vector,
       tracer 1, tracer 2, i_bessel, key for tapering, limits of tapering
       correlation function.
 */
void ccl_correlation(ccl_cosmology *cosmo,
		     int n_ell,double *ell,double *cls,
		     int n_theta,double *theta,double *wtheta,
		     int corr_type,int corr_space,int do_taper_cl,double *taper_cl_limits,
		     int flag_method,int *status)
{
  //  int corr_space=CCL_CORR_ANG;
  //printf("ccl_correlation.c: ccl_tracer_corr doing something\n");
  if(flag_method==CCL_CORR_FFTLOG_PROJECTED) {
    ccl_tracer_corr_fftlog_projected(cosmo,n_ell,ell,cls,n_theta,theta,wtheta,corr_type,corr_space,
			   do_taper_cl,taper_cl_limits,status);
  }
  else if(flag_method==CCL_CORR_FFTLOG_3D) {
    ccl_tracer_corr_fftlog_3D(cosmo,n_ell,ell,cls,n_theta,theta,wtheta,corr_type,corr_space,
			   do_taper_cl,taper_cl_limits,status);
  }
  else if(flag_method==CCL_CORR_LGNDRE) {//corr_space should be 'l' or 'ell'
    ccl_tracer_corr_legendre(cosmo,n_ell,ell,cls,n_theta,theta,wtheta,corr_type,
			     do_taper_cl,taper_cl_limits,status);
  }
  else if(flag_method==CCL_CORR_BESSEL) {
    ccl_tracer_corr_bessel(cosmo,n_ell,ell,cls,n_theta,theta,wtheta,corr_type,status);
  }
  else {
    *status=CCL_ERROR_INCONSISTENT;
    strcpy(cosmo->status_message,"ccl_correlation.c: ccl_correlation. Unknown algorithm\n");
  }

  ccl_check_status(cosmo,status);
}
