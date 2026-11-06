// SPDX-License-Identifier: GPL-2.0-or-later
// Linux での簡単な試し：libobs を立ち上げ、プラグインを読み込み、同じプロセスの送り手（vmx::ipc::StreamPublisher）が
// 書いた音が、OBS のソースの音声として届くかを見る。Windows の OBS での確認の代わりにはならない。
// 使い方：vmx_obs_receiver_smoke <プラグインの .so のパス>
#include <obs.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "vmx/ipc/publisher.hpp"

namespace {
std::atomic<std::uint64_t> g_frames{0};
std::atomic<float> g_peak{0};

void onAudio(void*, obs_source_t*, const audio_data* a, bool muted) {
  if (muted || a->frames == 0) return;
  g_frames.fetch_add(a->frames);
  const auto* p = reinterpret_cast<const float*>(a->data[0]);  // OBS の中は ch ごとの平面
  float pk = g_peak.load();
  for (std::uint32_t i = 0; i < a->frames; ++i) pk = std::max(pk, std::fabs(p[i]));
  g_peak.store(pk);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: vmx_obs_receiver_smoke <plugin.so>\n");
    return 2;
  }
  if (!obs_startup("en-US", nullptr, nullptr)) return 3;
  obs_audio_info ai{};
  ai.samples_per_sec = 48000;
  ai.speakers = SPEAKERS_STEREO;
  if (!obs_reset_audio(&ai)) return 4;
  obs_module_t* mod = nullptr;
  if (obs_open_module(&mod, argv[1], nullptr) != MODULE_SUCCESS || !obs_init_module(mod)) return 5;

  vmx::ipc::Format f;  // 48 kHz・2ch・64 フレーム
  vmx::ipc::StreamPublisher pub;
  if (!pub.start(f, 1, "smoke")) return 6;
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<float> buf(f.samplesPerBlock());
    const std::uint64_t t0 = vmx::ipc::nowNs();
    for (std::uint64_t n = 0; !stop.load(); ++n) {
      const std::uint64_t due = t0 + n * 64ull * 1'000'000'000ull / 48000ull;
      const std::uint64_t now = vmx::ipc::nowNs();
      if (now < due) std::this_thread::sleep_for(std::chrono::nanoseconds(due - now));
      for (std::size_t i = 0; i < 64; ++i)
        buf[2 * i] = buf[2 * i + 1] = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * double(n * 64 + i) / 48000.0));
      pub.writer().writeBlock(buf.data(), n * 64, vmx::ipc::nowNs(), 0);
      if (n % 75 == 0) pub.heartbeat();
    }
  });

  obs_source_t* src = obs_source_create("shobon_virtual_mixer_receiver", "smoke", nullptr, nullptr);
  if (src == nullptr) return 7;
  obs_source_add_audio_capture_callback(src, onAudio, nullptr);
  obs_set_output_source(0, src);  // 音声の出力に混ぜる（これが無いと音声の処理が回らない）
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const std::uint64_t frames = g_frames.load();
  const float peak = g_peak.load();

  obs_set_output_source(0, nullptr);
  obs_source_remove_audio_capture_callback(src, onAudio, nullptr);
  obs_source_release(src);
  stop.store(true);
  writer.join();
  pub.stop();
  obs_shutdown();
  std::printf("frames=%llu peak=%.3f\n", static_cast<unsigned long long>(frames), peak);
  // 3 秒のうち、つながるまでの 0.5 秒と OBS のバッファを除いても 1 秒ぶんは届くはず
  const bool ok = frames >= 48000 && peak > 0.2f && peak < 0.3f;
  std::puts(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
