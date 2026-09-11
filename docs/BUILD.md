# 构建与板级配置

这是独立的 `project(camera_lab)` ESP-IDF 项目。源码列表只包含相机启动、UI、USB 摄像头、板载摄像头与人脸服务；没有音乐生态缸、BT 音频、GMF 或 HID 应用依赖。

## 已使用的工具链

ESP32-S31 需要包含该芯片支持的 ESP-IDF，不能直接假定任意稳定版支持它。原始硬件验证使用：

- ESP-IDF `6.2-dev`，提交 [`c712a0dde385d659a1470a136251980d31a70bc1`](https://github.com/espressif/esp-idf/commit/c712a0dde385d659a1470a136251980d31a70bc1)。
- GCC 16 的 Espressif RISC-V 工具链。
- Python 3.11、CMake 4.0.3、Ninja 1.12.1，Windows PowerShell 构建。

ESP-IDF 的来源是 [espressif/esp-idf](https://github.com/espressif/esp-idf)。SDK 与编译器不随本仓库分发；请按相应 SDK 版本的安装说明准备并激活环境。Python 必须来自该 ESP-IDF 环境。Linux/macOS 下的全新环境安装尚未验证。

## 编译

在已经激活 ESP-IDF 的终端，进入仓库根目录：

```sh
python tools/build.py --jobs 8
```

脚本调用 PATH 中的 CMake/Ninja，使用当前 `IDF_PATH` 和 Python。它只配置与编译，不下载或安装 SDK，不清空任何已有目录，也不烧录硬件。组件管理器会根据 manifests 和锁文件解析组件；首次构建需要网络或事先完整的组件缓存。

可用 `--cmake`、`--ninja` 显式指定工具可执行文件。Windows 启动器若同时传入 `PATH` 和 `Path`，脚本会保留两个条目中的工具路径并为子进程合并成一个 `PATH`，避免丢失已经激活的编译器环境。

构建产物是 `build/camera_lab.bin` 和 `build/camera_lab.elf`。可通过 `--build-dir build-alt` 创建另一套构建目录，保留当前产物。常规 ESP-IDF 环境也可以执行 `idf.py -DIDF_TARGET=esp32s31 build`；项目中已带生成的板级配置，无需再运行 board-manager 代码生成器。

## 组件版本

主要依赖固定到原始硬件验证使用的版本；完整传递依赖由 `dependencies.lock` 记录。

| 组件 | 版本 |
| --- | --- |
| esp_board_manager / esp_boards | 0.7.2 / 0.6.1 |
| esp_lvgl_port / LVGL | 2.9.0 / 9.5.0 |
| USB Host / UVC | 1.5.0 / 2.5.2 |
| esp_video / esp_cam_sensor | 2.4.1 / 2.4.0 |
| ESP-DL | 3.3.11 |
| human_face_detect / human_face_recognition | 0.4.2 / 0.3.2 |
| esp_lcd_touch_gt1151 | 1.1.1 |

导出时已将上述现用组件与组件缓存中的 registry `CHECKSUMS.json` 逐项核对。没有需要额外分发的 ESP-DL、UVC、LVGL 或摄像头组件补丁。Board Manager 的生成 Kconfig 只有换行格式差异。ESP-DL 对 ESP32-S31 的模型选择与 PIE 指令实现来自该版本上游组件。

GCC 16 对 ESP-DL 3.3.11 的匿名 typedef 新增一个 C++ 诊断；根 CMake 仅将 `non-c-typedef-for-linkage` 从错误降为警告，其他编译错误检查照常保留。

## 随仓库保留的板级代码

`components/gen_bmgr_codes` 保存了 ESP32-S31-Korvo-1 的生成配置、Kconfig、板级 YAML 与两个板级 C 文件。来源为 Espressif `esp_boards 0.6.1` / `esp_board_manager 0.7.2`，上游提交 `2beb9b22b0892b343bd555a1ebc9929a7edce8fc`。原始版权标记和 [该目录的 LICENSE](../components/gen_bmgr_codes/LICENSE) 一并保留。

生成的配置表仍描述整块开发板，因此按键、编解码器、SD 等板级支持依赖仍会被解析。应用只按需启动 LCD、触摸和摄像头，不启动蓝牙、音频播放或 SD 文件系统。保留表结构避免在整理发布文件时改变已验证的硬件初始化关系。

导出的 CMake 只使用仓库内相对路径。`board_manager.defaults` 中删除了原开发机路径，并将 Flash 模式设置为实际验证使用的 DIO；板级目录里的 `sdkconfig.defaults.board` 保留上游参考原文，构建不直接加载它。LCD 的双帧缓冲与 20 行内部 bounce buffer、USB 图像缓冲租约、DVP 翻转与 Core 1 人脸推理仍由应用源代码控制。

## Flash 布局

`partitions.csv` 使用 16 MiB Flash，分区表偏移 `0x8000`，应用从 `0x10000` 开始，容量 8 MiB。原官方资源区域 `coffee_pjpg` 保留为未使用区；仓库不包含该资源内容。

原硬件验证只更新了兼容分区布局上的应用范围。准备给另一块板烧录前，需要先确认它的芯片、Flash 容量与分区表；单独写入 `camera_lab.bin` 不会安装匹配的分区表或 bootloader。本项目没有默认一键整片擦除或烧录脚本。

### 已有匹配分区布局：只更新应用

确认开发板的现有 bootloader 支持本应用、分区表与本仓库一致，且需要保留的旧应用已备份后，在仓库根目录运行：

```sh
python -m esptool --chip esp32s31 --port PORT write-flash 0x10000 build/camera_lab.bin
python -m esptool --chip esp32s31 --port PORT verify-flash 0x10000 build/camera_lab.bin
```

`PORT` 替换为当前板卡串口。上述命令只指定应用文件，不使用整片擦除；第二条核对 Flash 中的应用与本次构建文件。现有 NVS、PHY 和资源区不属于这次写入范围。烧录后可用 `idf.py -p PORT monitor` 查看启动日志。

### 首次部署或分区不一致

这类板卡需要安装与本次构建相匹配的 bootloader 和分区表，不能只执行上面的应用命令。先备份原有内容并核对硬件，按 [ESP-IDF ESP32-S31 入门说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/)部署本项目生成的完整构建产物。标准 `idf.py -p PORT flash` 会写入构建清单中的 bootloader、分区表与应用，它与只更新应用的范围不同。

当前实机记录没有覆盖空白板首次部署。仓库未附带官方示例的 `coffee_pjpg` 资源镜像，本应用也不依赖该内容；不应将其他项目的整片镜像与本项目应用随意混用。
