/* 
 * ADIOS is freely available under the terms of the BSD license described
 * in the COPYING file in the top level directory of this source distribution.
 *
 * Copyright (c) 2008 - 2009.  UT-BATTELLE, LLC. All rights reserved.
 */

/* ADIOS C Example: write some attributes along with variables
*/
#include <stdio.h>
#include <string.h>
#include "mpi.h"
#include "adios.h"
int main (int argc, char ** argv[] ) 
{
    char        filename [256];
    char        meshname [256];
    char        xmlfilename[256];
    int         rank, size, i;
    int         NX = 10;  
    double      t[NX], mean = 0;
    MPI_Comm    comm = MPI_COMM_WORLD;
    const char * str = "Jul, 2012";

    int         adios_err;
    uint64_t    adios_groupsize, adios_totalsize;
    int64_t     adios_handle;

    MPI_Init (&argc, &argv);
    MPI_Comm_rank (comm, &rank);
    MPI_Comm_size (comm, &size);

    int xydim = size+NX;
    float       X[size];
    float       Y[NX];
    float       XY[size+NX];

    X[0] = 0;
    for (i = 1; i< size; i++)
    {
        X[i] = i + X[i-1];
    }

    Y[0] = 0;
    for (i = 1; i< NX; i++)
    {
        Y[i] = i + 2*Y[i-1];
    }

    for (i = 0; i< size+NX; i++)
    {
        if (i < size)
            XY[i] = X[i];
        else
            XY[i] = Y[i];
    }

    for (i = 0; i < NX; i++)
    {
        t[i] = rank * NX + i;
        mean += t[i];
    }

    mean /= NX;

    strcpy (meshname, "rectilinear");

    strcpy (filename, "rectilinear");
    strcat (filename, ".bp");

    strcpy (xmlfilename,meshname);
    strcat (xmlfilename,".xml");
    adios_init (xmlfilename);

    adios_open (&adios_handle, "schema", filename, "w", &comm);

    adios_groupsize = 4 \
                      + 4 \
                + 4 \
                + 4 \
                + 8 \
                + strlen(str) \
                + 4 * (1) * (size) \
                + 4 * (1) * (NX) \
                + 4 * (1) * (NX + size) \
                + 8 * (1) * (NX);
    adios_group_size (adios_handle, adios_groupsize, &adios_totalsize);
    adios_write (adios_handle, "NX", &NX);
    adios_write (adios_handle, "size", &size);
    adios_write (adios_handle, "rank", &rank);
    adios_write (adios_handle, "mean", &mean);
    adios_write (adios_handle, "xydim", &xydim);
    adios_write (adios_handle, "X", X);
    adios_write (adios_handle, "Y", Y);
    adios_write (adios_handle, "XY",  XY);
    adios_write (adios_handle, "date", str);
    adios_write (adios_handle, "temperature", t);

    adios_close (adios_handle);

    MPI_Barrier (comm);

    adios_finalize (rank);

    MPI_Finalize ();

    return 0;
}