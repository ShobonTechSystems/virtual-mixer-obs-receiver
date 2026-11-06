// 共有メモリのリングの書き手と読み手（docs/IPC_PROTOCOL.md §4、§5）。
// 依存なし（標準ライブラリだけ）。メモリの確保・待ち・ロックをしないので、音声スレッドから呼べる。
// 共有メモリの作成・名前・パイプは OS ごとの層で行い、ここでは渡された先頭アドレスだけを扱う。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vmx/ipc/protocol.h"

namespace vmx::ipc {

struct Format {
  std::uint32_t sampleRate = 48000;
  std::uint32_t channels = 2;
  std::uint32_t blockFrames = 64;
  std::uint32_t ringBlocks = 2048;

  bool valid() const {
    return sampleRate > 0 && channels >= 1 && channels <= VMX_IPC_MAX_CHANNELS && blockFrames > 0 &&
           ringBlocks >= 2;
  }
  std::size_t samplesPerBlock() const { return std::size_t{blockFrames} * channels; }
  std::uint64_t totalBytes() const { return vmx_ipc_total_bytes(ringBlocks, blockFrames, channels); }
  bool operator==(const Format&) const = default;
};

// プロセスをまたいで使うので、ロックなしの atomic でなければならない
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free, "32-bit atomics must be lock-free");

namespace detail {

template <class T>
std::atomic_ref<T> at(void* base, std::uint64_t offset) {
  return std::atomic_ref<T>(*reinterpret_cast<T*>(static_cast<unsigned char*>(base) + offset));
}

inline std::uint64_t metaOffset(std::uint64_t slot) { return vmx_ipc_meta_offset() + slot * sizeof(vmx_ipc_block_meta); }
inline std::uint64_t sampleOffset(const Format& f, std::uint64_t slot) {
  return vmx_ipc_samples_offset(f.ringBlocks) + slot * f.samplesPerBlock() * 4u;
}
// 形式に関係なく同じ位置（形式が変わっても、古い読み手のスロットへの書き込みが音声を壊さない）
inline std::uint64_t readerOffset(std::uint32_t index) {
  return vmx_ipc_readers_offset() + std::uint64_t{index} * sizeof(vmx_ipc_reader_slot);
}

constexpr std::uint64_t kHeaderEpoch = offsetof(vmx_ipc_header, epoch);
constexpr std::uint64_t kHeaderWritten = offsetof(vmx_ipc_header, blocks_written);
constexpr std::uint64_t kMetaSeq = offsetof(vmx_ipc_block_meta, slot_seq);
constexpr std::uint64_t kMetaPos = offsetof(vmx_ipc_block_meta, sample_pos);
constexpr std::uint64_t kMetaQpc = offsetof(vmx_ipc_block_meta, qpc_ns);
constexpr std::uint64_t kMetaSequence = offsetof(vmx_ipc_block_meta, sequence);
constexpr std::uint64_t kMetaFlags = offsetof(vmx_ipc_block_meta, flags);
constexpr std::uint64_t kMetaNan = offsetof(vmx_ipc_block_meta, nan_count);

}  // namespace detail

// 書き手（本体、ストリームごとに1つ）。読み手を待たず、読み手スロットも判断に使わない
class Writer {
 public:
  Writer(void* memory, std::size_t bytes) : mem_(memory), bytes_(bytes) {}

  // 形式を置いて epoch を進める。形式を変えるときも同じ（epoch を 0 にしてから書き直す）
  // 音声スレッドの外で呼ぶ（共有メモリ全体の初期化を含むため）
  bool init(const Format& f, const std::uint8_t (&sessionUuid)[16], std::uint32_t streamId, std::uint32_t epoch) {
    if (!f.valid() || epoch == 0 || f.totalBytes() > bytes_ || mem_ == nullptr) return false;
    // 同じ epoch を使い回すと、読み手が書き手の再起動に気づけない
    if (detail::at<std::uint32_t>(mem_, detail::kHeaderEpoch).load(std::memory_order_acquire) == epoch) return false;
    // init は音声スレッドの外で、まれにしか呼ばないので、順序の強い seq_cst で書く（fence を使わない）
    detail::at<std::uint32_t>(mem_, detail::kHeaderEpoch).store(0, std::memory_order_seq_cst);
    auto* h = static_cast<vmx_ipc_header*>(mem_);
    // ヘッダーの普通の欄は epoch = 0 の間に書く。読み手は epoch の前後一致で確かめる
    storeHeader32(offsetof(vmx_ipc_header, magic), VMX_IPC_MAGIC);
    storeHeader32(offsetof(vmx_ipc_header, version), VMX_IPC_VERSION);
    for (std::size_t i = 0; i < 16; ++i)
      std::atomic_ref<std::uint8_t>(h->session_uuid[i]).store(sessionUuid[i], std::memory_order_seq_cst);
    storeHeader32(offsetof(vmx_ipc_header, stream_id), streamId);
    storeHeader32(offsetof(vmx_ipc_header, sample_rate), f.sampleRate);
    storeHeader32(offsetof(vmx_ipc_header, channels), f.channels);
    storeHeader32(offsetof(vmx_ipc_header, block_frames), f.blockFrames);
    storeHeader32(offsetof(vmx_ipc_header, ring_blocks), f.ringBlocks);
    for (std::uint64_t s = 0; s < f.ringBlocks; ++s)
      detail::at<std::uint64_t>(mem_, detail::metaOffset(s) + detail::kMetaSeq).store(0, std::memory_order_seq_cst);
    detail::at<std::uint64_t>(mem_, detail::kHeaderWritten).store(0, std::memory_order_seq_cst);
    format_ = f;
    next_ = 0;
    detail::at<std::uint32_t>(mem_, detail::kHeaderEpoch).store(epoch, std::memory_order_seq_cst);
    ready_ = true;
    return true;
  }

  // 1ブロック（ch 交互、samplesPerBlock 個）を書く。NaN・Inf は 0 にして書き、その数を返す
  std::uint32_t writeBlock(const float* interleaved, std::uint64_t samplePos, std::uint64_t qpcNs,
                           std::uint32_t flags) {
    if (!ready_) return 0;
    const std::uint64_t n = next_;
    const std::uint64_t slot = n % format_.ringBlocks;
    const std::uint64_t meta = detail::metaOffset(slot);
    auto seq = detail::at<std::uint64_t>(mem_, meta + detail::kMetaSeq);

    // §4.1：書き込み中（奇数）を置いてから中身を書く。
    // 中身は1つずつ release で書く。release は前の書き込み（奇数の版）を追い越さないので、
    // 読み手が新しい中身を1つでも見たら、奇数の版も見える（fence を使わない形。x86 では追加の命令なし）
    seq.store(2 * n + 1, std::memory_order_relaxed);

    std::uint32_t nan = 0;
    const std::uint64_t base = detail::sampleOffset(format_, slot);
    const std::size_t count = format_.samplesPerBlock();
    for (std::size_t i = 0; i < count; ++i) {
      float v = interleaved[i];
      if (!std::isfinite(v)) {
        v = 0.0f;
        ++nan;
      }
      detail::at<std::uint32_t>(mem_, base + i * 4u).store(std::bit_cast<std::uint32_t>(v), std::memory_order_release);
    }
    detail::at<std::uint64_t>(mem_, meta + detail::kMetaPos).store(samplePos, std::memory_order_release);
    detail::at<std::uint64_t>(mem_, meta + detail::kMetaQpc).store(qpcNs, std::memory_order_release);
    detail::at<std::uint64_t>(mem_, meta + detail::kMetaSequence).store(n, std::memory_order_release);
    detail::at<std::uint32_t>(mem_, meta + detail::kMetaFlags).store(flags, std::memory_order_release);
    detail::at<std::uint32_t>(mem_, meta + detail::kMetaNan).store(nan, std::memory_order_release);

    // 書き終えた（偶数）→ 書き終えたブロック数、の順に release で公開する
    seq.store(2 * n + 2, std::memory_order_release);
    detail::at<std::uint64_t>(mem_, detail::kHeaderWritten).store(n + 1, std::memory_order_release);
    next_ = n + 1;
    return nan;
  }

  std::uint64_t blocksWritten() const { return next_; }
  const Format& format() const { return format_; }

 private:
  void storeHeader32(std::uint64_t off, std::uint32_t v) {
    detail::at<std::uint32_t>(mem_, off).store(v, std::memory_order_seq_cst);
  }

  void* mem_;
  std::size_t bytes_;
  Format format_{};
  std::uint64_t next_ = 0;
  bool ready_ = false;
};

enum class ReadStatus {
  Ok,              // 1ブロック読めた
  NotReady,        // 書き手がいない・準備中（epoch 0）。無音
  FormatMismatch,  // magic・版・形式が期待と違う。読まない（無音）
  NoData,          // まだ新しいブロックがない（不足）。無音
  Dropped,         // 一周された・上書きされた・epoch が変わったので合わせ直した。無音
};

struct ReadResult {
  ReadStatus status = ReadStatus::NotReady;
  std::uint64_t sequence = 0;
  std::uint64_t samplePos = 0;
  std::uint64_t qpcNs = 0;
  std::uint32_t flags = 0;
  std::uint32_t nanCount = 0;
};

// 読み手（OBS / DAW Receiver）。自分のカーソルは自分のメモリに持つ（§5）
class Reader {
 public:
  // margin：合わせ直すとき、最新から何ブロック手前に置くか（1 以上、ringBlocks 未満）
  Reader(void* memory, std::size_t bytes, const Format& expected, std::uint32_t marginBlocks)
      : mem_(memory), bytes_(bytes), fmt_(expected), margin_(marginBlocks) {}

  // 空いている読み手スロットを取る。取れなくても読むことはできる（診断が出ないだけ）
  bool attach(std::uint32_t pid) {
    if (pid == 0 || !layoutFits()) return false;
    for (std::uint32_t i = 0; i < VMX_IPC_MAX_READERS; ++i) {
      std::uint32_t expected = 0;
      if (slotPid(i).compare_exchange_strong(expected, pid, std::memory_order_acq_rel)) {
        slot_ = static_cast<int>(i);
        return true;
      }
    }
    return false;
  }

  void detach() {
    if (slot_ >= 0) slotPid(static_cast<std::uint32_t>(slot_)).store(0, std::memory_order_release);
    slot_ = -1;
  }

  // out に samplesPerBlock 個を書く（Ok 以外は無音）
  ReadResult read(float* out, std::uint64_t nowNs = 0) {
    ReadResult r;
    r.status = readImpl(out, r);
    if (r.status != ReadStatus::Ok) std::memset(out, 0, fmt_.samplesPerBlock() * sizeof(float));
    publishSlot(nowNs);
    return r;
  }

  std::uint64_t dropped() const { return dropped_; }
  // 初めてつないだとき、つなぐ前に書かれていて読まなかったブロック数（取りこぼしには数えない）
  std::uint64_t skippedOnAttach() const { return skippedOnAttach_; }
  std::uint64_t cursor() const { return cursor_; }
  std::uint32_t epoch() const { return epoch_; }
  int slotIndex() const { return slot_; }

  // 試験用：写した直後、版を確かめる前に呼ばれる（T12 の上書きの並びを作るため）
  void setAfterCopyHookForTest(void (*fn)(void*), void* ctx) {
    hook_ = fn;
    hookCtx_ = ctx;
  }

  // 未読のブロック数（余裕の水位）。書き手がいなければ 0
  std::uint64_t fill() const {
    if (!layoutFits() || epoch_ == 0) return 0;
    const std::uint64_t w = detail::at<std::uint64_t>(mem_, detail::kHeaderWritten).load(std::memory_order_acquire);
    return w > cursor_ ? w - cursor_ : 0;
  }

 private:
  bool layoutFits() const { return mem_ != nullptr && fmt_.valid() && fmt_.totalBytes() <= bytes_ && margin_ >= 1 && margin_ < fmt_.ringBlocks; }

  std::atomic_ref<std::uint32_t> slotPid(std::uint32_t i) const {
    return detail::at<std::uint32_t>(mem_, detail::readerOffset(i) + offsetof(vmx_ipc_reader_slot, pid));
  }

  std::uint32_t load32(std::uint64_t off) const {
    return detail::at<std::uint32_t>(mem_, off).load(std::memory_order_acquire);
  }

  bool headerMatches() const {
    if (load32(offsetof(vmx_ipc_header, magic)) != VMX_IPC_MAGIC) return false;
    if (load32(offsetof(vmx_ipc_header, version)) != VMX_IPC_VERSION) return false;
    Format f;
    f.sampleRate = load32(offsetof(vmx_ipc_header, sample_rate));
    f.channels = load32(offsetof(vmx_ipc_header, channels));
    f.blockFrames = load32(offsetof(vmx_ipc_header, block_frames));
    f.ringBlocks = load32(offsetof(vmx_ipc_header, ring_blocks));
    return f == fmt_;
  }

  // 最新の少し手前へ進める。カーソルを戻すことはしない（同じブロックを2度数えない）
  void resync(std::uint64_t written) {
    const std::uint64_t target = written > margin_ ? written - margin_ : 0;
    if (target > cursor_) {
      dropped_ += target - cursor_;
      cursor_ = target;
    }
  }

  std::uint64_t loadWritten() const {
    return detail::at<std::uint64_t>(mem_, detail::kHeaderWritten).load(std::memory_order_acquire);
  }

  ReadStatus readImpl(float* out, ReadResult& r) {
    if (!layoutFits()) return ReadStatus::FormatMismatch;
    auto epochRef = detail::at<std::uint32_t>(mem_, detail::kHeaderEpoch);
    const std::uint32_t e1 = epochRef.load(std::memory_order_acquire);
    if (e1 == 0) return ReadStatus::NotReady;

    if (e1 != epoch_) {
      // 新しい epoch：ヘッダーを確かめ、読んでいる間に epoch が変わっていないことも確かめる
      const bool match = headerMatches();  // 欄は acquire で読むので、次の epoch の読みは前へ出ない
      if (epochRef.load(std::memory_order_acquire) != e1) return ReadStatus::NotReady;
      if (!match) return ReadStatus::FormatMismatch;
      const bool hadEpoch = epoch_ != 0;
      epoch_ = e1;
      const std::uint64_t w = loadWritten();
      const std::uint64_t target = w > margin_ ? w - margin_ : 0;
      if (!hadEpoch) {
        // 初めてつないだ：つなぐ前の分は取りこぼしに数えない
        skippedOnAttach_ = target;
        cursor_ = target;
      } else {
        // epoch が変わった：前の epoch の続きは無い。不連続を1つ数え、新しい流れで飛ばした分も数える
        ++dropped_;
        dropped_ += target;
        cursor_ = target;
        return ReadStatus::Dropped;
      }
    }

    const std::uint64_t w = loadWritten();
    if (w < cursor_) {
      // epoch が同じなのに書き終えた数が戻った（約束違反の書き手）。不連続として合わせ直す
      ++dropped_;
      cursor_ = w > margin_ ? w - margin_ : 0;
      return ReadStatus::Dropped;
    }
    if (cursor_ >= w) return ReadStatus::NoData;
    if (w - cursor_ >= fmt_.ringBlocks) {  // 一周された
      resync(w);
      return ReadStatus::Dropped;
    }

    const std::uint64_t n = cursor_;
    const std::uint64_t slot = n % fmt_.ringBlocks;
    const std::uint64_t meta = detail::metaOffset(slot);
    auto seq = detail::at<std::uint64_t>(mem_, meta + detail::kMetaSeq);
    const std::uint64_t s1 = seq.load(std::memory_order_acquire);
    if (s1 != 2 * n + 2) {  // すでに上書きが始まっている
      ++dropped_;
      cursor_ = n + 1;
      resync(loadWritten());
      return ReadStatus::Dropped;
    }

    const std::uint64_t base = detail::sampleOffset(fmt_, slot);
    const std::size_t count = fmt_.samplesPerBlock();
    for (std::size_t i = 0; i < count; ++i)
      out[i] = std::bit_cast<float>(detail::at<std::uint32_t>(mem_, base + i * 4u).load(std::memory_order_acquire));
    r.samplePos = detail::at<std::uint64_t>(mem_, meta + detail::kMetaPos).load(std::memory_order_acquire);
    r.qpcNs = detail::at<std::uint64_t>(mem_, meta + detail::kMetaQpc).load(std::memory_order_acquire);
    r.sequence = detail::at<std::uint64_t>(mem_, meta + detail::kMetaSequence).load(std::memory_order_acquire);
    r.flags = detail::at<std::uint32_t>(mem_, meta + detail::kMetaFlags).load(std::memory_order_acquire);
    r.nanCount = detail::at<std::uint32_t>(mem_, meta + detail::kMetaNan).load(std::memory_order_acquire);

    if (hook_ != nullptr) hook_(hookCtx_);

    // 写した後に版と epoch をもう一度確かめる（§5）。違えば写している間に上書きされた。
    // 中身を acquire で読んでいるので、この読みは写す前へ出ない。新しい中身を1つでも見ていれば、
    // ここで奇数（またはもっと新しい）版が見える
    const std::uint64_t s2 = seq.load(std::memory_order_acquire);
    const std::uint32_t e2 = epochRef.load(std::memory_order_acquire);
    if (s2 != s1 || e2 != e1) {
      ++dropped_;
      cursor_ = n + 1;
      resync(loadWritten());
      return ReadStatus::Dropped;
    }
    cursor_ = n + 1;
    lastSequence_ = n;
    return ReadStatus::Ok;
  }

  void publishSlot(std::uint64_t nowNs) {
    if (slot_ < 0) return;
    const std::uint64_t off = detail::readerOffset(static_cast<std::uint32_t>(slot_));
    detail::at<std::uint64_t>(mem_, off + offsetof(vmx_ipc_reader_slot, last_sequence)).store(lastSequence_, std::memory_order_relaxed);
    detail::at<std::uint64_t>(mem_, off + offsetof(vmx_ipc_reader_slot, dropped)).store(dropped_, std::memory_order_relaxed);
    detail::at<std::uint64_t>(mem_, off + offsetof(vmx_ipc_reader_slot, last_alive_ns)).store(nowNs, std::memory_order_relaxed);
  }

  void* mem_;
  std::size_t bytes_;
  Format fmt_;
  std::uint32_t margin_;
  std::uint32_t epoch_ = 0;
  std::uint64_t cursor_ = 0;
  std::uint64_t lastSequence_ = 0;
  std::uint64_t dropped_ = 0;
  std::uint64_t skippedOnAttach_ = 0;
  int slot_ = -1;
  void (*hook_)(void*) = nullptr;
  void* hookCtx_ = nullptr;
};

}  // namespace vmx::ipc
