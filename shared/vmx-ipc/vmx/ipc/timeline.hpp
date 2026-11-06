// OBS へ渡す時刻を作る部品と、受け渡しの遅延の分布（docs/P3C_OBS_PCM.md §2 の C4・C5）。
// 依存なし。メモリを確保しない（分布は固定の箱）。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vmx::ipc {

struct TimelineConfig {
  // サンプル位置から作った時刻と QPC の差がこれを超えたら結び付け直す。OBS（32.0.0 の obs-source.c）は、前の続きとの差が
  // 70 ms（TS_SMOOTHING_THRESHOLD）未満なら渡した時刻を捨てて自分の連続の時刻を使い、それ以上なら渡した時刻へ跳ぶ。
  // ここを 70 ms 未満にしても OBS の中ではずれが積み上がるだけなので、OBS と同じ 70 ms にそろえる
  std::uint64_t maxDriftNs = 70'000'000;
  double minPpmSeconds = 10.0;            // ppm の推定を出すまでの観察の長さ
};

// 最初のブロック（と不連続の後）で「サンプル位置 → QPC」を結び付け、以後はサンプル位置から連続に時刻を作る。
// 書き手の揺れがあっても、続いた音には隙間も重なりもない時刻が付く。
// あわせて、QPC − サンプル位置の時刻 の傾き（サンプルの時計と QPC の差、ppm）を最小二乗で推定する
class Timeline {
 public:
  explicit Timeline(TimelineConfig c = {}) : cfg_(c) {}

  // ブロックの先頭の時刻（固定遅延は含まない）。discontinuity：不連続フラグ、取りこぼし、epoch の変化、再接続
  std::uint64_t place(std::uint64_t samplePos, std::uint64_t qpcNs, std::uint32_t sampleRate, bool discontinuity) {
    if (!has_ || discontinuity || sampleRate != rate_ || samplePos < basePos_) {
      rebase(samplePos, qpcNs, sampleRate, true);
      return qpcNs;
    }
    const std::uint64_t t = at(samplePos);
    observe(samplePos, qpcNs);
    const std::uint64_t diff = t > qpcNs ? t - qpcNs : qpcNs - t;
    if (diff > cfg_.maxDriftNs) {
      ++driftRebases_;
      rebase(samplePos, qpcNs, sampleRate, false);  // ppm の観察は続ける（時計の差を測りたいので）
      return qpcNs;
    }
    return t;
  }

  void reset() { has_ = false; }

  std::uint64_t rebases() const { return rebases_; }            // 結び付けた回数（最初を含む）
  std::uint64_t driftRebases() const { return driftRebases_; }  // そのうち、ずれが大きかった回数
  // サンプルの時計に対する QPC の進み（ppm）。正なら、サンプルの時計が QPC より遅い。観察が短ければ false
  bool ppm(double& out) const {
    if (n_ < 2) return false;
    const double span = maxX_ - minX_;
    if (span < cfg_.minPpmSeconds) return false;
    const double den = n_ * sxx_ - sx_ * sx_;
    if (den <= 0.0) return false;
    // y は ns、x は秒なので、傾きは ns/秒。1 ppm = 1000 ns/秒
    out = (n_ * sxy_ - sx_ * sy_) / den / 1000.0;
    return true;
  }
  double observedSeconds() const { return n_ > 0 ? maxX_ - minX_ : 0.0; }

 private:
  void rebase(std::uint64_t pos, std::uint64_t qpc, std::uint32_t rate, bool resetPpm) {
    has_ = true;
    basePos_ = pos;
    baseQpc_ = qpc;
    rate_ = rate;
    ++rebases_;
    if (resetPpm) {
      n_ = sx_ = sy_ = sxx_ = sxy_ = 0.0;
      ppmPos_ = pos;
      ppmQpc_ = qpc;
      minX_ = maxX_ = 0.0;
    }
    observe(pos, qpc);
  }

  std::uint64_t at(std::uint64_t pos) const {
    const std::uint64_t frames = pos - basePos_;
    return baseQpc_ + frames / rate_ * 1000000000ull + frames % rate_ * 1000000000ull / rate_;
  }

  void observe(std::uint64_t pos, std::uint64_t qpc) {
    // x：ppm を測り始めてからのサンプルの時刻（秒）、y：QPC − サンプルの時刻（ns）
    const double x = static_cast<double>(pos - ppmPos_) / static_cast<double>(rate_);
    const double y = (static_cast<double>(qpc) - static_cast<double>(ppmQpc_)) - x * 1e9;
    n_ += 1.0;
    sx_ += x;
    sy_ += y;
    sxx_ += x * x;
    sxy_ += x * y;
    if (x < minX_) minX_ = x;
    if (x > maxX_) maxX_ = x;
  }

  TimelineConfig cfg_;
  bool has_ = false;
  std::uint64_t basePos_ = 0, baseQpc_ = 0;
  std::uint32_t rate_ = 0;
  std::uint64_t rebases_ = 0, driftRebases_ = 0;
  std::uint64_t ppmPos_ = 0, ppmQpc_ = 0;
  double n_ = 0, sx_ = 0, sy_ = 0, sxx_ = 0, sxy_ = 0, minX_ = 0, maxX_ = 0;
};

// 遅延の分布（0.1 ms の箱、0〜100 ms。超えた分は最後の箱）
class DelayHistogram {
 public:
  static constexpr std::size_t kBins = 1001;
  static constexpr std::uint64_t kBinNs = 100'000;

  void add(std::uint64_t ns) {
    const std::uint64_t b = ns / kBinNs;
    ++bins_[b < kBins - 1 ? b : kBins - 1];
    if (count_ == 0 || ns < min_) min_ = ns;
    if (ns > max_) max_ = ns;
    ++count_;
  }
  void clear() { *this = DelayHistogram{}; }

  std::uint64_t count() const { return count_; }
  std::uint64_t minNs() const { return min_; }
  std::uint64_t maxNs() const { return max_; }  // 箱ではなく実際の最大
  // 分位（箱の上端、多めの見積もり）。q は 0〜1
  std::uint64_t quantileNs(double q) const {
    if (count_ == 0) return 0;
    const auto target = static_cast<std::uint64_t>(q * static_cast<double>(count_ - 1)) + 1;
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < kBins; ++i) {
      acc += bins_[i];
      if (acc >= target) return i == kBins - 1 ? max_ : (i + 1) * kBinNs;
    }
    return max_;
  }
  const std::array<std::uint64_t, kBins>& bins() const { return bins_; }

 private:
  std::array<std::uint64_t, kBins> bins_{};
  std::uint64_t count_ = 0, min_ = 0, max_ = 0;
};

}  // namespace vmx::ipc
