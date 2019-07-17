/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <iostream>
#include <vector>
#include "fbgemm/Fbgemm.h"

namespace fbgemm {

template <int SPATIAL_DIM, typename ACC_T>
bool takeDepthWiseFastPath(const conv_param_t<SPATIAL_DIM>& conv_p) {
  // Note: Depthwise convolutions (both 2D and 3D) are optimized for the most
  // common case.
  return std::is_same<ACC_T, std::int32_t>::value && conv_p.G == conv_p.IC &&
      conv_p.G == conv_p.OC && conv_p.G % 8 == 0 &&
      std::all_of(
             conv_p.stride.begin(),
             conv_p.stride.end(),
             [](int i) { return i == 1 || i == 2; }) &&
      std::all_of(
             conv_p.K.begin(), conv_p.K.end(), [](int i) { return i == 3; }) &&
      std::all_of(
             conv_p.dilation.begin(),
             conv_p.dilation.end(),
             [](int i) { return i == 1; }) &&
      std::all_of(conv_p.pad.begin(), conv_p.pad.end(), [](int i) {
           return i == 1;
         });
}

template <int SPATIAL_DIM, typename ACC_T>
optimized_conv_t ConvFastPath(const conv_param_t<SPATIAL_DIM>& conv_p) {
  if (takeDepthWiseFastPath<SPATIAL_DIM, ACC_T>(conv_p)) {
    return optimized_conv_t::depthwise;
  } else if (fbgemmOptimizedGConv<SPATIAL_DIM>(conv_p)) {
    return optimized_conv_t::groupwise;
  } else {
    return optimized_conv_t::im2col;
  }
}

template <typename processOutputType, int SPATIAL_DIM, typename ACC_T>
int fbgemmConv(
    const conv_param_t<SPATIAL_DIM>& conv_p,
    const std::uint8_t* activations,
    PackWeightsForConv<SPATIAL_DIM, std::int8_t, ACC_T>& packed_weights,
    typename processOutputType::outType* out,
    std::int32_t* outBuffer,
    processOutputType& outProcess,
    int thread_id,
    int num_threads,
    const BlockingFactors* blocking_params) {
  static_assert(
      SPATIAL_DIM == 2 || SPATIAL_DIM == 3,
      "Only 2D and 3D convolutions are supported");

  if (!packed_weights.isPackingCompliant(conv_p)) {
    throw std::logic_error(
        "[FBGEMM_CONV_ERROR] Prepacked weights can't be used"
        " with these convolution parameters!");
  }

  switch (ConvFastPath<SPATIAL_DIM, ACC_T>(conv_p)) {
    case optimized_conv_t::depthwise: {
      // 2D and 3D depthwise fast path
      // std::cout << "Depthwise fast path" << std::endl;
      const std::int32_t* B_zero_point = outProcess.getBZeroPoint();
      const float* C_multiplier = outProcess.getCMultiplier();
      if (SPATIAL_DIM == 3) {
        static_assert(
            std::is_same<typename processOutputType::outType, std::uint8_t>::
                value,
            "For depthwise, only requantized output is supported");
        depthwise_3x3x3_pad_1(
            conv_p.MB, // mini batch
            conv_p.IN_DIM[0], // T
            conv_p.IN_DIM[1], // H
            conv_p.IN_DIM[2], // W
            conv_p.OC, // output channels
            conv_p.stride[0], // stride_t
            conv_p.stride[1], // stride_h
            conv_p.stride[2], // stride_w
            outProcess.getAZeroPoint(),
            activations,
            B_zero_point[0],
            *(packed_weights.getPackedWFor3DDW()),
            C_multiplier[0],
            outProcess.getCZeroPoint(),
            out,
            outProcess.getColOffsets(),
            outProcess.getBias(),
            outProcess.RELU_FUSED, // fuse_relu
            thread_id,
            num_threads);
      } else {
        depthwise_3x3_pad_1(
            conv_p.MB, // mini batch
            conv_p.IN_DIM[0], // H
            conv_p.IN_DIM[1], // W
            conv_p.OC, // output channels
            conv_p.stride[0], // stride_h
            conv_p.stride[1], // stride_w
            outProcess.getAZeroPoint(),
            activations,
            B_zero_point[0],
            *(packed_weights.getPackedWFor2DDW()),
            C_multiplier[0],
            outProcess.getCZeroPoint(),
            out,
            outProcess.getColOffsets(),
            outProcess.getBias(),
            outProcess.RELU_FUSED, // fuse_relu
            thread_id,
            num_threads);
      }
      break;
    }
    case optimized_conv_t::groupwise: {
      // optimized groupwise convolution
      // std::cout << "Groupwise fast path" << std::endl;
      assert(
          SPATIAL_DIM == 2 && "Only 2D groupwise convolutions are supported");
      std::vector<int32_t> row_offset_buf(
          rowOffsetBufferSizeGConv<SPATIAL_DIM>(conv_p));
      outProcess.setRowOffsets(row_offset_buf.data());
      fbgemmGroupwiseConv(
          conv_p,
          activations,
          outProcess.getAZeroPoint(),
          row_offset_buf.data(),
          *(packed_weights.getPackedWForGroupwise()),
          out,
          outBuffer,
          outProcess,
          thread_id,
          num_threads);
      break;
    }
    case optimized_conv_t::im2col: {
      // All other convolutions go through im2col-based implementation
      // std::cout << "Im2col path" << std::endl;
      std::vector<int32_t> row_offset_buf(
          PackAWithIm2Col<uint8_t, ACC_T, SPATIAL_DIM>::rowOffsetBufferSize());

      const std::int32_t* b_zero_point = outProcess.getBZeroPoint();
      bool b_symmetric = b_zero_point[0] == 0;
      PackAWithIm2Col<uint8_t, ACC_T, SPATIAL_DIM> packA(
          conv_p,
          activations,
          nullptr, /* buffer for packed matrix */
          outProcess.getAZeroPoint(),
          row_offset_buf.data(),
          b_symmetric,
          blocking_params);

      outProcess.setRowOffsets(row_offset_buf.data());
      fbgemmPacked(
          packA,
          *(packed_weights.getPackedWForIm2col()),
          out,
          outBuffer,
          conv_p.OC,
          outProcess,
          thread_id,
          num_threads,
          blocking_params);
      break;
    }
  } // switch

  return 0;
}

#define INSTANTIATE_BASE(ACC_T, Q_GRAN, RELU, SPATIAL_DIM)                 \
  template int fbgemmConv(                                                 \
      const conv_param_t<SPATIAL_DIM>& conv_p,                             \
      const std::uint8_t* activations,                                     \
      PackWeightsForConv<SPATIAL_DIM, std::int8_t, ACC_T>& packed_weights, \
      std::uint8_t* out,                                                   \
      std::int32_t* outBuffer,                                             \
      ReQuantizeOutput<RELU, Q_GRAN>& outProcess,                          \
      int thread_id,                                                       \
      int num_threads,                                                     \
      const BlockingFactors* blocking_params);

#define INSTANTIATE_SPATIAL_DIM(ACC_T, Q_GRAN, RELU) \
  INSTANTIATE_BASE(ACC_T, Q_GRAN, RELU, 2);          \
  INSTANTIATE_BASE(ACC_T, Q_GRAN, RELU, 3);

#define INSTANTIATE_RELU(ACC_T, Q_GRAN)         \
  INSTANTIATE_SPATIAL_DIM(ACC_T, Q_GRAN, true); \
  INSTANTIATE_SPATIAL_DIM(ACC_T, Q_GRAN, false);

#define INSTANTIATE_Q_GRANS(ACC_T)                          \
  INSTANTIATE_RELU(ACC_T, QuantizationGranularity::TENSOR); \
  INSTANTIATE_RELU(ACC_T, QuantizationGranularity::GROUP);  \
  INSTANTIATE_RELU(ACC_T, QuantizationGranularity::OUT_CHANNEL);

INSTANTIATE_Q_GRANS(std::int32_t);

#undef INSTANTIATE_Q_GRANS
#undef INSTANTIATE_RELU
#undef INSTANTIATE_SPATIAL_DIM
#undef INSTANTIATE_BASE

template bool takeDepthWiseFastPath<2, std::int32_t>(
    const conv_param_t<2>& conv_p);
template bool takeDepthWiseFastPath<3, std::int32_t>(
    const conv_param_t<3>& conv_p);
template bool takeDepthWiseFastPath<2, std::int16_t>(
    const conv_param_t<2>& conv_p);
template bool takeDepthWiseFastPath<3, std::int16_t>(
    const conv_param_t<3>& conv_p);

template optimized_conv_t ConvFastPath<2, std::int32_t>(
    const conv_param_t<2>& conv_p);
template optimized_conv_t ConvFastPath<3, std::int32_t>(
    const conv_param_t<3>& conv_p);
template optimized_conv_t ConvFastPath<2, std::int16_t>(
    const conv_param_t<2>& conv_p);
template optimized_conv_t ConvFastPath<3, std::int16_t>(
    const conv_param_t<3>& conv_p);

} // namespace fbgemm