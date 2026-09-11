# Third-party notices

## Scope of the root license

The root `LICENSE` contains Apache License 2.0 for application files marked
`SPDX-License-Identifier: Apache-2.0`. It does **not** replace licenses attached
to retained Espressif board files or downloaded dependencies.

Existing source copyright and SPDX notices are preserved. The camera application
integrates changes for dual preview, display frame ownership, USB-source face
inference, the Camera Lab UI, local diagnostics and LCD scheduling. Files retained
under another license remain under that license.

## Board files included in this repository

The generated board definitions originate from **espressif/esp_board_manager
0.7.2**, and the Korvo board support originates from **espressif/esp_boards
0.6.1**, from [espressif/esp-board-manager](https://github.com/espressif/esp-board-manager)
at commit `2beb9b22b0892b343bd555a1ebc9929a7edce8fc`.

Files carrying `LicenseRef-Espressif-Modified-MIT` retain the **Espressif Modified
MIT License**, including its restriction to use with Espressif products and the
conditions on redistribution for non-Espressif products. Exact license texts are
included with the board component. Read them before reusing those files on other
hardware.

The custom `camera_sensor_bringup.c` is separately marked Apache-2.0. Its license
is not inferred from neighboring board files.

## Dependencies downloaded during a build

The source repository does not vendor the following libraries or face-model
binaries. The Component Manager resolves the pinned versions; downloaded packages
carry their own license texts. If redistributing a firmware binary or a copy of
the downloaded sources, include the applicable licenses and notices from **all**
resolved components, including transitive dependencies.

| Component | Pinned version | Packaged license |
| --- | --- | --- |
| `espressif/esp-dl` | 3.3.11 | MIT |
| `espressif/human_face_detect` | 0.4.2 | MIT |
| `espressif/human_face_recognition` | 0.3.2 | MIT |
| `espressif/esp_video` | 2.4.1 | ESPRESSIF MIT License, with an Espressif-products condition |
| `espressif/esp_cam_sensor` | 2.4.0 | Apache-2.0 |
| `espressif/esp_lvgl_port` | 2.9.0 | Apache-2.0 |
| `espressif/usb_host_uvc` | 2.5.2 | Apache-2.0 |
| `espressif/usb` | 1.5.0 | Apache-2.0 |
| `lvgl/lvgl` | 9.5.0 | MIT |
| `espressif/esp_board_manager` | 0.7.2 | Espressif Modified MIT |

Model package provenance: face detection and recognition packages are from
[espressif/esp-dl](https://github.com/espressif/esp-dl), commit
`165e966ea69410c332f9cafc452a15d3784436b0`, under `models/human_face_detect` and
`models/human_face_recognition`. ESP-DL 3.3.11 is from commit
`5d9c36063dddbe98b5387828c831d6bbadb1370f`, under `esp-dl`.

The package license review is not a separate audit of model training datasets.
No face-enrollment datasets or enrolled feature databases are included in this repository.
The hardware photograph in `docs/images/camera-lab-hardware.jpg` was supplied by
the repository maintainer.

## Project origin

This project began from the
[Espressif ESP32-S31 official example](https://esp32-s31.espressif.com/zh-hans/official-projects/5).
Third-party trademarks belong to their respective owners.
