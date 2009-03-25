#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>

// mpi
#include "mpi.h"

// xml parser
#include <mxml.h>

#include "adios.h"
#include "adios_transport_hooks.h"
#include "adios_bp_v1.h"
#include "adios_internals.h"

static int adios_mpi_stagger_initialized = 0;

struct adios_MPI_data_struct
{
    MPI_File fh;
    MPI_Request req;
    MPI_Status status;
    MPI_Comm group_comm;
    int rank;
    int size;

    struct adios_bp_buffer_struct_v1 b;

    struct adios_index_process_group_struct_v1 * old_pg_root;
    struct adios_index_var_struct_v1 * old_vars_root;
    struct adios_index_attribute_struct_v1 * old_attrs_root;

    uint64_t vars_start;
    uint64_t vars_header_size;
    uint64_t biggest_size;     // largest writer's data size (round up)
    uint16_t storage_targets;  // number of storage targets being used

    int16_t max_storage_targets;
    int16_t max_stripe_count;
    int16_t min_stripe_count;
    int16_t files_number;
    int16_t overlap_factor;
};

static void set_stripe_size (struct adios_file_struct * fd
                            ,struct adios_MPI_data_struct * md
                            ,const char * filename
                            );

static void adios_var_to_comm (const char * comm_name
                              ,enum ADIOS_FLAG host_language_fortran
                              ,void * data
                              ,MPI_Comm * comm
                              )
{
    if (data)
    {
        int t = *(int *) data;

        if (!comm_name)
        {
            if (!t)
            {
                fprintf (stderr, "communicator not provided and none "
                                 "listed in XML.  Defaulting to "
                                 "MPI_COMM_SELF\n"
                        );

                *comm = MPI_COMM_SELF;
            }
            else
            {
                if (host_language_fortran == adios_flag_yes)
                {
                    *comm = MPI_Comm_f2c (t);
                }
                else
                {
                    *comm = *(MPI_Comm *) data;
                }
            }
        }
        else
        {
            if (!strcmp (comm_name, ""))
            {
                if (!t)
                {
                    fprintf (stderr, "communicator not provided and none "
                                     "listed in XML.  Defaulting to "
                                     "MPI_COMM_SELF\n"
                            );

                    *comm = MPI_COMM_SELF;
                }
                else
                {
                    if (host_language_fortran == adios_flag_yes)
                    {
                        *comm = MPI_Comm_f2c (t);
                    }
                    else
                    {
                        *comm = *(MPI_Comm *) data;
                    }
                }
            }
            else
            {
                if (!t)
                {
                    fprintf (stderr, "communicator not provided but one "
                                     "listed in XML.  Defaulting to "
                                     "MPI_COMM_WORLD\n"
                            );

                    *comm = MPI_COMM_WORLD;
                }
                else
                {
                    if (host_language_fortran == adios_flag_yes)
                    {
                        *comm = MPI_Comm_f2c (t);
                    }
                    else
                    {
                        *comm = *(MPI_Comm *) data;
                    }
                }
            }
        }
    }
    else
    {
        fprintf (stderr, "coordination-communication not provided. "
                         "Using MPI_COMM_WORLD instead\n"
                );

        *comm = MPI_COMM_WORLD;
    }
}

void adios_mpi_stagger_init (const char * parameters
                    ,struct adios_method_struct * method
                    )
{
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                    method->method_data;
    if (!adios_mpi_stagger_initialized)
    {
        adios_mpi_stagger_initialized = 1;
    }
    method->method_data = malloc (sizeof (struct adios_MPI_data_struct));
    md = (struct adios_MPI_data_struct *) method->method_data;
    md->fh = 0;
    md->req = 0;
    memset (&md->status, 0, sizeof (MPI_Status));
    md->rank = 0;
    md->size = 0;
    md->group_comm = MPI_COMM_NULL;
    md->old_pg_root = 0;
    md->old_vars_root = 0;
    md->old_attrs_root = 0;
    md->vars_start = 0;
    md->vars_header_size = 0;
    md->biggest_size = 0;
    md->storage_targets = 0;

    md->max_storage_targets = -1;
    md->max_stripe_count = -1;
    md->min_stripe_count = -1;
    md->files_number = -1;
    md->overlap_factor = -1;

    // parse the parameters into key=value segments for optional settings
    if (parameters)
    {
        int param_len = strlen (parameters);
        if (param_len > 0)
        {
            char * p = strdup (parameters);
            char * token = strtok (p, ";");

            while (token)
            {
                char * equal_pos = strchr (token, '=');
                if (!equal_pos)
                {
                    continue;
                }
                int equal = equal_pos - token + 1;
                int len = strlen (token);
                char * key = malloc (len);
                char * value = malloc (len);
                strncpy (key, token, equal);
                key [equal - 1] = 0;
                strncpy (value, equal_pos + 1, len - equal);
                value [len - equal] = 0;

                if (key && value)
                {
                    if (strcasecmp (key, "max_storage_targets") == 0)
                    {
                        int v = atoi (value);
                        md->max_storage_targets = v;
                    }
                    else if (strcasecmp (key, "max_stripe_count") == 0)
                    {
                        int v = atoi (value);
                        md->max_stripe_count = v;
                    }
                    else if (strcasecmp (key, "min_stripe_count") == 0)
                    {
                        int v = atoi (value);
                        md->min_stripe_count = v;
                    }
                    else if (strcasecmp (key, "files_number") == 0)
                    {
                        int v = atoi (value);
                        md->files_number = v;
                    }
                    else if (strcasecmp (key, "overlap_factor") == 0)
                    {
                        int v = atoi (value);
                        md->overlap_factor = v;
                    }
                    else
                    {
                        printf ("MPI_STAGGER parameter: key: {%s} value: {%s} "
                                "not recognized. Ignored\n"
                               ,key, value
                               );
                    }
                }

                free (key);
                free (value);

                token = strtok (NULL, ";");
            }

            free (p);
        }
    }

    adios_buffer_struct_init (&md->b);
}

int adios_mpi_stagger_open (struct adios_file_struct * fd
                   ,struct adios_method_struct * method
                   )
{
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                    method->method_data;

    // we have to wait for the group_size (should_buffer) to get the comm
    // before we can do an open for any of the modes

    return 1;
}

static
void build_offsets (struct adios_bp_buffer_struct_v1 * b
                   ,MPI_Offset * offsets, int size, char * group_name
                   ,struct adios_index_process_group_struct_v1 * pg_root
                   )
{
    while (pg_root)
    {
        if (!strcasecmp (pg_root->group_name, group_name))
        {
            MPI_Offset size = 0;

            if (pg_root->next)
            {
                size = pg_root->next->offset_in_file - pg_root->offset_in_file;
            }
            else
            {
                size = b->pg_index_offset - pg_root->offset_in_file;
            }

            offsets [pg_root->process_id * 3] = pg_root->offset_in_file;
            offsets [pg_root->process_id * 3 + 1] = size;
            offsets [pg_root->process_id * 3 + 2] = b->version;
        }

        pg_root = pg_root->next;
    }
}

static void
adios_mpi_build_file_offset(struct adios_MPI_data_struct *md,
                            struct adios_file_struct *fd, char *name)
{
    if (md->group_comm != MPI_COMM_NULL)
    {
        if (md->rank == 0)
        {
            // make one space for offset and one for size
            MPI_Offset * offsets = malloc(sizeof (MPI_Offset)
                                           * md->size * 3);
            int i;

            offsets [0] = fd->write_size_bytes;
            MPI_Gather (offsets, 1, MPI_LONG_LONG
                       ,offsets, 1, MPI_LONG_LONG
                       ,0, md->group_comm);

// top section: make things a consistent stripe size
// bottom section: just pack the file
#if 1
            // find the largest and use that as a basis for stripe
            // size for each process writing
            uint64_t biggest_size = 0;
            for (i = 0; i < md->size; i++)
            {
                if (offsets [i] > biggest_size)
                    biggest_size = offsets [i];
            }
            // now round up to the next stripe size increment (Lustre: 64 KB)
#define STRIPE_INCREMENT (64 * 1024)
            // (according to the Lustre reps, use 1 MB instead of 64 KB?)
//#define STRIPE_INCREMENT (1024 * 1024)
            if (biggest_size % (STRIPE_INCREMENT))
            {
                biggest_size = (  ((biggest_size / STRIPE_INCREMENT) + 1) 
                                * STRIPE_INCREMENT
                               );
            }
            // also round up the base_offset, just in case
            if (fd->base_offset % (STRIPE_INCREMENT))
            {
                fd->base_offset = (  ((fd->base_offset / STRIPE_INCREMENT) + 1) 
                                   * STRIPE_INCREMENT
                                  );
            }
            md->biggest_size = biggest_size;
            set_stripe_size (fd, md, name);
#undef STRIPE_INCREMENT
            offsets [0 + 0] = fd->base_offset;
            offsets [0 + 1] = biggest_size;
            offsets [0 + 2] = md->storage_targets;
            for (i = 1; i < md->size; i++)
            {
                offsets [i * 3 + 0] = offsets [(i - 1) * 3 + 0] + biggest_size;
                offsets [i * 3 + 1] = biggest_size;
                offsets [i * 3 + 2] = md->storage_targets;
            }
            md->b.pg_index_offset =   offsets [(md->size - 1) * 3 + 0]
                                    + biggest_size;
#else
            uint64_t biggest_size = 0;
            uint64_t last_offset = offsets [0];
            offsets [0] = fd->base_offset;
            for (i = 1; i < md->size; i++)
            {
                uint64_t this_offset = offsets [i];
                offsets [i] = offsets [i - 1] + last_offset;
                last_offset = this_offset;
            }
            md->b.pg_index_offset =   offsets [md->size - 1]
                                    + last_offset;
            md->biggest_size = biggest_size;
#endif
            MPI_Scatter (offsets, 3, MPI_LONG_LONG
                        ,offsets, 3, MPI_LONG_LONG
                        ,0, md->group_comm
                        );
            fd->base_offset = offsets [0];
            fd->pg_start_in_file = fd->base_offset;
            free (offsets);
        }
        else
        {
            MPI_Offset offset [3];
            offset [0] = fd->write_size_bytes;

            MPI_Gather (offset, 1, MPI_LONG_LONG
                       ,offset, 1, MPI_LONG_LONG
                       ,0, md->group_comm
                       );

            MPI_Scatter (offset, 3, MPI_LONG_LONG
                        ,offset, 3, MPI_LONG_LONG
                        ,0, md->group_comm
                        );
            fd->base_offset = offset [0];
            md->biggest_size = offset [1];
            md->storage_targets = offset [2];
            fd->pg_start_in_file = fd->base_offset;
        }
    }
    else
    {
        md->b.pg_index_offset = fd->write_size_bytes;
    }
}

// calc_stripe_info figures out how many OSTs to use based on either an
// assumption of one file for all processes or a different count if the
// parameters are supplied in the XML file. These parameters are defined as
// follows:

// max_storage_targets - the number of OSTs in the system.
//                       On ewok, this is 12. On jaguarpf, this is 671, I think.
// max_stripe_count - the maximum number of OSTs available for a single file.
//                    This is 160 on jaguarpf and 12 on ewok.
// files_number - the number of files being written simultaneously. This also
//                implies that all MPI processes are involved in the write.
// overlap_factor - what percentage of the allocated portion of the OSTs should
//                  be allowed to overlap with the next file set. For example,
//                  a value of 50 means that half of the next set of OSTs will
//                  also be used for this set (e.g., set 0= 0-15, set 1= 10-25,
//                  set 2=20-35, ...).
// min_stripe_count - the fewest OSTs to use per file. This will override the
//                    overlap_factor, if necessary.

// The way to put it into the XML, which is currently pretty unforgiving, is
// like this:
// <transport method="MPI_STAGGER" group="restart">max_storage_targets=671;max_stripe_count=160;files_number=16;overlap_factor=50;min_stripe_count=10</transport>

// This says to divide the 671 OSTs so that there are 16 groups. Each group
// will consist of 1/16 portion of the whole plus 1/32 as an overlap factor.
// This will not exceed the max_stripe_count. (671/16 = 112 + 50% = 178, but
// max is 160 so limited to 160 [0-159, 112-272, 225-385, etc.]).

// If this were set for 600 files, then it would be 10 OSTs per file at an
// offset of 2 [0-9, 2-11, 4-13, etc.]

// Initial assumption is that based on the rank, we can guess which set of OSTs
// to use. If this won�t work, then we need to add a communication to exchange
// information so that the processes can make that decision (and MPI_All_to_all
// of the rank is sufficient so that each main process can determine the
// ordering and then calculate which set to use).
static void calc_stripe_info (struct adios_MPI_data_struct * md
                             ,int * stripe_offset
                             ,int * stripe_count
                             )
{
    if (   md->max_storage_targets > 0
        && md->max_stripe_count > 0
        && md->files_number > 0
        && md->overlap_factor > 0
        && md->min_stripe_count > 0
       )
    {
        int targets_per_file = md->max_storage_targets / md->files_number;
        if (md->max_storage_targets % md->files_number)
            targets_per_file++;

        int overlap_count = (int) (  targets_per_file
                                   * (md->overlap_factor/100.0)
                                  );

        int net_targets_per_file = targets_per_file + overlap_count;

        if (net_targets_per_file < md->min_stripe_count)
            net_targets_per_file = md->min_stripe_count;
        if (net_targets_per_file > md->max_stripe_count)
            net_targets_per_file = md->max_stripe_count;

        int rank = 0;
        int size = 0;
        int range = 0;
        int number = 0;
        MPI_Comm_rank (MPI_COMM_WORLD, &rank);
        MPI_Comm_size (MPI_COMM_WORLD, &size);

        range = size / md->files_number;
        number = rank / range;

        *stripe_offset = targets_per_file * number;
        *stripe_count = net_targets_per_file;
        printf ("rank: %d calculated stripe offset: %d count: %d\n", rank, *stripe_offset, *stripe_count);
    }
    else
    {
        *stripe_count = UINT16_MAX;
        printf ("defaulting\n"
                "max storage targets: %d\n"
                "max stripe count: %d\n"
                "files number: %d\n"
                "overlap factor: %d\n"
                "min stripe count: %d\n"
               ,md->max_storage_targets
               ,md->max_stripe_count
               ,md->files_number
               ,md->overlap_factor
               ,md->min_stripe_count
               );
    }
}


// LUSTRE Structure
// from /usr/include/lustre/lustre_user.h
#define LUSTRE_SUPER_MAGIC 0x0BD00BD0
#  define LOV_USER_MAGIC 0x0BD10BD0
#  define LL_IOC_LOV_SETSTRIPE  _IOW ('f', 154, long)
#  define LL_IOC_LOV_GETSTRIPE  _IOW ('f', 155, long)
#define O_LOV_DELAY_CREATE 0100000000

struct lov_user_ost_data {           // per-stripe data structure
        uint64_t l_object_id;        // OST object ID
        uint64_t l_object_gr;        // OST object group (creating MDS number)
        uint32_t l_ost_gen;          // generation of this OST index
        uint32_t l_ost_idx;          // OST index in LOV
} __attribute__((packed));
struct lov_user_md {                 // LOV EA user data (host-endian)
        uint32_t lmm_magic;          // magic number = LOV_USER_MAGIC_V1
        uint32_t lmm_pattern;        // LOV_PATTERN_RAID0, LOV_PATTERN_RAID1
        uint64_t lmm_object_id;      // LOV object ID
        uint64_t lmm_object_gr;      // LOV object group
        uint32_t lmm_stripe_size;    // size of stripe in bytes
        uint16_t lmm_stripe_count;   // num stripes in use for this object
        uint16_t lmm_stripe_offset;  // starting stripe offset in lmm_objects
        struct lov_user_ost_data  lmm_objects[0]; // per-stripe data
} __attribute__((packed));

// do the magic ioctl calls to set Lustre's stripe size
static void set_stripe_size (struct adios_file_struct * fd
                            ,struct adios_MPI_data_struct * md
                            ,const char * filename
                            )
{
    struct statfs fsbuf;
    int err;

    int f;
    int old_mask;
    int perm;

    old_mask = umask (0);
    umask (old_mask);
    perm = old_mask ^ 0666;

    f = open (filename, O_RDONLY | O_CREAT | O_LOV_DELAY_CREATE, perm);
    // Note: Since each file might have different write_buffer,
    // So we will reset write_buffer even buffer_size != 0
    err = statfs (filename, &fsbuf);
    if (!err && fsbuf.f_type == LUSTRE_SUPER_MAGIC)
    {
        if (f != -1)
        {
            int stripe_count;
            int stripe_offset;
            struct lov_user_md lum;
            lum.lmm_magic = LOV_USER_MAGIC;
            // get what Lustre assigns by default
            err = ioctl (f, LL_IOC_LOV_GETSTRIPE, (void *) &lum);
            stripe_count = lum.lmm_stripe_count;
            stripe_offset = lum.lmm_stripe_offset;
            calc_stripe_info (md, &stripe_offset, &stripe_count);

            // fixup for our desires
            lum.lmm_magic = LOV_USER_MAGIC;
            lum.lmm_pattern = 0;
            lum.lmm_stripe_size = md->biggest_size;
            lum.lmm_stripe_count = stripe_count; // maximize number of targets
            lum.lmm_stripe_offset = stripe_offset;
            err = ioctl (f, LL_IOC_LOV_SETSTRIPE, (void *) &lum);
            lum.lmm_stripe_count = 0;
            err = ioctl (f, LL_IOC_LOV_GETSTRIPE, (void *) &lum);
            // if err != 0, the must not be Lustre
            if (err == 0)
            {
                md->storage_targets = lum.lmm_stripe_count;
            }
            close (f);

            int rank = 0;
            MPI_Comm_rank (MPI_COMM_WORLD, &rank);
            printf ("rank: %d stripe_offset: %d stripe_count: %d "
                    "stripe_size: %d\n"
                   ,rank, lum.lmm_stripe_offset, lum.lmm_stripe_count
                   ,lum.lmm_stripe_size
                   );
        }
    }
}

enum ADIOS_FLAG adios_mpi_stagger_should_buffer (struct adios_file_struct * fd
                                        ,struct adios_method_struct * method
                                        ,void * comm
                                        )
{
    int i;
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                      method->method_data;
    char * name;
    int err;
    int flag;    // used for coordinating the MPI_File_open

    int previous;
    int current;
    int next;

    name = malloc (strlen (method->base_path) + strlen (fd->name) + 1);
    sprintf (name, "%s%s", method->base_path, fd->name);

    adios_var_to_comm (fd->group->group_comm
                      ,fd->group->adios_host_language_fortran
                      ,comm
                      ,&md->group_comm
                      );
    if (md->group_comm != MPI_COMM_NULL)
    {
        MPI_Comm_rank (md->group_comm, &md->rank);
        MPI_Comm_size (md->group_comm, &md->size);
    }
    fd->group->process_id = md->rank;

    if (md->rank == md->size - 1)
        next = -1;
    else
        next = md->rank + 1;
    previous = md->rank - 1;
    current = md->rank;

    fd->base_offset = 0;

    switch (fd->mode)
    {
        case adios_mode_read:
        {
            if (md->group_comm == MPI_COMM_NULL || md->rank == 0)
            {
                err = MPI_File_open (MPI_COMM_SELF, name, MPI_MODE_RDONLY
                                    ,MPI_INFO_NULL, &md->fh
                                    );
                if (err != MPI_SUCCESS)
                {
                    char e [MPI_MAX_ERROR_STRING];
                    int len = 0;
                    memset (e, 0, MPI_MAX_ERROR_STRING);
                    MPI_Error_string (err, e, &len);
                    fprintf (stderr, "MPI open read failed for %s: '%s'\n"
                            ,name, e
                            );
                    free (name);

                    return adios_flag_no;
                }

                MPI_Offset file_size;
                MPI_File_get_size (md->fh, &file_size);
                md->b.file_size = file_size;

                adios_init_buffer_read_version (&md->b);
                MPI_File_seek (md->fh, md->b.file_size - md->b.length
                              ,MPI_SEEK_SET
                              );
                MPI_File_read (md->fh, md->b.buff, md->b.length, MPI_BYTE
                              ,&md->status
                              );
                adios_parse_version (&md->b, &md->b.version);

                adios_init_buffer_read_index_offsets (&md->b);
                // already in the buffer
                adios_parse_index_offsets_v1 (&md->b);

                adios_init_buffer_read_process_group_index (&md->b);
                MPI_File_seek (md->fh, md->b.pg_index_offset
                              ,MPI_SEEK_SET
                              );
                MPI_File_read (md->fh, md->b.buff, md->b.pg_size, MPI_BYTE
                              ,&md->status
                              );
                adios_parse_process_group_index_v1 (&md->b
                                                   ,&md->old_pg_root
                                                   );

#if 1
                adios_init_buffer_read_vars_index (&md->b);
                MPI_File_seek (md->fh, md->b.vars_index_offset
                              ,MPI_SEEK_SET
                              );
                MPI_File_read (md->fh, md->b.buff, md->b.vars_size, MPI_BYTE
                              ,&md->status
                              );
                adios_parse_vars_index_v1 (&md->b, &md->old_vars_root);

                adios_init_buffer_read_attributes_index (&md->b);
                MPI_File_seek (md->fh, md->b.attrs_index_offset
                              ,MPI_SEEK_SET
                              );
                MPI_File_read (md->fh, md->b.buff, md->b.attrs_size, MPI_BYTE
                              ,&md->status
                              );
                adios_parse_attributes_index_v1 (&md->b, &md->old_attrs_root);
#endif

                fd->base_offset = md->b.end_of_pgs;
            }

            if (   md->group_comm != MPI_COMM_NULL
                && md->group_comm != MPI_COMM_SELF
               )
            {
                if (md->rank == 0)
                {
                    MPI_Offset * offsets = malloc (  sizeof (MPI_Offset)
                                                   * md->size * 3
                                                  );
                    memset (offsets, 0, sizeof (MPI_Offset) * md->size * 3);

                    // go through the pg index to build the offsets array
                    build_offsets (&md->b, offsets, md->size
                                  ,fd->group->name, md->old_pg_root
                                  );
                    MPI_Scatter (offsets, 3, MPI_LONG_LONG
                                ,offsets, 3, MPI_LONG_LONG
                                ,0, md->group_comm
                                );
                    md->b.read_pg_offset = offsets [0];
                    md->b.read_pg_size = offsets [1];
                    free (offsets);
                }
                else
                {
                    MPI_Offset offset [3];
                    offset [0] = offset [1] = offset [2] = 0;

                    MPI_Scatter (offset, 3, MPI_LONG_LONG
                                ,offset, 3, MPI_LONG_LONG
                                ,0, md->group_comm
                                );

                    md->b.read_pg_offset = offset [0];
                    md->b.read_pg_size = offset [1];
                    md->b.version = offset [2];
                }
            }

            // cascade the opens to avoid trashing the metadata server
            if (previous == -1)
            {
                // note rank 0 is already open
                // don't open it again here

                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
            }
            else
            {
                MPI_Recv (&flag, 1, MPI_INTEGER, previous, previous
                         ,md->group_comm, &md->status
                         );
                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_RDONLY
                                    ,MPI_INFO_NULL
                                    ,&md->fh
                                    );
            }

            if (err != MPI_SUCCESS)
            {
                char e [MPI_MAX_ERROR_STRING];
                int len = 0;
                memset (e, 0, MPI_MAX_ERROR_STRING);
                MPI_Error_string (err, e, &len);
                fprintf (stderr, "MPI open write failed for %s: '%s'\n"
                        ,name, e
                        );
                free (name);

                return adios_flag_no;
            }

            break;
        }

        case adios_mode_write:
        {
            fd->base_offset = 0;
            fd->pg_start_in_file = 0;

            if (previous == -1)
            {
                MPI_File_delete (name, MPI_INFO_NULL);  // make sure clean
            }

            // figure out the offsets and create the file with proper striping
            // before the MPI_File_open is called
            adios_mpi_build_file_offset (md, fd, name);

            // cascade the opens to avoid trashing the metadata server
            if (previous == -1)
            {
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_WRONLY | MPI_MODE_CREATE
                                    ,MPI_INFO_NULL
                                    ,&md->fh
                                    );
                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
            }
            else
            {
                MPI_Recv (&flag, 1, MPI_INTEGER, previous, previous
                         ,md->group_comm, &md->status
                         );
                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_WRONLY
                                    ,MPI_INFO_NULL
                                    ,&md->fh
                                    );
            }

            if (err != MPI_SUCCESS)
            {
                char e [MPI_MAX_ERROR_STRING];
                int len = 0;
                memset (e, 0, MPI_MAX_ERROR_STRING);
                MPI_Error_string (err, e, &len);
                fprintf (stderr, "MPI open write failed for %s: '%s'\n"
                        ,name, e
                        );
                free (name);

                return adios_flag_no;
            }

            break;
        }

        case adios_mode_append:
        {
            int old_file = 1;
            adios_buffer_struct_clear (&md->b);

            err = MPI_File_open (MPI_COMM_SELF, name, MPI_MODE_RDONLY
                                ,MPI_INFO_NULL, &md->fh
                                );

            if (err != MPI_SUCCESS)
            {
                old_file = 0;
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_WRONLY | MPI_MODE_CREATE
                                    ,MPI_INFO_NULL, &md->fh
                                    );
                if (err != MPI_SUCCESS)
                {
                    char e [MPI_MAX_ERROR_STRING];
                    int len = 0;
                    memset (e, 0, MPI_MAX_ERROR_STRING);
                    MPI_Error_string (err, e, &len);
                    fprintf (stderr, "MPI open write failed for %s: '%s'\n"
                            ,name, e
                            );
                    free (name);

                    return adios_flag_no;
                }
            }

            if (old_file)
            {
                if (md->group_comm == MPI_COMM_NULL || md->rank == 0)
                {
                    if (err != MPI_SUCCESS)
                    {
                        md->b.file_size = 0;
                    }
                    else
                    {
                        MPI_Offset file_size;
                        MPI_File_get_size (md->fh, &file_size);
                        md->b.file_size = file_size;
                    }

                    adios_init_buffer_read_version (&md->b);
                    MPI_File_seek (md->fh, md->b.file_size - md->b.length
                                  ,MPI_SEEK_SET
                                  );
                    MPI_File_read (md->fh, md->b.buff, md->b.length, MPI_BYTE
                                  ,&md->status
                                  );
                    adios_parse_version (&md->b, &md->b.version);

                    adios_init_buffer_read_index_offsets (&md->b);
                    // already in the buffer
                    adios_parse_index_offsets_v1 (&md->b);

                    adios_init_buffer_read_process_group_index (&md->b);
                    MPI_File_seek (md->fh, md->b.pg_index_offset
                                  ,MPI_SEEK_SET
                                  );
                    MPI_File_read (md->fh, md->b.buff, md->b.pg_size, MPI_BYTE
                                  ,&md->status
                                  );

                    adios_parse_process_group_index_v1 (&md->b
                                                       ,&md->old_pg_root
                                                       );

                    adios_init_buffer_read_vars_index (&md->b);
                    MPI_File_seek (md->fh, md->b.vars_index_offset
                                  ,MPI_SEEK_SET
                                  );
                    MPI_File_read (md->fh, md->b.buff, md->b.vars_size, MPI_BYTE
                                  ,&md->status
                                  );
                    adios_parse_vars_index_v1 (&md->b, &md->old_vars_root);

                    adios_init_buffer_read_attributes_index (&md->b);
                    MPI_File_seek (md->fh, md->b.attrs_index_offset
                                  ,MPI_SEEK_SET
                                  );
                    MPI_File_read (md->fh, md->b.buff, md->b.attrs_size
                                  ,MPI_BYTE, &md->status
                                  );
                    adios_parse_attributes_index_v1 (&md->b
                                                    ,&md->old_attrs_root
                                                    );

                    fd->base_offset = md->b.end_of_pgs;
                    fd->pg_start_in_file = fd->base_offset;
                }
                else
                {
                    fd->base_offset = 0;
                    fd->pg_start_in_file = 0;
                }

                MPI_File_close (&md->fh);
            }
            else
            {
                fd->base_offset = 0;
                fd->pg_start_in_file = 0;
            }

            // figure out the offsets and create the file with proper striping
            // before the MPI_File_open is called
            adios_mpi_build_file_offset (md, fd, name);

            // cascade the opens to avoid trashing the metadata server
            if (previous == -1)
            {
                // we know it exists, because we created it if it didn't
                // when reading the old file so can just open wronly
                // but adding the create for consistency with write mode
                // so it is easier to merge write/append later
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_WRONLY | MPI_MODE_CREATE
                                    ,MPI_INFO_NULL
                                    ,&md->fh
                                    );
                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
            }
            else
            {
                MPI_Recv (&flag, 1, MPI_INTEGER, previous, previous
                         ,md->group_comm, &md->status
                         );
                if (next != -1)
                {
                    MPI_Isend (&flag, 1, MPI_INTEGER, next, current
                              ,md->group_comm, &md->req
                              );
                }
                err = MPI_File_open (MPI_COMM_SELF, name
                                    ,MPI_MODE_WRONLY
                                    ,MPI_INFO_NULL
                                    ,&md->fh
                                    );
            }

            if (err != MPI_SUCCESS)
            {
                char e [MPI_MAX_ERROR_STRING];
                int len = 0;
                memset (e, 0, MPI_MAX_ERROR_STRING);
                MPI_Error_string (err, e, &len);
                fprintf (stderr, "MPI open write failed for %s: '%s'\n"
                        ,name, e
                        );
                free (name);

                return adios_flag_no;
            }

            break;
        }

        default:
        {
            fprintf (stderr, "Unknown file mode: %d\n", fd->mode);

            free (name);

            return adios_flag_no;
        }
    }

    free (name);

    if (fd->shared_buffer == adios_flag_no && fd->mode != adios_mode_read)
    {
        // write the process group header
        adios_write_process_group_header_v1 (fd, fd->write_size_bytes);

        MPI_File_seek (md->fh, fd->base_offset, MPI_SEEK_SET);
        MPI_File_write (md->fh, fd->buffer, fd->bytes_written, MPI_BYTE
                       ,&md->status
                       );
        int count;
        MPI_Get_count (&md->status, MPI_BYTE, &count);
        if (count != fd->bytes_written)
        {
            fprintf (stderr, "a:MPI method tried to write %llu, "
                             "only wrote %d\n"
                    ,fd->bytes_written
                    ,count
                    );
        }
        fd->base_offset += count;
        fd->offset = 0;
        fd->bytes_written = 0;
        adios_shared_buffer_free (&md->b);

        // setup for writing vars
        adios_write_open_vars_v1 (fd);
        md->vars_start = fd->base_offset;
        md->vars_header_size = fd->offset;
        fd->base_offset += fd->offset;
        MPI_File_seek (md->fh, md->vars_header_size, MPI_SEEK_CUR);
        fd->offset = 0;
        fd->bytes_written = 0;
        adios_shared_buffer_free (&md->b);
    }

    return fd->shared_buffer;
}

void adios_mpi_stagger_write (struct adios_file_struct * fd
                     ,struct adios_var_struct * v
                     ,void * data
                     ,struct adios_method_struct * method
                     )
{
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                      method->method_data;

    if (v->got_buffer == adios_flag_yes)
    {
        if (data != v->data)  // if the user didn't give back the same thing
        {
            if (v->free_data == adios_flag_yes)
            {
                free (v->data);
                adios_method_buffer_free (v->data_size);
            }
        }
        else
        {
            // we already saved all of the info, so we're ok.
            return;
        }
    }

    if (fd->shared_buffer == adios_flag_no)
    {
        // var payload sent for sizing information
        adios_write_var_header_v1 (fd, v);

        MPI_File_write (md->fh, fd->buffer, fd->bytes_written
                       ,MPI_BYTE, &md->status
                       );
        int count;
        MPI_Get_count (&md->status, MPI_BYTE, &count);
        if (count != fd->bytes_written)
        {
            fprintf (stderr, "b:MPI method tried to write %llu, "
                             "only wrote %d\n"
                    ,fd->bytes_written
                    ,count
                    );
        }
        fd->base_offset += count;
        fd->offset = 0;
        fd->bytes_written = 0;
        adios_shared_buffer_free (&md->b);

        // write payload
        // adios_write_var_payload_v1 (fd, v);
        uint64_t var_size = adios_get_var_size (v, fd->group, v->data);
        MPI_File_write (md->fh, v->data, var_size, MPI_BYTE, &md->status);
        MPI_Get_count (&md->status, MPI_BYTE, &count);
        if (count != var_size)
        {
            fprintf (stderr, "c:MPI method tried to write %llu, "
                             "only wrote %d\n"
                    ,var_size
                    ,count
                    );
        }
        fd->base_offset += count;
        fd->offset = 0;
        fd->bytes_written = 0;
        adios_shared_buffer_free (&md->b);
    }
}

void adios_mpi_stagger_get_write_buffer (struct adios_file_struct * fd
                                ,struct adios_var_struct * v
                                ,uint64_t * size
                                ,void ** buffer
                                ,struct adios_method_struct * method
                                )
{
    uint64_t mem_allowed;

    if (*size == 0)
    {
        *buffer = 0;

        return;
    }

    if (v->data && v->free_data)
    {
        adios_method_buffer_free (v->data_size);
        free (v->data);
    }

    mem_allowed = adios_method_buffer_alloc (*size);
    if (mem_allowed == *size)
    {
        *buffer = malloc (*size);
        if (!*buffer)
        {
            adios_method_buffer_free (mem_allowed);
            fprintf (stderr, "Out of memory allocating %llu bytes for %s\n"
                    ,*size, v->name
                    );
            v->got_buffer = adios_flag_no;
            v->free_data = adios_flag_no;
            v->data_size = 0;
            v->data = 0;
            *size = 0;
            *buffer = 0;
        }
        else
        {
            v->got_buffer = adios_flag_yes;
            v->free_data = adios_flag_yes;
            v->data_size = mem_allowed;
            v->data = *buffer;
        }
    }
    else
    {
        adios_method_buffer_free (mem_allowed);
        fprintf (stderr, "OVERFLOW: Cannot allocate requested buffer of %llu "
                         "bytes for %s\n"
                ,*size
                ,v->name
                );
        *size = 0;
        *buffer = 0;
    }
}

void adios_mpi_stagger_read (struct adios_file_struct * fd
                    ,struct adios_var_struct * v, void * buffer
                    ,uint64_t buffer_size
                    ,struct adios_method_struct * method
                    )
{
    v->data = buffer;
    v->data_size = buffer_size;
}

static void adios_mpi_do_read (struct adios_file_struct * fd
                              ,struct adios_method_struct * method
                              )
{
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                      method->method_data;
    struct adios_var_struct * v = fd->group->vars;

    struct adios_parse_buffer_struct data;

    data.vars = v;
    data.buffer = 0;
    data.buffer_len = 0;

    switch (md->b.version)
    {
        case 1:
        {
            // the three section headers
            struct adios_process_group_header_struct_v1 pg_header;
            struct adios_vars_header_struct_v1 vars_header;
            struct adios_attributes_header_struct_v1 attrs_header;

            struct adios_var_header_struct_v1 var_header;
            struct adios_var_payload_struct_v1 var_payload;
            struct adios_attribute_struct_v1 attribute;

            int i;

            adios_init_buffer_read_process_group (&md->b);
            MPI_File_seek (md->fh, md->b.read_pg_offset
                          ,MPI_SEEK_SET
                          );
            MPI_File_read (md->fh, md->b.buff, md->b.read_pg_size, MPI_BYTE
                          ,&md->status
                          );
            adios_parse_process_group_header_v1 (&md->b, &pg_header);

            adios_parse_vars_header_v1 (&md->b, &vars_header);

            for (i = 0; i < vars_header.count; i++)
            {
                memset (&var_payload, 0
                       ,sizeof (struct adios_var_payload_struct_v1)
                       );
                adios_parse_var_data_header_v1 (&md->b, &var_header);

                struct adios_var_struct * v1 = v;
                while (v1)
                {
                    if (   strcasecmp (var_header.name, v1->name)
                        || strcasecmp (var_header.path, v1->path)
                       )
                    {
                        v1 = v1->next;
                    }
                    else
                    {
                        break;
                    }
                }

                if (v1)
                {
                    var_payload.payload = v1->data;
                    adios_parse_var_data_payload_v1 (&md->b, &var_header
                                                    ,&var_payload
                                                    ,v1->data_size
                                                    );
                }
                else
                {
                    printf ("MPI read: skipping name: %s path: %s\n"
                           ,var_header.name, var_header.path
                           );
                    adios_parse_var_data_payload_v1 (&md->b, &var_header
                                                    ,NULL, 0
                                                    );
                }

                adios_clear_var_header_v1 (&var_header);
            }

#if 1
            adios_parse_attributes_header_v1 (&md->b, &attrs_header);

            for (i = 0; i < attrs_header.count; i++)
            {
                adios_parse_attribute_v1 (&md->b, &attribute);
                adios_clear_attribute_v1 (&attribute);
            }
#endif
            adios_clear_process_group_header_v1 (&pg_header);

            break;
        }

        default:
            fprintf (stderr, "MPI read: file version unknown: %u\n"
                    ,md->b.version
                    );
            return;
    }

    adios_buffer_struct_clear (&md->b);
}

void adios_mpi_stagger_close (struct adios_file_struct * fd
                     ,struct adios_method_struct * method
                     )
{
    struct adios_MPI_data_struct * md = (struct adios_MPI_data_struct *)
                                                 method->method_data;
    struct adios_attribute_struct * a = fd->group->attributes;

    struct adios_index_process_group_struct_v1 * new_pg_root = 0;
    struct adios_index_var_struct_v1 * new_vars_root = 0;
    struct adios_index_attribute_struct_v1 * new_attrs_root = 0;

    switch (fd->mode)
    {
        case adios_mode_read:
        {
            // read the index to find the place to start reading
            adios_mpi_do_read (fd, method);
            struct adios_var_struct * v = fd->group->vars;
            while (v)
            {
                v->data = 0;
                v = v->next;
            }

            break;
        }

        case adios_mode_write:
        {
            char * buffer = 0;
            uint64_t buffer_size = 0;
            uint64_t buffer_offset = 0;
            uint64_t index_start = md->b.pg_index_offset;

            if (fd->shared_buffer == adios_flag_no)
            {
                MPI_Offset new_off;
                // set it up so that it will start at 0, but have correct sizes
                MPI_File_get_position (md->fh, &new_off);
                fd->offset = fd->base_offset - md->vars_start;
                fd->vars_start = 0;
                fd->buffer_size = 0;
                adios_write_close_vars_v1 (fd);
                // fd->vars_start gets updated with the size written
                MPI_File_seek (md->fh, md->vars_start, MPI_SEEK_SET);
                MPI_File_write (md->fh, fd->buffer, md->vars_header_size
                               ,MPI_BYTE, &md->status
                               );
                int count;
                MPI_Get_count (&md->status, MPI_BYTE, &count);
                if (count != md->vars_header_size)
                {
                    fprintf (stderr, "d:MPI method tried to write %llu, "
                                     "only wrote %d\n"
                            ,md->vars_header_size
                            ,count
                            );
                }
                fd->offset = 0;
                fd->bytes_written = 0;
                adios_shared_buffer_free (&md->b);

                adios_write_open_attributes_v1 (fd);
                md->vars_start = new_off;
                md->vars_header_size = fd->offset;
                MPI_File_seek (md->fh, new_off + md->vars_header_size
                              ,MPI_SEEK_SET
                              ); // go back to end, but after attr header
                fd->base_offset += fd->offset;  // add size of header
                fd->offset = 0;
                fd->bytes_written = 0;

                while (a)
                {
                    adios_write_attribute_v1 (fd, a);
                    MPI_File_write (md->fh, fd->buffer, fd->bytes_written
                                   ,MPI_BYTE, &md->status
                                   );
                    MPI_Get_count (&md->status, MPI_BYTE, &count);
                    if (count != fd->bytes_written)
                    {
                        fprintf (stderr, "e:MPI method tried to write %llu, "
                                         "only wrote %d\n"
                                ,fd->bytes_written
                                ,count
                                );
                    }
                    fd->base_offset += count;
                    fd->offset = 0;
                    fd->bytes_written = 0;
                    adios_shared_buffer_free (&md->b);

                    a = a->next;
                }

                // set it up so that it will start at 0, but have correct sizes
                fd->offset = fd->base_offset - md->vars_start;
                fd->vars_start = 0;
                fd->buffer_size = 0;
                adios_write_close_attributes_v1 (fd);
                MPI_File_seek (md->fh, md->vars_start, MPI_SEEK_SET);
                // fd->vars_start gets updated with the size written
                MPI_File_write (md->fh, fd->buffer, md->vars_header_size
                               ,MPI_BYTE, &md->status
                               );
                MPI_Get_count (&md->status, MPI_BYTE, &count);
                if (count != md->vars_header_size)
                {
                    fprintf (stderr, "f:MPI method tried to write %llu, "
                                     "only wrote %d\n"
                            ,md->vars_header_size
                            ,count
                            );
                }
                fd->offset = 0;
                fd->bytes_written = 0;
            }

            // build index appending to any existing index
            adios_build_index_v1 (fd, &md->old_pg_root, &md->old_vars_root
                                 ,&md->old_attrs_root
                                 );
            // if collective, gather the indexes from the rest and call
            if (md->group_comm != MPI_COMM_NULL)
            {
                if (md->rank == 0)
                {
                    int * index_sizes = malloc (4 * md->size);
                    int * index_offsets = malloc (4 * md->size);
                    char * recv_buffer = 0;
                    uint32_t size = 0;
                    uint32_t total_size = 0;
                    int i;

                    MPI_Gather (&size, 1, MPI_INT
                               ,index_sizes, 1, MPI_INT
                               ,0, md->group_comm
                               );

                    for (i = 0; i < md->size; i++)
                    {
                        index_offsets [i] = total_size;
                        total_size += index_sizes [i];
                    } 

                    recv_buffer = malloc (total_size);

                    MPI_Gatherv (&size, 0, MPI_BYTE
                                ,recv_buffer, index_sizes, index_offsets
                                ,MPI_BYTE, 0, md->group_comm
                                );

                    char * buffer_save = md->b.buff;
                    uint64_t buffer_size_save = md->b.length;
                    uint64_t offset_save = md->b.offset;

                    for (i = 1; i < md->size; i++)
                    {
                        md->b.buff = recv_buffer + index_offsets [i];
                        md->b.length = index_sizes [i];
                        md->b.offset = 0;

                        adios_parse_process_group_index_v1 (&md->b
                                                           ,&new_pg_root
                                                           );
                        adios_parse_vars_index_v1 (&md->b, &new_vars_root);
                        adios_parse_attributes_index_v1 (&md->b
                                                        ,&new_attrs_root
                                                        );
                        adios_merge_index_v1 (&md->old_pg_root
                                             ,&md->old_vars_root
                                             ,&md->old_attrs_root
                                             ,new_pg_root, new_vars_root
                                             ,new_attrs_root
                                             );
                        new_pg_root = 0;
                        new_vars_root = 0;
                        new_attrs_root = 0;
                    }
                    md->b.buff = buffer_save;
                    md->b.length = buffer_size_save;
                    md->b.offset = offset_save;

                    free (recv_buffer);
                    free (index_sizes);
                    free (index_offsets);
                }
                else
                {
                    adios_write_index_v1 (&buffer, &buffer_size, &buffer_offset
                                         ,0, md->old_pg_root
                                         ,md->old_vars_root
                                         ,md->old_attrs_root
                                         );

                    MPI_Gather (&buffer_size, 1, MPI_INT, 0, 0, MPI_INT
                               ,0, md->group_comm
                               );
                    MPI_Gatherv (buffer, buffer_size, MPI_BYTE
                                ,0, 0, 0, MPI_BYTE
                                ,0, md->group_comm
                                );
                }
            }

            if (fd->shared_buffer == adios_flag_yes)
            {
                // if we have a comm and an OST count, stagger writing
                if (md->group_comm != MPI_COMM_NULL && md->storage_targets)
                {
                    // ordering (4 OSTs and 10 ranks):
                    // 0: A 1: B 2: C 3: D
                    // 4: A 5: B 6: C 7: D
                    // 8: A 9: B
                    // write all As, then Bs, then Cs, then Ds.
                    // each A will send to B, etc. to do the ordering
                    int next_rank = md->rank + 1;
                    int prev_rank = md->rank - 1;
                    int current_rank = md->rank;
                    int flag = 0;

                    if (   next_rank >= md->size
                        ||    current_rank % md->storage_targets
                           == md->storage_targets - 1
                       )
                        next_rank = -1;
                    if (current_rank % md->storage_targets == 0)
                        prev_rank = -1;

                    if (prev_rank != -1)
                    {
                        MPI_Recv (&flag, 1, MPI_INTEGER, prev_rank, prev_rank
                                 ,md->group_comm, &md->status
                                 );
                    }
                    //write
                    MPI_File_seek (md->fh, fd->base_offset, MPI_SEEK_SET);
                    MPI_File_write (md->fh, fd->buffer, fd->bytes_written
                                   ,MPI_BYTE, &md->status
                                   );
                    if (next_rank != -1)
                    {
                        //MPI_Isend (&flag, 1, MPI_INTEGER, next_rank
                        //          ,current_rank, md->group_comm, &md->req
                        //          );
                        MPI_Send (&flag, 1, MPI_INTEGER, next_rank
                                 ,current_rank, md->group_comm
                                 );
                    }
                }
                else
                {
                    // everyone writes their data
                    MPI_File_seek (md->fh, fd->base_offset, MPI_SEEK_SET);
                    MPI_File_write (md->fh, fd->buffer, fd->bytes_written
                                   ,MPI_BYTE, &md->status
                                   );
                }
            }

            if (md->rank == 0)
            {
                adios_write_index_v1 (&buffer, &buffer_size, &buffer_offset
                                     ,index_start, md->old_pg_root
                                     ,md->old_vars_root
                                     ,md->old_attrs_root
                                     );
                adios_write_version_v1 (&buffer, &buffer_size, &buffer_offset);

                MPI_File_seek (md->fh, md->b.pg_index_offset, MPI_SEEK_SET);
                MPI_File_write (md->fh, buffer, buffer_offset, MPI_BYTE
                               ,&md->status
                               );
            }

            if (buffer)
            {
                free (buffer);
                buffer = 0;
                buffer_size = 0;
                buffer_offset = 0;
            }

            adios_clear_index_v1 (new_pg_root, new_vars_root, new_attrs_root);
            adios_clear_index_v1 (md->old_pg_root, md->old_vars_root
                                 ,md->old_attrs_root
                                 );
            new_pg_root = 0;
            new_vars_root = 0;
            new_attrs_root = 0;
            md->old_pg_root = 0;
            md->old_vars_root = 0;
            md->old_attrs_root = 0;

            break;
        }

        case adios_mode_append:
        {
            char * buffer = 0;
            uint64_t buffer_size = 0;
            uint64_t buffer_offset = 0;
            uint64_t index_start = md->b.pg_index_offset;

            if (fd->shared_buffer == adios_flag_no)
            {
                MPI_Offset new_off;
                // set it up so that it will start at 0, but have correct sizes
                MPI_File_get_position (md->fh, &new_off);
                fd->offset = fd->base_offset - md->vars_start;
                fd->vars_start = 0;
                fd->buffer_size = 0;
                adios_write_close_vars_v1 (fd);
                // fd->vars_start gets updated with the size written
                MPI_File_seek (md->fh, md->vars_start, MPI_SEEK_SET);
                MPI_File_write (md->fh, fd->buffer, md->vars_header_size
                               ,MPI_BYTE, &md->status
                               );
                int count;
                MPI_Get_count (&md->status, MPI_BYTE, &count);
                if (count != md->vars_header_size)
                {
                    fprintf (stderr, "d:MPI method tried to write %llu, "
                                     "only wrote %d\n"
                            ,md->vars_header_size
                            ,count
                            );
                }
                fd->offset = 0;
                fd->bytes_written = 0;
                adios_shared_buffer_free (&md->b);

                adios_write_open_attributes_v1 (fd);
                md->vars_start = new_off;
                md->vars_header_size = fd->offset;
                MPI_File_seek (md->fh, new_off + md->vars_header_size
                              ,MPI_SEEK_SET
                              ); // go back to end, but after attr header
                fd->base_offset += fd->offset;  // add size of header
                fd->offset = 0;
                fd->bytes_written = 0;

                while (a)
                {
                    adios_write_attribute_v1 (fd, a);
                    MPI_File_write (md->fh, fd->buffer, fd->bytes_written
                                   ,MPI_BYTE, &md->status
                                   );
                    MPI_Get_count (&md->status, MPI_BYTE, &count);
                    if (count != fd->bytes_written)
                    {
                        fprintf (stderr, "e:MPI method tried to write %llu, "
                                         "only wrote %d\n"
                                ,fd->bytes_written
                                ,count
                                );
                    }
                    fd->base_offset += count;
                    fd->offset = 0;
                    fd->bytes_written = 0;
                    adios_shared_buffer_free (&md->b);

                    a = a->next;
                }

                // set it up so that it will start at 0, but have correct sizes
                fd->offset = fd->base_offset - md->vars_start;
                fd->vars_start = 0;
                fd->buffer_size = 0;
                adios_write_close_attributes_v1 (fd);
                MPI_File_seek (md->fh, md->vars_start, MPI_SEEK_SET);
                // fd->vars_start gets updated with the size written
                MPI_File_write (md->fh, fd->buffer, md->vars_header_size
                               ,MPI_BYTE, &md->status
                               );
                MPI_Get_count (&md->status, MPI_BYTE, &count);
                if (count != md->vars_header_size)
                {
                    fprintf (stderr, "f:MPI method tried to write %llu, "
                                     "only wrote %d\n"
                            ,md->vars_header_size
                            ,count
                            );
                }
                fd->offset = 0;
                fd->bytes_written = 0;
            }

            // build index appending to any existing index
            adios_build_index_v1 (fd, &md->old_pg_root, &md->old_vars_root
                                 ,&md->old_attrs_root
                                 );
            // if collective, gather the indexes from the rest and call
            if (md->group_comm != MPI_COMM_NULL)
            {
                if (md->rank == 0)
                {
                    int * index_sizes = malloc (4 * md->size);
                    int * index_offsets = malloc (4 * md->size);
                    char * recv_buffer = 0;
                    uint32_t size = 0;
                    uint32_t total_size = 0;
                    int i;

                    MPI_Gather (&size, 1, MPI_INT
                               ,index_sizes, 1, MPI_INT
                               ,0, md->group_comm
                               );

                    for (i = 0; i < md->size; i++)
                    {
                        index_offsets [i] = total_size;
                        total_size += index_sizes [i];
                    }

                    recv_buffer = malloc (total_size);

                    MPI_Gatherv (&size, 0, MPI_BYTE
                                ,recv_buffer, index_sizes, index_offsets
                                ,MPI_BYTE, 0, md->group_comm
                                );

                    char * buffer_save = md->b.buff;
                    uint64_t buffer_size_save = md->b.length;
                    uint64_t offset_save = md->b.offset;

                    for (i = 1; i < md->size; i++)
                    {
                        md->b.buff = recv_buffer + index_offsets [i];
                        md->b.length = index_sizes [i];
                        md->b.offset = 0;

                        adios_parse_process_group_index_v1 (&md->b
                                                           ,&new_pg_root
                                                           );
                        adios_parse_vars_index_v1 (&md->b, &new_vars_root);
                        adios_parse_attributes_index_v1 (&md->b
                                                        ,&new_attrs_root
                                                        );
                        adios_merge_index_v1 (&md->old_pg_root
                                             ,&md->old_vars_root
                                             ,&md->old_attrs_root
                                             ,new_pg_root, new_vars_root
                                             ,new_attrs_root
                                             );
                        new_pg_root = 0;
                        new_vars_root = 0;
                        new_attrs_root = 0;
                    }
                    md->b.buff = buffer_save;
                    md->b.length = buffer_size_save;
                    md->b.offset = offset_save;

                    free (recv_buffer);
                    free (index_sizes);
                    free (index_offsets);
                }
                else
                {
                    adios_write_index_v1 (&buffer, &buffer_size, &buffer_offset
                                         ,0, md->old_pg_root
                                         ,md->old_vars_root
                                         ,md->old_attrs_root
                                         );

                    MPI_Gather (&buffer_size, 1, MPI_INT, 0, 0, MPI_INT
                               ,0, md->group_comm
                               );
                    MPI_Gatherv (buffer, buffer_size, MPI_BYTE
                                ,0, 0, 0, MPI_BYTE
                                ,0, md->group_comm
                                );
                }
            }

            if (fd->shared_buffer == adios_flag_yes)
            {
                // everyone writes their data
                MPI_File_seek (md->fh, fd->base_offset, MPI_SEEK_SET);
                MPI_File_write (md->fh, fd->buffer, fd->bytes_written, MPI_BYTE
                               ,&md->status
                               );
            }

            if (md->rank == 0)
            {
                adios_write_index_v1 (&buffer, &buffer_size, &buffer_offset
                                     ,index_start, md->old_pg_root
                                     ,md->old_vars_root
                                     ,md->old_attrs_root
                                     );
                adios_write_version_v1 (&buffer, &buffer_size, &buffer_offset);

                MPI_File_seek (md->fh, md->b.pg_index_offset, MPI_SEEK_SET);
                MPI_File_write (md->fh, buffer, buffer_offset, MPI_BYTE
                               ,&md->status
                               );
            }

            free (buffer);

            adios_clear_index_v1 (new_pg_root, new_vars_root, new_attrs_root);
            adios_clear_index_v1 (md->old_pg_root, md->old_vars_root
                                 ,md->old_attrs_root
                                 );
            new_pg_root = 0;
            new_vars_root = 0;
            new_attrs_root = 0;
            md->old_pg_root = 0;
            md->old_vars_root = 0;
            md->old_attrs_root = 0;

            break;
        }

        default:
        {
            fprintf (stderr, "Unknown file mode: %d\n", fd->mode);
        }
    }

    if (md && md->fh)
        MPI_File_close (&md->fh);

    if (   md->group_comm != MPI_COMM_WORLD
        && md->group_comm != MPI_COMM_SELF
        && md->group_comm != MPI_COMM_NULL
       )
    {
        md->group_comm = MPI_COMM_NULL;
    }

    md->fh = 0;
    md->req = 0;
    memset (&md->status, 0, sizeof (MPI_Status));
    md->group_comm = MPI_COMM_NULL;

    adios_clear_index_v1 (md->old_pg_root, md->old_vars_root
                         ,md->old_attrs_root
                         );
    md->old_pg_root = 0;
    md->old_vars_root = 0;
    md->old_attrs_root = 0;
}

void adios_mpi_stagger_finalize (int mype, struct adios_method_struct * method)
{
// nothing to do here
    if (adios_mpi_stagger_initialized)
        adios_mpi_stagger_initialized = 0;
}

void adios_mpi_stagger_end_iteration (struct adios_method_struct * method)
{
}

void adios_mpi_stagger_start_calculation (struct adios_method_struct * method)
{
}

void adios_mpi_stagger_stop_calculation (struct adios_method_struct * method)
{
}
