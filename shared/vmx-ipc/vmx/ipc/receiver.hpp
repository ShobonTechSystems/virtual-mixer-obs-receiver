// 受け手の共通の処理（docs/P3C_OBS_PCM.md §4）。OBS の音声ソース（GPL、別 repo）と p3c-ipc-link recv が同じものを使う。
// 一覧を見てストリームを開き、たまったブロックを読み、時刻を付けて渡す。書き手の再起動・停止・応答なしを見分ける。
// 専用のスレッドから poll() を短い間隔で呼ぶ。音声スレッドではない（開くときにメモリを確保する）。
// 読むときはメモリを確保しない。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vmx/ipc/directory.hpp"
#include "vmx/ipc/ring.hpp"
#include "vmx/ipc/shm.hpp"
#include "vmx/ipc/timeline.hpp"

namespace vmx::ipc {

struct ReceiverConfig {
  std::string nameBase = VMX_IPC_NAME_BASE;  // 試験は別の名前
  std::uint32_t streamId = 1;
  std::uint32_t marginBlocks = 2;            // 合わせ直すとき、最新から何ブロック手前に置くか
  std::uint64_t fixedDelayNs = 10'000'000;   // OBS へ渡す時刻に足す固定遅延（§5）
  std::uint64_t reopenNs = 500'000'000;      // 開き直す・一覧を見直す間隔
  std::uint64_t stallNs = 1'000'000'000;     // これだけ新しいブロックが無く、生存の時刻も古ければ「応答なし」
  TimelineConfig timeline{};
};

enum class ReceiverEvent {
  WaitingForWriter,  // 一覧が無い・停止中・ストリームが無い（最初の1回と、状態が変わったとき）
  Connected,         // ストリームを開いた
  WriterRestarted,   // session が変わった（書き手が起動し直した）
  WriterStopped,     // 一覧が「停止」になった・ストリームが一覧から消えた
  WriterStalled,     // 書き手が応答しない（落ちた・止まった）
  WriterResumed,     // 応答なし・音なしの後に、また書き始めた
  FormatMismatch,    // ヘッダーの形式・magic・版が読めない・合わない
  WriterIdle,        // 書き手は生きている（生存の時刻が新しい）が、音を書いていない（主IF の停止・抜けなど）
  AccessDenied,      // 共有メモリはあるが、権限が足りず開けない（管理者の窓で送り手を起動したときなど）
};

inline const char* toString(ReceiverEvent e) {
  switch (e) {
    case ReceiverEvent::WaitingForWriter: return "waiting_for_writer";
    case ReceiverEvent::Connected: return "connected";
    case ReceiverEvent::WriterRestarted: return "writer_restarted";
    case ReceiverEvent::WriterStopped: return "writer_stopped";
    case ReceiverEvent::WriterStalled: return "writer_stalled";
    case ReceiverEvent::WriterResumed: return "writer_resumed";
    case ReceiverEvent::FormatMismatch: return "format_mismatch";
    case ReceiverEvent::WriterIdle: return "writer_idle";
    case ReceiverEvent::AccessDenied: return "access_denied";
  }
  return "?";
}

struct ReceivedBlock {
  const float* interleaved = nullptr;
  std::uint32_t frames = 0, channels = 0, sampleRate = 0;
  std::uint64_t timestampNs = 0;  // OBS へ渡す時刻（固定遅延を含む）
  std::uint64_t qpcNs = 0;        // 書き手がブロックに付けた時刻
  std::uint64_t samplePos = 0;
  std::uint64_t sequence = 0;
  std::uint32_t flags = 0;
};

struct ReceiverStats {
  std::uint64_t blocks = 0;         // 渡したブロック
  std::uint64_t dropped = 0;        // Reader が数えた取りこぼし（一周された・上書き・epoch の変化）
  std::uint64_t positionGaps = 0;   // 続いて受けたブロックのサンプル位置が飛んだ（不連続フラグの無いもの）
  std::uint64_t discontinuities = 0;  // 書き手の不連続フラグ
  std::uint64_t late = 0;           // 不足：渡した時刻が、付けた時刻（固定遅延を含む）より後だった
  std::uint64_t connects = 0, restarts = 0, stops = 0, stalls = 0, idles = 0, formatMismatches = 0, accessDenied = 0;
  std::uint64_t catchUpBlocks = 0;  // 接続・合わせ直しの直後に、余裕の分を追い付きで読んだブロック（handoff に入れない）
  DelayHistogram handoff;           // 渡した時刻 − ブロックの QPC（追い付きの分を除く）
};

using ClockFn = std::uint64_t (*)();

class Receiver {
 public:
  explicit Receiver(ReceiverConfig c, ClockFn clock = &nowNs) : cfg_(std::move(c)), clock_(clock) {}
  ~Receiver() { disconnect(); }

  // sink(const ReceivedBlock&)、log(ReceiverEvent, uint64_t nowNs) を呼ぶ。渡したブロック数を返す
  template <class Sink, class Log>
  std::uint64_t poll(Sink&& sink, Log&& log) {
    const std::uint64_t now = clock_();
    if (!reader_) {
      if (lastTry_ != 0 && now - lastTry_ < cfg_.reopenNs) return 0;
      lastTry_ = now;
      tryConnect(now, log);
      if (!reader_) return 0;
    }

    std::uint64_t delivered = 0;
    // 1 回の poll で読むのはリング 2 周ぶんまで（約束違反の書き手で回り続けないため）
    for (std::uint64_t guard = 0; guard < 2ull * fmt_.ringBlocks; ++guard) {
      const ReadResult r = reader_->read(block_.data(), now);
      if (reader_->dropped() != lastDropped_) {
        stats_.dropped += reader_->dropped() - lastDropped_;
        lastDropped_ = reader_->dropped();
        needRebase_ = true;
      }
      if (r.status == ReadStatus::Dropped) continue;  // 合わせ直した：続きを読む（無音のブロックは渡さない）
      if (r.status == ReadStatus::FormatMismatch || r.status == ReadStatus::NotReady) {
        // 同じ session のまま形式が変わった・書き直しの最中：閉じて開き直す（新しい形式で読む）
        if (r.status == ReadStatus::FormatMismatch) {
          ++stats_.formatMismatches;
          say(ReceiverEvent::FormatMismatch, now, log);
        }
        disconnect();
        return delivered;
      }
      if (r.status != ReadStatus::Ok) break;
      const bool disc = (r.flags & VMX_IPC_FLAG_DISCONTINUITY) != 0;
      // 接続・合わせ直しの直後にたまっていた分は、追い付きで読む（いちばん新しいブロックだけを遅延の分布に入れる）
      if (disc || needRebase_) catchUp_ = static_cast<std::uint32_t>(reader_->fill());
      if (disc) ++stats_.discontinuities;
      if (havePos_ && !disc && !needRebase_ && r.samplePos != lastPos_ + fmt_.blockFrames) ++stats_.positionGaps;
      const std::uint64_t t = timeline_.place(r.samplePos, r.qpcNs, fmt_.sampleRate, disc || needRebase_);
      needRebase_ = false;
      havePos_ = true;
      lastPos_ = r.samplePos;

      ReceivedBlock b;
      b.interleaved = block_.data();
      b.frames = fmt_.blockFrames;
      b.channels = fmt_.channels;
      b.sampleRate = fmt_.sampleRate;
      b.timestampNs = t + cfg_.fixedDelayNs;
      b.qpcNs = r.qpcNs;
      b.samplePos = r.samplePos;
      b.sequence = r.sequence;
      b.flags = r.flags;
      const std::uint64_t handedAt = clock_();
      sink(static_cast<const ReceivedBlock&>(b));
      if (catchUp_ > 0) {
        --catchUp_;
        ++stats_.catchUpBlocks;
      } else {
        stats_.handoff.add(handedAt > r.qpcNs ? handedAt - r.qpcNs : 0);
      }
      if (handedAt > b.timestampNs) ++stats_.late;
      ++stats_.blocks;
      ++delivered;
      lastBlockAt_ = now;
    }

    if (delivered > 0 && (stalled_ || idle_)) {
      stalled_ = false;
      idle_ = false;
      log(ReceiverEvent::WriterResumed, now);
    }
    if (now - lastDirCheck_ >= cfg_.reopenNs) {
      lastDirCheck_ = now;
      checkDirectory(now, log);
    }
    return delivered;
  }

  const ReceiverStats& stats() const { return stats_; }
  const Timeline& timeline() const { return timeline_; }
  bool connected() const { return static_cast<bool>(reader_); }
  const Format& format() const { return fmt_; }
  std::uint64_t fill() const { return reader_ ? reader_->fill() : 0; }
  std::string streamLabel() const { return label_; }

  void disconnect() {
    if (reader_) reader_->detach();
    reader_.reset();
    stream_.close();
    timeline_.reset();
    havePos_ = false;
    stalled_ = false;
    idle_ = false;
  }

 private:
  template <class Log>
  void say(ReceiverEvent e, std::uint64_t now, Log& log) {
    // 待っている間・開けない間は1回だけ
    if ((e == ReceiverEvent::WaitingForWriter || e == ReceiverEvent::AccessDenied) && lastEvent_ == e) return;
    lastEvent_ = e;
    log(e, now);
  }

  bool readDir(DirectoryContent& c, std::uint64_t* hb) {
    if (!dir_.isOpen() && !dir_.open(directoryName(cfg_.nameBase))) return false;
    return readDirectory(dir_.data(), dir_.size(), c, hb) == DirectoryRead::Ok;
  }

  template <class Log>
  void tryConnect(std::uint64_t now, Log& log) {
    DirectoryContent c;
    if (!readDir(c, nullptr)) {
      const bool denied = dir_.accessDenied();
      dir_.close();  // まだ無い・壊れている：次はもう一度開く
      if (denied) {
        if (lastEvent_ != ReceiverEvent::AccessDenied) ++stats_.accessDenied;
        say(ReceiverEvent::AccessDenied, now, log);
      } else {
        say(ReceiverEvent::WaitingForWriter, now, log);
      }
      return;
    }
    const vmx_ipc_dir_stream* s = c.find(cfg_.streamId);
    if (c.state != VMX_IPC_DIR_STATE_RUNNING || s == nullptr) {
      say(ReceiverEvent::WaitingForWriter, now, log);
      return;
    }
    if (!stream_.open(streamName(c.sessionUuid, cfg_.streamId, cfg_.nameBase)) || stream_.size() < sizeof(vmx_ipc_header)) {
      const bool denied = stream_.accessDenied();
      stream_.close();
      if (denied) {
        if (lastEvent_ != ReceiverEvent::AccessDenied) ++stats_.accessDenied;
        say(ReceiverEvent::AccessDenied, now, log);
      } else {
        say(ReceiverEvent::WaitingForWriter, now, log);
      }
      return;
    }
    Format f;
    if (!peekFormat(stream_.data(), stream_.size(), f)) {
      stream_.close();
      if (lastEvent_ != ReceiverEvent::FormatMismatch) {
        ++stats_.formatMismatches;
        say(ReceiverEvent::FormatMismatch, now, log);
      }
      return;
    }
    fmt_ = f;
    block_.assign(f.samplesPerBlock(), 0.0f);
    const std::uint32_t margin = cfg_.marginBlocks < f.ringBlocks ? cfg_.marginBlocks : f.ringBlocks - 1;
    reader_ = std::make_unique<Reader>(stream_.data(), stream_.size(), f, margin < 1 ? 1 : margin);
    reader_->attach(currentPid());  // 取れなくても読める
    lastDropped_ = 0;
    needRebase_ = true;
    session_ = c;
    label_ = s->name;
    lastBlockAt_ = now;
    lastDirCheck_ = now;
    ++stats_.connects;
    say(ReceiverEvent::Connected, now, log);
  }

  template <class Log>
  void checkDirectory(std::uint64_t now, Log& log) {
    DirectoryContent c;
    std::uint64_t hb = 0;
    if (!readDir(c, &hb)) return;  // 書いている最中：次の回に見る
    if (!c.sameSession(session_)) {
      ++stats_.restarts;
      disconnect();
      lastTry_ = 0;  // すぐ開き直す
      say(ReceiverEvent::WriterRestarted, now, log);
      return;
    }
    if (c.state != VMX_IPC_DIR_STATE_RUNNING || c.find(cfg_.streamId) == nullptr) {
      ++stats_.stops;
      disconnect();
      say(ReceiverEvent::WriterStopped, now, log);
      return;
    }
    const bool noBlocks = now - lastBlockAt_ >= cfg_.stallNs;
    const bool oldHeartbeat = hb == 0 || (now > hb && now - hb >= cfg_.stallNs);
    if (!stalled_ && noBlocks && oldHeartbeat) {
      stalled_ = true;
      ++stats_.stalls;
      say(ReceiverEvent::WriterStalled, now, log);
    } else if (!stalled_ && !idle_ && noBlocks && !oldHeartbeat) {
      idle_ = true;
      ++stats_.idles;
      say(ReceiverEvent::WriterIdle, now, log);
    }
  }

  // ヘッダーから形式を読む（epoch の前後一致で、書き直しの最中を避ける）
  static bool peekFormat(void* mem, std::size_t bytes, Format& f) {
    auto ld = [mem](std::size_t off) { return detail::at<std::uint32_t>(mem, off).load(std::memory_order_acquire); };
    const std::uint32_t e1 = ld(offsetof(vmx_ipc_header, epoch));
    if (e1 == 0) return false;
    if (ld(offsetof(vmx_ipc_header, magic)) != VMX_IPC_MAGIC || ld(offsetof(vmx_ipc_header, version)) != VMX_IPC_VERSION) return false;
    f.sampleRate = ld(offsetof(vmx_ipc_header, sample_rate));
    f.channels = ld(offsetof(vmx_ipc_header, channels));
    f.blockFrames = ld(offsetof(vmx_ipc_header, block_frames));
    f.ringBlocks = ld(offsetof(vmx_ipc_header, ring_blocks));
    if (ld(offsetof(vmx_ipc_header, epoch)) != e1) return false;
    return f.valid() && f.blockFrames <= 8192 && f.totalBytes() <= bytes;
  }

  ReceiverConfig cfg_;
  ClockFn clock_;
  SharedMemory dir_, stream_;
  std::unique_ptr<Reader> reader_;
  Format fmt_{};
  std::vector<float> block_;
  Timeline timeline_{cfg_.timeline};
  ReceiverStats stats_;
  DirectoryContent session_{};
  std::string label_;
  ReceiverEvent lastEvent_ = ReceiverEvent::WriterStopped;
  std::uint64_t lastTry_ = 0, lastDirCheck_ = 0, lastBlockAt_ = 0, lastDropped_ = 0, lastPos_ = 0;
  std::uint32_t catchUp_ = 0;
  bool needRebase_ = true, havePos_ = false, stalled_ = false, idle_ = false;
};

}  // namespace vmx::ipc
