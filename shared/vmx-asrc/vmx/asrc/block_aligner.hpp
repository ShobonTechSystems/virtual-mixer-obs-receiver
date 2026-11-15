// 受け手が共有メモリから読んだブロック（サンプル位置・書き手の QPC・形式）を StreamAligner に渡す前の見張り
// （docs/OBS_ASRC.md §4、A6）。OBS の受け手が使う。
//   不連続とみなすもの：書き手の不連続フラグ、サンプル位置の飛び（取りこぼし・つなぎ直し・書き手の再起動）、
//   markDiscontinuity（つながった・書き手の再起動の知らせ）、ブロックの QPC が stallNs 以上空いた（書き手が止まって再開）、
//   形式の変化（作り直す）。どれも「ずれによる結び付け直し」（driftRebases）には数えない
//   確保は形式が決まった・変わったときだけ。init に失敗した形式は、形式が変わるまで試し直さない
// R はリサンプラー（Resampler、または試験用の模型）。init(channels) を足して持つ。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cstdint>
#include <memory>

#include "vmx/asrc/stream_aligner.hpp"

namespace vmx::asrc {

struct BlockInfo {
  const float* interleaved = nullptr;
  std::uint32_t frames = 0, channels = 0, sampleRate = 0;
  std::uint64_t samplePos = 0, qpcNs = 0;
  bool writerDiscontinuity = false;
};

template <class R>
class BlockAligner {
 public:
  explicit BlockAligner(std::uint64_t fixedDelayNs = 10'000'000, std::uint64_t stallNs = 70'000'000)
      : fixedDelayNs_(fixedDelayNs), stallNs_(stallNs) {}

  AlignedChunk push(const BlockInfo& b) {
    bool disc = b.writerDiscontinuity || forceDisc_;
    if (!aligner_ || b.channels != channels_ || b.sampleRate != rate_ || b.frames > maxFrames_) {
      if (b.channels == failedChannels_ && b.sampleRate == failedRate_ && b.frames <= failedFrames_) return {};
      if (!setup(b)) return {};
      disc = true;
    } else if (b.samplePos != nextPos_) {
      disc = true;
    } else if (b.qpcNs > lastQpcNs_ + stallNs_) {
      disc = true;
      ++stalls_;
    }
    forceDisc_ = false;
    nextPos_ = b.samplePos + b.frames;
    lastQpcNs_ = b.qpcNs;
    if (disc) ++discontinuities_;
    return aligner_->push(b.interleaved, b.frames, b.qpcNs, disc);
  }

  // つながった・書き手が再起動した：次のブロックで結び付け直す（比率の推定は残す）
  void markDiscontinuity() { forceDisc_ = true; }
  // 設定（固定遅延・ストリーム）が変わった：比率の推定も数も捨てる
  void reset(std::uint64_t fixedDelayNs) {
    fixedDelayNs_ = fixedDelayNs;
    aligner_.reset();
    channels_ = rate_ = maxFrames_ = 0;
    failedChannels_ = failedRate_ = failedFrames_ = 0;
    baseRebases_ = baseDriftRebases_ = baseStepRebases_ = 0;
    discontinuities_ = stalls_ = 0;
    forceDisc_ = false;
  }

  double ppm() const { return aligner_ ? aligner_->ppm() : 0.0; }
  double filteredErrorMs() const { return aligner_ ? aligner_->filteredErrorMs() : 0.0; }
  // 累計（形式が変わって作り直しても続けて数える）
  std::uint64_t rebases() const { return baseRebases_ + (aligner_ ? aligner_->rebases() : 0); }
  std::uint64_t driftRebases() const { return baseDriftRebases_ + (aligner_ ? aligner_->driftRebases() : 0); }
  std::uint64_t stepRebases() const { return baseStepRebases_ + (aligner_ ? aligner_->stepRebases() : 0); }
  std::uint64_t discontinuities() const { return discontinuities_; }
  std::uint64_t stalls() const { return stalls_; }
  std::uint64_t droppedInputFrames() const { return rs_.droppedInputFrames(); }
  R& resampler() { return rs_; }

 private:
  bool setup(const BlockInfo& b) {
    if (aligner_) {
      baseRebases_ += aligner_->rebases();
      baseDriftRebases_ += aligner_->driftRebases();
      baseStepRebases_ += aligner_->stepRebases();
    }
    aligner_.reset();
    channels_ = rate_ = maxFrames_ = 0;
    if (b.channels == 0 || b.sampleRate == 0 || b.frames == 0 || !rs_.init(b.channels)) {
      failedChannels_ = b.channels;
      failedRate_ = b.sampleRate;
      failedFrames_ = b.frames;
      return false;
    }
    failedChannels_ = failedRate_ = failedFrames_ = 0;
    StreamAlignerConfig cfg;
    cfg.sampleRate = static_cast<double>(b.sampleRate);
    cfg.fixedDelayNs = fixedDelayNs_;
    aligner_ = std::make_unique<StreamAligner<R>>(rs_, cfg);
    aligner_->prepare(b.channels, b.frames);
    channels_ = b.channels;
    rate_ = b.sampleRate;
    maxFrames_ = b.frames;
    return true;
  }

  R rs_;
  std::unique_ptr<StreamAligner<R>> aligner_;
  std::uint64_t fixedDelayNs_, stallNs_;
  std::uint32_t channels_ = 0, rate_ = 0, maxFrames_ = 0;
  std::uint32_t failedChannels_ = 0, failedRate_ = 0, failedFrames_ = 0;
  bool forceDisc_ = false;
  std::uint64_t nextPos_ = 0, lastQpcNs_ = 0;
  std::uint64_t baseRebases_ = 0, baseDriftRebases_ = 0, baseStepRebases_ = 0, discontinuities_ = 0, stalls_ = 0;
};

}  // namespace vmx::asrc
