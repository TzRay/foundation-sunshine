# Fork 本地补丁

本分支以上游 `AlkaidLab/foundation-sunshine` 的功能实现为主体。

## 12 声道 PCM

- 保留 `PCM_S16` 最多 12 声道的准入，以及 48 kHz、16-bit、5 ms 帧对应的 5760 字节发送容量。
- 这是配套客户端扩展，不是所有 Moonlight 客户端通用的功能。客户端的 UDP 接收、排队和解密缓冲需要容纳完整数据包（建议至少 8 KB），并正确处理 12 声道映射和播放。
- 上游公共客户端库的 4 KB 接收容量不能承载该模式。未新增能力协商，不能依靠服务端自动识别旧客户端。
- AC3/E-AC3 的声道限制保持上游行为；PCM 扩展不代表 Dolby Atmos 对象音频透传。

## 构建与分发

- 主流水线使用 `.node-version` 和锁文件，构建 .NET 10 DualSense 组件，执行 Web UI、原生和 VDD 检查。
- 主流水线只上传 Actions 产物，不创建 Release，也不依赖上游机器人令牌或特定合并策略。
- CI 仍编译本次源码中的 Sidecar，以验证 .NET 工程；安装包则固定使用与上游基线 `645465c5` 对应的官方 `v2026.911.85650.杂鱼` 组件、文件大小和 SHA-256。
- RTX HDR 可选 SDK 沿用上游的仓库权限判断；当前 Fork 构建关闭该组件，普通 SDR/HDR 编码不受影响。
- 控制面板当前只信任 `AlkaidLab/foundation-sunshine` 的组件下载路径，因此不能将清单直接改为 Fork 地址。升级上游基线时必须同步核验并更新 `write-pinned-ds5-manifest.ps1`。
- 缺少 `DRIVER_DOWNLOAD_TOKEN` 时仍可构建，但缺失的私有驱动不会包含在安装包中。
- `Compile Local Windows Build` 仅生成替换文件，不是完整安装包，也不负责部署新驱动或 DualSense 运行时。完整升级使用主流水线安装包。
- 本地无标签运行 `build-ds5-sidecar.ps1` 时生成空下载地址；正式安装包由 `write-pinned-ds5-manifest.ps1` 写入经过核验的官方固定清单。

## 更新策略

NVENC/NVAPI 及 NVENC 各 SDK 头文件固定到上游版本，不再用 SDK 13 头文件替代 SDK 11/12，也不保留旧的枚举规避补丁。音频麦克风包类型采用上游公共库定义。

合并后检查 PCM 准入和发送容量，并运行 `scripts/test-fork-packaging.ps1`。真实 12 声道播放、HDR、手柄和安装升级仍需在配套硬件上验收。
