/* Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifdef PADDLE_WITH_HETERPS
#include "paddle/fluid/framework/fleet/ps_gpu_wrapper.h"
#include <algorithm>
#include <ctime>
#include <memory>
#include <numeric>
// #include "paddle/fluid/framework/fleet/heter_ps/optimizer_conf.h"
#include <xpu/runtime.h>  // NOLINT
#include "paddle/fluid/framework/lod_tensor.h"
#include "xpu/kernel/cluster_header.h"  // NOLINT
#include "xpu/kernel/debug.h"           // NOLINT
#include "xpu/kernel/math.h"            // NOLINT


namespace paddle {
namespace framework {

__global__ void PullCopy(float** dest, const FeatureValue* src,
                         const int64_t* len, int hidden, int slot_num,
                         int total_len, uint64_t** keys) {
  int cid = core_id();
  int ncores = core_num();
  if (cid >= ncores) {
    return;
  }
  int thread_id = ncores * cluster_id() + cid;
  int nthreads = ncores * cluster_num();
  __local__ int64_t local_len[slot_num];
  GM2LM(len, local_len, slot_num * sizeof(int64_t));

  for (int i = thread_id; i < slot_num; i += nthreads) {
    // max core local memory = 8KB
    // slot's max memory size = slot_len * sizeof(FeatureValue)
    int slot_len = i ? local_len[i] - local_len[i - 1] : local_len[0];
    int read_len = min(roundup_div(1024 * 8, sizeof(FeatureValue)), slot_len);
    int dest_len = i ? local_len[i - 1] : 0;
    __local__ FeatureValue local_slot_vals[read_len];
    __local__ float local_dest_vals[read_len * hidden];
    __local__ uint64_t local_slot_keys[read_len];

    // copy read_len(length) of slots' val to LM
    for (int k = 0; k < slot_len; k += read_len) {
      int real_read_len = min(read_len, slot_len - k);
      GM2LM(src + dest_len + k, local_slot_vals,
            real_read_len * sizeof(FeatureValue));
      GM2LM(keys[i] + k, local_slot_keys, real_read_len * sizeof(uint64_t));
      for (int j = 0; j < real_read_len; j++) {
        if (local_slot_keys[j] == 0) {
          local_dest_vals[j * hidden] = 0;
          local_dest_vals[j * hidden + 1] = 0;
          local_dest_vals[j * hidden + 2] = 0;
        } else {
          local_dest_vals[j * hidden] = local_slot_vals[j]->show;
          local_dest_vals[j * hidden + 1] = local_slot_vals[j]->clk;
          local_dest_vals[j * hidden + 2] = local_slot_vals[j]->lr;
        }

        if (local_slot_vals[j]->mf_size == 0 || local_slot_keys[j] == 0) {
          for (int m = 0; m < hidden - 3; m++) {
            local_dest_vals[j * hidden + 3 + m] = 0;
          }
        } else {
          for (int m = 0; m < hidden - 3; m++) {
            local_dest_vals[j * hidden + 3 + m] = local_slot_vals[j]->mf[1 + m];
          }
        }
      }
      LM2GM(local_dest_vals, dest[i] + k * hidden,
            real_read_len * hidden * sizeof(float));
    }
  }
}

__global__ void CopyKeysKernel(uint64_t** src_keys, uint64_t* dest_total_keys,
                               const int64_t* len, int slot_num,
                               int total_len) {
  int cid = core_id();
  int ncores = core_num();
  if (cid >= ncores) {
    return;
  }
  int thread_id = ncores * cluster_id() + cid;
  int nthreads = ncores * cluster_num();
  __local__ int64_t local_len[slot_num];
  GM2LM(len, local_len, slot_num * sizeof(int64_t));

  for (int i = thread_id; i < slot_num; i += nthreads) {
    // max core local memory = 8KB
    int slot_len = i ? local_len[i] - local_len[i - 1] : local_len[0];
    int read_len = min(slot_len, 1024);
    int dest_len = i ? local_len[i - 1] : 0;
    __local__ uint64_t local_slot_keys[read_len];

    for (int k = 0; k < slot_len; k += read_len) {
      int real_read_len = min(read_len, slot_len - k);
      GM2LM(src_keys[i] + k, local_slot_keys, real_read_len * sizeof(uint64_t));
      LM2GM(local_slot_keys, dest_total_keys + dest_len + k,
            real_read_len * sizeof(uint64_t));
    }
  }
}

__global__ void PushCopy(FeaturePushValue* dest, float** src, int64_t* len,
                         int hidden, int slot_num, int total_len, int bs,
                         int* slot_vector) {
  int cid = core_id();
  int ncores = core_num();
  if (cid >= ncores) {
    return;
  }
  int thread_id = ncores * cluster_id() + cid;
  int nthreads = ncores * cluster_num();
  __local__ int64_t local_len[slot_num];
  __local__ int local_slot[slot_num];
  GM2LM(len, local_len, slot_num * sizeof(int64_t));
  GM2LM(slot_vector, local_slot, slot_num * sizeof(int));

  for (int i = thread_id; i < slot_num; i += nthreads) {
    int slot_len = i ? local_len[i] - local_len[i - 1] : local_len[0];

    // max core local memory = 8KB
    // slot's max memory size = slot_len * hidden * 8
    int read_len = min(roundup_div(1024, hidden), slot_len);
    int dest_len = i ? local_len[i - 1] : 0;
    __local__ float local_slot_grads[read_len * hidden];
    __local__ FeaturePushValue local_dest_grads[read_len];

    // copy read_len(length) of slots' grad to LM
    for (int k = 0; k < slot_len; k += read_len) {
      int real_read_len = min(read_len, slot_len - k);
      GM2LM(src[i] + k * hidden, local_slot_grads,
            real_read_len * hidden * sizeof(float));
      // copy from slots' grad to total grad
      for (int j = 0; j < real_read_len; j++) {
        local_dest_grads[j]->slot = local_slot[i];
        local_dest_grads[j]->show = local_slot_grads[j * hidden];
        local_dest_grads[j]->clk = local_slot_grads[j * hidden + 1];
        local_dest_grads[j]->lr_g = local_slot_grads[j * hidden + 2] * -1. * bs;
        for (int m = 0; m < hidden - 3; j++) {
          local_dest_grads[j]->mf_g[m] =
              local_slot_grads[j * hidden + 3 + m] * -1. * bs;
        }
      }
      LM2GM(local_dest_grads, dest + dest_len + k,
            real_read_len * sizeof(FeaturePushValue));
    }
  }
}

void PSGPUWrapper::CopyForPull(const paddle::platform::Place& place,
                               uint64_t** gpu_keys,
                               const std::vector<float*>& values,
                               const FeatureValue* total_values_gpu,
                               const int64_t* gpu_len, const int slot_num,
                               const int hidden_size,
                               const int64_t total_length) {
  XPUStream stream = nullptr;
  auto dev_ctx = platform::DeviceContextPool::Instance().Get(place);
  stream = static_cast<platform::XPUDeviceContext*>(dev_ctx)
               ->x_context()
               ->xpu_stream;
  float* buf_value = nullptr;
  xpu_malloc(reinterpret_cast<void**>(&buf_value),
             values.size() * sizeof(float*));
  float** gpu_values = reinterpret_cast<float**>(&buf_value);
  xpu_memcpy(gpu_values, values.data(), values.size() * sizeof(float*),
             XPU_HOST_TO_DEVICE);

  PullCopy<<<2, 64, stream>>>(gpu_values, total_values_gpu, gpu_len,
                              hidden_size, slot_num, total_length, gpu_keys);

  xpu_wait(stream);
}

void PSGPUWrapper::CopyKeys(const paddle::platform::Place& place,
                            uint64_t** origin_keys, uint64_t* total_keys,
                            const int64_t* gpu_len, int slot_num,
                            int total_len) {
  XPUStream stream = nullptr;
  auto dev_ctx = platform::DeviceContextPool::Instance().Get(place);
  stream = static_cast<platform::XPUDeviceContext*>(dev_ctx)
               ->x_context()
               ->xpu_stream;
  CopyKeysKernel<<<2, 64, stream>>>(origin_keys, total_keys, gpu_len, slot_num,
                                    total_len);
  xpu_wait(stream);
}

void PSGPUWrapper::CopyForPush(const paddle::platform::Place& place,
                               const std::vector<const float*>& grad_values,
                               FeaturePushValue* total_grad_values_gpu,
                               const std::vector<int64_t>& slot_lengths,
                               const int hidden_size,
                               const int64_t total_length,
                               const int batch_size) {
  XPUStream stream = nullptr;
  auto dev_ctx = platform::DeviceContextPool::Instance().Get(place);
  stream = static_cast<platform::XPUDeviceContext*>(dev_ctx)
               ->x_context()
               ->xpu_stream;
  auto slot_lengths_lod = slot_lengths;
  for (int i = 1; i < slot_lengths_lod.size(); i++) {
    slot_lengths_lod[i] += slot_lengths_lod[i - 1];
  }

  T* buf_grad_value = nullptr;
  T* buf_length = nullptr;
  T* buf_slot_vector = nullptr;

  xpu_malloc(reinterpret_cast<void**>(&buf_grad_value),
             grad_values.size() * sizeof(float*));
  xpu_malloc(reinterpret_cast<void**>(&buf_length),
             slot_lengths.size() * sizeof(int64_t));
  xpu_malloc(reinterpret_cast<void**>(&buf_slot_vector),
             slot_lengths_lod.size() * sizeof(int));

  float** gpu_values = reinterpret_cast<float**>(&buf_grad_value);
  int64_t* gpu_len = reinterpret_cast<int64_t*>(buf_length);
  int* d_slot_vector = reinterpret_cast<int*>(buf_slot_vector);
  xpu_memcpy(gpu_values, grad_values.data(),
             grad_values.size() * sizeof(float*), XPU_HOST_TO_DEVICE);
  xpu_memcpy(gpu_len, slot_lengths_lod.data(),
             slot_lengths.size() * sizeof(int64_t), XPU_HOST_TO_DEVICE);
  xpu_memcpy(d_slot_vector, slot_vector_.data(),
             slot_lengths_lod.size() * sizeof(int), XPU_HOST_TO_DEVICE);

  PushCopy<<<2, 64, stream>>>(total_grad_values_gpu, gpu_values, gpu_len,
                              hidden_size, slot_lengths.size(), total_length,
                              batch_size, d_slot_vector);
  xpu_wait(stream);
}

}  // end namespace framework
}  // end namespace paddle
#endif
