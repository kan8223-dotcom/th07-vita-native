# 東方妖々夢 PS Vita ネイティブ移植（th07-vita-native）

PS Vita で東方妖々夢を遊ぶための非公式の移植です。
PSP 版（[th07-psp-native](https://github.com/kan8223-dotcom/th07-psp-native)）と同じ系統のゲーム本体を、Vita の CPU で直接動かし、Vita の GPU（GXM）で直接描いています。

**原作のデータは入っていません。** 遊ぶには、ご自身で持っている原作（Windows 版 東方妖々夢 1.00b）のファイルが必要です。

English: [README_EN.md](README_EN.md)

## 特徴

- **追加のランタイム不要。** OpenGL ES などの互換層を使わず、GXM を直接使っています。シェーダーはあらかじめコンパイルして VPK に入れてあるので、本体側に追加で入れるものはありません。
- **置くだけで起動。** 原作のフォルダの中身を決まった場所にコピーするだけです。名前の変更や変換はいりません。
- **フォント同梱。** Noto Sans JP（SIL OFL 1.1）を入れてあるので、フォントを用意しなくても遊べます。原作と同じ MS ゴシックにしたい場合だけ、任意で置けます。
- **MIDI（任意）。** Windows の gm.dls を置くと、原作の MIDI モードを Microsoft GS Wavetable Synth（MSGS）の音色で鳴らせます。MSGS の同時発音数（32＋8）では音が切れる場面があるため、96 声で鳴らしています。
- **画面いっぱい。** 640×480 の画面を 960×544 に引き伸ばして表示します。オプションの画面モードを Window にすると 4:3 で表示します。

## 必要なもの

- CFW（HENkaku / Ensō など）の入った PS Vita と VitaShell
- 原作: 東方妖々夢 1.00b（Windows 版）

## 導入

1. `th07-vita-native.vpk` を VitaShell でインストールします。
2. 原作のフォルダの中身を、`ux0:data/th07/` にそのままコピーします。

   | 必要なもの | |
   |---|---|
   | `th07.dat` | 必須 |
   | `thbgm.dat` | 必須 |
   | `th07.exe`（または `東方妖々夢.exe`） | 任意（アイコン用） |

3. 起動します。足りないファイルがあれば、その名前を画面に表示します（START で終了）。

exe を置いておくと、初回起動時にその exe からアイコン（32×32 を 4 倍、ぼかしなし）を取り出して、LiveArea のアイコンに書き込みます。exe はこのためだけに読みます。無くても遊べます（同梱の汎用アイコンのままになります）。

設定・スコア・リプレイは `ux0:data/th07/` に保存されます。

### 任意: MS ゴシック

Windows の `C:\Windows\Fonts\msgothic.ttc` を `ux0:data/th07/` にコピーすると、次の起動から原作と同じ字になります。無いとき、または読めないときは同梱の Noto Sans JP を使います。

### 任意: MIDI（MSGS の音色）

1. Windows の `C:\Windows\System32\drivers\gm.dls` を `ux0:data/th07/` にコピーします。
2. ゲームのオプションで BGM を「MIDI」にします。

gm.dls を置いた後の最初の起動で、妖々夢の曲で使う音色だけを gm.dls から取り出して `ux0:data/th07/midi-th07.tdsb` を作ります（画面に進み具合を表示します）。2 回目からはこれを読みます。gm.dls が無いときに MIDI を選ぶと、WAV で鳴ります。

`msgothic.ttc`・`gm.dls` と、そこから作られたファイル（`midi-th07.tdsb`）は個人用です。再配布しないでください。

## 操作（PSP 版と同じ）

| ボタン | 動作 |
|---|---|
| 方向キー / 左スティック | 移動 |
| × | ショット / 決定 |
| ○ | ボム / キャンセル |
| □ / L / R | 低速移動 |
| △ | 会話を飛ばす |
| START | ポーズ / メニュー |

## 実機での速さ

PS Vita（CPU 333 MHz / GPU 222 MHz。ゲームが起動時にこのクロックを設定します）で、Normal・霊夢 A・1〜6 面を通しで走らせた結果です（BGM は MIDI）。
数字は「1 フレームが 20 ms を超えた回数 / その面のフレーム数」です。面と面の間の読み込みは含みません（面が始まった直後のフレームは含みます）。

| 面 | 20 ms 超 / フレーム数 |
|---|---|
| 1 | 11 / 10,327 |
| 2 | 6 / 14,974 |
| 3 | 7 / 16,270 |
| 4 | 74 / 27,550 |
| 5 | 35 / 29,566 |
| 6 | 322 / 28,701 |

（2026-10-18 測定）

## 既存の Vita 移植との違い

妖々夢には [Crystal1612/th07_vita](https://github.com/Crystal1612/th07_vita) があります。どちらも some100 の th07 デコンパイル（portable branch）を元にしています。

| | この移植 | Crystal1612/th07_vita |
|---|---|---|
| 描画 | GXM を直接（シェーダーは同梱済み） | SDL2 と OpenGL ES（PIB） |
| 本体に入れるもの | VPK のみ（追加のランタイム不要） | PSM Runtime（CrystalPSM）と PIB-Configuration-Tool が必要 |
| フォント | Noto Sans JP を同梱（MS ゴシックは任意） | msgothic.ttc の用意が必要 |
| BGM | WAV、または gm.dls による MIDI | （README に記載なし） |
| 元にしたもの | PSP 版 th07-psp-native と同じ系統 | some100/th07 の Vita 用ブランチ |

## ビルド

[VitaSDK](https://vitasdk.org/)（SDL2・SDL2_image・SDL2_ttf・libvita2d・FreeType・libpng・libwebp・bzip2・zlib を含む標準のパッケージ一式）が必要です。

```sh
export VITASDK=/path/to/vitasdk
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build
```

`build/th07-vita-native.vpk` ができます。シェーダーは `third_party/touhou-modern-vita/shaders/` にソース（.cg）とコンパイル済み（.gxp）の両方があります（[SHADERS.md](third_party/touhou-modern-vita/SHADERS.md)）。

## クレジットとライセンス

- 本体: **CC0 1.0**（[LICENSE](LICENSE)）。元にした [some100/th07](https://github.com/some100/th07)（portable branch）と [th07-psp-native](https://github.com/kan8223-dotcom/th07-psp-native) に準じます。
- 経路: some100/th07（portable branch）→ [kan8223-dotcom/th07-psp-native](https://github.com/kan8223-dotcom/th07-psp-native) → この Vita 版。
- `third_party/touhou-modern-vita`（GXM 描画）と `third_party/touhou_dls_common`（gm.dls の MIDI 合成器）: MIT。touhou_dls_common は Microsoft のコードを含みません。利用者の gm.dls を読んで鳴らします。
- 同梱フォント Noto Sans JP: SIL Open Font License 1.1（[licenses/NotoSansJP/](licenses/NotoSansJP/)）。
- 冴風（Saekaze、旧 Swiizyu）さん — Switch ネイティブ移植（[saekaze/touhou-switch](https://github.com/saekaze/touhou-switch)）。この一連の移植のきっかけになりました。
- 静的リンクしているライブラリとそのライセンス: [THIRD_PARTY.md](THIRD_PARTY.md)。詳しいクレジット: [CREDITS.md](CREDITS.md)。

This software uses the FreeType Project font engine.

## 免責

- 東方妖々夢の著作権は上海アリス幻樂団にあります。この移植は非公式のもので、上海アリス幻樂団とは関係ありません。
- 原作のデータ・exe・音楽、MS ゴシック、gm.dls はこのリポジトリにも VPK にも含まれていません。
- 無保証です。上記の原作者・上流のプロジェクトへ、この移植についての問い合わせはしないでください。
