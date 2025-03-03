/* Copyright 2024 The JAX Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "jaxlib/gpu/solver_kernels_ffi.h"

#include <algorithm>
#include <cstdint>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "jaxlib/ffi_helpers.h"
#include "jaxlib/gpu/blas_handle_pool.h"
#include "jaxlib/gpu/gpu_kernel_helpers.h"
#include "jaxlib/gpu/make_batch_pointers.h"
#include "jaxlib/gpu/solver_handle_pool.h"
#include "jaxlib/gpu/vendor.h"
#include "xla/ffi/api/ffi.h"

namespace jax {
namespace JAX_GPU_NAMESPACE {

namespace ffi = ::xla::ffi;

namespace {
#define GETRF_KERNEL_IMPL(type, name)                                          \
  template <>                                                                  \
  struct GetrfKernel<type> {                                                   \
    static absl::StatusOr<int> BufferSize(gpusolverDnHandle_t handle, int m,   \
                                          int n) {                             \
      int lwork;                                                               \
      JAX_RETURN_IF_ERROR(JAX_AS_STATUS(                                       \
          name##_bufferSize(handle, m, n, /*A=*/nullptr, /*lda=*/m, &lwork))); \
      return lwork;                                                            \
    }                                                                          \
    static absl::Status Run(gpusolverDnHandle_t handle, int m, int n, type* a, \
                            type* workspace, int lwork, int* ipiv,             \
                            int* info) {                                       \
      return JAX_AS_STATUS(                                                    \
          name(handle, m, n, a, m, workspace, lwork, ipiv, info));             \
    }                                                                          \
  }

template <typename T>
struct GetrfKernel;
GETRF_KERNEL_IMPL(float, gpusolverDnSgetrf);
GETRF_KERNEL_IMPL(double, gpusolverDnDgetrf);
GETRF_KERNEL_IMPL(gpuComplex, gpusolverDnCgetrf);
GETRF_KERNEL_IMPL(gpuDoubleComplex, gpusolverDnZgetrf);
#undef GETRF_KERNEL_IMPL

template <typename T>
ffi::Error GetrfImpl(int64_t batch, int64_t rows, int64_t cols,
                     gpuStream_t stream, ffi::ScratchAllocator& scratch,
                     ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                     ffi::Result<ffi::Buffer<ffi::DataType::S32>> ipiv,
                     ffi::Result<ffi::Buffer<ffi::DataType::S32>> info) {
  FFI_ASSIGN_OR_RETURN(auto m, MaybeCastNoOverflow<int>(rows));
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));

  FFI_ASSIGN_OR_RETURN(auto handle, SolverHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(int lwork,
                       GetrfKernel<T>::BufferSize(handle.get(), m, n));

  auto maybe_workspace = scratch.Allocate(sizeof(T) * lwork);
  if (!maybe_workspace.has_value()) {
    return ffi::Error(ffi::ErrorCode::kUnknown,
                      "Unable to allocate workspace for getrf");
  }
  auto workspace = static_cast<T*>(maybe_workspace.value());

  auto a_data = static_cast<T*>(a.untyped_data());
  auto out_data = static_cast<T*>(out->untyped_data());
  auto ipiv_data = ipiv->typed_data();
  auto info_data = info->typed_data();
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(
        gpuMemcpyAsync(out_data, a_data, sizeof(T) * batch * rows * cols,
                       gpuMemcpyDeviceToDevice, stream)));
  }

  for (int i = 0; i < batch; ++i) {
    FFI_RETURN_IF_ERROR_STATUS(GetrfKernel<T>::Run(
        handle.get(), m, n, out_data, workspace, lwork, ipiv_data, info_data));
    out_data += m * n;
    ipiv_data += std::min(m, n);
    ++info_data;
  }
  return ffi::Error::Success();
}

#define GETRF_BATCHED_KERNEL_IMPL(type, name)                                 \
  template <>                                                                 \
  struct GetrfBatchedKernel<type> {                                           \
    static absl::Status Run(gpublasHandle_t handle, int n, type** a, int lda, \
                            int* ipiv, int* info, int batch) {                \
      return JAX_AS_STATUS(name(handle, n, a, lda, ipiv, info, batch));       \
    }                                                                         \
  }

template <typename T>
struct GetrfBatchedKernel;
GETRF_BATCHED_KERNEL_IMPL(float, gpublasSgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(double, gpublasDgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(gpublasComplex, gpublasCgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(gpublasDoubleComplex, gpublasZgetrfBatched);
#undef GETRF_BATCHED_KERNEL_IMPL

template <typename T>
ffi::Error GetrfBatchedImpl(int64_t batch, int64_t cols, gpuStream_t stream,
                            ffi::ScratchAllocator& scratch, ffi::AnyBuffer a,
                            ffi::Result<ffi::AnyBuffer> out,
                            ffi::Result<ffi::Buffer<ffi::DataType::S32>> ipiv,
                            ffi::Result<ffi::Buffer<ffi::DataType::S32>> info) {
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));
  FFI_ASSIGN_OR_RETURN(auto handle, BlasHandlePool::Borrow(stream));

  auto maybe_workspace = scratch.Allocate(sizeof(void*) * batch);
  if (!maybe_workspace.has_value()) {
    return ffi::Error(ffi::ErrorCode::kUnknown,
                      "Unable to allocate workspace for batched getrf");
  }
  auto workspace = maybe_workspace.value();

  auto a_data = a.untyped_data();
  auto out_data = out->untyped_data();
  auto ipiv_data = ipiv->typed_data();
  auto info_data = info->typed_data();
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(
        gpuMemcpyAsync(out_data, a_data, sizeof(T) * batch * cols * cols,
                       gpuMemcpyDeviceToDevice, stream)));
  }

  MakeBatchPointersAsync(stream, out_data, workspace, batch, sizeof(T) * n * n);
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuGetLastError()));

  auto batch_ptrs = static_cast<T**>(workspace);
  FFI_RETURN_IF_ERROR_STATUS(GetrfBatchedKernel<T>::Run(
      handle.get(), n, batch_ptrs, n, ipiv_data, info_data, batch));

  return ffi::Error::Success();
}

ffi::Error GetrfDispatch(gpuStream_t stream, ffi::ScratchAllocator scratch,
                         ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                         ffi::Result<ffi::Buffer<ffi::DataType::S32>> ipiv,
                         ffi::Result<ffi::Buffer<ffi::DataType::S32>> info) {
  auto dataType = a.element_type();
  if (dataType != out->element_type()) {
    return ffi::Error(
        ffi::ErrorCode::kInvalidArgument,
        "The input and output to getrf must have the same element type");
  }
  FFI_ASSIGN_OR_RETURN((auto [batch, rows, cols]),
                       SplitBatch2D(a.dimensions()));
  if (batch > 1 && rows == cols && rows / batch <= 128) {
    if (dataType == ffi::DataType::F32) {
      return GetrfBatchedImpl<float>(batch, cols, stream, scratch, a, out, ipiv,
                                     info);
    } else if (dataType == ffi::DataType::F64) {
      return GetrfBatchedImpl<double>(batch, cols, stream, scratch, a, out,
                                      ipiv, info);
    } else if (dataType == ffi::DataType::C64) {
      return GetrfBatchedImpl<gpublasComplex>(batch, cols, stream, scratch, a,
                                              out, ipiv, info);
    } else if (dataType == ffi::DataType::C128) {
      return GetrfBatchedImpl<gpublasDoubleComplex>(
          batch, cols, stream, scratch, a, out, ipiv, info);
    }
  } else {
    if (dataType == ffi::DataType::F32) {
      return GetrfImpl<float>(batch, rows, cols, stream, scratch, a, out, ipiv,
                              info);
    } else if (dataType == ffi::DataType::F64) {
      return GetrfImpl<double>(batch, rows, cols, stream, scratch, a, out, ipiv,
                               info);
    } else if (dataType == ffi::DataType::C64) {
      return GetrfImpl<gpuComplex>(batch, rows, cols, stream, scratch, a, out,
                                   ipiv, info);
    } else if (dataType == ffi::DataType::C128) {
      return GetrfImpl<gpuDoubleComplex>(batch, rows, cols, stream, scratch, a,
                                         out, ipiv, info);
    }
  }
  return ffi::Error(ffi::ErrorCode::kInvalidArgument,
                    "Unsupported element type for getrf");
}
}  // namespace

XLA_FFI_DEFINE_HANDLER_SYMBOL(
    GetrfFfi, GetrfDispatch,
    ffi::Ffi::Bind()
        .Ctx<ffi::PlatformStream<gpuStream_t>>()
        .Ctx<ffi::ScratchAllocator>()
        .Arg<ffi::AnyBuffer>()                   // a
        .Ret<ffi::AnyBuffer>()                   // out
        .Ret<ffi::Buffer<ffi::DataType::S32>>()  // ipiv
        .Ret<ffi::Buffer<ffi::DataType::S32>>()  // info
);

}  // namespace JAX_GPU_NAMESPACE
}  // namespace jax
