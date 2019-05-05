/*********************************************************************
 *
 * Copyright (C) 2018, Northwestern University
 * See COPYRIGHT notice in top-level directory.
 *
 * This program uses the E3SM I/O patterns recorded by the PIO library to
 * evaluate the performance of two PnetCDF APIs: ncmpi_vard_all(), and
 * ncmpi_iput_varn(). The E3SM I/O patterns consist of a large number of small,
 * noncontiguous requests on each MPI process, which presents a challenge for
 * achieving a good performance.
 *
 * See README.md for compile and run instructions.
 *
 *********************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include <e3sm_io.h>

struct off_len {
    int off;
    int len;
};

/*----< compare() >---------------------------------------------------------*/
/* This subroutine is used in qsort() */
static int compare(const void *p1, const void *p2)
{
    int i = ((struct off_len*)p1)->off;
    int j = ((struct off_len*)p2)->off;
    if (i > j) return (1);
    if (i < j) return (-1);
    return (0);
}

/*----< read_io_decomp() >-------------------------------------------------*/
/* Read I/O decomposition file, infname. The contents of the file are, for
 * example from 866x72_16p.nc
 *
 *   % ncmpidump -h f_case_866x72_16p.nc
 *   netcdf f_case_866x72_16p.nc {
 *   // file format: CDF-1
 *   dimensions:
 *       decomp_nprocs = 16 ;
 *       D1.total_nreqs = 47 ;
 *       D2.total_nreqs = 407 ;
 *       D3.total_nreqs = 29788 ;
 *   variables:
 *       int D1.nreqs(decomp_nprocs) ;
 *           D1.nreqs:description = "Number of noncontiguous requests per process" ;
 *       int D1.offsets(D1.total_nreqs) ;
 *           D1.offsets:description = "Flattened starting indices of noncontiguous requests" ;
 *       int D1.lengths(D1.total_nreqs) ;
 *           D1.lengths:description = "Lengths of noncontiguous requests" ;
 *       int D2.nreqs(decomp_nprocs) ;
 *           D2.nreqs:description = "Number of noncontiguous requests per process" ;
 *       int D2.offsets(D2.total_nreqs) ;
 *           D2.offsets:description = "Flattened starting indices of noncontiguous requests" ;
 *       int D2.lengths(D2.total_nreqs) ;
 *           D2.lengths:description = "Lengths of noncontiguous requests" ;
 *       int D3.nreqs(decomp_nprocs) ;
 *           D3.nreqs:description = "Number of noncontiguous requests per process" ;
 *       int D3.offsets(D3.total_nreqs) ;
 *           D3.offsets:description = "Flattened starting indices of noncontiguous requests" ;
 *       int D3.lengths(D3.total_nreqs) ;
 *           D3.lengths:description = "Lengths of noncontiguous requests" ;
 *
 *   // global attributes:
 *       :command_line = "./dat2nc -o f_case_866x72_16p.nc -1 datasets/piodecomp16tasks16io01dims_ioid_514.dat -2 datasets/piodecomp16tasks16io01dims_ioid_516.dat -3 datasets/piodecomp16tasks16io02dims_ioid_548.dat " ;
 *       :D1.ndims = 1 ;
 *       :D1.dim_0 = 866 ;
 *       :D1.max_nreqs = 4 ;
 *       :D1.min_nreqs = 2 ;
 *       :D2.ndims = 1 ;
 *       :D2.dim_0 = 866 ;
 *       :D2.max_nreqs = 39 ;
 *       :D2.min_nreqs = 13 ;
 *       :D3.ndims = 2 ;
 *       :D3.dim_0 = 72 ;
 *       :D3.dim_1 = 866 ;
 *       :D3.max_nreqs = 2808 ;
 *       :D3.min_nreqs = 936 ;
 *   }
 *
 * Arguments:
 *     infname: input file name, a NetCDF file containing the data
 *              decomposition generated by PIO library from an E3SM run.
 *
 *     dims:         (OUT) dimension lengths.
 *     contig_nreqs: (OUT) number of noncontiguous requests assigned to this
 *                         process. May be zero.
 *     disps:        (OUT) starting offsets of individual requests (sorted)
 *                         Memory space will be allocated in this subroutine
 *                         and to be freed by the caller. Any pair of disps[][i]
 *                         blocklens[][i] is within the size of the least
 *                         significant dimension.
 *     blocklens:    (OUT) lengths of individual requests
 *                         Memory space will be allocated in this subroutine
 *                         and to be freed by the caller.
 */
int
read_decomp(const char *infname,        /* IN */
            int        *num_decomp,     /* OUT */
            MPI_Offset  dims[][2],      /* OUT */
            int         contig_nreqs[], /* OUT */
            int        *disps[],        /* OUT: to be freed by caller */
            int        *blocklens[])    /* OUT: to be freed by caller */
{
    char name[128];
    int err, nerrs=0, rank, nprocs, ncid, varid, proc_start, proc_numb;
    int i, j, nreqs, *all_nreqs, ndims, dimids[2], decomp_id;
    MPI_Offset num, decomp_nprocs, total_nreqs, start, count;
    struct off_len *myreqs;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

    /* open input file that contains I/O decomposition information */
    err = ncmpi_open(MPI_COMM_WORLD, infname, NC_NOWRITE, MPI_INFO_NULL,
                     &ncid); ERR

    /* number of decompositions stored in file */
    err = ncmpi_inq_dimid(ncid, "num_decomp", &dimids[0]); ERR
    err = ncmpi_inq_dimlen(ncid, dimids[0], &num); ERR
    *num_decomp = (int)num;

    /* number of processes used when the decomposition was produced */
    err = ncmpi_inq_dimid(ncid, "decomp_nprocs", &dimids[0]); ERR
    err = ncmpi_inq_dimlen(ncid, dimids[0], &decomp_nprocs); ERR

    /* decomp_nprocs is the number of processes used to generate the E3SM data
     * decomposition. nprocs is the number of processes running this benchmark.
     * This benchmark allows nprocs to be different from decomp_nprocs. When
     * nprocs is less than decomp_nprocs, some of nprocs processes will carry
     * out the requests for other decomp_nprocs processes. The requests
     * responsible by this process starts from proc_start with the number
     * proc_numb. When nprocs is bigger than decomp_nprocs, then processes with
     * rank ID >= decomp_nprocs will have no write request and will simply
     * participate the collective I/O subroutines.
     */
    proc_numb = decomp_nprocs / nprocs;
    proc_start = rank * proc_numb;
    if (rank < decomp_nprocs % nprocs) {
        proc_start += rank;
        proc_numb++;
    }
    else
        proc_start += decomp_nprocs % nprocs;
    /* proc_numb is the number of decomposition processes this process is
     * responsible to carry out their requests. proc_start is the starting
     * rank ID of the decomposition process.
     */

    /* decomp_id: D1, D2, ... D6, indicates different decompositions */
    for (decomp_id=0; decomp_id<*num_decomp; decomp_id++) {
        contig_nreqs[decomp_id] = 0;
        disps[decomp_id] = NULL;
        blocklens[decomp_id] = NULL;

        /* total number of noncontiguous requests of all processes */
        sprintf(name, "D%d.total_nreqs", decomp_id+1);
        err = ncmpi_inq_dimid(ncid, name, &dimids[1]); ERR
        err = ncmpi_inq_dimlen(ncid, dimids[1], &total_nreqs); ERR

        /* ndims: number of decomposition dimensions, not variable dimensions
         * In E3SM, decomposition is along the lowest 1 or 2 dimensions of 2D
         * or 3D variables.
         */
        sprintf(name, "D%d.dims", decomp_id+1);
        /* obtain the number of dimensions of this decomposition */
        err = ncmpi_inq_attlen(ncid, NC_GLOBAL, name, &num); ERR
        ndims = num;
        /* obtain the dimension lengths of this decomposition */
        err = ncmpi_get_att_longlong(ncid, NC_GLOBAL, name, dims[decomp_id]); ERR

        /* obtain varid of request variable Dx.nreqs */
        sprintf(name, "D%d.nreqs", decomp_id+1);
        err = ncmpi_inq_varid(ncid, name, &varid); ERR

        /* read all numbers of requests */
        all_nreqs = (int*) malloc(decomp_nprocs * sizeof(int));
        err = ncmpi_get_var_int_all(ncid, varid, all_nreqs); ERR

        /* calculate start index in Dx.offsets for this process */
        i = 0;
        start = 0;
        for (start=0,i=0; i<proc_start; i++) start += all_nreqs[i];

        /* calculate number of requests for this process */
        count = 0;
        for (; i<proc_start + proc_numb; i++) count += all_nreqs[i];
        nreqs = count;
        free(all_nreqs);

        if (verbose)
            printf("D%d rank %d: proc_start=%d proc_numb=%d start%lld count=%lld\n",
                   decomp_id+1,rank,proc_start,proc_numb,start,count);

        /* read starting offsets of requests into disps[] */
        disps[decomp_id] = (int*) malloc(nreqs * sizeof(int));
        sprintf(name, "D%d.offsets", decomp_id+1);
        err = ncmpi_inq_varid(ncid, name, &varid); ERR
        err = ncmpi_get_vara_int_all(ncid, varid, &start, &count, disps[decomp_id]); ERR

        /* read lengths of requests into blocklens[] */
        blocklens[decomp_id] = (int*) malloc(nreqs * sizeof(int));
        sprintf(name, "D%d.lengths", decomp_id+1);
        err = ncmpi_inq_varid(ncid, name, &varid); ERR
        err = ncmpi_get_vara_int_all(ncid, varid, &start, &count, blocklens[decomp_id]); ERR

        /* sort all disps[] of all responsible requests into an increasing
         * order (this is to satisfy the MPI fileview or monotonically
         * nondecreasing file offset requirement)
         */
        myreqs = (struct off_len*) malloc(nreqs*sizeof(struct off_len));
        for (i=0; i<nreqs; i++) {
            myreqs[i].off = disps[decomp_id][i];
            myreqs[i].len = blocklens[decomp_id][i];
        }
        qsort((void*)myreqs, nreqs, sizeof(struct off_len), compare);
        for (i=0; i<nreqs; i++) {
            disps[decomp_id][i] = myreqs[i].off;
            blocklens[decomp_id][i] = myreqs[i].len;
        }
        free(myreqs);

        /* coalesce offset-length pairs */
        j = 0;
        for (i=1; i<nreqs; i++) {
            if (disps[decomp_id][i] % dims[decomp_id][ndims-1] == 0 ||
                disps[decomp_id][i] > disps[decomp_id][j] + blocklens[decomp_id][j]) {
                /* break contiguity at dimension boundaries or noncontiguous */
                j++;
                if (j < i) {
                    disps[decomp_id][j] = disps[decomp_id][i];
                    blocklens[decomp_id][j] = blocklens[decomp_id][i];
                }
            }
            else
                blocklens[decomp_id][j] += blocklens[decomp_id][i];
        }
        /* update number of true noncontiguous requests */
        if (nreqs > 0) contig_nreqs[decomp_id] = j + 1;

        if (verbose) {
            int min_blocklen = blocklens[decomp_id][0];
            int max_blocklen = blocklens[decomp_id][0];
            for (i=1; i<contig_nreqs[decomp_id]; i++) {
                max_blocklen = MAX(blocklens[decomp_id][i], max_blocklen);
                min_blocklen = MIN(blocklens[decomp_id][i], min_blocklen);
            }
            printf("D%d rank %d nreqs=%d contig nreqs=%4d max_blocklen=%d min_blocklen=%d\n",
                   decomp_id+1, rank, nreqs, contig_nreqs[decomp_id], max_blocklen, min_blocklen);
        }
    }

    err = ncmpi_close(ncid); ERR

fn_exit:
    if (nerrs) {
        for (decomp_id=0; decomp_id<*num_decomp; decomp_id++) {
            contig_nreqs[decomp_id] = 0;
            if (disps[decomp_id] != NULL) free(disps[decomp_id]);
            if (blocklens[decomp_id] != NULL) free(blocklens[decomp_id]);
        }
    }
    return nerrs;
}

