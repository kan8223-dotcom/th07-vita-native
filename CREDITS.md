# Credits

## 原作

- 上海アリス幻樂団 / ZUN — 東方妖々夢 ～ Perfect Cherry Blossom。原作データはこのプロジェクトに含まれません。

## エンジンと移植の系譜

- [some100/th07](https://github.com/some100/th07) — 東方妖々夢 1.00b のデコンパイル。[portable branch](https://github.com/some100/th07/tree/portable) のコミット `7e2ccb4` を土台にしています。
- [GensokyoClub/th06](https://github.com/GensokyoClub/th06) — 共通するエンジン構造、型、命名、挙動の参照元。
- [GensokyoClub/th08](https://github.com/GensokyoClub/th08) — LZSS/PBG 系処理の参照元。
- [kan8223-dotcom/th07-psp-native](https://github.com/kan8223-dotcom/th07-psp-native) — 直接の元。プラットフォーム分離、原作データ分離、操作配置、文字描画、gm.dls の音色セットを作るコード（vita/MidiBuild.cpp）。
- [kan8223-dotcom/th06-vita-native](https://github.com/kan8223-dotcom/th06-vita-native) — 起動時の確認画面、exe からのアイコン、gm.dls の MIDI の Vita 側の作り。

## このプロジェクトのための部品（MIT, Copyright (c) 2026 kan82）

- `third_party/touhou-modern-vita` — GXM の描画（GxmRenderer / ModernGxmRenderer）と CPU コア割り当て（CoreRuntime）。
- `third_party/touhou_dls_common` — DLS の読み込み、音色サブセット、整数合成器、ZUN の MIDI シーケンサ。Microsoft のプログラムや音色データは含みません。gm.dls は利用者が用意します。

## ライブラリ・ツール

- [VitaSDK](https://vitasdk.org/) — ツールチェーン、newlib、pthread-embedded、各ライブラリのパッケージ。
- [libvita2d](https://github.com/xerpi/libvita2d)（Sergi Granell / xerpi）— GXM の初期化とテクスチャ管理、起動時の文字表示。
- SDL2 / SDL2_image / SDL2_ttf、FreeType、libpng、libwebp、zlib、bzip2 — 詳細とライセンスは [THIRD_PARTY.md](THIRD_PARTY.md)。
- [open-shacccg](https://github.com/frangarcj/open-shacccg)（MIT）— 同梱シェーダー（.gxp）のコンパイルに使用。実行ファイルにはリンクしていません。
- Noto Sans CJK JP（Adobe / Google、SIL OFL 1.1）— 同梱フォント。

## 謝辞

- 冴風（Saekaze、旧 Swiizyu）さん — 東方 Project の Switch ネイティブ移植（[saekaze/touhou-switch](https://github.com/saekaze/touhou-switch)）。この一連の移植のきっかけになりました。
- HENkaku / Ensō、VitaShell — Vita homebrew の基盤。
- 移植: kan82、OpenAI Codex、Anthropic Claude。

上記の原作者・上流・参照先は、この移植のサポート窓口ではありません。問い合わせはしないでください。
