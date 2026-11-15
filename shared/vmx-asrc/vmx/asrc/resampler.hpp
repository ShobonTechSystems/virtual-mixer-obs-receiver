// 比率を連続に変えられるリサンプラー（libsamplerate の SRC_SINC_MEDIUM_QUALITY を包む。FASTEST は通過域が約 19 kHz までで A4 に届かない。docs/OBS_ASRC.md §4.2）。
// 確保は init のときだけ。process は確保しない（libsamplerate の src_process は確保しない）。
// 入力はインターリーブの float。process は入力を全部使い、作った出力のフレーム数を返す。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cstdint>

namespace vmx::asrc {

class Resampler {
 public:
  Resampler() = default;
  ~Resampler();
  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;

  // 失敗したら false
  bool init(std::uint32_t channels);
  // 状態を捨てる（不連続の後）。確保はしない
  void reset();
  // ratio＝出力 ÷ 入力。out には inFrames × ratio ＋ 16 フレーム以上の場所が要る
  std::uint32_t process(const float* in, std::uint32_t inFrames, double ratio, float* out, std::uint32_t outCapacityFrames);
  // 入力が出力に出るまでの遅れ（入力のフレーム、比率 1 のとき）。init で測る
  double latencyFrames() const { return latency_; }
  std::uint32_t channels() const { return channels_; }
  // 使われずに捨てた入力のフレーム（出力の場所が足りない・処理の失敗）。0 のはず
  std::uint64_t droppedInputFrames() const { return droppedInput_; }

 private:
  void* state_ = nullptr;  // SRC_STATE*
  std::uint32_t channels_ = 0;
  double latency_ = 0.0;
  std::uint64_t droppedInput_ = 0;
};

}  // namespace vmx::asrc
