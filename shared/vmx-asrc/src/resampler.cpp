// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#include "vmx/asrc/resampler.hpp"

#include <samplerate.h>

#include <vector>

namespace vmx::asrc {

Resampler::~Resampler() {
  if (state_ != nullptr) src_delete(static_cast<SRC_STATE*>(state_));
}

bool Resampler::init(std::uint32_t channels) {
  if (state_ != nullptr) src_delete(static_cast<SRC_STATE*>(state_));
  state_ = nullptr;
  if (channels == 0) return false;
  int err = 0;
  SRC_STATE* s = src_new(SRC_SINC_MEDIUM_QUALITY, static_cast<int>(channels), &err);
  if (s == nullptr) return false;
  state_ = s;
  channels_ = channels;
  // 遅れを測る：比率 1 で無音を流し、出てこなかった分が遅れ（ここだけ確保してよい）
  const std::uint32_t n = 4096;
  std::vector<float> in(std::size_t{n} * channels, 0.0f), out(std::size_t{n + 64} * channels, 0.0f);
  const std::uint32_t got = process(in.data(), n, 1.0, out.data(), n + 64);
  latency_ = static_cast<double>(n) - static_cast<double>(got);
  reset();
  return true;
}

void Resampler::reset() {
  if (state_ != nullptr) src_reset(static_cast<SRC_STATE*>(state_));
}

std::uint32_t Resampler::process(const float* in, std::uint32_t inFrames, double ratio, float* out,
                                 std::uint32_t outCapacityFrames) {
  if (state_ == nullptr || inFrames == 0) return 0;
  SRC_DATA d{};
  d.data_in = in;
  d.input_frames = static_cast<long>(inFrames);
  d.data_out = out;
  d.output_frames = static_cast<long>(outCapacityFrames);
  d.src_ratio = ratio;
  d.end_of_input = 0;
  if (src_process(static_cast<SRC_STATE*>(state_), &d) != 0) return 0;
  return static_cast<std::uint32_t>(d.output_frames_gen);
}

}  // namespace vmx::asrc
