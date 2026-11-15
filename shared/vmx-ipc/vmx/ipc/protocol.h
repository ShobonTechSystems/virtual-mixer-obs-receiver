/*
 * Shobon Virtual Mixer の共有メモリの約束（版1、提案）。docs/IPC_PROTOCOL.md §2〜§5。
 * C11 と C++ の両方から読める。OBS Receiver（GPL、別 repo）へはこのヘッダーを写して使う。
 *
 * 読み書きの順序（§4.1、§5）：共有メモリのすべての欄（サンプル・メタを含む）は原子的に読み書きする。
 * C の読み手は atomic_load_explicit(..., memory_order_acquire) で読むこと（版 → 中身 → 版の順）。
 * 普通の読み（memcpy など）では、上書きの検出が保証されない。atomic はロックなしであること。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Shobon Tech Systems
 */
#ifndef VMX_IPC_PROTOCOL_H
#define VMX_IPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "SVMX" をリトルエンディアンで読んだ値 */
#define VMX_IPC_MAGIC 0x584D5653u
#define VMX_IPC_VERSION 1u
#define VMX_IPC_MAX_READERS 8u
#define VMX_IPC_MAX_CHANNELS 2u

/* 名前（提案）。<session UUID> と <stream ID> は書き手が埋める */
#define VMX_IPC_PIPE_NAME "\\\\.\\pipe\\ShobonVirtualMixer.v1" /* 使わない案（P3-C で一覧に置き換え、docs/P3C_OBS_PCM.md §3） */
#define VMX_IPC_SHM_PREFIX "Local\\ShobonVirtualMixer.v1."
/* 名前の本体（OS の接頭辞なし）。Windows は "Local\\" を、POSIX は "/" を前に付ける */
#define VMX_IPC_NAME_BASE "ShobonVirtualMixer.v1"
#define VMX_IPC_DIRECTORY_SUFFIX ".directory"

/* ブロックのフラグ */
#define VMX_IPC_FLAG_DISCONTINUITY 0x1u /* 計画の差し替えでフェード、パニックミュート、主IF の再開 */

/* 先頭 64 バイト。atomic と書いた欄は、両側とも原子的に読み書きする */
typedef struct vmx_ipc_header {
  uint32_t magic;          /*  0 */
  uint32_t version;        /*  4 */
  uint8_t session_uuid[16];/*  8 */
  uint32_t stream_id;      /* 24 */
  uint32_t sample_rate;    /* 28 */
  uint32_t channels;       /* 32：1 か 2 */
  uint32_t block_frames;   /* 36：1ブロックのフレーム数 */
  uint32_t ring_blocks;    /* 40：リングのブロック数 */
  uint32_t epoch;          /* 44：atomic。0 は準備中 */
  uint64_t blocks_written; /* 48：atomic。書き終えたブロック数（増え続ける） */
  uint8_t reserved[8];     /* 56 */
} vmx_ipc_header;

/* ブロックごとのメタ（40 バイト）。slot_seq は §4.1 の版：
 * 連番 n を書いている間は 2n+1、書き終えたら 2n+2。0 は一度も書いていない */
typedef struct vmx_ipc_block_meta {
  uint64_t slot_seq;      /* atomic */
  uint64_t sample_pos;    /* 主クロックのサンプル位置 */
  uint64_t qpc_ns;        /* QPC の時刻（ns） */
  uint64_t sequence;      /* 連番 n */
  uint32_t flags;         /* VMX_IPC_FLAG_* */
  uint32_t nan_count;     /* 書く前に 0 にした NaN・Inf の数 */
} vmx_ipc_block_meta;

/* 読み手のスロット（32 バイト）。読み手は自分のスロットだけに書く。書き手は読まない（表示・診断用） */
typedef struct vmx_ipc_reader_slot {
  uint32_t pid;           /* atomic。0 は空き。CAS で取る */
  uint32_t reserved;
  uint64_t last_sequence; /* atomic。最後に読んだ連番 */
  uint64_t dropped;       /* atomic。取りこぼし数 */
  uint64_t last_alive_ns; /* atomic。最後の生存時刻 */
} vmx_ipc_reader_slot;

/* ストリームの一覧（P3-C、docs/P3C_OBS_PCM.md §3）。決まった名前の共有メモリに1つ。
 * 書き手（本体）は音声スレッドの外で書く。dir_seq は版：書いている間は奇数、書き終えたら偶数。
 * heartbeat_ns 以外の欄は 4 バイトずつ原子的に読み書きする。読み手は dir_seq（acquire）→ 中身 → dir_seq の順に読み、
 * 前後が同じ偶数のときだけ使う */
#define VMX_IPC_DIR_MAGIC 0x444D5653u /* "SVMD" */
#define VMX_IPC_DIR_VERSION 1u
#define VMX_IPC_DIR_MAX_STREAMS 16u
#define VMX_IPC_DIR_NAME_BYTES 56u
#define VMX_IPC_DIR_STATE_STOPPED 0u
#define VMX_IPC_DIR_STATE_RUNNING 1u

typedef struct vmx_ipc_dir_stream {
  uint32_t stream_id;
  uint32_t channels;
  char name[VMX_IPC_DIR_NAME_BYTES]; /* UTF-8、NUL で終わる */
} vmx_ipc_dir_stream;

typedef struct vmx_ipc_directory {
  uint32_t magic;           /*  0 */
  uint32_t version;         /*  4 */
  uint32_t dir_seq;         /*  8：atomic */
  uint32_t writer_pid;      /* 12 */
  uint8_t session_uuid[16]; /* 16 */
  uint32_t state;           /* 32：VMX_IPC_DIR_STATE_* */
  uint32_t stream_count;    /* 36 */
  uint64_t heartbeat_ns;    /* 40：atomic（64 ビット）。書き手の生存の時刻（QPC、ns）。dir_seq の外 */
  uint8_t reserved[16];     /* 48 */
  vmx_ipc_dir_stream streams[VMX_IPC_DIR_MAX_STREAMS]; /* 64 */
} vmx_ipc_directory;

#ifdef __cplusplus
}
#endif

/* 位置の検査（C と C++ の両方） */
#ifdef __cplusplus
#define VMX_IPC_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define VMX_IPC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
VMX_IPC_STATIC_ASSERT(sizeof(vmx_ipc_header) == 64, "header must be 64 bytes");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_header, stream_id) == 24, "stream_id offset");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_header, epoch) == 44, "epoch offset");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_header, blocks_written) == 48, "blocks_written offset");
VMX_IPC_STATIC_ASSERT(sizeof(vmx_ipc_block_meta) == 40, "block meta must be 40 bytes");
VMX_IPC_STATIC_ASSERT(sizeof(vmx_ipc_reader_slot) == 32, "reader slot must be 32 bytes");
VMX_IPC_STATIC_ASSERT(sizeof(vmx_ipc_dir_stream) == 64, "directory stream entry must be 64 bytes");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_directory, dir_seq) == 8, "dir_seq offset");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_directory, heartbeat_ns) == 40, "heartbeat offset");
VMX_IPC_STATIC_ASSERT(offsetof(vmx_ipc_directory, streams) == 64, "streams offset");
VMX_IPC_STATIC_ASSERT(sizeof(vmx_ipc_directory) == 64 + 64 * VMX_IPC_DIR_MAX_STREAMS, "directory size");

/* 並び：header(64) | reader_slot[VMX_IPC_MAX_READERS](256) | meta[ring_blocks] | float32 samples（8 バイトに揃える）
 * 読み手スロットは形式（SR・ch・ブロック長・リング長）に関係なく同じ位置に置く。形式が変わったあとに古い形式の
 * 読み手がスロットへ書いても、音声やメタを壊さないため */
static inline uint64_t vmx_ipc_readers_offset(void) { return 64u; }
static inline uint64_t vmx_ipc_meta_offset(void) { return 64u + (uint64_t)VMX_IPC_MAX_READERS * 32u; }
static inline uint64_t vmx_ipc_samples_offset(uint32_t ring_blocks) {
  return vmx_ipc_meta_offset() + (uint64_t)ring_blocks * 40u;
}
static inline uint64_t vmx_ipc_samples_bytes(uint32_t ring_blocks, uint32_t block_frames, uint32_t channels) {
  uint64_t b = (uint64_t)ring_blocks * block_frames * channels * 4u;
  return (b + 7u) & ~(uint64_t)7u;
}
static inline uint64_t vmx_ipc_total_bytes(uint32_t ring_blocks, uint32_t block_frames, uint32_t channels) {
  return vmx_ipc_samples_offset(ring_blocks) + vmx_ipc_samples_bytes(ring_blocks, block_frames, channels);
}

#endif /* VMX_IPC_PROTOCOL_H */
