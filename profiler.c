/** \file profiler.c
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Written by:
 *     Nikhil Jain <nikhil.jain@acm.org>
 *     Abhinav Bhatele <bhatele@llnl.gov>
 *
 * LLNL-CODE-678958. All rights reserved.
 *
 * This file is part of BGQNCL. For details, see:
 * https://github.com/LLNL/bgqncl
 * Please also read the LICENSE file for our notice and the LGPL.
 */

#include "profiler.h"
#include "spi/include/kernel/process.h"
#include "spi/include/kernel/location.h"
#include <firmware/include/personality.h>
#include "bgpm/include/bgpm.h"

#define BGQ_DEBUG	0
#define NUM_REGIONS	100
#define NUM_TORUS_LINKS	10
#define ROOT_RANK 0

#ifdef __cplusplus
extern "C" {
#endif

int myrank, numranks, isZero, isMaster, masterRank;
unsigned int curset, hNWSet, maxset, numevents; 
MPI_Comm profile_comm;
FILE *dataFile;

UPC_NW_LinkMasks linkmask[] = { UPC_NW_LINK_A_MINUS,
UPC_NW_LINK_A_PLUS,
UPC_NW_LINK_B_MINUS,   
UPC_NW_LINK_B_PLUS,
UPC_NW_LINK_C_MINUS,   
UPC_NW_LINK_C_PLUS,
UPC_NW_LINK_D_MINUS,   
UPC_NW_LINK_D_PLUS,
UPC_NW_LINK_E_MINUS,   
UPC_NW_LINK_E_PLUS };

typedef struct _counters {
  uint64_t counters[60];
  double time;
} Counters;

Counters values[NUM_REGIONS];

INLINE void PROFILER_INIT() 
{
  MPI_Comm_rank(MPI_COMM_WORLD,&myrank);
  MPI_Comm_size(MPI_COMM_WORLD,&numranks);
  int coords[6], tmasterRank;
  MPIX_Rank2torus(myrank, coords);

  /* choose the MPI rank on (0, 0, 0, 0, 0) [0] as the master rank */
  if(coords[0]+coords[1]+coords[2]+coords[3]+coords[4] == 0 && coords[5] == ROOT_RANK) {
    isMaster = 1;
#if BGQ_DEBUG
    printf("Init intercepted by bgqcounter unit\n");
#endif
  } else {
    isMaster = 0;
  }

  char *filename = getenv("BGQ_COUNTER_FILE");
  if(isMaster) {
    if(filename != NULL)
      dataFile = fopen(filename,"w");
    else
      dataFile = stdout;
  }
#if BGQ_DEBUG
  if(isMaster) {
    printf("File opened, Initializing BGPM\n");
  }
#endif

  Bgpm_Init(BGPM_MODE_SWDISTRIB);
#if BGQ_DEBUG
  if(isMaster) {
    printf("Initialized BGPM, Splitting communicator\n");
  }
#endif

  /* split communicator based on the T dimension */
  isZero = (coords[5] == ROOT_RANK) ? 1 : 0;
  MPI_Comm_split(MPI_COMM_WORLD, isZero, myrank, &profile_comm);

#if BGQ_DEBUG
  if(isMaster) {
    printf("Communicator split done, find master\n");
  }
#endif

  /* Every process needs to know the master rank in profile_comm to know
     the root of the broadcast */
  coords[0] = coords[1] = coords[2] = coords[3] = coords[4] = 0; coords[5] = ROOT_RANK;
  MPIX_Torus2rank(coords, &tmasterRank);

#if BGQ_DEBUG
  if(isMaster) {
    printf("Found master, informing master\n");
  }
#endif

  if(isMaster) {
    MPI_Comm_rank(profile_comm, &masterRank);
  }

  /* Broadcast the rank of the master in profile_comm */
  MPI_Bcast(&masterRank, 1, MPI_INT, tmasterRank, MPI_COMM_WORLD);

#if BGQ_DEBUG
  if(isMaster) {
    printf("Informed master, attaching counters\n");
  }
#endif

  if(isZero) {
    hNWSet = Bgpm_CreateEventSet();
    Bgpm_AddEvent(hNWSet, PEVT_NW_USER_PP_SENT);
    //Bgpm_AddEvent(hNWSet, PEVT_NW_USER_DYN_PP_SENT);
    //Bgpm_AddEvent(hNWSet, PEVT_NW_USER_ESC_PP_SENT);
    //Bgpm_AddEvent(hNWSet, PEVT_NW_USER_SUBC_COL_SENT);
    //Bgpm_AddEvent(hNWSet, PEVT_NW_USER_PP_RECV);
    //Bgpm_AddEvent(hNWSet, PEVT_NW_USER_PP_RECV_FIFO);
    numevents = 1;
    if (Bgpm_Attach(hNWSet, UPC_NW_ALL_TORUS_LINKS, 0) != 0) {
      printf("Error: something went wrong in attaching link counters\n");
    }
    curset = maxset = 0;
    for(unsigned int i = 0; i < NUM_REGIONS; i++) {
      for(unsigned int j = 0; j < NUM_TORUS_LINKS * numevents; j++) {
        values[i].counters[j] = 0;
      }
      values[i].time = 0;
    }
  }
  if(isMaster) {
#if BGQ_DEBUG
    printf("Init intercept complete\n");
#endif
  }
}

INLINE void PROFILER_PCONTROL(int ctrl) {
  if(isMaster) {
#if BGQ_DEBUG
    printf("Pcontrol change from %d to %d\n",curset,ctrl);
#endif
  }
  if(isZero) {
    if(ctrl == 0 && curset == 0) return;

    /* Save the current counter values and change curset to the new value of ctrl */
    if(curset != 0) {
      values[curset].time += MPI_Wtime();
      unsigned int cnt = 0;
      uint64_t val;
      unsigned int numEvts = Bgpm_NumEvents(hNWSet);
      assert(numEvts == numevents);
      for(unsigned int i = 0; i < NUM_TORUS_LINKS; i++) {
        for(unsigned int j = 0; j < numevents; j++) {
          Bgpm_NW_ReadLinkEvent(hNWSet, j, linkmask[i], &val);
          values[curset].counters[cnt++] += val;
        }
      }
    }
    if(ctrl != 0) {
      Bgpm_ResetStart(hNWSet);
      values[ctrl].time -= MPI_Wtime();
    }
    curset = ctrl;
    if(curset > maxset) maxset = ctrl;
    
  }
}

INLINE void PROFILER_FINALIZE() {
  uint64_t *allCounters;
  double *times;
  int nranks;
  if(isMaster) {
#if BGQ_DEBUG
    printf("Finalize intercepted: numevents: %u, max set: %u\n",numevents, maxset);
#endif
    MPI_Comm_size(profile_comm,&nranks);
    allCounters = (uint64_t*) malloc(NUM_TORUS_LINKS * numevents * nranks *sizeof(uint64_t));
    times = (double*) malloc(nranks * sizeof(double));
  }
  if(isZero) {
    for(unsigned int i = 1; i <= maxset; i++) {
      /* collect all counter data into allCounters */
      MPI_Gather(values[i].counters, NUM_TORUS_LINKS * numevents,
	  MPI_UNSIGNED_LONG_LONG, allCounters, NUM_TORUS_LINKS * numevents,
	  MPI_UNSIGNED_LONG_LONG, masterRank, profile_comm);
      MPI_Gather(&values[i].time, 1, MPI_DOUBLE, times, 1, MPI_DOUBLE, masterRank, 
                profile_comm);

      if(isMaster) {
        MPI_Group world, profile_group;
        MPI_Comm_group(MPI_COMM_WORLD, &world);
        MPI_Comm_group(profile_comm, &profile_group);

        int *world_ranks, *profile_ranks;
        profile_ranks = (int*)malloc(nranks*sizeof(int));
        world_ranks = (int*)malloc(nranks*sizeof(int));
        for(unsigned int j = 0; j < nranks; j++) {
          profile_ranks[j] = j;
        }

	/* find the ranks in MPI_COMM_WORLD for all processes in profile_comm */
        MPI_Group_translate_ranks(profile_group, nranks, profile_ranks, world, world_ranks);
        unsigned int cnt = 0;
        int coords[6];
        for(unsigned int j = 0; j < nranks; j++) {
          //MPIX_Rank2torus(j*Kernel_ProcessCount(), coords); 
          MPIX_Rank2torus(world_ranks[j], coords); 
          fprintf(dataFile,"%d %d ",i,world_ranks[j]);
          fprintf(dataFile,"%d %d %d %d %d %d ** ",coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);
          for(unsigned int k = 0; k < NUM_TORUS_LINKS * numevents; k++) {
            fprintf(dataFile,"%lu ", allCounters[cnt++]);
          }
          fprintf(dataFile,"\n");
        }
        double min, max, avg = 0;
        min = max = times[0];
        for(unsigned j = 0; j < nranks; j++) {
          avg += times[j];
          if(min > times[j]) {
            min = times[j];
          }
          if(max < times[j]) {
            max = times[j];
          }
        }
        avg /=  nranks;
        printf("Timing Summary for region %d: min - %.3f s, avg - %.3f s, max - %.3f s\n", i, min, avg, max);
        free(profile_ranks); free(world_ranks);
      }
    }
  }
  if(isMaster) {
    if(dataFile != stdout)
      fclose(dataFile);
#if BGQ_DEBUG
    printf("Done profiling, exiting\n");
#endif
    free(allCounters);
    free(times);
  }
}
#ifdef __cplusplus
}
#endif

