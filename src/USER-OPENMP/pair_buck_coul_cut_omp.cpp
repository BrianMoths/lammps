/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Eduardo Bringa (LLNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "pair_buck_coul_cut_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"

#if defined(_OPENMP)
#include <omp.h>
#endif

using namespace LAMMPS_NS;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

/* ---------------------------------------------------------------------- */

PairBuckCoulCutOMP::PairBuckCoulCutOMP(LAMMPS *lmp) : PairOMP(lmp) {}

/* ---------------------------------------------------------------------- */

PairBuckCoulCutOMP::~PairBuckCoulCutOMP()
{
  if (allocated) {
    memory->destroy_2d_int_array(setflag);
    memory->destroy_2d_double_array(cutsq);

    memory->destroy_2d_double_array(cut_lj);
    memory->destroy_2d_double_array(cut_ljsq);
    memory->destroy_2d_double_array(cut_coul);
    memory->destroy_2d_double_array(cut_coulsq);
    memory->destroy_2d_double_array(a);
    memory->destroy_2d_double_array(rho);
    memory->destroy_2d_double_array(c);
    memory->destroy_2d_double_array(rhoinv);
    memory->destroy_2d_double_array(buck1);
    memory->destroy_2d_double_array(buck2);
    memory->destroy_2d_double_array(offset);
  }
}

/* ---------------------------------------------------------------------- */

void PairBuckCoulCutOMP::compute(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  if (evflag) {
    if (eflag) {
      if (force->newton_pair) return eval<1,1,1>();
      else return eval<1,1,0>();
    } else {
      if (force->newton_pair) return eval<1,0,1>();
      else return eval<1,0,0>();
    }
  } else {
    if (force->newton_pair) return eval<0,0,1>();
    else return eval<0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairBuckCoulCutOMP::eval()
{

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {

     int i,j,ii,jj,inum,jnum,itype,jtype, tid;
     double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
     double rsq,r2inv,r6inv,forcecoul,forcebuck,factor_coul,factor_lj;
     double r,rexp;
     int *ilist,*jlist,*numneigh,**firstneigh;

    evdwl = ecoul = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double **f = atom->f;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;

    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;

    // loop over neighbors of my atoms

    int iifrom, iito;
    f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
    for (ii = iifrom; ii < iito; ++ii) {

      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = factor_lj = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;

	 if (rsq < cutsq[itype][jtype]) {
          r2inv = 1.0/rsq;

	if (rsq < cut_coulsq[itype][jtype])
	  forcecoul = qqrd2e * qtmp*q[j]*sqrt(r2inv);
	else forcecoul = 0.0;

	if (rsq < cut_ljsq[itype][jtype]) {
	  r6inv = r2inv*r2inv*r2inv;
          r = sqrt(rsq);
	  rexp = exp(-r*rhoinv[itype][jtype]);
	  forcebuck = buck1[itype][jtype]*r*rexp - buck2[itype][jtype]*r6inv;
	} else forcebuck = 0.0;

	fpair = (factor_coul*forcecoul + factor_lj*forcebuck) * r2inv;

	f[i][0] += delx*fpair;
	f[i][1] += dely*fpair;
	f[i][2] += delz*fpair;
	if (NEWTON_PAIR || j < nlocal) {
	  f[j][0] -= delx*fpair;
	  f[j][1] -= dely*fpair;
	  f[j][2] -= delz*fpair;
	}

	  if (EFLAG) {
	    if (rsq < cut_coulsq[itype][jtype])
                ecoul = factor_coul * qqrd2e * qtmp*q[j]*sqrt(r2inv);
            else ecoul = 0.0;
            if (rsq < cut_ljsq[itype][jtype]) {
                evdwl = a[itype][jtype]*rexp - c[itype][jtype]*r6inv -
	        offset[itype][jtype];
                evdwl *= factor_lj;
	    }else evdwl = 0.0;
	  }

	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
				   evdwl,ecoul,fpair,delx,dely,delz,tid);
	}
      }
    }
  }
  ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  setflag = memory->create_2d_int_array(n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  cutsq = memory->create_2d_double_array(n+1,n+1,"pair:cutsq");

  cut_lj = memory->create_2d_double_array(n+1,n+1,"pair:cut_lj");
  cut_ljsq = memory->create_2d_double_array(n+1,n+1,"pair:cut_ljsq");
  cut_coul = memory->create_2d_double_array(n+1,n+1,"pair:cut_coul");
  cut_coulsq = memory->create_2d_double_array(n+1,n+1,"pair:cut_coulsq");
  a = memory->create_2d_double_array(n+1,n+1,"pair:a");
  rho = memory->create_2d_double_array(n+1,n+1,"pair:rho");
  c = memory->create_2d_double_array(n+1,n+1,"pair:c");
  rhoinv = memory->create_2d_double_array(n+1,n+1,"pair:rhoinv");
  buck1 = memory->create_2d_double_array(n+1,n+1,"pair:buck1");
  buck2 = memory->create_2d_double_array(n+1,n+1,"pair:buck2");
  offset = memory->create_2d_double_array(n+1,n+1,"pair:offset");
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::settings(int narg, char **arg)
{
  if (narg < 1 || narg > 2) error->all("Illegal pair_style command");

  cut_lj_global = force->numeric(arg[0]);
  if (narg == 1) cut_coul_global = cut_lj_global;
  else cut_coul_global = force->numeric(arg[1]);

  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
	if (setflag[i][j]) {
	  cut_lj[i][j] = cut_lj_global;
	  cut_coul[i][j] = cut_coul_global;
	}
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::coeff(int narg, char **arg)
{
  if (narg < 5 || narg > 7) error->all("Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double a_one = force->numeric(arg[2]);
  double rho_one = force->numeric(arg[3]);
  if (rho_one <= 0) error->all("Incorrect args for pair coefficients");
  double c_one = force->numeric(arg[4]);

  double cut_lj_one = cut_lj_global;
  double cut_coul_one = cut_coul_global;
  if (narg >= 6) cut_coul_one = cut_lj_one = force->numeric(arg[5]);
  if (narg == 7) cut_coul_one = force->numeric(arg[6]);

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      a[i][j] = a_one;
      rho[i][j] = rho_one;
      c[i][j] = c_one;
      cut_lj[i][j] = cut_lj_one;
      cut_coul[i][j] = cut_coul_one;
      setflag[i][j] = 1;
      count++;
    }
  }

  if (count == 0) error->all("Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::init_style()
{
  if (!atom->q_flag)
    error->all("Pair style buck/coul/cut requires atom attribute q");

  int irequest = neighbor->request(this);
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairBuckCoulCutOMP::init_one(int i, int j)
{
  if (setflag[i][j] == 0) error->all("All pair coeffs are not set");

  double cut = MAX(cut_lj[i][j],cut_coul[i][j]);
  cut_ljsq[i][j] = cut_lj[i][j] * cut_lj[i][j];
  cut_coulsq[i][j] = cut_coul[i][j] * cut_coul[i][j];

  rhoinv[i][j] = 1.0/rho[i][j];
  buck1[i][j] = a[i][j]/rho[i][j];
  buck2[i][j] = 6.0*c[i][j];

  if (offset_flag) {
    double rexp = exp(-cut_lj[i][j]/rho[i][j]);
    offset[i][j] = a[i][j]*rexp - c[i][j]/pow(cut_lj[i][j],6.0);
  } else offset[i][j] = 0.0;

  cut_ljsq[j][i] = cut_ljsq[i][j];
  cut_coulsq[j][i] = cut_coulsq[i][j];
  a[j][i] = a[i][j];
  c[j][i] = c[i][j];
  rhoinv[j][i] = rhoinv[i][j];
  buck1[j][i] = buck1[i][j];
  buck2[j][i] = buck2[i][j];
  offset[j][i] = offset[i][j];

  return cut;
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
	fwrite(&a[i][j],sizeof(double),1,fp);
	fwrite(&rho[i][j],sizeof(double),1,fp);
	fwrite(&c[i][j],sizeof(double),1,fp);
	fwrite(&cut_lj[i][j],sizeof(double),1,fp);
	fwrite(&cut_coul[i][j],sizeof(double),1,fp);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
	if (me == 0) {
	  fread(&a[i][j],sizeof(double),1,fp);
	  fread(&rho[i][j],sizeof(double),1,fp);
	  fread(&c[i][j],sizeof(double),1,fp);
	  fread(&cut_lj[i][j],sizeof(double),1,fp);
	  fread(&cut_coul[i][j],sizeof(double),1,fp);
	}
	MPI_Bcast(&a[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&rho[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&c[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&cut_lj[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&cut_coul[i][j],1,MPI_DOUBLE,0,world);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::write_restart_settings(FILE *fp)
{
  fwrite(&cut_lj_global,sizeof(double),1,fp);
  fwrite(&cut_coul_global,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairBuckCoulCutOMP::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_lj_global,sizeof(double),1,fp);
    fread(&cut_coul_global,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_lj_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_coul_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ---------------------------------------------------------------------- */

double PairBuckCoulCutOMP::single(int i, int j, int itype, int jtype,
			       double rsq,
			       double factor_coul, double factor_lj,
			       double &fforce)
{
  double r2inv,r6inv,r,rexp,forcecoul,forcebuck,phicoul,phibuck;

  r2inv = 1.0/rsq;
  if (rsq < cut_coulsq[itype][jtype])
    forcecoul = force->qqrd2e * atom->q[i]*atom->q[j]*sqrt(r2inv);
  else forcecoul = 0.0;
  if (rsq < cut_ljsq[itype][jtype]) {
    r6inv = r2inv*r2inv*r2inv;
    r = sqrt(rsq);
    rexp = exp(-r*rhoinv[itype][jtype]);
    forcebuck = buck1[itype][jtype]*r*rexp - buck2[itype][jtype]*r6inv;
  } else forcebuck = 0.0;
  fforce = (factor_coul*forcecoul + factor_lj*forcebuck) * r2inv;

  double eng = 0.0;
  if (rsq < cut_coulsq[itype][jtype]) {
    phicoul = force->qqrd2e * atom->q[i]*atom->q[j]*sqrt(r2inv);
    eng += factor_coul*phicoul;
  }
  if (rsq < cut_ljsq[itype][jtype]) {
    phibuck = a[itype][jtype]*rexp - c[itype][jtype]*r6inv -
      offset[itype][jtype];
    eng += factor_lj*phibuck;
  }
  return eng;
}

/* ---------------------------------------------------------------------- */

double PairBuckCoulCutOMP::memory_usage()
{
  const int n=atom->ntypes;

  double bytes = PairOMP::memory_usage();

  bytes += 9*((n+1)*(n+1) * sizeof(double) + (n+1)*sizeof(double *));
  bytes += 1*((n+1)*(n+1) * sizeof(int) + (n+1)*sizeof(int *));

  return bytes;
}