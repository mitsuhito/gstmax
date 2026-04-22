# gstmax

English | [日本語](README.ja.md)

GStreamer externals for Max/MSP.

License: MIT. See [LICENSE](LICENSE). Third-party dependency notes are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

`gst.sink~` and `gst.src~` connect Max/MSP signals to GStreamer pipelines via `appsrc` and `appsink`.

Both objects use `audio/x-raw, format=F32LE, layout=interleaved` internally, with channel count and sample rate matched to Max DSP settings.

## Requirements

- macOS
- CMake
- `pkg-config`
- Git
- Homebrew GStreamer
- `max-sdk-base`

Example packages:

```zsh
brew install cmake gstreamer gst-plugins-base ninja pkg-config
```

Clone the repository:

```zsh
git clone --recurse-submodules <repo-url>
cd gstmax
```

## Setup

Initialize or update the submodule:

```zsh
./scripts/setup_max_sdk.sh
```

Build the externals:

```zsh
./scripts/build.sh
```

Outputs:

- `externals/gst.sink~.mxo`
- `externals/gst.src~.mxo`

If you already have a local `max-sdk-base` checkout, you can override the path:

```zsh
MAX_SDK_BASE_DIR=/path/to/max-sdk-base ./scripts/build.sh
```

If you cloned the repository without submodules:

```zsh
git submodule update --init --recursive
```

## Package Layout

- `source/max-sdk-base/`
- `source/projects/gst.sink~/`
- `source/projects/gst.src~/`
- `source/common/`
- `help/`
- `docs/`
- `init/`
- `package-info.json`

If you need an Xcode project:

```zsh
./scripts/build_xcode.sh
```

To use the externals in Max, place this repository under `~/Documents/Max 9/Packages/` or add the `externals` directory to Max File Preferences.

`init/object-mappings.txt` contains wrapper definitions for `mc.gst.sink~` and `mc.gst.src~`.

## CI

GitHub Actions runs build checks on `macos-13` and `macos-14` with `.github/workflows/build.yml`. It uses the same `./scripts/build.sh` entry point as local development.

## License Notes

- The original `gstmax` source code is licensed under MIT.
- GStreamer is an external dependency and is provided under separate licenses.
- GStreamer plugins may have different effective licenses depending on the plugin.
- If you distribute prebuilt packages that bundle GStreamer or plugins, review the license of each bundled component.

## gst.sink~

`gst.sink~ <channels>`

- The number of signal inlets is set by the constructor argument. Default: `2`
- Use `pipeline ...` to set the downstream GStreamer pipeline after the internal `appsrc`
- `start` starts the pipeline
- `stop` stops the pipeline
- `clear` discards buffered outgoing audio

Internal graph:

```text
appsrc ! queue ! audioconvert ! audioresample ! <your pipeline>
```

Example:

```text
gst.sink~ 2
pipeline opusenc ! rtpopuspay pt=96 ! udpsink host=127.0.0.1 port=5004
start
```

## gst.src~

`gst.src~ <channels>`

- The number of signal outlets is set by the constructor argument. Default: `2`
- Use `pipeline ...` to set the upstream GStreamer pipeline before the internal `appsink`
- `start` starts receiving
- `stop` stops receiving
- `clear` discards buffered incoming audio

Internal graph:

```text
<your pipeline> ! audioconvert ! audioresample ! queue ! appsink
```

Example:

```text
gst.src~ 2
pipeline udpsrc port=5004 caps="application/x-rtp,media=audio,encoding-name=OPUS,payload=96,clock-rate=48000" ! rtpopusdepay ! opusdec
start
```

## Notes

- Channel count is fixed when the object is created. Inlet and outlet counts are not changed after instantiation.
- Library search paths for GStreamer are set from `pkg-config`.
- Additional plugins such as `gst-plugins-good` or `gst-plugins-bad` may be required depending on your codecs and sources.
