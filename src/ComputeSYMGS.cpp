
//@HEADER
// ***************************************************
//
// HPCG: High Performance Conjugate Gradient Benchmark
//
// Contact:
// Michael A. Heroux ( maherou@sandia.gov)
// Jack Dongarra     (dongarra@eecs.utk.edu)
// Piotr Luszczek    (luszczek@eecs.utk.edu)
//
// ***************************************************
//@HEADER

/*!
 @file ComputeSYMGS.cpp

 HPCG routine
 */

#include "ComputeSYMGS.hpp"
#include "ExchangeHalo.hpp"

#include <hip/hip_runtime.h>

__launch_bounds__(1024)
__global__ void kernel_symgs_sweep(local_int_t m,
                                   local_int_t n,
                                   local_int_t block_nrow,
                                   local_int_t offset,
                                   local_int_t ell_width,
                                   const local_int_t* __restrict__ ell_col_ind,
                                   const double* __restrict__ ell_val,
                                   const double* __restrict__ inv_diag,
                                   const double* __restrict__ x,
                                   double* __restrict__ y)
{
    local_int_t gid = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(gid >= block_nrow)
    {
        return;
    }

    local_int_t row = gid + offset;

#if defined(__HIP_PLATFORM_HCC__)
    double sum = __builtin_nontemporal_load(x + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    double sum = x[row];
#endif

    for(local_int_t p = 0; p < ell_width; ++p)
    {
        local_int_t idx = p * m + row;
#if defined(__HIP_PLATFORM_HCC__)
        local_int_t col = __builtin_nontemporal_load(ell_col_ind + idx);
#elif defined(__HIP_PLATFORM_NVCC__)
        local_int_t col = ell_col_ind[idx];
#endif

        if(col >= 0 && col < n && col != row)
        {
#if defined(__HIP_PLATFORM_HCC__)
            sum = fma(-__builtin_nontemporal_load(ell_val + idx), __ldg(y + col), sum);
#elif defined(__HIP_PLATFORM_NVCC__)
            sum = fma(-ell_val[idx], __ldg(y + col), sum);
#endif
        }
    }

#if defined(__HIP_PLATFORM_HCC__)
    __builtin_nontemporal_store(sum * __builtin_nontemporal_load(inv_diag + row), y + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    y[row] = sum * inv_diag[row];
#endif
}

__launch_bounds__(1024)
__global__ void kernel_symgs_interior(local_int_t m,
                                      local_int_t block_nrow,
                                      local_int_t ell_width,
                                      const local_int_t* __restrict__ ell_col_ind,
                                      const double* __restrict__ ell_val,
                                      const double* __restrict__ inv_diag,
                                      const double* __restrict__ x,
                                      double* __restrict__ y)
{
    local_int_t row = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(row >= block_nrow)
    {
        return;
    }

#if defined(__HIP_PLATFORM_HCC__)
    double sum = __builtin_nontemporal_load(x + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    double sum = x[row];
#endif

    for(local_int_t p = 0; p < ell_width; ++p)
    {
        local_int_t idx = p * m + row;
#if defined(__HIP_PLATFORM_HCC__)
        local_int_t col = __builtin_nontemporal_load(ell_col_ind + idx);
#elif defined(__HIP_PLATFORM_NVCC__)
        local_int_t col = ell_col_ind[idx];
#endif

        if(col >= 0 && col < m && col != row)
        {
#if defined(__HIP_PLATFORM_HCC__)
            sum = fma(-__builtin_nontemporal_load(ell_val + idx), __ldg(y + col), sum);
#elif defined(__HIP_PLATFORM_NVCC__)
            sum = fma(-ell_val[idx], __ldg(y + col), sum);
#endif
        }
    }

#if defined(__HIP_PLATFORM_HCC__)
    __builtin_nontemporal_store(sum * __builtin_nontemporal_load(inv_diag + row), y + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    y[row] = sum * inv_diag[row];
#endif
}

__global__ void kernel_symgs_halo(local_int_t m,
                                  local_int_t n,
                                  local_int_t block_nrow,
                                  local_int_t halo_width,
                                  const local_int_t* halo_row_ind,
                                  const local_int_t* halo_col_ind,
                                  const double* halo_val,
                                  const double* inv_diag,
                                  const local_int_t* perm,
                                  const double* x,
                                  double* y)
{
    local_int_t row = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(row >= m)
    {
        return;
    }

    local_int_t halo_idx = halo_row_ind[row];
    local_int_t perm_idx = perm[halo_idx];

    if(perm_idx >= block_nrow)
    {
        return;
    }

    double sum = 0.0;

    for(local_int_t p = 0; p < halo_width; ++p)
    {
        local_int_t idx = p * m + row;
        local_int_t col = halo_col_ind[idx];

        if(col >= 0 && col < n)
        {
            sum = fma(-halo_val[idx], y[col], sum);
        }
    }

    y[perm_idx] = fma(sum, inv_diag[halo_idx], y[perm_idx]);
}

__launch_bounds__(1024)
__global__ void kernel_pointwise_mult(local_int_t size,
                                      const double* __restrict__ x,
                                      const double* __restrict__ y,
                                      double* __restrict__ out)
{
    local_int_t gid = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(gid >= size)
    {
        return;
    }

    out[gid] = x[gid] * y[gid];
}

__launch_bounds__(1024)
__global__ void kernel_forward_sweep_0(local_int_t m,
                                       local_int_t block_nrow,
                                       local_int_t offset,
                                       local_int_t ell_width,
                                       const local_int_t* __restrict__ ell_col_ind,
                                       const double* __restrict__ ell_val,
                                       const local_int_t* __restrict__ diag_idx,
                                       const double* __restrict__ x,
                                       double* __restrict__ y)
{
    local_int_t gid = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(gid >= block_nrow)
    {
        return;
    }

    local_int_t row = gid + offset;

#if defined(__HIP_PLATFORM_HCC__)
    double sum = __builtin_nontemporal_load(x + row);
    local_int_t diag = __builtin_nontemporal_load(diag_idx + row);
    double diag_val = __builtin_nontemporal_load(ell_val + diag * m + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    double sum = x[row];
    local_int_t diag = diag_idx[row];
    double diag_val = ell_val[diag * m + row];
#endif

    for(local_int_t p = 0; p < diag; ++p)
    {
        local_int_t idx = p * m + row;
#if defined(__HIP_PLATFORM_HCC__)
        local_int_t col = __builtin_nontemporal_load(ell_col_ind + idx);
#elif defined(__HIP_PLATFORM_NVCC__)
        local_int_t col = ell_col_ind[idx];
#endif

        // Every entry above offset is zero
        if(col >= 0 && col < offset)
        {
#if defined(__HIP_PLATFORM_HCC__)
            sum = fma(-__builtin_nontemporal_load(ell_val + idx), __ldg(y + col), sum);
#elif defined(__HIP_PLATFORM_NVCC__)
            sum = fma(-ell_val[idx], __ldg(y + col), sum);
#endif
        }
    }

#if defined(__HIP_PLATFORM_HCC__)
    __builtin_nontemporal_store(sum / diag_val, y + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    y[row] = sum / diag_val;
#endif
}

__launch_bounds__(1024)
__global__ void kernel_backward_sweep_0(local_int_t m,
                                        local_int_t block_nrow,
                                        local_int_t offset,
                                        local_int_t ell_width,
                                        const local_int_t* __restrict__ ell_col_ind,
                                        const double* __restrict__ ell_val,
                                        const local_int_t* __restrict__ diag_idx,
                                        double* __restrict__ x)
{
    local_int_t gid = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;

    if(gid >= block_nrow)
    {
        return;
    }

    local_int_t row = gid + offset;
#if defined(__HIP_PLATFORM_HCC__)
    local_int_t diag = __builtin_nontemporal_load(diag_idx + row);
    double sum = __builtin_nontemporal_load(x + row);
    double diag_val = __builtin_nontemporal_load(ell_val + diag * m + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    local_int_t diag = diag_idx[row];
    double sum = x[row];
    double diag_val = ell_val[diag * m + row];
#endif

    // Scale result with diagonal entry
    sum *= diag_val;

    for(local_int_t p = diag + 1; p < ell_width; ++p)
    {
        local_int_t idx = p * m + row;
#if defined(__HIP_PLATFORM_HCC__)
        local_int_t col = __builtin_nontemporal_load(ell_col_ind + idx);
#elif defined(__HIP_PLATFORM_NVCC__)
        local_int_t col = ell_col_ind[idx];
#endif

        // Every entry below offset should not be taken into account
        if(col >= offset && col < m)
        {
#if defined(__HIP_PLATFORM_HCC__)
            sum = fma(-__builtin_nontemporal_load(ell_val + idx), __ldg(x + col), sum);
#elif defined(__HIP_PLATFORM_NVCC__)
            sum = fma(-ell_val[idx], __ldg(x + col), sum);
#endif
        }
    }

#if defined(__HIP_PLATFORM_HCC__)
    __builtin_nontemporal_store(sum / diag_val, x + row);
#elif defined(__HIP_PLATFORM_NVCC__)
    x[row] = sum / diag_val;
#endif
}

/*!
  Routine to compute one step of symmetric Gauss-Seidel:

  Assumption about the structure of matrix A:
  - Each row 'i' of the matrix has nonzero diagonal value whose address is matrixDiagonal[i]
  - Entries in row 'i' are ordered such that:
       - lower triangular terms are stored before the diagonal element.
       - upper triangular terms are stored after the diagonal element.
       - No other assumptions are made about entry ordering.

  Symmetric Gauss-Seidel notes:
  - We use the input vector x as the RHS and start with an initial guess for y of all zeros.
  - We perform one forward sweep.  Since y is initially zero we can ignore the upper triangular terms of A.
  - We then perform one back sweep.
       - For simplicity we include the diagonal contribution in the for-j loop, then correct the sum after

  @param[in] A the known system matrix
  @param[in] r the input vector
  @param[inout] x On entry, x should contain relevant values, on exit x contains the result of one symmetric GS sweep with r as the RHS.

  @return returns 0 upon success and non-zero otherwise

  @warning Early versions of this kernel (Version 1.1 and earlier) had the r and x arguments in reverse order, and out of sync with other kernels.

  @see ComputeSYMGS_ref
*/
int ComputeSYMGS(const SparseMatrix& A, const Vector& r, Vector& x)
{
    assert(x.localLength == A.localNumberOfColumns);

#ifndef HPCG_NO_MPI
    if(A.geom->size > 1)
    {
        PrepareSendBuffer(A, x);
    }
#endif

    hipLaunchKernelGGL((kernel_symgs_interior),
                       dim3((A.sizes[0] - 1) / 1024 + 1),
                       dim3(1024),
                       0,
                       stream_interior,
                       A.localNumberOfRows,
                       A.sizes[0],
                       A.ell_width,
                       A.ell_col_ind,
                       A.ell_val,
                       A.inv_diag,
                       r.d_values,
                       x.d_values);

#ifndef HPCG_NO_MPI
    if(A.geom->size > 1)
    {
        ExchangeHaloAsync(A);
        ObtainRecvBuffer(A, x);

        hipLaunchKernelGGL((kernel_symgs_halo),
                           dim3((A.halo_rows - 1) / 128 + 1),
                           dim3(128),
                           0,
                           0,
                           A.halo_rows,
                           A.localNumberOfColumns,
                           A.sizes[0],
                           A.ell_width,
                           A.halo_row_ind,
                           A.halo_col_ind,
                           A.halo_val,
                           A.inv_diag,
                           A.perm,
                           r.d_values,
                           x.d_values);
    }
#endif

    // Solve L
    for(local_int_t i = 1; i < A.nblocks; ++i)
    {
        hipLaunchKernelGGL((kernel_symgs_sweep),
                           dim3((A.sizes[i] - 1) / 1024 + 1),
                           dim3(1024),
                           0,
                           0,
                           A.localNumberOfRows,
                           A.localNumberOfColumns,
                           A.sizes[i],
                           A.offsets[i],
                           A.ell_width,
                           A.ell_col_ind,
                           A.ell_val,
                           A.inv_diag,
                           r.d_values,
                           x.d_values);
    }

    // Solve U
    for(local_int_t i = A.ublocks; i >= 0; --i)
    {
        hipLaunchKernelGGL((kernel_symgs_sweep),
                           dim3((A.sizes[i] - 1) / 1024 + 1),
                           dim3(1024),
                           0,
                           0,
                           A.localNumberOfRows,
                           A.localNumberOfColumns,
                           A.sizes[i],
                           A.offsets[i],
                           A.ell_width,
                           A.ell_col_ind,
                           A.ell_val,
                           A.inv_diag,
                           r.d_values,
                           x.d_values);
    }

    return 0;
}

int ComputeSYMGSZeroGuess(const SparseMatrix& A, const Vector& r, Vector& x)
{
    assert(x.localLength == A.localNumberOfColumns);

    // Solve L
    hipLaunchKernelGGL((kernel_pointwise_mult),
                       dim3((A.sizes[0] - 1) / 1024 + 1),
                       dim3(1024),
                       0,
                       0,
                       A.sizes[0],
                       r.d_values,
                       A.inv_diag,
                       x.d_values);

    for(local_int_t i = 1; i < A.nblocks; ++i)
    {
        hipLaunchKernelGGL((kernel_forward_sweep_0),
                           dim3((A.sizes[i] - 1) / 1024 + 1),
                           dim3(1024),
                           0,
                           0,
                           A.localNumberOfRows,
                           A.sizes[i],
                           A.offsets[i],
                           A.ell_width,
                           A.ell_col_ind,
                           A.ell_val,
                           A.diag_idx,
                           r.d_values,
                           x.d_values);
    }

    // Solve U
    for(local_int_t i = A.ublocks; i >= 0; --i)
    {
        hipLaunchKernelGGL((kernel_backward_sweep_0),
                           dim3((A.sizes[i] - 1) / 1024 + 1),
                           dim3(1024),
                           0,
                           0,
                           A.localNumberOfRows,
                           A.sizes[i],
                           A.offsets[i],
                           A.ell_width,
                           A.ell_col_ind,
                           A.ell_val,
                           A.diag_idx,
                           x.d_values);
    }

    return 0;
}
