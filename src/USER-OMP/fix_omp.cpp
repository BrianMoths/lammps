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
   OpenMP based threading support for LAMMPS
   Contributing author: Axel Kohlmeyer (Temple U)
------------------------------------------------------------------------- */

#include "atom.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_request.h"
#include "update.h"
#include "integrate.h"
#include "min.h"

#include "fix_omp.h"
#include "thr_data.h"
#include "thr_omp.h"

#include "pair_hybrid.h"
#include "bond_hybrid.h"
#include "angle_hybrid.h"
#include "dihedral_hybrid.h"
#include "improper_hybrid.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

using namespace LAMMPS_NS;

static int get_tid()
{
  int tid = 0;
#if defined(_OPENMP)
  tid = omp_get_thread_num();
#endif
  return tid;
}

/* ---------------------------------------------------------------------- */

FixOMP::FixOMP(LAMMPS *lmp, int narg, char **arg) :  Fix(lmp, narg, arg),
  thr(NULL), last_omp_style(NULL), _nthr(-1), _neighbor(true), _newton(false)
{
  if ((narg < 4) || (narg > 6)) error->all(FLERR,"Illegal fix OMP command");
  if (strcmp(arg[1],"all") != 0) error->all(FLERR,"Illegal fix OMP command");

  int nthreads = 1;
  if (narg > 3) {
#if defined(_OPENMP)
    if (strcmp(arg[3],"*") == 0)
#pragma omp parallel default(none) shared(nthreads)
      nthreads = omp_get_num_threads();
    else
      nthreads = atoi(arg[3]);
#endif
  }

  if (nthreads < 1)
    error->all(FLERR,"Illegal number of threads requested.");

  if (nthreads != comm->nthreads) {
#if defined(_OPENMP)
    omp_set_num_threads(nthreads);
#endif
    comm->nthreads = nthreads;
    if (comm->me == 0) {
      if (screen)
	fprintf(screen,"  reset %d OpenMP thread(s) per MPI task\n", nthreads);
      if (logfile)
	fprintf(logfile,"  reset %d OpenMP thread(s) per MPI task\n", nthreads);
    }
  }

  if (narg > 4) {
    if (strcmp(arg[4],"force/neigh") == 0)
      _neighbor = true;
    else if (strcmp(arg[4],"force") == 0)
      _neighbor = false;
    else
      error->all(FLERR,"Illegal fix omp mode requested.");

    if (comm->me == 0) {
      const char * const mode = _neighbor ? "OpenMP capable" : "serial";
      
      if (screen)
	fprintf(screen,"  using %s neighbor list subroutines\n", mode);
      if (logfile)
	fprintf(logfile,"  using %s neighbor list subroutines\n", mode);
    }
  }

#if 0 /* to be enabled when we can switch between half and full neighbor lists */
  if (narg > 5) {
    if (strcmp(arg[5],"neigh/half") == 0)
      _newton = true;
    else if (strcmp(arg[5],"neigh/full") == 0)
	_newton = false;
    else
      error->all(FLERR,"Illegal fix OMP command");

    if (comm->me == 0) {
      const char * const mode = _newton ? "half" : "full";
      
      if (screen)
	fprintf(screen,"  using /omp styles with %s neighbor list builds\n", mode);
      if (logfile)
	fprintf(logfile,"  using /omp styles with %s neighbor list builds\n", mode);
    }
  }
#endif

  // allocate list for per thread accumulator manager class instances
  // and then have each thread create an instance of this class to 
  // encourage the OS to use storage that is "close" to each thread's CPU.
  thr = new ThrData *[nthreads];
  _nthr = nthreads;
#if defined(_OPENMP)
#pragma omp parallel default(none)
#endif
  {
    const int tid = get_tid();
    thr[tid] = new ThrData(tid);
  }
}

/* ---------------------------------------------------------------------- */

FixOMP::~FixOMP()
{
#if defined(_OPENMP)
#pragma omp parallel default(none)
#endif
  {
    const int tid = get_tid();
    delete thr[tid];
  }
  delete[] thr;
}

/* ---------------------------------------------------------------------- */

int FixOMP::setmask()
{
  int mask = 0;
  mask |= PRE_FORCE;
  mask |= PRE_FORCE_RESPA;
  mask |= MIN_PRE_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixOMP::init()
{
  int check_hybrid;
  last_omp_style = NULL;
  char *last_omp_name = NULL;

// determine which is the last force style with OpenMP
// support as this is the one that has to reduce the forces

#define CheckStyleForOMP(name)						\
  check_hybrid = 0;							\
  if (force->name) {							\
    if ( (strcmp(force->name ## _style,"hybrid") == 0) ||		\
         (strcmp(force->name ## _style,"hybrid/overlay") == 0) )	\
      check_hybrid=1;							\
    int len = strlen(force->name ## _style);				\
    char *suffix = force->name ## _style + len - 4;			\
    if (strcmp(suffix,"/omp") == 0) {					\
      last_omp_name = force->name ## _style;				\
      last_omp_style = (void *) force->name;				\
    }									\
  }

#define CheckHybridForOMP(name,Class)					\
  if (check_hybrid) {							\
    Class ## Hybrid *style = (Class ## Hybrid *) force->name;		\
    for (int i=0; i < style->nstyles; i++) {				\
      int len = strlen(style->keywords[i]);				\
      char *suffix = style->keywords[i] + len - 4;			\
      if (strcmp(suffix,"/omp") == 0) {					\
	last_omp_name = force->name ## _style;				\
	last_omp_style = (void *) force->name;				\
      }									\
    }									\
  }

  CheckStyleForOMP(pair);
  CheckHybridForOMP(pair,Pair);

  CheckStyleForOMP(bond);
  CheckHybridForOMP(bond,Bond);

  CheckStyleForOMP(angle);
  CheckHybridForOMP(angle,Angle);

  CheckStyleForOMP(dihedral);
  CheckHybridForOMP(dihedral,Dihedral);

  CheckStyleForOMP(improper);
  CheckHybridForOMP(improper,Improper);

  CheckStyleForOMP(kspace);

#undef CheckStyleForOMP
#undef CheckHybridForOMP

  set_neighbor_omp();
}

/* ---------------------------------------------------------------------- */

void FixOMP::set_neighbor_omp()
{
  // select or deselect multi-threaded neighbor
  // list build depending on setting in package omp.
  // NOTE: since we are at the top of the list of
  // fixes, we cannot adjust neighbor lists from
  // other fixes. those have to be re-implemented
  // as /omp fix styles. :-(

  const int neigh_omp = _neighbor ? 1 : 0;
  const int nrequest = neighbor->nrequest;

  for (int i = 0; i < nrequest; ++i)
    neighbor->requests[i]->omp = neigh_omp;
}

/* ---------------------------------------------------------------------- */

// adjust size and clear out per thread accumulator arrays
void FixOMP::pre_force(int)
{
  const int nall = atom->nlocal + atom->nghost;

  double **f = atom->f;
  double **torque = atom->torque;
  double *erforce = atom->erforce;
  double *de = atom->de;
  double *drho = atom->drho;

#if defined(_OPENMP)
#pragma omp parallel default(none) shared(f,torque,erforce,de,drho)
#endif
  {
    const int tid = get_tid();
    thr[tid]->check_tid(tid);
    thr[tid]->init_force(nall,f,torque,erforce,de,drho);
  }
}

/* ---------------------------------------------------------------------- */

double FixOMP::memory_usage()
{
  double bytes = comm->nthreads * (sizeof(ThrData *) + sizeof(ThrData));
  bytes += comm->nthreads * thr[0]->memory_usage();
  
  return bytes;
}
