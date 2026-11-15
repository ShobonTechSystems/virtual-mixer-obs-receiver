# virtual-mixer-obs-receiver

仮想ミキサー（Shobon Virtual Mixer）の本体が送る音を、OBS Studio の音声ソース「仮想ミキサーの音（試作）」として受け取るプラグインです。本体とは共有メモリでつながります。

An OBS Studio audio source that receives PCM streams from the Shobon Virtual Mixer over shared memory. **Prototype (P3-C).**

いまは試作の段階（本体の工程 P3-C）です。配布用ではありません。

## ライセンス

- このプラグイン：GPL-2.0-or-later（[LICENSE](LICENSE)）。OBS Studio の libobs にリンクするためです
- 受け渡しの約束（`shared/vmx-ipc/`）：MIT（[shared/vmx-ipc/LICENSE](shared/vmx-ipc/LICENSE)）。本体の repo から写したもので、ここでは直しません（[SOURCE.md](shared/vmx-ipc/SOURCE.md)）
- 仮想ミキサーの本体は、このプラグインとは別のプログラムで、共有メモリで音を渡すだけです

このプラグインのコードは、AI のコーディングツール（Claude Code）を使って書いています。

## 動き

- 専用のスレッドが 1 ms おきに起き、本体が書いたブロックをすべて OBS へ渡します
- OBS へ渡す時刻は、最初のブロックの時刻（QPC）に結び付けて、サンプルの位置から続けて作り、固定遅延を足します
- 本体の起動・停止・再起動は自動で追います（500 ms おきに開き直す）
- OBS のログに、つながった・止まった・戻ったときと、1 分おきの要約（受け取り、取りこぼし、間に合わなかった数、受け渡しの遅延、時計の差の ppm）を書きます

設定（ソースのプロパティ）：

| 項目 | 既定 | 意味 |
|---|---|---|
| ストリーム ID | 1 | 本体のどのストリームを受けるか |
| 固定遅延（ms） | 10 | OBS へ渡す時刻に足す遅れ。受け渡しの揺れを吸う。P3-C の測定で初期値を決める |

## 対象

- Windows 11 x64 の OBS Studio 32.0 以上（予定）。入っている版は、OBS の「ヘルプ」→「OBS Studio について」で確かめる。32.0 より古い OBS は、32.0.0 のヘッダーで作ったこのプラグインを読み込みません
- OBS の「セーフモード」では、他社のプラグインは読み込まれません

## ビルド（Windows）

Visual Studio（C++ のデスクトップ開発）、CMake、Git が要ります。「Developer PowerShell for VS」で、この repo の一番上から：

1. 対象の一番古い版の OBS のソースを取る（ヘッダーだけ使う。libobs は、自分より新しい版のヘッダーでビルドしたプラグインを読まないため）

   ```powershell
   git clone --depth 1 --branch 32.0.0 https://github.com/obsproject/obs-studio.git C:\vmx-obs-src
   ```

2. インストールされている OBS の `obs.dll` から、リンク用の `obs.lib` を作る（OBS を別の場所に入れたら `-ObsDll` で指定）

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\make-obs-lib.ps1 -OutDir C:\vmx-obs-lib
   ```

3. ビルドする（ビルドの置き場は、日本語を含まないパスにする）

   ```powershell
   cmake -S . -B C:\vmx-obs-build -A x64 -DOBS_SOURCE_DIR=C:\vmx-obs-src -DOBS_IMPORT_LIB=C:\vmx-obs-lib\obs.lib
   cmake --build C:\vmx-obs-build --config Release
   ```

   `C:\vmx-obs-build\Release\vmx-obs-receiver.dll` ができます。

## 入れる場所

OBS を閉じてから、DLL を次の場所に置きます（管理者の権限が要ります）。

```
C:\ProgramData\obs-studio\plugins\vmx-obs-receiver\bin\64bit\vmx-obs-receiver.dll
```

OBS を開いて、ソースの「＋」から「仮想ミキサーの音（試作）」を足します。消すときは `vmx-obs-receiver` のフォルダを消します。

DLL を置くのに管理者の窓を使ったら、その窓は閉じてください。仮想ミキサーの本体（試作の送り手）と OBS は、どちらも管理者でない普通の窓・普通の起動で動かします。一方だけが管理者だと、共有メモリを開けません（OBS のログに「権限が足りず開けません」と出ます）。

## ビルド（Linux、確認用）

Ubuntu では `libobs-dev` を入れて次のとおりにすると、プラグインのビルドと、libobs にプラグインを読み込ませて音が OBS まで届くかの試し（`vmx_obs_receiver_smoke`）ができます。試しには画面が要るので、`xvfb-run` で動かします。Linux の OBS は対象ではなく、この試しは Windows の OBS での確認の代わりになりません。

```sh
cmake -S . -B build && cmake --build build
xvfb-run -a ctest --test-dir build --output-on-failure
```
