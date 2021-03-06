/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#ifdef FUSION_DECONVBNRELU_OP

#include "operators/kernel/deconv_bn_relu_kernel.h"
#include <cmath>
#include "framework/operator.h"
#include "operators/op_param.h"

namespace paddle_mobile {
namespace operators {

template <>
bool DeconvBNReluKernel<FPGA, float>::Init(
    FusionDeconvBNReluParam<FPGA> *param) {
  paddle_mobile::fpga::ActivationType activation_enable =
      paddle_mobile::fpga::LEAKYRELU;
  int16_t leaky_relu_negative_slope = 0;
  auto input = const_cast<LoDTensor *>(param->Input());
  const Tensor *bias = param->InputBias();
  auto bias_ptr = bias->data<float>();
  auto filter = const_cast<LoDTensor *>(param->Filter());
  auto out = param->Output();
  float Si = input->scale[0];
  float So = out->scale[0];
  float Sf = fpga::filter_find_max(filter);
  auto bn_mean_ptr = param->InputMean()->data<float>();
  auto bn_var_ptr = param->InputVariance()->data<float>();
  auto bn_scale_ptr = param->InputScale()->data<float>();
  auto bn_bias_ptr = param->InputBias()->data<float>();
  const float epsilon = param->Epsilon();

  PADDLE_MOBILE_ENFORCE(out->dims()[1] == bias->dims()[0],
                        "Output channel should be equal to bias number");
  int channel = out->dims()[1];
  auto new_scale = new Tensor();
  auto new_bias = new Tensor();
  auto new_scale_ptr = new_scale->mutable_data<float>({channel});
  auto new_bias_ptr = new_bias->mutable_data<float>({channel});
  for (int i = 0; i < channel; i++) {
    new_scale_ptr[i] = bn_scale_ptr[i] /
                       static_cast<float>(pow((bn_var_ptr[i] + epsilon), 0.5));
    new_bias_ptr[i] = bn_bias_ptr[i] + (0 - bn_mean_ptr[i]) * new_scale_ptr[i];
  }

  int sub_conv_n = param->Strides()[0];
  auto bs_ptr = (float *)fpga::fpga_malloc(2 * channel * sub_conv_n *  // NOLINT
                                           sizeof(float));             // NOLINT
  if (param->Groups() == channel) {
    for (int i = 0; i < channel * sub_conv_n; i++) {
      bs_ptr[i + sub_conv_n * channel] = new_scale_ptr[i % channel] * Si / So;
      bs_ptr[i] = new_bias_ptr[i % (channel)] * 127.0f / So;
    }
  } else {
    for (int i = 0; i < channel * sub_conv_n; i++) {
      bs_ptr[i + sub_conv_n * channel] =
          new_scale_ptr[i % channel] * Si / So * Sf / 127.0f;
      bs_ptr[i] = new_bias_ptr[i % (channel)] * 127.0f / So;
    }
  }
  PADDLE_MOBILE_ENFORCE(param->Strides()[1] == param->Strides()[0],
                        "stride_width should be equal to stride_height ");
  PADDLE_MOBILE_ENFORCE(filter->dims()[2] == filter->dims()[3],
                        "filter width should be equal to filter height ");
  PADDLE_MOBILE_ENFORCE(((filter->dims()[2] % param->Strides()[0]) == 0),
                        "filter axis should be the multiple of stride axis ");
  if (param->Groups() == channel) {
    fpga::format_DWDeconv_data(filter, out, &bs_ptr, param->Groups(),
                               sub_conv_n);
    fpga::DWDeconvArgs DWDeconv_arg = {0};
    fpga::fill_DWDeconv_arg(&DWDeconv_arg, input, out, filter, true,
                            param->Strides()[0], param->Strides()[1],
                            param->Paddings()[0], param->Paddings()[1], bs_ptr);
    param->SetFpgaArgs(DWDeconv_arg);
  } else {
    fpga::format_deconv_data(filter, out, &bs_ptr, param->Groups(), sub_conv_n);
    fpga::DeconvArgs deconv_arg = {0};
    fpga::fill_deconv_arg(&deconv_arg, input, out, filter, true,
                          param->Groups(), param->Strides()[0],
                          param->Strides()[1], param->Paddings()[0],
                          param->Paddings()[1], bs_ptr);
    param->SetFpgaArgs(deconv_arg);
  }
  delete new_scale;
  delete new_bias;
  return true;
}

template <>
void DeconvBNReluKernel<FPGA, float>::Compute(
    const FusionDeconvBNReluParam<FPGA> &param) {
  if (param.Groups() == param.Output()->dims()[1]) {
    fpga::ComputeDWDeconv(param.FpgaDWDconvArgs());
  } else {
    fpga::ComputeFpgaDeconv(param.FpgaArgs());
  }
}

}  // namespace operators
}  // namespace paddle_mobile

#endif
