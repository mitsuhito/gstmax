# Third-Party Notices

This repository's original source code is licensed under the MIT License. Third-party dependencies are provided under their own licenses.

## Max SDK / max-sdk-base

- Project: Cycling '74 Max SDK / max-sdk-base
- Upstream: https://github.com/Cycling74/max-sdk
- Local use in this repository: build-time SDK dependency only
- License: MIT-style license distributed by Cycling '74 in `LICENSE.md`

Notes:

- This repository does not need to redistribute the SDK as part of the `gstmax` source tree.
- If you vendor or redistribute SDK files, include the original Cycling '74 license text with those files.

## GStreamer

- Project: GStreamer
- Upstream: https://gstreamer.freedesktop.org/
- Local use in this repository: external runtime and development dependency
- Core framework license: LGPL

Notes:

- GStreamer plugins may have different effective licenses depending on the plugin and the libraries they use.
- Before redistributing any bundled GStreamer runtime or plugins, review the license for each plugin you ship.
- You can inspect a plugin license locally with `gst-inspect-1.0 <plugin-name>`.

## Distribution Guidance

- The preferred distribution model for `gstmax` is to distribute the Max externals and require users to install GStreamer separately.
- If you later publish prebuilt packages that bundle additional third-party components, update this file with the exact components and licenses included in the package.
