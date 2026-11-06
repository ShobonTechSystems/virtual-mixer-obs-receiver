# SPDX-License-Identifier: GPL-2.0-or-later
# インストールされている OBS の obs.dll から、リンク用の obs.lib を作る（README.md「ビルド（Windows）」）。
# 「Developer PowerShell for VS」で実行する（dumpbin と lib を使うため）。
#   powershell -ExecutionPolicy Bypass -File tools\make-obs-lib.ps1 [-ObsDll <obs.dll のパス>] [-OutDir obs-lib]
param(
  [string]$ObsDll = "C:\Program Files\obs-studio\bin\64bit\obs.dll",
  [string]$OutDir = "obs-lib"
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $ObsDll)) { throw "obs.dll が見つかりません: $ObsDll（-ObsDll で指定してください）" }
foreach ($tool in "dumpbin", "lib") {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool が見つかりません。Developer PowerShell for VS で実行してください" }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$names = New-Object System.Collections.Generic.List[string]
# dumpbin /exports の行：「ordinal hint RVA name」。名前だけを取る
foreach ($line in (& dumpbin /nologo /exports $ObsDll)) {
  if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $names.Add($Matches[1]) }
}
if ($names.Count -eq 0) { throw "obs.dll から公開名を読めませんでした" }
$def = Join-Path $OutDir "obs.def"
$lines = @("LIBRARY obs", "EXPORTS") + ($names | ForEach-Object { "    $_" })
Set-Content -Path $def -Value $lines -Encoding ASCII
$lib = Join-Path $OutDir "obs.lib"
# 引数は 1 つの文字列にして渡す（PowerShell が /out: と括弧の中を別の引数に分けないように、空白を含むパスにも備えて）
& lib /nologo "/def:$def" /machine:x64 "/out:$lib"
if ($LASTEXITCODE -ne 0) { throw "lib が失敗しました" }
Write-Host "作りました: $lib（公開名 $($names.Count) 個）"
