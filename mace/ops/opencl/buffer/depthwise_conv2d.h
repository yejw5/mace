// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef MACE_OPS_OPENCL_BUFFER_DEPTHWISE_CONV2D_H_
#define MACE_OPS_OPENCL_BUFFER_DEPTHWISE_CONV2D_H_

#include "mace/ops/opencl/depthwise_conv2d.h"

#include <functional>
#include <memory>
#include <vector>

#include "mace/ops/opencl/buffer/utils.h"
#include "mace/ops/opencl/helper.h"

namespace mace {
namespace ops {
namespace opencl {
namespace buffer {
namespace depthwise {

MaceStatus DepthwiseConv2d(OpContext *context,
                           cl::Kernel *kernel,
                           const Tensor *padded_input,   // NHWC
                           const Tensor *filter,  // HWIM
                           const Tensor *bias,
                           const int *strides,
                           const int *dilations,
                           const DataType dt,
                           const ActivationType activation,
                           const float relux_max_limit,
                           const float leakyrelu_coefficient,
                           const bool input_changed,
                           Tensor *output,
                           StatsFuture *future);
}  // namespace depthwise


template <typename T>
class DepthwiseConv2dKernel : public OpenCLDepthwiseConv2dKernel {
 public:
  DepthwiseConv2dKernel() : old_scratch_size_(0) {}
  MaceStatus Compute(
      OpContext *context,
      const Tensor *input,
      const Tensor *filter,
      const Tensor *bias,
      const int *strides,
      const Padding &padding_type,
      const std::vector<int> &padding_data,
      const int *dilations,
      const ActivationType activation,
      const float relux_max_limit,
      const float leakyrelu_coefficient,
      Tensor *output) override;

 private:
  index_t old_scratch_size_;
  cl::Kernel kernels_[2];
  uint32_t kwg_size_;
  std::vector<index_t> input_shape_;
};

template <typename T>
MaceStatus DepthwiseConv2dKernel<T>::Compute(
    OpContext *context,
    const Tensor *input,
    const Tensor *filter,
    const Tensor *bias,
    const int *strides,
    const Padding &padding_type,
    const std::vector<int> &padding_data,
    const int *dilations,
    const ActivationType activation,
    const float relux_max_limit,
    const float leakyrelu_coefficient,
    Tensor *output) {
  StatsFuture pad_future, dw_conv_future;
  index_t filter_w = filter->dim(3);

  // Create a fake conv_2d filter to calculate the paddings and output size
  std::vector<index_t> fake_filter_shape(4);
  fake_filter_shape[0] = filter->dim(0) * filter->dim(1);
  fake_filter_shape[1] = filter->dim(1);
  fake_filter_shape[2] = filter->dim(2);
  fake_filter_shape[3] = filter->dim(3);

  std::vector<index_t> output_shape(4);
  std::vector<int> paddings(2);
  if (padding_data.empty()) {
    ops::CalcNHWCPaddingAndOutputSize(
        input->shape().data(), fake_filter_shape.data(), dilations, strides,
        padding_type, output_shape.data(), paddings.data());
  } else {
    paddings = padding_data;
    CalcOutputSize(input->shape().data(), fake_filter_shape.data(),
                   padding_data.data(), dilations, strides, RoundType::FLOOR,
                   output_shape.data());
  }
  MACE_RETURN_IF_ERROR(output->Resize(output_shape));

  // calculate padded input shape
  index_t width = output_shape[2];
  index_t channels = output_shape[3];

  index_t input_height = input->dim(1);
  index_t input_width = input->dim(2);
  index_t input_channels = input->dim(3);

  int pad_top = paddings[0] >> 1;
  int pad_left = paddings[1] >> 1;

  MACE_CHECK(filter->dim(0) == 1, "Multiplier > 1 not supported");
  MACE_CHECK(filter->dim(0) * input_channels == channels);
  MACE_CHECK(filter->dim(1) == input_channels, filter->dim(1), " != ",
             input_channels);

  // Mark whether input changed or not
  bool input_changed = !IsVecEqual(input_shape_, input->shape());
  input_shape_ = input->shape();

  std::vector<index_t> padded_output_shape = output_shape;
  index_t tile_w = 4, tile_c = 4;
  padded_output_shape[2] = RoundUp<index_t>(width, tile_w);

  std::vector<index_t> padded_input_shape = input->shape();
  padded_input_shape[1] = input_height + paddings[0];
  padded_input_shape[2] = (padded_output_shape[2] - 1) * strides[1] +
      (filter_w - 1) * dilations[1] + 1;
  padded_input_shape[3] = RoundUp<index_t>(input_channels, tile_c);

  const Tensor *padded_input_ptr = input;
  // pad input
  std::unique_ptr<Tensor> padded_input;
  if (padded_input_shape[1] != input_height ||
      padded_input_shape[2] != input_width ||
      padded_input_shape[3] != input_channels) {
    index_t total_scratch_size = 0;
    index_t padded_input_size = 0;

    padded_input_size =
        std::accumulate(padded_input_shape.begin(),
                        padded_input_shape.end(),
                        1,
                        std::multiplies<index_t>())
            * GetEnumTypeSize(input->dtype()) + MACE_EXTRA_BUFFER_PAD_SIZE;
    total_scratch_size += padded_input_size;

    // Init scratch buffer
    ScratchBuffer *scratch = context->device()->scratch_buffer();
    scratch->Rewind();
    scratch->GrowSize(total_scratch_size);
    if (old_scratch_size_ != scratch->size()) {
      input_changed |= scratch->size() != old_scratch_size_;
      old_scratch_size_ = scratch->size();
    }

    padded_input.reset(new Tensor(scratch->Scratch(padded_input_size),
                                  input->dtype()));

    padded_input->Resize(padded_input_shape);
    PadInput(context, &kernels_[0], input, pad_top, pad_left,
             input_changed, padded_input.get(), &pad_future);
    padded_input_ptr = padded_input.get();
  }

  MACE_RETURN_IF_ERROR(
      depthwise::DepthwiseConv2d(
          context, &kernels_[1], padded_input_ptr, filter, bias, strides,
          dilations, DataTypeToEnum<T>::v(), activation, relux_max_limit,
          leakyrelu_coefficient, input_changed, output, &dw_conv_future));
  MergeMultipleFutureWaitFn({pad_future, dw_conv_future}, context->future());
  return MaceStatus::MACE_SUCCESS;
}

}  // namespace buffer
}  // namespace opencl
}  // namespace ops
}  // namespace mace

#endif  // MACE_OPS_OPENCL_BUFFER_DEPTHWISE_CONV2D_H_
