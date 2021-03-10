## Parallel I/O Kernel Case Study -- E3SM

This repository contains a case study of parallel I/O kernel from the
[E3SM](https://github.com/E3SM-Project/E3SM) climate simulation model. The
E3SM I/O module makes use of [PIO](https://github.com/NCAR/ParallelIO)
library which is built on top of
[PnetCDF](https://github.com/Parallel-NetCDF/PnetCDF) and
[NetCDF-4](http://www.unidata.ucar.edu/software/netcdf). PnetCDF is a
parallel I/O library for accessing the classic NetCDF files, i.e. CDF-1,
CDF-2, and CDF-5 formats. NetCDF-4 provides parallel I/O capability for
[HDF5](https://www.hdfgroup.org/solutions/hdf5) based NetCDF file format.
The benchmark program in this repository, e3sm_io.c, is designed to evaluate
the E3SM I/O kernel when using the PnetCDF library to perform the I/O task.
In particular, it studies one of the E3SM's most challenging I/O patterns
when the problem domain is represented by cubed sphere grids which produce
long lists of small and noncontiguous requests in each of all MPI processes.

The I/O kernel is implemented by using PnetCDF nonblocking varn APIs, which
can aggregate multiple requests to a variable or across variables. In addition
to timings and I/O bandwidths, the benchmark reports the PnetCDF internal
memory footprint (high water mark).

The I/O patterns (data decompositions among MPI processes) used in this case
study were captured by the [PIO](https://github.com/NCAR/ParallelIO) library.
A data decomposition file records the data access patterns at the array element
level for each MPI process. The access offsets are stored in a text file,
referred by PIO as the `decomposition file. This benchmark currently studies
two cases from E3SM, namely F and G cases. The F case uses three unique data
decomposition patterns shared by 388 2D and 3D variables (2 sharing
Decomposition 1, 323 sharing Decomposition 2, and 63 sharing Decomposition 3).
The G case uses 6 data decompositions shared by 52 variables (6 sharing
Decomposition 1, 2 sharing Decomposition 2, 25 sharing Decomposition 3, 2
sharing Decomposition 4, 2 sharing Decomposition 5, and 4 sharing Decomposition
6).

### Prepare the data decomposition file in NetCDF file format
* For the F case, the three data decomposition files generated by PIO library
  are in text format with file extension name `.dat`. The decomposition files
  must first be combined and converted into a NetCDF file, to be read in
  parallel as the input file to this benchmark program. Similarly for the G
  case, the six decomposition files need to be converted first.

### To install on Cori
You can change the Pnetcdf and HDF5 path to your customized ones.
```
0. git clone https://github.com/QiaoK/E3SM-IO.git
1. cd E3SM-IO
2. git checkout HDF5_develop
3. autoreconf -fi
4. ./configure --prefix=($pwd) --with-pnetcdf=/opt/cray/pe/parallel-netcdf/1.12.0.1/INTEL/19.1 --with-hdf5=/opt/cray/pe/hdf5-parallel/1.12.0.0/INTEL/19.1
5. make
6. make -j8
```
### Example script on Cori
```
#SBATCH -N 1
#SBATCH --time-min=00:05:00
#SBATCH -t 00:05:00
#SBATCH -o qout.%j
#SBATCH -e qout.%j
#SBATCH -L SCRATCH
#SBATCH -C knl,quad,cache
#SBATCH --mail-type=begin,end,fail
#SBATCH --mail-user=QKang@lbl.gov

ulimit -c unlimited

cd $SLURM_SUBMIT_DIR

export MPICH_GNI_DYNAMIC_CONN=disabled

export ROMIO_PRINT_HINTS=1
# specifiy the number of MPI process per compute node
nprocs_per_node=64

# calculate the number of logical cores to allocate per MPI process

# NP is the total number of MPI processes to run
NP=$(($nprocs_per_node * $SLURM_JOB_NUM_NODES))

KNL_OPTS="-c 4 --cpu_bind=cores"
#KNL_OPTS=""

echo "--------------------------------------------------"
echo "---- Running on $NP MPI processes on Cori NKL ----"
echo "---- SLURM_JOB_NUM_NODES = $SLURM_JOB_NUM_NODES"
echo "---- Running $nprocs_per_node MPI processes per KNL node"
echo "---- command: srun -n $NP $KNL_OPTS"
echo "--------------------------------------------------"
echo ""

# input/output files
OUTDIR=$SCRATCH/FS_1M_128
cp e3sm_io $OUTDIR
cp datasets/*.nc $OUTDIR
cd $OUTDIR

#export LD_LIBRARY_PATH=/global/homes/q/qkt561/mpich_develop/multi_dataset/hdf5/install/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/opt/cray/pe/hdf5-parallel/1.12.0.0/INTEL/19.1/lib:$LD_LIBRARY_PATH
RUN_CMMD=e3sm_io
cd $OUTDIR
#RUN_OPTS="-k -o $OUTDIR -n -a hdf5 -f 0 $SCRATCH/FS_1M_128/f_case_72x777602_21632p.nc"
RUN_OPTS="-k -o $OUTDIR -n -a hdf5 -f 0 $SCRATCH/FS_1M_128/f_case_866x72_16p.nc"
#RUN_OPTS="-k -o $OUTDIR -n -a hdf5 -f 0 $SCRATCH/FS_1M_128/f_case_48602x72_512p.nc"
command="srun -n $NP $KNL_OPTS ./$RUN_CMMD $RUN_OPTS"
echo "command=$command"
echo "---- KNL $RUN_CMMD F case ----"
$command
```
