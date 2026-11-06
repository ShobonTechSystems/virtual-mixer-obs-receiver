// 受け手の余裕（未読ブロック数）の傾きから、書き手と読み手のクロックのずれを見つける（IPC_PROTOCOL §5.2）。
// 余裕は読み方の揺れで上下するので、指数で重みを付けた最小二乗の傾きを見る。
// これは「クロック不一致」の表示と数のためで、ずれを直すもの（ASRC）ではない。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cmath>
#include <cstdint>

namespace vmx::ipc {

struct DriftMonitorConfig {
  double timeConstantSec = 60.0;    // 重みの時定数
  double slopeThreshold = 0.02;     // ブロック/秒。48 kHz・64 フレームでは約 27 ppm
  double minObserveSec = 40.0;      // これより短い観察では判定しない
};

class FillDriftMonitor {
 public:
  explicit FillDriftMonitor(DriftMonitorConfig c = {}) : cfg_(c) {}

  // 時刻 tSec（単調増加）での余裕 fillBlocks を渡す
  void update(double tSec, double fillBlocks) {
    if (!started_) {
      started_ = true;
      startT_ = tSec;
      lastT_ = tSec;
    }
    const double dt = tSec - lastT_;
    lastT_ = tSec;
    // 過去の点の時刻を dt だけ前へずらし（新しい点を t = 0 に置く）、重みを減らす
    const double k = std::exp(-dt / cfg_.timeConstantSec);
    sTT_ = (sTT_ - 2.0 * dt * sT_ + dt * dt * s0_) * k;
    sTF_ = (sTF_ - dt * sF_) * k;
    sT_ = (sT_ - dt * s0_) * k;
    sF_ *= k;
    s0_ *= k;
    s0_ += 1.0;
    sF_ += fillBlocks;
    // t = 0 の点は sT・sTT・sTF に何も足さない

    const double denom = s0_ * sTT_ - sT_ * sT_;
    slope_ = denom > 1e-12 ? (s0_ * sTF_ - sT_ * sF_) / denom : 0.0;

    if (tSec - startT_ >= cfg_.minObserveSec) {
      if (!mismatch_ && std::fabs(slope_) > cfg_.slopeThreshold) {
        mismatch_ = true;
        ++mismatchEvents_;
      } else if (mismatch_ && std::fabs(slope_) < cfg_.slopeThreshold * 0.5) {
        mismatch_ = false;
      }
    }
  }

  // 合わせ直し（余裕が跳ぶ）の後に呼ぶ。表示中の「クロック不一致」は残す
  void restartObservation() {
    started_ = false;
    s0_ = sT_ = sF_ = sTT_ = sTF_ = 0.0;
    slope_ = 0.0;
  }

  double slope() const { return slope_; }
  bool clockMismatch() const { return mismatch_; }
  std::uint64_t mismatchEvents() const { return mismatchEvents_; }

 private:
  DriftMonitorConfig cfg_;
  bool started_ = false;
  double startT_ = 0.0;
  double lastT_ = 0.0;
  double s0_ = 0.0, sT_ = 0.0, sF_ = 0.0, sTT_ = 0.0, sTF_ = 0.0;
  double slope_ = 0.0;
  bool mismatch_ = false;
  std::uint64_t mismatchEvents_ = 0;
};

}  // namespace vmx::ipc
