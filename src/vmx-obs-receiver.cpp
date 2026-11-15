// SPDX-License-Identifier: GPL-2.0-or-later
// 仮想ミキサーの音（OBS の音声ソース、試作）
// Copyright (C) 2026 Shobon Tech Systems
//
// This program is free software; you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation; either version 2 of the License, or (at your
// option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License (LICENSE) for more details.
//
// 仮想ミキサー（Shobon Virtual Mixer）の本体が共有メモリに書いた PCM を読み、obs_source_output_audio で OBS へ渡す。
// 受け手の処理（一覧を見て開く・読む・時刻を付ける・再起動を見分ける）は shared/vmx-ipc（MIT）の vmx::ipc::Receiver。
// このプラグインが使うのは libobs と shared/vmx-ipc だけ。
// 専用のスレッドが 1 ms おきに起き、たまったブロックをすべて渡す。
// 本体の時計（主IF）と OBS の時計（QPC）の差は、shared/vmx-asrc（MIT）の StreamAligner でブロックを QPC の速さへ
// 少しだけリサンプルして吸収する。OBS へ渡す時刻は、最初のブロックの QPC ＋ 固定遅延（設定、既定 10 ms）に結び付け、
// 出したフレーム数から連続に作る（virtual-mixer の docs/OBS_ASRC.md §4）。
// 不連続（つなぎ直し・取りこぼし・サンプル位置の飛び・形式の変化）と、ずれが 70 ms を超えたときだけ結び付け直す。
// OBS のログには、つながった・止まった・戻ったときと、1 分おきの要約だけを書く。
#include <obs-module.h>
#include <util/platform.h>

#include "vmx/asrc/resampler.hpp"
#include "vmx/asrc/stream_aligner.hpp"
#include "vmx/ipc/receiver.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <timeapi.h>
#endif

#ifndef VMX_OBS_RECEIVER_VERSION
#define VMX_OBS_RECEIVER_VERSION "0.0.0"
#endif

OBS_DECLARE_MODULE()

namespace {

constexpr const char* kSourceId = "shobon_virtual_mixer_receiver";
constexpr std::uint64_t kSummaryNs = 60'000'000'000ull;

const char* describe(vmx::ipc::ReceiverEvent e) {
  using E = vmx::ipc::ReceiverEvent;
  switch (e) {
    case E::WaitingForWriter: return "仮想ミキサーを待っています";
    case E::Connected: return "仮想ミキサーとつながりました";
    case E::WriterRestarted: return "仮想ミキサーが起動し直しました（つなぎ直します）";
    case E::WriterStopped: return "仮想ミキサーが送るのをやめました";
    case E::WriterStalled: return "仮想ミキサーが応答しません";
    case E::WriterResumed: return "仮想ミキサーがまた送り始めました";
    case E::FormatMismatch: return "形式を読めません（版が違う可能性があります）";
    case E::WriterIdle: return "仮想ミキサーは動いていますが、音が届いていません";
    case E::AccessDenied: return "権限が足りず開けません（仮想ミキサーと OBS の一方だけを管理者として起動していませんか）";
  }
  return "?";
}

// 時計のずれの吸収。確保は形式が決まった・変わったときだけ（ブロックごとには確保しない）
class Asrc {
 public:
  // ブロックを渡し、OBS へ出す分を返す（frames が 0 なら出す物はない）
  vmx::asrc::AlignedChunk push(const vmx::ipc::ReceivedBlock& b, std::uint64_t fixedDelayNs) {
    bool disc = (b.flags & VMX_IPC_FLAG_DISCONTINUITY) != 0;
    if (!aligner_ || b.channels != channels_ || b.sampleRate != rate_ || b.frames > maxFrames_) {
      if (!setup(b, fixedDelayNs)) return {};
      disc = true;
    } else if (b.samplePos != nextPos_) {
      disc = true;  // 取りこぼし・つなぎ直し・書き手の再起動
    } else if (b.qpcNs > lastQpcNs_ + kStallNs) {
      disc = true;  // 書き手が止まって再開した（時計のずれではないので、ずれによる結び付け直しに数えない）
    }
    nextPos_ = b.samplePos + b.frames;
    lastQpcNs_ = b.qpcNs;
    return aligner_->push(b.interleaved, b.frames, b.qpcNs, disc);
  }
  // つなぎ直したとき：次のブロックで結び付け直す（比率の推定は残す）
  void markDiscontinuity() { nextPos_ = ~std::uint64_t{0}; }
  // 設定が変わったとき：比率の推定も数も捨てる
  void reset() {
    aligner_.reset();
    channels_ = rate_ = maxFrames_ = 0;
    baseRebases_ = baseDriftRebases_ = 0;
  }
  double ppm() const { return aligner_ ? aligner_->ppm() : 0.0; }
  double filteredErrorMs() const { return aligner_ ? aligner_->filteredErrorMs() : 0.0; }
  // 累計（形式が変わって作り直しても続けて数える）
  std::uint64_t rebases() const { return baseRebases_ + (aligner_ ? aligner_->rebases() : 0); }
  std::uint64_t driftRebases() const { return baseDriftRebases_ + (aligner_ ? aligner_->driftRebases() : 0); }

 private:
  static constexpr std::uint64_t kStallNs = 70'000'000;  // StreamAlignerConfig::hardResyncMs と同じ

  bool setup(const vmx::ipc::ReceivedBlock& b, std::uint64_t fixedDelayNs) {
    if (aligner_) {
      baseRebases_ += aligner_->rebases();
      baseDriftRebases_ += aligner_->driftRebases();
    }
    aligner_.reset();
    channels_ = rate_ = maxFrames_ = 0;
    if (b.channels == 0 || b.sampleRate == 0 || b.frames == 0 || !rs_.init(b.channels)) return false;
    vmx::asrc::StreamAlignerConfig cfg;
    cfg.sampleRate = static_cast<double>(b.sampleRate);
    cfg.fixedDelayNs = fixedDelayNs;
    aligner_ = std::make_unique<vmx::asrc::StreamAligner<vmx::asrc::Resampler>>(rs_, cfg);
    aligner_->prepare(b.channels, b.frames);
    channels_ = b.channels;
    rate_ = b.sampleRate;
    maxFrames_ = b.frames;
    return true;
  }

  vmx::asrc::Resampler rs_;
  std::unique_ptr<vmx::asrc::StreamAligner<vmx::asrc::Resampler>> aligner_;
  std::uint32_t channels_ = 0, rate_ = 0, maxFrames_ = 0;
  std::uint64_t nextPos_ = 0, lastQpcNs_ = 0, baseRebases_ = 0, baseDriftRebases_ = 0;
};

struct ReceiverSource {
  obs_source_t* source = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<bool> reconfigure{true};
  std::atomic<std::uint32_t> streamId{1};
  std::atomic<std::uint32_t> delayMs{10};
  std::thread worker;

  void run();
};

void ReceiverSource::run() {
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
  std::unique_ptr<vmx::ipc::Receiver> rx;
  Asrc asrc;
  std::uint32_t id = 0;
  std::uint64_t fixedDelayNs = 0;
  std::uint64_t lastSummary = os_gettime_ns();
  vmx::ipc::ReceiverStats reported;
  std::uint64_t reportedRebases = 0, reportedAsrcRebases = 0, reportedDriftRebases = 0;
  std::uint64_t late = 0, reportedLate = 0;  // OBS へ渡した時点で、付けた時刻を過ぎていた

  auto sink = [this, &asrc, &fixedDelayNs, &late](const vmx::ipc::ReceivedBlock& b) {
    const vmx::asrc::AlignedChunk c = asrc.push(b, fixedDelayNs);
    if (c.frames == 0) return;  // リサンプラーの立ち上がり（最初の数十フレーム）
    obs_source_audio audio = {};
    audio.data[0] = reinterpret_cast<const uint8_t*>(c.data);
    audio.frames = c.frames;
    audio.speakers = b.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_FLOAT;  // ch 交互の float
    audio.samples_per_sec = b.sampleRate;
    audio.timestamp = c.timestampNs;
    if (os_gettime_ns() > c.timestampNs) ++late;
    obs_source_output_audio(source, &audio);
  };

  while (!stop.load()) {
    if (reconfigure.exchange(false)) {
      vmx::ipc::ReceiverConfig cfg;
      cfg.streamId = streamId.load();
      cfg.fixedDelayNs = std::uint64_t{delayMs.load()} * 1'000'000ull;
      id = cfg.streamId;
      fixedDelayNs = cfg.fixedDelayNs;
      rx = std::make_unique<vmx::ipc::Receiver>(cfg);  // OBS の os_gettime_ns と同じ時計（QPC）
      asrc.reset();  // 固定遅延・ストリームが変わった：比率の推定も捨てる
      reported = {};
      reportedRebases = reportedAsrcRebases = reportedDriftRebases = 0;
      late = reportedLate = 0;
      blog(LOG_INFO, "[vmx-receiver] ストリーム %u、固定遅延 %u ms で受けます", id, delayMs.load());
    }
    auto log = [id, &asrc](vmx::ipc::ReceiverEvent e, std::uint64_t) {
      blog(LOG_INFO, "[vmx-receiver] ストリーム %u: %s", id, describe(e));
      if (e == vmx::ipc::ReceiverEvent::Connected || e == vmx::ipc::ReceiverEvent::WriterRestarted)
        asrc.markDiscontinuity();
    };
    rx->poll(sink, log);

    const std::uint64_t now = os_gettime_ns();
    if (now - lastSummary >= kSummaryNs) {
      lastSummary = now;
      const auto& s = rx->stats();
      if (s.blocks != reported.blocks) {
        double ppm = 0;
        const bool hasPpm = rx->timeline().ppm(ppm);
        // 「ASRC の結び付け直し」は接続・不連続を含む。「ずれによる」は、そのうち誤差が 70 ms を超えた分（OBS で音が飛ぶ）。
        // 「時計の差（推定）」と「（参考）推定の結び付け直し」は受け手の Timeline の値で、OBS へ渡す時刻には使っていない
        blog(LOG_INFO,
             "[vmx-receiver] ストリーム %u 1分: 受け取り %llu, 取りこぼし +%llu, 位置の飛び +%llu, 間に合わない +%llu, "
             "受け渡し(ms、累計) 中央 %.2f / p99 %.2f / 最大 %.2f, "
             "ASRC 比率 %+.1f ppm / 誤差 %+.2f ms / 結び付け直し +%llu（ずれによる +%llu）, "
             "時計の差（推定） %s%.1f ppm, （参考）推定の結び付け直し +%llu",
             id, (unsigned long long)(s.blocks - reported.blocks), (unsigned long long)(s.dropped - reported.dropped),
             (unsigned long long)(s.positionGaps - reported.positionGaps), (unsigned long long)(late - reportedLate),
             double(s.handoff.quantileNs(0.5)) / 1e6, double(s.handoff.quantileNs(0.99)) / 1e6,
             double(s.handoff.maxNs()) / 1e6, asrc.ppm(), asrc.filteredErrorMs(),
             (unsigned long long)(asrc.rebases() - reportedAsrcRebases),
             (unsigned long long)(asrc.driftRebases() - reportedDriftRebases), hasPpm ? "" : "（未推定）",
             hasPpm ? ppm : 0.0, (unsigned long long)(rx->timeline().rebases() - reportedRebases));
        reported = s;
        reportedRebases = rx->timeline().rebases();
        reportedAsrcRebases = asrc.rebases();
        reportedDriftRebases = asrc.driftRebases();
        reportedLate = late;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  rx.reset();
#ifdef _WIN32
  timeEndPeriod(1);
#endif
}

const char* getName(void*) { return "仮想ミキサーの音（試作）"; }

void getDefaults(obs_data_t* settings) {
  obs_data_set_default_int(settings, "stream_id", 1);
  obs_data_set_default_int(settings, "delay_ms", 10);
}

obs_properties_t* getProperties(void*) {
  obs_properties_t* p = obs_properties_create();
  obs_properties_add_int(p, "stream_id", "ストリーム ID", 1, 65535, 1);
  obs_properties_add_int(p, "delay_ms", "固定遅延（ms）", 0, 500, 1);
  return p;
}

// 値が変わったときだけ受け手を作り直す（OBS は同じ値でも update を呼ぶことがあり、作り直すと短く途切れるため）
void applySettings(ReceiverSource* r, obs_data_t* settings) {
  const auto id = static_cast<std::uint32_t>(obs_data_get_int(settings, "stream_id"));
  const auto delay = static_cast<std::uint32_t>(obs_data_get_int(settings, "delay_ms"));
  const bool changed = r->streamId.exchange(id) != id;
  const bool delayChanged = r->delayMs.exchange(delay) != delay;
  if (changed || delayChanged) r->reconfigure.store(true);
}

void update(void* data, obs_data_t* settings) { applySettings(static_cast<ReceiverSource*>(data), settings); }

void* create(obs_data_t* settings, obs_source_t* source) {
  auto* r = new ReceiverSource;
  r->source = source;
  applySettings(r, settings);
  r->worker = std::thread([r] { r->run(); });
  return r;
}

void destroy(void* data) {
  auto* r = static_cast<ReceiverSource*>(data);
  r->stop.store(true);
  if (r->worker.joinable()) r->worker.join();
  delete r;
}

}  // namespace

MODULE_EXPORT const char* obs_module_name(void) { return "Shobon Virtual Mixer Receiver"; }
MODULE_EXPORT const char* obs_module_description(void) {
  return "Receives PCM streams from the Shobon Virtual Mixer over shared memory (prototype).";
}

bool obs_module_load(void) {
  obs_source_info info = {};
  info.id = kSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = getName;
  info.create = create;
  info.destroy = destroy;
  info.get_defaults = getDefaults;
  info.get_properties = getProperties;
  info.update = update;
  obs_register_source(&info);
  blog(LOG_INFO, "[vmx-receiver] 版 %s を読み込みました（受け渡しの約束の版 %u）", VMX_OBS_RECEIVER_VERSION, VMX_IPC_VERSION);
  return true;
}
