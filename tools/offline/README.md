# NR Nexus 离线验证工具

本整合分支移植了通用 D3D12 E0 回放与既有 FG／XeFG suite adapter。它们不启动游戏，也不编译 ControlMask Shader。ControlMask 专项验证和 NGX Feature 18 探针仍由原平台分支维护。

通用 D3D12 helper 还提供可复用的 timestamp query、resolve、readback 与毫秒换算；E0 当前只报告资源传输／读回和 Fence 结果，不把 shader-only 计时扩展为 NR Evaluate 性能。

固定宿主及 MSVC／Windows SDK 以 host/lock.json 和项目构建说明为准。此工作树可以显式使用已准备的宿主目录：

~~~powershell
pwsh -File tools/offline/build.ps1 -Backend replay -HostRoot <固定宿主目录> -OutputDirectory tmp/offline/replay-build
~~~

回放支持固定尺寸 sequence/1 和逐帧尺寸 sequence/2。先生成或校验合成 Color／Depth／Motion 序列，再显式选择 WARP 或 NVIDIA：

~~~powershell
python tools/offline/run.py prepare --output tmp/offline/sequence --width 64 --height 36 --frames 8 --seed 7
python tools/offline/run.py verify-sequence tmp/offline/sequence
python tools/offline/run.py replay tmp/offline/sequence --backend-exe tmp/offline/replay-build/offline_replay_tests.exe --adapter warp --timeout 60 --output tmp/offline/replay-warp
~~~

Runner 核对序列、构建、输入和原始 float32 读回身份，并要求每帧具有递增 Fence 完成证明。后续帧输入损坏时，已完成帧的读回／Fence 留存，失败帧标记未完成，整体结果失败。Reset、Jitter、Motion Scale 与 Subrect 只验证 STATIC 元数据；回放只执行真实 D3D12 upload／copy／readback，不调用 NGX，也不重建网络 History。WARP 与 NVIDIA 资源执行分别标记 WARP／NATIVE_GPU。

既有 FG／XeFG 合同通过 suite adapter 运行，不复制 GPU 调度实现：

~~~powershell
python tools/offline/suites.py tests/fixtures/offline/fg-contracts.json --output tmp/offline/fg-contracts-run
~~~

报告区分 STATIC、MOCK、WARP、NATIVE_GPU、NATIVE_NGX、IN_GAME 与 VISUAL；fake SDK 不视作真实 SDK 执行。输出目录必须是新目录，失败报告和原始证据保留在 tmp/offline/。
