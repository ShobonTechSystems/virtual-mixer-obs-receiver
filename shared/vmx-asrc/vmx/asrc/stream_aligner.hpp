// 受け取ったブロック（サンプル位置と QPC）を、受け手の時計（QPC）の速さへリサンプルし、隙間も重なりもない
// 出力の時刻を付ける（docs/OBS_ASRC.md §4）。OBS の受け手が使う。
//   出力の時刻：最初のブロック（と結び付け直しの後）で「ブロックの QPC ＋ 固定遅延」に結び付け、以後は出したフレーム数から
//   連続に作る。誤差＝出力の最後のフレームの時刻 −（そのフレームに当たる入力の QPC ＋ 固定遅延）を DriftController に渡す。
//   不連続・誤差が大きすぎる（hardResyncMs 以上）ときは、リサンプラーの状態を捨てて結び付け直す（比率の推定は残す）
//   いったん落ち着いた後（均した誤差が settleMs 未満の状態が settleHoldSec 続いた後）に、均した誤差が stepResyncMs を
//   超えたら、時刻の段差（書き手の一時的な止まりなど）とみなして結び付け直す。比率の変化の上限（10 ppm/s）のもとでは
//   数 ms を超える段差を制御で戻すと、大きく振れて何分も収まらないため（自己レビューの指摘、試験「段差」）
// R はリサンプラー（Resampler、または試験用の模型）。process(in, frames, ratio, out, cap) と latencyFrames() を持つ。
// 確保は prepare のときだけ。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "vmx/asrc/drift_controller.hpp"

namespace vmx::asrc {

struct StreamAlignerConfig {
  double sampleRate = 48000.0;
  std::uint64_t fixedDelayNs = 10'000'000;
  double hardResyncMs = 70.0;  // OBS の TS_SMOOTHING_THRESHOLD と同じ
  double stepResyncMs = 3.0;   // 落ち着いた後の、均した誤差の上限
  double settleMs = 1.0;
  double settleHoldSec = 30.0;
  DriftControllerConfig control{};
};

struct AlignedChunk {
  const float* data = nullptr;  // インターリーブ。次の push まで有効
  std::uint32_t frames = 0;
  std::uint64_t timestampNs = 0;  // 先頭のフレームを出す時刻（QPC、固定遅延を含む）
};

template <class R>
class StreamAligner {
 public:
  StreamAligner(R& resampler, StreamAlignerConfig c = {}) : rs_(resampler), cfg_(c), ctl_(c.control) {}

  // maxBlockFrames：1 回に渡すブロックの最大のフレーム数
  void prepare(std::uint32_t channels, std::uint32_t maxBlockFrames) {
    channels_ = channels;
    capacity_ = static_cast<std::uint32_t>(maxBlockFrames * (1.0 + cfg_.control.maxPpm * 1e-6)) + 64;
    out_.assign(std::size_t{capacity_} * channels, 0.0f);
    anchored_ = false;
  }

  // discontinuity：再接続・取りこぼし・形式の変化など
  AlignedChunk push(const float* in, std::uint32_t frames, std::uint64_t blockQpcNs, bool discontinuity) {
    AlignedChunk c;
    if (frames == 0 || out_.empty()) return c;
    if (!anchored_ || discontinuity) anchor(blockQpcNs);

    const double ratio = 1.0 + ctl_.ppm() * 1e-6;
    const std::uint32_t got = rs_.process(in, frames, ratio, out_.data(), capacity_);
    c.data = out_.data();
    c.frames = got;
    c.timestampNs = timeOf(outFrames_);
    outFrames_ += got;

    // このブロックの最後の入力が出る時刻と、出すべき時刻の差
    const double inLastSec = (static_cast<double>(frames) - rs_.latencyFrames()) / cfg_.sampleRate;
    const double want = static_cast<double>(blockQpcNs) + inLastSec * 1e9 + static_cast<double>(cfg_.fixedDelayNs);
    const double have = static_cast<double>(timeOf(outFrames_));
    const double errMs = (have - want) / 1e6;
    lastErrorMs_ = errMs;
    if (std::fabs(errMs) >= cfg_.hardResyncMs) {
      // 次のブロックから結び付け直す（このブロックの出力はそのまま渡す。OBS は跳ぶ）
      ++driftRebases_;
      needAnchor_ = true;
    } else {
      // 経過時間はフレーム数から測る（ブロックの QPC の揺れを、比率の変化の速さに持ち込まない）
      const double dt = static_cast<double>(frames) / cfg_.sampleRate;
      ctl_.update(errMs, dt);
      const double f = std::fabs(ctl_.filteredErrorMs());
      if (!settled_) {
        calmSec_ = f < cfg_.settleMs ? calmSec_ + dt : 0.0;
        if (calmSec_ >= cfg_.settleHoldSec) settled_ = true;
      } else if (f >= cfg_.stepResyncMs) {
        ++stepRebases_;
        needAnchor_ = true;
      }
    }
    if (needAnchor_) {
      anchored_ = false;
      needAnchor_ = false;
    }
    return c;
  }

  double ppm() const { return ctl_.ppm(); }
  double lastErrorMs() const { return lastErrorMs_; }
  double filteredErrorMs() const { return ctl_.filteredErrorMs(); }
  std::uint64_t rebases() const { return rebases_; }            // 結び付けた回数（最初を含む）
  std::uint64_t driftRebases() const { return driftRebases_; }  // そのうち、誤差が hardResyncMs 以上になった回数
  std::uint64_t stepRebases() const { return stepRebases_; }    // そのうち、落ち着いた後の時刻の段差
  bool settled() const { return settled_; }

 private:
  void anchor(std::uint64_t blockQpcNs) {
    rs_.reset();
    ctl_.resetError();
    // 出力の k 番目は入力の k 番目に当たる（リサンプラーの遅れは「まだ出てこない分」として、誤差の式で扱う）。
    // 出力が手元に来るのは、遅れの分だけ後になる：固定遅延の中に含める（A5）
    baseNs_ = static_cast<double>(blockQpcNs) + static_cast<double>(cfg_.fixedDelayNs);
    outFrames_ = 0;
    anchored_ = true;
    ++rebases_;
  }
  std::uint64_t timeOf(std::uint64_t outFrame) const {
    return static_cast<std::uint64_t>(baseNs_ + static_cast<double>(outFrame) / cfg_.sampleRate * 1e9);
  }

  R& rs_;
  StreamAlignerConfig cfg_;
  DriftController ctl_;
  std::vector<float> out_;
  std::uint32_t channels_ = 0, capacity_ = 0;
  bool anchored_ = false, needAnchor_ = false, settled_ = false;
  double baseNs_ = 0.0, lastErrorMs_ = 0.0, calmSec_ = 0.0;
  std::uint64_t outFrames_ = 0, rebases_ = 0, driftRebases_ = 0, stepRebases_ = 0;
};

}  // namespace vmx::asrc
