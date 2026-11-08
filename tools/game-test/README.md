# 巫师3本地测试入口

这里只迁入既有 Steam／巫师3 自动化：原存档保护、唯一时间戳选档、焦点核验、所属进程监控、菜单退出、恢复与有界重试。没有新增游戏适配，不包含悟空无人选档。

自动 menu／smoke 在首次 OCR／输入及采集边界自动取得本次游戏前台，不需要人工截图确认或交接令牌。每次核对 HWND、PID、进程创建时间和路径；已在前台立即继续，后台启动或最小化时自动恢复窗口。激活失败／身份变化时阻断输入并进入受保护恢复。AllowSetForegroundWindow 的返回只作诊断，真实前台核验通过才可输入。Smoke 以原始存档哈希、唯一 UI 时间戳和选中行确定目标存档；加载后确认读档菜单退出，再用现有画面健康检查确认可采集，不用场景内容 OCR 二次鉴定。

两个 PowerShell 入口接受 `-CaptureScript <本仓库 tools/ 或 tmp/ 下的 .py>`，同时指定非零 `-CaptureHoldSeconds`。入口在安装前校验参数，普通桌面上下文重启保留该绝对路径。runner 预检记录脚本 SHA256，在 `CAPTURE_READY` 后复核并仅启动一次，调用形式为同一 Python 的 `-B <脚本> <本次运行目录>`，输出写 `automatic-capture.log`。该选项不能与 `ManualImeSession` 或 `InstallOnly` 搭配。

采集器读取 `capture-ready.json` 的 `automatic_foreground=true` 和本次 `owned-process.json`，核对唯一进程／窗口身份并自动激活。动态采样与转镜期间持续核验前台和几何，失焦即停止，不在采样中反复激活。收到 `capture-cancel.json` 应停止输入并结束自己的采样子进程；异常写 `capture-failed.json`，成功收尾才写匹配 PID 的 `capture-complete.json`。使用 CaptureScript 时还必须由父进程观察到脚本零码退出并写 `capture-script-result.json`，worker 才恢复菜单退出输入。提前完成、失败／损坏凭据或超时不能释放门禁。恢复前先结束采集器和 worker，文件保护保持到本次游戏退出并恢复完成。

回归覆盖后台激活、最小化恢复、身份变化拒绝、失败门禁、脚本单次启动、完成与退出顺序及保护恢复。单测属于 MOCK／本地文件合同；独立 Windows 窗口探针验证前台 API 路径，IN_GAME／VISUAL 和长时稳定性另行验收。

选档前在已知存档菜单内用不带按钮的移动事件将光标停在窗口右下角，避免游戏光标遮挡日期；仍核验本次身份与前台，多显示器坐标基于虚拟桌面计算。仅SetCursorPos在实机未改变游戏渲染光标，SendInput版已完成指定场景的严格选档、采集边界和正常退出。选档优先点击唯一可见的目标时间戳，随后重新确认选中行；点击直接触发加载时，连续两次确认读档列表消失后进入加载流程。原始文件唯一时间戳、SHA-256和UI选中行绑定目标存档；不再要求识别任务文字或场景语义。Smoke 在采集前运行黑屏、进程响应和画面变化检查。

## 本地配置与依赖

将 `target.example.json` 复制到仓库根 `.local/game-test/witcher3-steam.json`，填入原有的 `default_save`、`expected_hdr_monitor` 和 `performance_target_fps`。该目录被 Git 忽略；不要提交实际存档标识、显示器身份或运行凭据。已有本地配置无需重新生成。

`--target`／`-TargetPath` 指定该文件。适配器仍限于巫师3 Steam；换配置不表示支持其他游戏。`--output-root`／`-OutputRoot` 只接受本仓库 `tmp/` 或 `artifacts/` 下的目录；工作区互斥锁不随输出目录改变。

多显示环境可同时配置 `expected_hdr_adapter_luid`（查询返回的16位十六进制字符串）和 `expected_hdr_target_id`（JSON uint32 整数）。入口只接受唯一匹配该身份的 active 对象，并继续核对 `expected_hdr_monitor` 精确名称与 HDR enabled；不按同名对象的顺序猜测。仅配置一个字段、身份缺失／不匹配、重复身份或 HDR 未启用均在安装前拒绝。两字段省略或均为 null 时，保留原有“恰好一个 active 输出”的检查。先只读确认物理输出映射，再填写本地配置；该选择不会改变系统显示设置。

需要实际 Python 3（含 Pillow），PowerShell 7、可交互的 Windows 桌面、中文 Windows OCR、已登录的 Steam。通过 `-Python` 明确传入解释器，不依赖 WindowsApps 占位命令。OCR 构建使用系统 .NET Framework64 编译器及 Windows SDK 10.0.26100.0 metadata。HDR 查询工具使用 MSVC 14.44／SDK 26100；不随源码分发其生成 EXE。

Windows 入口会在替换安装文件前，通过合成文件的最终句柄路径检查 AppData 是否被应用环境重定向。发生重定向时，整个入口从当前用户的普通桌面上下文重新运行；备份、游戏启动、存档保护和恢复继续处于同一上下文。验证失败会拒绝安装，不以包身份查询或日志中的逻辑路径代替实际落点检查。该流程不改变插件保存路径、系统配置或宿主，启动输出仍实时转发给原采集流程。

已经运行的平台客户端可能保留旧启动环境；首次切换前须正常退出并从普通桌面环境重启 Steam，不接管已有游戏。预设仍保存到普通 `%LOCALAPPDATA%\NR Nexus\Presets`。排查时可单独运行 `normal_process.py --probe`（正常返回0，重定向返回2，检查失败返回1）；直接调用 `runner.py` 遇到重定向会拒绝启动，应改用上述 PowerShell 入口。

## 不启动游戏的回归

在仓库根运行，`$pythonPath` 为已核验的解释器绝对路径：

```powershell
& $pythonPath -B -m unittest discover -s tools/game-test -p 'test_*.py' -v
```

单测包含真实 Windows 文件共享锁检查和 mock 编排检查，不启动游戏。符号链接用例可能因系统权限跳过。

真实 Windows 后台／最小化激活探针使用两个短命独立测试窗口，不启动游戏、不发送键鼠输入：

```powershell
& $pythonPath -B ./tools/game-test/probe_foreground.py --output-root tmp/automatic-foreground-probe
```

普通桌面上下文可通过 `normal_process.py --run <Python绝对路径> -B <probe_foreground.py绝对路径> --output-root <本仓库tmp绝对路径>` 运行同一探针。核对 result.json 的各场景和 cleanup，不能将请求激活当作实际成功。

## 后续获授权的实机入口

下列步骤会写本地输出；完整入口还会临时替换游戏文件并启动游戏。执行前核对进程、候选 manifest、目标安装和存档，并取得本轮实机授权。

```powershell
# 仅构建并查询显示状态；不改变 Windows HDR。
& ./tools/game-test/hdr-state.ps1 -Command query

# 本轮要求完整恢复原安装；必须显式传入恢复开关。
& $pythonPath -B ./tools/game-test/fg_acceptance.py `
    --candidate-manifest $candidateManifest `
    --game-binary-directory $gameBinaryDirectory `
    --target .local/game-test/witcher3-steam.json `
    --python $pythonPath `
    --output-root artifacts/game-tests `
    --hdr-tool tmp/hdr-state/hdr-state.exe `
    --restore-installation
```

`--restore-installation` 只透传 wrapper 原有的 `-RestoreInstallation`，使其在游戏退出后恢复原 DLL 和完整配置；原存档保护及恢复校验由既有 runner 承担。进程仍存活时保留 `restore_pending`，不能将该状态报告为已恢复。默认留装分支仍为旧逻辑，因此本轮禁止省略该开关。

直接 wrapper 入口同样可使用 `-Python $pythonPath -TargetPath <配置> -OutputRoot <目录> -RestoreInstallation`。普通 smoke／menu／calibrate 入口为 `tools/run-game-test.ps1`，继续接受原 `-Game/-Platform/-Scenario/-Save/-CaptureHoldSeconds/-Retries/-Preflight` 参数，需要自动采集时附加 `-CaptureScript <本仓库 tools/ 或 tmp/ 下的 .py>`。`-Preflight` 会构建 OCR 并写自检产物，不能与只读语法检查混淆。

若游戏将 Home 绑定为切换 HUD，直接 wrapper 可加 `-IsolateOverlayKey`，仅在本次备份保护的 ReShade 配置中将叠加层热键设为 F7；调用方须先确认 F7 无冲突并使用 F7 开关叠加层。测试结束完整恢复原配置。默认菜单按键保持 200 ms、鼠标保持 120 ms，以避免按帧采样的界面漏掉瞬时输入；释放采集等待前须确认叠加层已关闭，不能将等待结束视为菜单状态已确认。

IME 等交互专项可在 `menu` 场景显式传 `-CaptureHoldSeconds 900`，只进入主菜单、核验实际加载插件并等待，不加载存档；`smoke` 仍在载档后等待。范围为 0 或 10–900 秒，默认不变；同步在本地目标副本中给 `run_timeout` 留足启动、采集和退出时间。完成后向本次 `capture-ready.json` 的同一 PID 写入 `capture-complete.json`，内容为 `{"pid": <本次PID>, "completed": true}`；超时仍记为未完成验收。发送前必须取消未提交组合、关闭原生编辑窗及 ReShade 叠加层，并核对实际游戏画面，随后由既有菜单退出流程收尾，保护持续到退出恢复完成。

由用户操作的 IME 验收须额外传 `-ManualImeSession -Scenario menu -RestoreInstallation`，不自动重试。此模式到期只请求介入并继续保护，不向游戏发退出按键；即使工作线程／看门狗失败，恢复路径也保持文件锁等待用户退出，不强退游戏。完成输入后先关闭编辑窗及叠加层，写入上述完成信号，再由用户从游戏菜单正常退出。实际退出、插件 detach 日志、崩溃检查及原文件恢复仍分别验证；提前退出或没有完成信号不记为通过。无人处理时保护会一直保持到所属游戏进程退出，不能关闭保护器进程代替收尾。

人工模式启动后不执行 OCR、截图、激活窗口或按键；程序仅确认本次进程／DX12 路径和实际加载模块后等待。用户负责进入主菜单并操作界面；自动记录的进程路径不等于已确认主菜单。原来的非人工 `menu`／`smoke` 行为保持。冻结 worker 快照须同时包含 `runner.py`、`desktop.py`、`guard.py` 和 `normal_process.py`，依赖导入回归不启动游戏。

双模型定向验证可向直接 wrapper 传 `-NrRuntimePath <310.9.16-DLL>`、
`-NrPreset 0|1|2|3`、`-NrPass2Preset 0|1|2` 和 `-NrPasses 1..4`。
逐层阶段定向验证可额外传 `-NrHookPoint 0|1|2`、`-NrPass2Stage 0..3`、
`-NrPass3Stage 0..3`、`-NrPass4Stage 0..3`；省略保留原 wrapper 的 Present 选择。
例如 `-NrHookPoint 1 -NrPass2Stage 2 -NrPasses 2 -NrPreset 1 -NrPass2Preset 2`
请求 Render A → 游戏 SR → Upscaled B。Stage 参数同样只允许完整恢复的受保护会话，
不能与 `-InstallOnly` 合用；新增参数不授权恢复已暂停的无人流程。
巫师3的 P0 SR 定向可加 `-SrOnly`，在完整备份的游戏配置中临时请求关闭 RR、DLSS FG、
XeFG 和 FSR FG；保持 HDR／分辨率。退出后完整恢复，日志须另外确认真实源 Feature 与运行路径。
`-TelemetrySeconds 1` 可为短场景临时请求逐秒性能／阶段遥测，沿用完整恢复保护；默认不覆盖游戏现有值。
仅指定的键会临时变化；Runtime 先核对版本和 SHA-256，与已安装文件相同则直接复用，
不同才备份替换并在退出后恢复。恢复哈希核验完成且游戏／CrashReporter 已退出时自动删除冗余二进制备份，保留版本、哈希及回执；恢复未完成时继续保护备份。必须同时使用 `-RestoreInstallation`，不以创建
参数日志代替实际网络确认或画质／性能验收。

runner 与 wrapper 复用公共测试产物生命周期。结果写出后自动删除导航／OCR 截图；失败保留关键样本，恢复核验后释放存档副本，配置副本保留供验收使用。Full／Acceptance 与 Standard 同样收尾；显式 `NR_NEXUS_TEST_KEEP_RAW=1` 才保留完整数据。tmp/ 与 artifacts/ 共享默认 10 GiB 软预算（`NR_NEXUS_TEST_BUDGET_BYTES`），预算只用于额外收缩，设为 0 不影响正常收尾。活动任务、恢复材料及明确保留的证据不受预算删除。

连续录制自动核验各原帧哈希，生成不超过 1280×720 的 WebP 回看预览并核验解码帧数／时间间隔；保留预览、摘要、录制记录及首尾原图，自动释放其余原帧。转换失败则保留整段原帧及错误记录。预览只用于采样回看；原分辨率画质分析或完整原帧专项须在启动前显式 KEEP_RAW。

已授权的历史游戏产物清理可运行 `python tools/game-test/cleanup_artifacts.py --root artifacts`，只接入有完整 guard／恢复／退出证据的旧运行；游戏或 CrashReporter 仍在运行时拒绝执行。

`-ControlMaskUseCoreContext` 仅用于 `-SrOnly`、单 Pass Upscaled 的受保护 ControlMask 诊断。depth-far FG case 固定走 Present，会在临时配置中强制写入 `NRControlMaskUseCoreContext=0`；退出时恢复启动前值。其余 ControlMask case 省略该开关时保留原配置。
`-ControlMaskAutoMask 0|1` 与 `-ControlMaskBlendMode 0..3` 可在同一受保护 case 上覆盖对应测试参数；省略保留 case 默认值。无显式 case 时提前拒绝，receipt 记录请求值，实际采用仍须核对运行日志。
`fg_acceptance.py` 可用 `--capture-script <tools/ 或 tmp/ 下的 Python 脚本>` 将 capture-ready 阶段委托给受保护 runner 启动的采样流程；`--start-nr-disabled` 使该流程从 NR OFF 开始，并且必须与采样脚本同时指定。省略时保持原有静态采集。

完整 FG 入口保持原 HDR 显示条件检查、NR 开关／移动及退出流程；显示器名称和性能目标来自本地 target。测试通过仅覆盖相应保护与编排合同，截图仍需单独判断。

性能对照前核对原 `NRPresentBlockHudless` 值：入口省略 `--block-optional-hudless` 时临时设为 `0`，传入时临时设为 `1`。应与既有验收条件一致，分别记录两个颜色面和单面路由，不能将不同路由的 FPS 直接比较；完整恢复开关会在退出后恢复原值。

XeFG A/B/C 可复用同一候选和目标配置：默认 `--nr-hook-point 2` 运行 Present，`--block-optional-hudless` 仅把 AUTO Present 固定为 Backbuffer 单颜色，`--nr-hook-point 0` 选择 Upscaled 对照。可用 `--nr-passes 1|2` 在受保护会话里临时指定 Pass 数；省略时保留原配置值。每轮均显式使用 `--restore-installation`；`--gpu-timers` 开启逐颜色／Pass GPU Timer，`--telemetry-seconds 1` 记录逐秒实际 Evaluate 计数。比较时保持模型 Preset、工作分辨率、Pass 数及 HDR 条件一致；结果会分别保留应用／显示 FPS、FG 服务 Evaluate 计数、遥测和 GPU Timer 行，视觉质量仍须独立人工判断。

单颜色适用条件还要求 UI Composition 关闭。`--xefg-ui-composition 0|1`（PowerShell：`-XeFGUiComposition 0|1`）仅在完整恢复的受保护会话中，临时修改 `OptiScaler.ini` 已有 `[XeFG] UIComposition`；省略保留用户值，非 AUTO 或启用 UI Composition 时可能保留双链。原文件先备份，退出后整文件校验恢复，不修改 SDK 行为。入口会给临时 NR 策略写入迁移标记，使明确的单／双链测试选择不被一次性升级覆盖；生产迁移由配置测试单独验证。纯合成变换检查为 `tests/run-fg-acceptance-config-tests.ps1`。

XeFG SDK 隔离候选使用 `--nr-off-control --disable-xefg-sdk-detours`，并且必须同时使用 `--start-nr-disabled`（PowerShell：`-StartNrDisabled -DisableXeFGSdkDetours`）和 `--restore-installation`。该开关在启动前决定，整个会话不安装 Nexus 的 XeFG SDK 导出／Swapchain Detour；不在运行中的 Context 上拆卸 Hook，也不通过关闭 XeFG 伪造视觉成功。隔离分支只验证候选加载、NR Evaluate=0、退出和恢复；SDK 资源／UI Composition 到达情况标记为不可观测，`routing_passed=null`，必须另做相同场景的动态人工对照。

正常 Detour 分支的 `ReShade.log` 可用有限样本 `resource_contract` 和 `ui_composition_contract` 核对：原始／转发 Tag、Present ID、有效期、尺寸、资源是否替换、底层 SDK 返回值及实际应用状态。日志中的 SDK 成功、配置值或 `ui_composition=1` 只证明调用／状态契约，不能替代小地图透影和连续运动画质验收。

逐窗口 Evaluate 必须读取 telemetry 的 nr[evals=…]，不能取 nr_stage[evals=…] 累计值。前者用于匹配本窗口的应用帧数和 NR On／Off，后者仅为阶段累计诊断；修正解析后可用已保存的完整日志重新验证，保留修正前派生结果和原始会话记录，不必重新启动游戏。
