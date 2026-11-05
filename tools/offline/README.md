# NR Nexus 离线验证工具

本目录提供通用 D3D12 E0 回放与既有 FG／XeFG suite adapter。它们不启动游戏，也不编译 ControlMask Shader。ControlMask 专项验证和 NGX Feature 18 探针仍由原平台分支维护。

通用 D3D12 helper 还提供可复用的 timestamp query、resolve、readback 与毫秒换算；E0 当前只报告资源传输／读回和 Fence 结果，不把 shader-only 计时扩展为 NR Evaluate 性能。

固定宿主及 MSVC／Windows SDK 以 host/lock.json 和项目构建说明为准。此工作树可以显式使用已准备的宿主目录：

~~~powershell
pwsh -File tools/offline/build.ps1 -Backend replay -HostRoot <固定宿主目录> -OutputDirectory tmp/offline/replay-build
~~~

回放支持固定尺寸 sequence/1 和逐帧尺寸 sequence/2。先生成或校验合成 Color／Depth／Motion 序列，再显式选择 WARP 或 NVIDIA：

~~~powershell
$python = & pwsh -File tools/offline/test.ps1 -ResolveOnly
& $python -S tools/offline/run.py prepare --output tmp/offline/sequence --width 64 --height 36 --frames 8 --seed 7
& $python -S tools/offline/run.py verify-sequence tmp/offline/sequence/inputs
& $python -S tools/offline/run.py replay --sequence tmp/offline/sequence/inputs --backend-exe tmp/offline/replay-build/offline_replay_tests.exe --adapter warp --timeout 60 --output tmp/offline/replay-warp
~~~

Runner 核对序列、构建、输入和原始 float32 读回身份，并要求每帧具有递增 Fence 完成证明。后续帧输入损坏时，已完成帧的读回／Fence 留存，失败帧标记未完成，整体结果失败。Reset、Jitter、Motion Scale 与 Subrect 只验证 STATIC 元数据；回放只执行真实 D3D12 upload／copy／readback，不调用 NGX，也不重建网络 History。WARP 与 NVIDIA 资源执行分别标记 WARP／NATIVE_GPU。

既有 FG／XeFG 合同通过 suite adapter 运行，不复制 GPU 调度实现：

~~~powershell
$python = & pwsh -File tools/offline/test.ps1 -ResolveOnly
& $python -S tools/offline/suites.py tests/fixtures/offline/fg-contracts.json --output tmp/offline/fg-contracts-run
~~~

报告区分 STATIC、MOCK、WARP、NATIVE_GPU、NATIVE_NGX、IN_GAME 与 VISUAL；fake SDK 不视作真实 SDK 执行。输出目录必须是新目录，失败报告和原始证据保留在 tmp/offline/。

## 自动回归

使用固定版本的 64 位 CPython 3.12.14；测试脚本验证解释器实际版本和架构，显式使用绝对路径，不依赖当前 shell 的 PATH。发现顺序为显式 -PythonPath／NR_NEXUS_PYTHON、Worktree .venv、用户 uv 托管安装和 Python 3.12 用户安装路径。测试只用 Python 标准库。运行 all 或 backend 测试集必须提供已构建 Replay EXE，防止真实后端测试被静默跳过。

~~~powershell
pwsh -File tools/offline/test.ps1 -ReplayBackendExe tmp/offline/replay-build/offline_replay_tests.exe -Adapter warp
pwsh -File tools/offline/test.ps1 -TestSet all -ReplayBackendExe tmp/offline/replay-build/offline_replay_tests.exe -Adapter nvidia
~~~

TestSet 可选 all、backend、contracts 或 suites。all 运行全部 offline Python 测试；backend、contracts、suites 分别运行对应文件。`-ResolveOnly` 只输出同一发现逻辑核验过的 CPython 绝对路径，供 run.py／suites.py 等命令复用。解释器可由 PythonPath 参数或 NR_NEXUS_PYTHON 指定；uv 可由 NR_NEXUS_UV 指定。脚本不自动下载、安装或修改系统环境。

Windows CI 通过 `tools/offline/setup-python.ps1` 准备独立的 CPython 3.12.14 x64。归档来自 [Astral python-build-standalone 20260929](https://github.com/astral-sh/python-build-standalone/releases/tag/20260929)，URL 与 SHA-256 固定在 `host/tools.lock.json`；解压前核对归档哈希，之后复用 `test.ps1` 核验解释器版本和架构，不依赖 WindowsApps 或 PATH。脚本拒绝覆盖已有目录；本地可传 `-PythonRoot <新目录>`，标准输出为经过核验的解释器绝对路径。

同一 Windows job 复用 Release 构建准备的固定宿主，通过 `build.ps1 -Backend replay` 构建真实 E0 EXE，再以 `test.ps1 -TestSet all -Adapter warp` 运行 21 项测试：7 项通用契约、7 项 suite adapter 合成合同和 7 项真实 Replay 后端回归。真实回放包括奇数行跨度、单像素宽度、动态 Resize／Subrect、失败帧保留及输出／路径保护。已有 FG／XeFG／生命周期执行仍由原 `tools/run-tests.ps1 -Suite all -SoftwareOnly` 覆盖，不再通过 suite adapter 重跑。缺少解释器或 Replay EXE 会失败；NVIDIA 硬件验证保留为本地入口。

每次 CI 的 `offline-warp-<attempt>` artifact 在成功或失败时保留 `tmp/offline/` 下的编译日志、输入身份、Python 测试日志和原始回放结果，保存 14 天；解释器准备失败时可直接查看该步骤日志。该 artifact 仅供工程验证，不进入插件安装包。
