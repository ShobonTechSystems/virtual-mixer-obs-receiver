// ストリームの一覧の書き手と読み手（docs/P3C_OBS_PCM.md §3）。名前付きパイプの代わりの案。
// 書き手は読み手を待たない。読み手は seqlock（dir_seq）で写し、書いている最中の一覧を使わない。
// 依存なし。音声スレッドからは呼ばない（一覧は形式やストリームが変わるときだけ書く）。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "vmx/ipc/protocol.h"

namespace vmx::ipc {

inline constexpr std::size_t kDirectoryBytes = sizeof(vmx_ipc_directory);

namespace dirdetail {

constexpr std::size_t kWords = kDirectoryBytes / 4;
constexpr std::size_t kSeqWord = offsetof(vmx_ipc_directory, dir_seq) / 4;
constexpr std::size_t kHeartbeatWord = offsetof(vmx_ipc_directory, heartbeat_ns) / 4;  // 2 語ぶん（64 ビット）

inline std::atomic_ref<std::uint32_t> word(void* base, std::size_t i) {
  return std::atomic_ref<std::uint32_t>(static_cast<std::uint32_t*>(base)[i]);
}
inline std::atomic_ref<std::uint64_t> heartbeat(void* base) {
  return std::atomic_ref<std::uint64_t>(static_cast<vmx_ipc_directory*>(base)->heartbeat_ns);
}
// dir_seq と heartbeat 以外の語（中身）
constexpr bool isContent(std::size_t i) { return i != kSeqWord && i != kHeartbeatWord && i != kHeartbeatWord + 1; }

}  // namespace dirdetail

// 一覧の中身（読み手が写したもの、書き手が置くもの）
struct DirectoryContent {
  std::uint32_t writerPid = 0;
  std::uint8_t sessionUuid[16] = {};
  std::uint32_t state = VMX_IPC_DIR_STATE_STOPPED;
  std::uint32_t streamCount = 0;
  vmx_ipc_dir_stream streams[VMX_IPC_DIR_MAX_STREAMS] = {};

  bool sameSession(const DirectoryContent& o) const {
    return writerPid == o.writerPid && std::memcmp(sessionUuid, o.sessionUuid, 16) == 0;
  }
  const vmx_ipc_dir_stream* find(std::uint32_t streamId) const {
    for (std::uint32_t i = 0; i < streamCount && i < VMX_IPC_DIR_MAX_STREAMS; ++i)
      if (streams[i].stream_id == streamId) return &streams[i];
    return nullptr;
  }
  // 足せなければ false（満杯、ch 数が不正）
  bool addStream(std::uint32_t id, std::uint32_t channels, std::string_view name) {
    if (streamCount >= VMX_IPC_DIR_MAX_STREAMS || channels < 1 || channels > VMX_IPC_MAX_CHANNELS || find(id)) return false;
    vmx_ipc_dir_stream& s = streams[streamCount++];
    s = {};
    s.stream_id = id;
    s.channels = channels;
    std::memcpy(s.name, name.data(), name.size() < VMX_IPC_DIR_NAME_BYTES - 1 ? name.size() : VMX_IPC_DIR_NAME_BYTES - 1);
    return true;
  }
};

class DirectoryWriter {
 public:
  DirectoryWriter(void* memory, std::size_t bytes) : mem_(bytes >= kDirectoryBytes ? memory : nullptr) {
    if (mem_ != nullptr) {
      // 前の書き手の一覧が残っていれば、その版の続きから書く（読み手が版の戻りで混乱しないため）。
      // 前の書き手が書いている途中で落ちて奇数のままなら、偶数へそろえる
      seq_ = dirdetail::word(mem_, dirdetail::kSeqWord).load(std::memory_order_acquire);
      if (seq_ % 2 != 0) ++seq_;
    }
  }

  bool valid() const { return mem_ != nullptr; }

  void publish(const DirectoryContent& c) {
    if (mem_ == nullptr) return;
    vmx_ipc_directory d{};
    d.magic = VMX_IPC_DIR_MAGIC;
    d.version = VMX_IPC_DIR_VERSION;
    d.writer_pid = c.writerPid;
    std::memcpy(d.session_uuid, c.sessionUuid, 16);
    d.state = c.state;
    d.stream_count = c.streamCount > VMX_IPC_DIR_MAX_STREAMS ? VMX_IPC_DIR_MAX_STREAMS : c.streamCount;
    std::memcpy(d.streams, c.streams, sizeof(d.streams));
    std::uint32_t w[dirdetail::kWords];
    std::memcpy(w, &d, sizeof(w));

    auto seq = dirdetail::word(mem_, dirdetail::kSeqWord);
    // ring.hpp の §4.1 と同じ形：奇数 → 中身を release で1つずつ → 偶数を release
    seq.store(++seq_, std::memory_order_relaxed);
    for (std::size_t i = 0; i < dirdetail::kWords; ++i)
      if (dirdetail::isContent(i)) dirdetail::word(mem_, i).store(w[i], std::memory_order_release);
    seq.store(++seq_, std::memory_order_release);
  }

  // 生存の時刻。一覧の版とは別（1 秒に何度も書くので、一覧を書き直さない）
  void heartbeat(std::uint64_t nowNs) {
    if (mem_ != nullptr) dirdetail::heartbeat(mem_).store(nowNs, std::memory_order_release);
  }

 private:
  void* mem_;
  std::uint32_t seq_ = 0;
};

enum class DirectoryRead {
  Ok,
  Busy,     // 書いている最中だった（あとでもう一度）
  Invalid,  // magic・版が違う、まだ書かれていない
};

// 一覧を写す。Busy のときは何度か試す（書き手は短い間しか奇数にしない）
inline DirectoryRead readDirectory(void* memory, std::size_t bytes, DirectoryContent& out, std::uint64_t* heartbeatNs = nullptr,
                                   int attempts = 4) {
  if (memory == nullptr || bytes < kDirectoryBytes) return DirectoryRead::Invalid;
  auto seq = dirdetail::word(memory, dirdetail::kSeqWord);
  for (int a = 0; a < attempts; ++a) {
    const std::uint32_t s1 = seq.load(std::memory_order_acquire);
    if (s1 % 2 != 0) continue;
    std::uint32_t w[dirdetail::kWords] = {};
    for (std::size_t i = 0; i < dirdetail::kWords; ++i)
      if (dirdetail::isContent(i)) w[i] = dirdetail::word(memory, i).load(std::memory_order_acquire);
    const std::uint32_t s2 = seq.load(std::memory_order_acquire);
    if (s1 != s2) continue;
    vmx_ipc_directory d;
    std::memcpy(&d, w, sizeof(d));
    if (d.magic != VMX_IPC_DIR_MAGIC || d.version != VMX_IPC_DIR_VERSION || d.stream_count > VMX_IPC_DIR_MAX_STREAMS)
      return DirectoryRead::Invalid;
    out = DirectoryContent{};
    out.writerPid = d.writer_pid;
    std::memcpy(out.sessionUuid, d.session_uuid, 16);
    out.state = d.state;
    out.streamCount = d.stream_count;
    std::memcpy(out.streams, d.streams, sizeof(out.streams));
    for (auto& s : out.streams) s.name[VMX_IPC_DIR_NAME_BYTES - 1] = '\0';
    if (heartbeatNs != nullptr) *heartbeatNs = dirdetail::heartbeat(memory).load(std::memory_order_acquire);
    return DirectoryRead::Ok;
  }
  return DirectoryRead::Busy;
}

}  // namespace vmx::ipc
