// OS の共有メモリと時計（docs/P3C_OBS_PCM.md §3）。
// Windows：名前付きの共有メモリ（Local\...）と QueryPerformanceCounter（OBS の os_gettime_ns と同じ時計）。
// それ以外：POSIX の共有メモリ（/...）と CLOCK_MONOTONIC。Linux の試験用（製品の対象は Windows だけ）。
// 依存なし（OS の API と標準ライブラリだけ）。音声スレッドからは nowNs() だけを呼ぶ。
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shobon Tech Systems
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "vmx/ipc/protocol.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace vmx::ipc {

// 単調な時計（ns）。Windows は QPC を os_gettime_ns と同じ計算で ns にする
inline std::uint64_t nowNs() {
#ifdef _WIN32
  static const std::uint64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<std::uint64_t>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  const auto t = static_cast<std::uint64_t>(c.QuadPart);
  return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

inline std::uint32_t currentPid() {
#ifdef _WIN32
  return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(getpid());
#endif
}

// セッションごとの UUID（版 4 の形）。乱数は起動時に1回だけ
inline void makeSessionUuid(std::uint8_t (&out)[16]) {
  std::random_device rd;
  for (std::size_t i = 0; i < 16; i += 4) {
    const std::uint32_t r = rd();
    for (std::size_t k = 0; k < 4; ++k) out[i + k] = static_cast<std::uint8_t>(r >> (8 * k));
  }
  out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x40);
  out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80);
}

inline std::string uuidHex(const std::uint8_t (&u)[16]) {
  std::string s;
  char b[3];
  for (std::uint8_t v : u) {
    std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(v));
    s += b;
  }
  return s;
}

// 名前（OS の接頭辞なし）。base は既定で VMX_IPC_NAME_BASE。試験は別の base を使い、動いている本体とぶつからないようにする
inline std::string directoryName(const std::string& base = VMX_IPC_NAME_BASE) { return base + VMX_IPC_DIRECTORY_SUFFIX; }
inline std::string streamName(const std::uint8_t (&uuid)[16], std::uint32_t streamId, const std::string& base = VMX_IPC_NAME_BASE) {
  return base + "." + uuidHex(uuid) + "." + std::to_string(streamId);
}

// 名前付きの共有メモリ。書き手は create、読み手は open。閉じるのはデストラクター
class SharedMemory {
 public:
  SharedMemory() = default;
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  ~SharedMemory() { close(); }

  // 既にあれば、その領域を開く（Windows は、だれかが開いている限り同じ領域が返る）。existed() で分かる
  bool create(const std::string& name, std::size_t bytes) { return map(name, bytes, true); }
  // 読み手。読み手スロットに書くので、読み書きで開く
  bool open(const std::string& name) { return map(name, 0, false); }

  void* data() const { return data_; }
  std::size_t size() const { return size_; }
  bool existed() const { return existed_; }
  // 直前の create / open が、権限が足りずに失敗した（Windows の ERROR_ACCESS_DENIED、POSIX の EACCES）
  bool accessDenied() const { return denied_; }
  bool isOpen() const { return data_ != nullptr; }

  void close() {
#ifdef _WIN32
    if (data_ != nullptr) UnmapViewOfFile(data_);
    if (handle_ != nullptr) CloseHandle(handle_);
    handle_ = nullptr;
#else
    if (data_ != nullptr) munmap(data_, size_);
#endif
    data_ = nullptr;
    size_ = 0;
    existed_ = false;
  }

  // POSIX では名前が残るので消す（Windows は全員が閉じれば消える）。
  // 一覧は消さない：開いたままの読み手が、次の書き手の一覧を見られるように（Windows と同じ振る舞い）
  static void unlink(const std::string& name) {
#ifndef _WIN32
    shm_unlink(("/" + name).c_str());
#else
    (void)name;
#endif
  }

 private:
  bool map(const std::string& shortName, std::size_t bytes, bool create) {
    close();
    denied_ = false;
#ifdef _WIN32
    std::wstring name = L"Local\\";
    for (char c : shortName) name += static_cast<wchar_t>(static_cast<unsigned char>(c));  // 名前は ASCII だけ
    if (create) {
      const auto b = static_cast<std::uint64_t>(bytes);
      handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(b >> 32),
                                   static_cast<DWORD>(b & 0xFFFFFFFFu), name.c_str());
      existed_ = handle_ != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    } else {
      handle_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    }
    if (handle_ == nullptr) {
      denied_ = GetLastError() == ERROR_ACCESS_DENIED;
      return false;
    }
    data_ = MapViewOfFile(handle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, create ? bytes : 0);
    if (data_ == nullptr) {
      const bool denied = GetLastError() == ERROR_ACCESS_DENIED;
      close();
      denied_ = denied;
      return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(data_, &mbi, sizeof(mbi)) == 0) {
      close();
      return false;
    }
    size_ = mbi.RegionSize;  // ページ単位に切り上がる。既にあった領域が小さければ、呼び手が size() で確かめる
    return true;
#else
    const std::string name = "/" + shortName;
    int fd = -1;
    if (create) {
      fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
      if (fd < 0) {
        fd = shm_open(name.c_str(), O_RDWR, 0);
        existed_ = fd >= 0;
      }
    } else {
      fd = shm_open(name.c_str(), O_RDWR, 0);
    }
    if (fd < 0) {
      denied_ = errno == EACCES;
      return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
      ::close(fd);
      return false;
    }
    std::size_t sz = static_cast<std::size_t>(st.st_size);
    if (create && sz < bytes) {
      if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return false;
      }
      sz = bytes;
    }
    if (sz == 0) {
      ::close(fd);
      return false;
    }
    void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
      existed_ = false;
      return false;
    }
    data_ = p;
    size_ = sz;
    return true;
#endif
  }

  void* data_ = nullptr;
  std::size_t size_ = 0;
  bool existed_ = false;
  bool denied_ = false;
#ifdef _WIN32
  HANDLE handle_ = nullptr;
#endif
};

}  // namespace vmx::ipc
