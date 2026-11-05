# NR Nexus 离线验证工具

本目录提供通用 D3D12 E0 回放、既有 FG／XeFG suite adapter 与 ControlMask 专项验证，均无需启动游戏。Replay 构建不依赖 ControlMask Shader；GPU 专项后端单独编译当前源码中的 ControlMask Shader。

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

## ControlMask 专项验证

使用同一构建入口选择 `-Backend gpu`，指定已准备的固定宿主、未变 Shader 的生成头目录、FXC 和 embed_file。当前工作树产品头优先于宿主 addon 映射；当前 `control_mask.cs_5_1.hlsl` 在本次输出目录重新编译并生成嵌入头，不修改共享生成文件。

~~~powershell
pwsh -File tools/offline/build.ps1 -Backend gpu -HostRoot <固定宿主目录> -ShaderIncludeDirectory <已有生成头目录> -Fxc <fxc.exe> -EmbedFile <embed_file.exe> -OutputDirectory tmp/offline/control-mask-build
$python = & pwsh -File tools/offline/test.ps1 -ResolveOnly
& $python -B -S -m unittest discover -s tests -p 'test_control_mask_*.py' -v
& $python -B -S tools/offline/control_mask.py tests/fixtures/offline/control-mask-smoke.json --backend-exe tmp/offline/control-mask-build/offline_gpu_tests.exe --adapter warp --output tmp/offline/control-mask-warp
& $python -B -S tools/offline/control_mask.py --contracts --backend-exe tmp/offline/control-mask-build/offline_gpu_tests.exe --adapter warp --output tmp/offline/control-mask-contracts
& $python -B -S tools/offline/control_mask.py tests/fixtures/offline/control-mask-smoke.json --backend-exe tmp/offline/control-mask-build/offline_gpu_tests.exe --adapter nvidia --output tmp/offline/control-mask-nvidia
~~~

7-case smoke 覆盖中性、固定 RGB、矩形、G/B 通道和退化边界，独立 CPU oracle 检查原始 RGBA8 读回、FLOAT→UNORM 容差、提交完成和原始 timestamp。Shader 执行分别记录 WARP／NATIVE_GPU；模型 A/B、AutoMask、BlendMode 仅为 MOCK 参数合同。`--contracts` 仅接受 WARP，复用已有失败与资源退休测试，不提供 NVIDIA 对称入口。计时范围为 ControlMask dispatch 和 UAV barrier，不是 NR Evaluate 或游戏性能。

专项 CLI 复用通用输出路径、构建身份和设备核验，冻结专项及通用 Python 输入，并在执行后复核身份。每次使用新输出目录，保留 `result.json`、后端原始结果、读回与日志。专项 Python 文件命名为 `test_control_mask_*`，不加入通用 `test.ps1` 的 21 项回归。

生产失败恢复通过已有 `fg-native` 夹具执行：

~~~powershell
./tools/run-tests.ps1 -Suite fg-native -NativeCase @('control-mask-safety','control-mask-safety-hardware') -HostRoot <固定宿主目录> -ShaderIncludeDirectory <当前生成头目录> -UiPreview None -OutputDirectory tmp/tests/control-mask-safety
~~~

两个 case 分别使用 WARP／NVIDIA，复用同一编译目标和真实 command list／queue／Fence。日志中每项 `CASE` 记录故障位置、8×8 输入、资源保留／重建状态和恢复条件。覆盖 Root Signature／PSO／Descriptor／资源分配失败、Evaluate 替身失败、未提交录制的 Reset 丢弃、池换 key／跨 owner 驱逐、健康队列在飞连续帧、提交预约分配异常及 Signal 失败。三种提交故障还检查换 owner、输出轮换、选项关闭及 owner 释放／再注册后的资源数量，确认活跃或退休 ControlMask 资源仍存在时，未知提交会阻止继续分配；有效 Fence 正常在飞不触发该门禁。未知提交不以超时或测试独立 Fence 变成生产成功，pending／可重放录制保留到可信生命周期边界。`control-mask` 保留为 WARP 入口。

故障返回、NGX Runtime 和设备移除观察均为 MOCK；真实 GPU 证据只覆盖资源、Shader、提交与完成。移除观察在本次实际 GPU 工作独立完成后注入，验证旧设备拒绝处理、生产退休／计时消费者及独立新设备重建；不会主动移除真实设备。跨队列不在 P0 支持范围。Upscaled 的 NGX 入口只提供 command list，最终 queue 到提交拦截时才确定；同 device、同 queue type、Present queue 或上次提交均不能证明下一次提交身份。目前没有可靠的同队列录制门禁，新增的未知提交门禁也不承担此职责。解锁需要集成方在录制前提供并保证实际提交 queue 与 recording generation 的绑定，且覆盖重放／Reset；提交后发现不匹配不能撤销已录命令。真实驱动故障、原生 NGX、游戏接入和画质仍须分别验证。

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
