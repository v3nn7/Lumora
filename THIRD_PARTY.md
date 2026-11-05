# Third-party software

Lumora is licensed under GPL-3.0-or-later. It builds on the following components, all under
licences compatible with it. Versions are pinned (and hash-verified) in `cmake/` and
`application/launcher/CMakeLists.txt`.

| Component | Use | Licence |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Network, Svg, Wayland client) | application UI; shipped as separate shared libraries | LGPL-3.0 |
| GLM | math | MIT |
| EnTT | entity-component system | MIT |
| Vulkan Memory Allocator | GPU memory | MIT |
| nlohmann/json | JSON | MIT |
| cgltf | glTF import | MIT |
| stb_image / stb_image_write | image decoding | MIT or public domain |
| Jolt Physics | rigid-body physics | MIT |
| miniz | ZIP extraction (editor installer) | MIT |
| Monocypher | Ed25519 signature checks (editor installer), XChaCha20-Poly1305 (session datagrams) | BSD-2-Clause or CC0 |
| micro-ecc | ECDSA P-256 for the session host's TLS certificate | BSD-2-Clause |
| wayland-protocols (pointer-constraints, relative-pointer XML) | Wayland pointer lock | MIT |
| GoogleTest | tests only, not shipped | BSD-3-Clause |

Packages ship Qt's libraries unmodified as separate shared libraries (`lib/libQt6*.so*`,
`Qt6*.dll`). You may replace them with compatible builds of the same Qt version. Qt's source
code is available from <https://download.qt.io/official_releases/qt/>.
