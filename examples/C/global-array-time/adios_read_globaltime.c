/* ADIOS C Example: read global arrays from a BP file
 *
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpi.h"
#include "adios_read.h"

int main (int argc, char ** argv) 
{
    char        filename [256];
    int         rank, size, i, j, k;
    MPI_Comm    comm = MPI_COMM_WORLD;
    void * data = NULL;
    uint64_t start[3], count[3], bytes_read = 0;

    MPI_Init (&argc, &argv);
    MPI_Comm_rank (comm, &rank);
    MPI_Comm_size (comm, &size);

    ADIOS_FILE * f = adios_fopen ("restart.bp", comm);
    if (f == NULL)
    {
        printf ("%s\n", adios_errmsg());
        return -1;
    }

    ADIOS_GROUP * g = adios_gopen (f, "temperature");
    if (g == NULL)
    {
        printf ("%s\n", adios_errmsg());
        return -1;
    }

    ADIOS_VARINFO * v = adios_inq_var (g, "temperature");

    // read in two timesteps
    data = malloc (2 * v->dims[1] * v->dims[2] * sizeof (double));
    if (data == NULL)
    {
        fprintf (stderr, "malloc failed.\n");
        return -1;
    }

    // read in timestep 10, 11
    start[0] = 10;
    count[0] = 1;

    start[1] = 0;
    count[1] = v->dims[1];

    start[2] = 0;
    count[2] = v->dims[2];
       
    bytes_read = adios_read_var (g, "temperature", start, count, data);

    for (i = 0; i < 1; i++)
        for (j = 0; j < v->dims[1]; j++)
            for (k = 0; k < v->dims[2]; k++)
            printf ("[%d,%d,%d] %e\t", i, j, k, * (double *)data + i * v->dims[1] * v->dims[2] + j * v->dims[2] + k);

    printf ("\n");

    free (data);
    adios_free_varinfo (v);

    adios_gclose (g);
    adios_fclose (f);

    MPI_Barrier (comm);

    MPI_Finalize ();
    return 0;
}
