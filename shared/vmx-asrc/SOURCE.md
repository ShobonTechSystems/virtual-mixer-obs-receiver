# shared/vmx-asrc

仮想ミキサーの本体（`shobonbondao/virtual-mixer`）の `libs/asrc/`（`include/vmx/asrc/` と `src/resampler.cpp`）を写したものです。時計のずれの吸収（ASRC）の部品で、ライセンスは MIT（[LICENSE](LICENSE)）。このフォルダだけはプラグイン（GPL-2.0-or-later）と別です。

- 写した元：`shobonbondao/virtual-mixer` のブランチ `claude/obs-asrc` の 910ebbb（試作。main に入ったら commit を書き直す）
- ここでは直さない。直すときは本体の側を直してから写し直す
- 方式と完了条件：本体の `docs/OBS_ASRC.md`
- 使う依存：libsamplerate 0.2.2（BSD-2-Clause、commit c96f5e3）。このプラグインの CMake が構成時に取る
