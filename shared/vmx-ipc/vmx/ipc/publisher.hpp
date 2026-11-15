// 書き手の側をまとめたもの（docs/P3C_OBS_PCM.md §3）：一覧と1つのストリームの共有メモリを作り、Writer を用意する。
// start / stop / heartbeat は音声スレッドの外で呼ぶ。音声スレッドからは writer().writeBlock() だけを呼ぶ。
// 試作では 1 つの書き手が 1 つのストリームを持つ（製品の本体は複数のストリームを持つ）。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <memory>
#include <string>

#include "vmx/ipc/directory.hpp"
#include "vmx/ipc/ring.hpp"
#include "vmx/ipc/shm.hpp"

namespace vmx::ipc {

class StreamPublisher {
 public:
  explicit StreamPublisher(std::string nameBase = VMX_IPC_NAME_BASE) : base_(std::move(nameBase)) {}
  ~StreamPublisher() { stop(); }

  // 失敗したら false（共有メモリを作れない・形式が不正）。uuid を省くと新しく作る
  bool start(const Format& f, std::uint32_t streamId, const std::string& label, const std::uint8_t* uuid = nullptr) {
    stop();
    otherWriterRunning_ = false;
    if (!f.valid()) return false;
    if (uuid != nullptr)
      std::memcpy(uuid_, uuid, 16);
    else
      makeSessionUuid(uuid_);
    streamId_ = streamId;
    if (!dirMem_.create(directoryName(base_), kDirectoryBytes) || dirMem_.size() < kDirectoryBytes) return fail();
    if (dirMem_.existed()) {
      // ほかの書き手が動いていれば断る（一覧を取り合うと、読み手が「再起動」を繰り返すため）。
      // 「動作中」のまま生存の時刻が古いのは、落ちた書き手の残り：引き継ぐ
      DirectoryContent old;
      std::uint64_t hb = 0;
      const std::uint64_t now = nowNs();
      if (readDirectory(dirMem_.data(), dirMem_.size(), old, &hb) == DirectoryRead::Ok && old.state == VMX_IPC_DIR_STATE_RUNNING &&
          old.writerPid != currentPid() && hb != 0 && (hb > now || now - hb < kOtherWriterAliveNs)) {
        otherWriterRunning_ = true;
        return fail();
      }
    }
    dir_ = std::make_unique<DirectoryWriter>(dirMem_.data(), dirMem_.size());
    streamShmName_ = streamName(uuid_, streamId, base_);
    if (!streamMem_.create(streamShmName_, static_cast<std::size_t>(f.totalBytes())) || streamMem_.size() < f.totalBytes())
      return fail();
    writer_ = std::make_unique<Writer>(streamMem_.data(), streamMem_.size());
    if (!writer_->init(f, uuid_, streamId, 1)) return fail();

    content_ = DirectoryContent{};
    content_.writerPid = currentPid();
    std::memcpy(content_.sessionUuid, uuid_, 16);
    content_.state = VMX_IPC_DIR_STATE_RUNNING;
    content_.addStream(streamId, f.channels, label);
    dir_->heartbeat(nowNs());
    dir_->publish(content_);
    running_ = true;
    return true;
  }

  // 一覧を「停止」にしてから閉じる。読み手は「送り手が止まった」と分かる
  void stop() {
    if (running_ && dir_) {
      content_.state = VMX_IPC_DIR_STATE_STOPPED;
      content_.streamCount = 0;
      dir_->publish(content_);
    }
    running_ = false;
    writer_.reset();
    dir_.reset();
    streamMem_.close();
    if (!streamShmName_.empty()) SharedMemory::unlink(streamShmName_);
    streamShmName_.clear();
    dirMem_.close();
  }

  // 試験用：一覧を「停止」にせず閉じる（書き手が落ちたときと同じ）
  void abandonForTest() {
    running_ = false;
    writer_.reset();
    dir_.reset();
    streamMem_.close();
    dirMem_.close();
  }

  void heartbeat() { heartbeat(nowNs()); }
  void heartbeat(std::uint64_t ns) {
    if (dir_) dir_->heartbeat(ns);
  }

  Writer& writer() { return *writer_; }
  bool running() const { return running_; }
  // 直前の start が、ほかの書き手が動いているために失敗した
  bool otherWriterRunning() const { return otherWriterRunning_; }
  static constexpr std::uint64_t kOtherWriterAliveNs = 1'500'000'000;
  const std::uint8_t (&sessionUuid() const)[16] { return uuid_; }

 private:
  bool fail() {
    abandonForTest();
    if (!streamShmName_.empty()) SharedMemory::unlink(streamShmName_);
    streamShmName_.clear();
    return false;
  }

  std::string base_;
  SharedMemory dirMem_, streamMem_;
  std::unique_ptr<DirectoryWriter> dir_;
  std::unique_ptr<Writer> writer_;
  DirectoryContent content_{};
  std::uint8_t uuid_[16] = {};
  std::uint32_t streamId_ = 0;
  std::string streamShmName_;
  bool running_ = false;
  bool otherWriterRunning_ = false;
};

}  // namespace vmx::ipc
