// 時計のずれを吸収する比率の制御（docs/OBS_ASRC.md §4.1）。依存なし、メモリを確保しない。
// 誤差（ms）＝「実際に出している時刻 − 出すべき時刻」。正なら出すのが遅れている（出力を作りすぎ）ので比率を下げる。
// 比率は ppm（出力 ÷ 入力 − 1 の 100 万倍）で返す。
//   ・誤差は 1 次の低域で均してから使う（ブロックの時刻の揺れ ±1 ms を比率の揺れにしない）
//   ・PI 制御：u = −Kp·e + z、z' = −Ki·e。e' = 0.001·(u − u*)（1 ppm は 1 ms あたり 0.001 ms/s）なので、
//     特性方程式は s² + 0.001·Kp·s + 0.001·Ki = 0。既定は ωn ≈ 0.15 rad/s、ζ ≈ 0.7（整定 60 秒以内）
//   ・比率の変化は 1 秒あたり maxSlewPpmPerSec まで、大きさは ±maxPpm まで（A3）。頭打ちの間は積分を止める
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <algorithm>
#include <cmath>

namespace vmx::asrc {

struct DriftControllerConfig {
  double kpPpmPerMs = 210.0;
  double kiPpmPerMsSec = 22.5;
  double filterSeconds = 1.0;
  double maxPpm = 1000.0;
  double maxSlewPpmPerSec = 10.0;
};

class DriftController {
 public:
  explicit DriftController(DriftControllerConfig c = {}) : cfg_(c) {}

  // 誤差を 1 つ足して、新しい比率（ppm）を返す。dt は前回からの秒
  double update(double errorMs, double dtSec) {
    if (!(dtSec > 0.0)) return ppm_;
    if (!hasFiltered_) {
      filtered_ = errorMs;
      hasFiltered_ = true;
    } else {
      const double a = std::min(1.0, dtSec / cfg_.filterSeconds);
      filtered_ += a * (errorMs - filtered_);
    }
    const double want = -cfg_.kpPpmPerMs * filtered_ + integral_;
    const double step = cfg_.maxSlewPpmPerSec * dtSec;
    double next = std::clamp(want, ppm_ - step, ppm_ + step);
    next = std::clamp(next, -cfg_.maxPpm, cfg_.maxPpm);
    const bool limited = next != want;
    // 頭打ち・変化の制限の間は、制限を強める向きの積分を止める（積み過ぎ防止）
    const double dz = -cfg_.kiPpmPerMsSec * filtered_ * dtSec;
    if (!limited || (want > next && dz < 0.0) || (want < next && dz > 0.0)) integral_ += dz;
    integral_ = std::clamp(integral_, -cfg_.maxPpm, cfg_.maxPpm);
    ppm_ = next;
    return ppm_;
  }

  // 結び付け直しの後：誤差の均しは捨てる。比率と積分（＝推定した時計の差）は残す（A6）
  void resetError() { hasFiltered_ = false; }
  void resetAll() {
    hasFiltered_ = false;
    filtered_ = integral_ = ppm_ = 0.0;
  }

  double ppm() const { return ppm_; }
  double filteredErrorMs() const { return filtered_; }

 private:
  DriftControllerConfig cfg_;
  bool hasFiltered_ = false;
  double filtered_ = 0.0, integral_ = 0.0, ppm_ = 0.0;
};

}  // namespace vmx::asrc
