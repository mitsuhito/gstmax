# gstmax

[English](README.md) | 日本語

GStreamer externals for Max/MSP.

ライセンスは MIT です。詳細は [LICENSE](LICENSE) を参照してください。依存ライブラリの扱いは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。

`gst.sink~` と `gst.src~` は、Max/MSP の signal と GStreamer パイプラインを `appsrc` / `appsink` 経由で接続する external です。

両オブジェクトとも内部で `audio/x-raw, format=F32LE, layout=interleaved` を使い、チャンネル数とサンプルレートは Max 側の DSP 設定に合わせます。

## 前提

- macOS
- CMake
- `pkg-config`
- Git
- Homebrew 版 GStreamer
- `max-sdk-base`

必要なパッケージ例:

```zsh
brew install cmake gstreamer gst-plugins-base ninja pkg-config
```

リポジトリの clone:

```zsh
git clone --recurse-submodules <repo-url>
cd gstmax
```

## セットアップ

submodule の初期化または更新:

```zsh
./scripts/setup_max_sdk.sh
```

デフォルトのビルド:

```zsh
./scripts/build.sh
```

生成物:

- `externals/gst.sink~.mxo`
- `externals/gst.src~.mxo`

既存の `max-sdk-base` checkout を使いたい場合は `MAX_SDK_BASE_DIR` を指定できます。

```zsh
MAX_SDK_BASE_DIR=/path/to/max-sdk-base ./scripts/build.sh
```

すでに clone 済みで submodule を後から取得したい場合:

```zsh
git submodule update --init --recursive
```

## パッケージ構成

- `source/max-sdk-base/`
- `source/projects/gst.sink~/`
- `source/projects/gst.src~/`
- `source/common/`
- `help/`
- `docs/`
- `init/`
- `package-info.json`

Xcode プロジェクトが必要な場合:

```zsh
./scripts/build_xcode.sh
```

Max から使うには、このリポジトリを `~/Documents/Max 9/Packages/` 以下へ置くか、`externals` ディレクトリを Max の File Preferences に追加してください。

`init/object-mappings.txt` には `mc.gst.sink~` / `mc.gst.src~` 用の wrapper 定義を入れています。

## CI

GitHub Actions では `.github/workflows/build.yml` で `macos-13` と `macos-14` のビルドチェックを行います。ローカルと同じく `./scripts/build.sh` を使うので、開発環境と CI の差分を小さく保てます。

## ライセンス

- `gstmax` のオリジナルコードは MIT ライセンスです。
- GStreamer は外部依存であり、別ライセンスで提供されます。
- GStreamer plugin は plugin ごとに実効ライセンスが異なる場合があります。
- prebuilt 配布で GStreamer や plugin を同梱する場合は、含める component ごとのライセンス確認が必要です。

## gst.sink~

`gst.sink~ <channels>`

- signal inlet 数はコンストラクタ引数で決まります。省略時は `2`
- `pipeline ...` で、内部 `appsrc` の後ろにつなぐ GStreamer パイプラインを設定します
- `start` でパイプライン開始
- `stop` で停止
- `clear` で送信待ちバッファを破棄

内部構成:

```text
appsrc ! queue ! audioconvert ! audioresample ! <your pipeline>
```

例:

```text
gst.sink~ 2
pipeline opusenc ! rtpopuspay pt=96 ! udpsink host=127.0.0.1 port=5004
start
```

## gst.src~

`gst.src~ <channels>`

- signal outlet 数はコンストラクタ引数で決まります。省略時は `2`
- `pipeline ...` で、内部 `appsink` の前段につなぐ GStreamer パイプラインを設定します
- `start` で受信開始
- `stop` で停止
- `clear` で受信済みバッファを破棄

内部構成:

```text
<your pipeline> ! audioconvert ! audioresample ! queue ! appsink
```

例:

```text
gst.src~ 2
pipeline udpsrc port=5004 caps="application/x-rtp,media=audio,encoding-name=OPUS,payload=96,clock-rate=48000" ! rtpopusdepay ! opusdec
start
```

## 注意

- この実装は Max の channel 数を固定で扱います。オブジェクト生成後に inlet / outlet 数は変更しません。
- `pkg-config` で検出した GStreamer のライブラリパスを `rpath` に設定しています。
- 使用する codec / network source に応じて `gst-plugins-good` や `gst-plugins-bad` など追加 plugin が必要です。
