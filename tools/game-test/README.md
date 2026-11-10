# 巫师3本地测试入口

本入口支持 Steam／巫师3 DX12。默认用游戏脚本桥操作标题菜单、精确加载存档及正常退出；runner 在受保护会话中临时部署本仓库的 source-only bridge Mod，使用 -net -debugscripts 与本机 loopback 调试端口通信。现有存档／配置保护、进程归属、崩溃检查、恢复流程继续复用。它不是通用游戏驱动，不包含悟空无人选档。

客户端连接 `127.0.0.1:37001`；已验证的游戏调试服务监听 `0.0.0.0:37001`，外部可达范围取决于现有防火墙设置。调试服务随本次所属游戏进程退出而结束。

默认 smoke／menu／calibrate 不建 OCR，也不以鼠标或键盘导航游戏菜单。Smoke 必须每轮传 -Save 和当前存档文件名；游戏通过 ListSavedGames 按唯一完整文件名载入，runner 先核对该存档 SHA-256，再连续两次确认 GetLoadGameProgress 非错误且玩家对象有效，才记为游戏态就绪。菜单退出调用游戏 API 并等待所属进程退出。需要 CaptureScript 捕获桌面时，采集边界仍核对 HWND、PID、创建时间、路径和真实前台；只在采集要求下激活窗口。确定性脚本错误立即失败，不拖到整段超时；默认自动重试为 0。

代码桥接会等待游戏标题菜单可用，但不会跳过启动片头。与本机 Witcher 3 5.00 匹配的 Skip Intro Movies - Remastered 尚未在本机安装或实测；没有该验证时，不能把片头已跳过作为测试能力。

两个 PowerShell 入口接受 `-CaptureScript 指向 tools/ 或 tmp/ 下的 Python 脚本`，同时指定非零 `-CaptureHoldSeconds`。入口在安装前校验参数，普通桌面上下文重启保留该绝对路径。runner 预检记录脚本 SHA256，在 `CAPTURE_READY` 后复核并仅启动一次，调用形式为同一 Python 的 `-B <脚本> <本次运行目录>`，输出写 `automatic-capture.log`。该选项不能与 `ManualImeSession` 或 `InstallOnly` 搭配。

采集器读取 capture-ready.json 的 automatic_foreground=true 和本次 owned-process.json，核对唯一进程／窗口身份并自动激活。动态采样与转镜期间持续核验前台和几何，失焦即停止，不在采样中反复激活。收到 capture-cancel.json 应停止输入并结束自己的采样子进程；异常写 capture-failed.json，成功收尾才写匹配 PID 的 capture-complete.json。使用 CaptureScript 时还必须由父进程观察到脚本零码退出并写 capture-script-result.json，worker 才允许请求正常退出。提前完成、失败／损坏凭据或超时不能释放门禁。恢复前先结束采集器和 worker，文件保护保持到本次游戏退出并恢复完成。

回归覆盖后台激活、最小化恢复、身份变化拒绝、失败门禁、脚本单次启动、完成与退出顺序及保护恢复。单测属于 MOCK／本地文件合同；独立 Windows 窗口探针验证前台 API 路径，IN_GAME／VISUAL 和长时稳定性另行验收。

旧桌面选档曾依赖唯一时间戳、焦点和鼠标输入；默认脚本流程不再调用这段路径。游戏桥按存档文件名加载，并将游戏引擎状态与本地存档哈希分别记录。采集前的进程响应检查仍执行；它不替代画面、视觉效果或性能验收。

## 本地配置与依赖

将 `target.example.json` 复制到仓库根 `.local/game-test/witcher3-steam.json`，填入原有的 `default_save`、`expected_hdr_monitor` 和 `performance_target_fps`。该目录被 Git 忽略；不要提交实际存档标识、显示器身份或运行凭据。已有本地配置无需重新生成。

`--target`／`-TargetPath` 指定该文件。适配器仍限于巫师3 Steam；换配置不表示支持其他游戏。`--output-root`／`-OutputRoot` 只接受本仓库 `tmp/` 或 `artifacts/` 下的目录；工作区互斥锁不随输出目录改变。

多显示环境可同时配置 `expected_hdr_adapter_luid`（查询返回的16位十六进制字符串）和 `expected_hdr_target_id`（JSON uint32 整数）。入口只接受唯一匹配该身份的 active 对象，并继续核对 `expected_hdr_monitor` 精确名称与 HDR enabled；不按同名对象的顺序猜测。仅配置一个字段、身份缺失／不匹配、重复身份或 HDR 未启用均在安装前拒绝。两字段省略或均为 null 时，保留原有“恰好一个 active 输出”的检查。先只读确认物理输出映射，再填写本地配置；该选择不会改变系统显示设置。

需要 Python 3（含 Pillow）、PowerShell 7、已登录的 Steam；桌面采集另需可交互的 Windows 桌面。通过 -Python 明确传入解释器，不依赖 WindowsApps 占位命令。默认脚本后端不需要 OCR；HDR 查询工具使用 MSVC 14.44／SDK 26100，不随源码分发其生成 EXE。

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

所有 smoke 游戏验收每轮都要显式指定本轮存档文件名（PowerShell 入口 -Save，Python 入口 --save）。入口会在启动前核验文件存在并记录存档身份；不使用 target 配置中的默认存档。

```powershell
# 仅构建并查询显示状态；不改变 Windows HDR。
& ./tools/game-test/hdr-state.ps1 -Command query

# 本轮要求完整恢复原安装；必须显式传入恢复开关。
& $pythonPath -B ./tools/game-test/fg_acceptance.py `
    --candidate-manifest $candidateManifest `
    --game-binary-directory $gameBinaryDirectory `
    --target .local/game-test/witcher3-steam.json `
    --save $saveName `
    --python $pythonPath `
    --output-root artifacts/game-tests `
    --hdr-tool tmp/hdr-state/hdr-state.exe `
    --restore-installation
```

`--restore-installation` 只透传 wrapper 原有的 `-RestoreInstallation`，使其在游戏退出后恢复原 DLL 和完整配置；原存档保护及恢复校验由既有 runner 承担。进程仍存活时保留 `restore_pending`，不能将该状态报告为已恢复。默认留装分支仍为旧逻辑，因此本轮禁止省略该开关。

直接 wrapper 入口可省略 -Python；工具会优先使用仓库的 .local/game-compare-venv/Scripts/python.exe，并忽略 WindowsApps 占位符，也可显式用 -Python $pythonPath -TargetPath 指定目标配置。-OutputRoot 指定输出目录，-RestoreInstallation 控制完整恢复。普通 smoke／menu／calibrate 入口为 tools/run-game-test.ps1，继续接受原 -Game/-Platform/-Scenario/-Save/-CaptureHoldSeconds/-Retries/-Preflight 参数，需要自动采集时附加 -CaptureScript 指向 tools/ 或 tmp/ 下的 Python 脚本。-Preflight 解析目标并打印本地配置；默认脚本后端不构建 OCR，也不启动或安装游戏。

`-IsolateOverlayKey` 将本轮 ReShade 叠加层热键临时切到 F7，并在结束后恢复。游戏菜单、存档和退出由脚本桥处理；采集脚本需要开关叠加层时使用对应热键，并在完成采集前确认面板已关闭。

IME 等交互专项可在 `menu` 场景显式传 `-CaptureHoldSeconds 900`，只进入主菜单、核验实际加载插件并等待，不加载存档；`smoke` 仍在载档后等待。范围为 0 或 10–900 秒，默认不变；同步在本地目标副本中给 `run_timeout` 留足启动、采集和退出时间。完成后向本次 `capture-ready.json` 的同一 PID 写入 `capture-complete.json`，内容为 `{"pid": <本次PID>, "completed": true}`；超时仍记为未完成验收。发送前必须取消未提交组合、关闭原生编辑窗及 ReShade 叠加层，并核对实际游戏画面，随后由既有菜单退出流程收尾，保护持续到退出恢复完成。

由用户操作的 IME 验收须额外传 `-ManualImeSession -Scenario menu -RestoreInstallation`，不自动重试。此模式到期只请求介入并继续保护，不向游戏发退出按键；即使工作线程／看门狗失败，恢复路径也保持文件锁等待用户退出，不强退游戏。完成输入后先关闭编辑窗及叠加层，写入上述完成信号，再由用户从游戏菜单正常退出。实际退出、插件 detach 日志、崩溃检查及原文件恢复仍分别验证；提前退出或没有完成信号不记为通过。无人处理时保护会一直保持到所属游戏进程退出，不能关闭保护器进程代替收尾。

人工模式启动后不执行 OCR、截图、激活窗口或按键；程序仅确认本次进程／DX12 路径和实际加载模块后等待。用户负责进入主菜单并操作界面；自动记录的进程路径不等于已确认主菜单。非人工 menu／smoke 由脚本桥处理，不执行 OCR 菜单导航。脚本桥使用 [Rusty Witcher 3 Debugger](https://github.com/SpontanCombust/rusty_witcher_debugger) v0.8.0 CLI；将 rw3d_cli.exe 放在被忽略的 .local/game-test/rw3d_cli.exe，SHA-256 必须为 0f04cccc69a9d833d59006fc9a7ff57b6342ac0c7a5b4c985d9379fbc07adf4c。不自动下载或随项目分发；runner 每个脚本请求调用一次 CLI，并记录退出码及回执。冻结 worker 快照包含 runner、desktop、guard、normal_process、桥接客户端及脚本 Mod；依赖导入回归不启动游戏。

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

看门狗发现持续窗口无响应或整轮超时时，会先结束采集器及 worker，在游戏恢复前用独立进程保存 `hang-evidence/game.dmp` 和结果清单。转储通过 Windows DbgHelp 捕获线程栈、线程信息及模块，不包含完整进程内存；工具核对所属 PID、创建时间和路径，默认限时20秒。证据仅留本地，失败或超时仍继续原有受保护恢复，结果不会因此转为通过。

画面停止但窗口仍响应时，窗口无响应检查可能不触发。可在本次受保护会话仍存活时，从另一个终端保存一次转储；`$runDirectory` 必须指向本次 runner 输出，输出目录必须尚不存在。此命令不操作游戏输入、不结束游戏，也不释放原 runner 的保护：

```powershell
& $pythonPath -B ./tools/game-test/hang_evidence.py `
    --process "$runDirectory/owned-process.json" --output "$runDirectory/hang-evidence-manual"
```

连续录制自动核验各原帧哈希，生成不超过 1280×720 的 WebP 回看预览并核验解码帧数／时间间隔；保留预览、摘要、录制记录及首尾原图，自动释放其余原帧。转换失败则保留整段原帧及错误记录。预览只用于采样回看；原分辨率画质分析或完整原帧专项须在启动前显式 KEEP_RAW。

已授权的历史游戏产物清理可运行 `python tools/game-test/cleanup_artifacts.py --root artifacts`，只接入有完整 guard／恢复／退出证据的旧运行；游戏或 CrashReporter 仍在运行时拒绝执行。

`-ControlMaskUseCoreContext` 用于 `-SrOnly`、单 Pass Upscaled 的受保护 ControlMask 诊断。depth-far case 可走 Present，或走 Upscaled + Core Context；后一路径同样要求 `-SrOnly`。退出时恢复启动前值。
`-ControlMaskAutoMask 0|1` 与 `-ControlMaskBlendMode 0..3` 可在同一受保护 case 上覆盖对应测试参数；省略保留 case 默认值。无显式 case 时提前拒绝，receipt 记录请求值，实际采用仍须核对运行日志。
`fg_acceptance.py` 可用 `--capture-script <tools/ 或 tmp/ 下的 Python 脚本>` 将 capture-ready 阶段委托给受保护 runner 启动的采样流程；`--start-nr-disabled` 使该流程从 NR OFF 开始，并且必须与采样脚本同时指定。省略时保持原有静态采集。

ControlMaskDepthNear 和 ControlMaskDepthFar 须成对指定，仅用于 depth-far case，并满足 0 <= Near < Far <= 1。Python入口参数为 --control-mask-depth-near 与 --control-mask-depth-far；receipt 记录请求值，首次 ControlMask active 日志也记录实际采用的深度范围。

需要同帧模型／遮罩定位时，PowerShell 包装器可显式传 `-ModelDiagnosticCapture`：本轮截图键临时改为 F10，捕获来源为 Evaluated model buffers，并开启可选诊断平面；整个 ReShade.ini 仍按原流程备份和恢复。F5 在巫师 3 同时会快存，诊断采集不使用它。成功执行遮罩的 Inline 最后求值层可随同帧模型输入／输出导出 `control_mask`，远景模式另导出本次实际 Depth Subrect 的 `control_mask_depth`。仅在有模型诊断请求且开启平面时读回；一般场景图、API 成功或请求参数不能代替这些 GPU 数据。

完整 FG 入口保持原 HDR 显示条件检查、NR 开关／移动及退出流程；显示器名称和性能目标来自本地 target。测试通过仅覆盖相应保护与编排合同，截图仍需单独判断。

性能对照前核对原 `NRPresentBlockHudless` 值：入口省略 `--block-optional-hudless` 时临时设为 `0`，传入时临时设为 `1`。应与既有验收条件一致，分别记录两个颜色面和单面路由，不能将不同路由的 FPS 直接比较；完整恢复开关会在退出后恢复原值。

XeFG A/B/C 可复用同一候选和目标配置：默认 `--nr-hook-point 2` 运行 Present，`--block-optional-hudless` 仅把 AUTO Present 固定为 Backbuffer 单颜色，`--nr-hook-point 0` 选择 Upscaled 对照。可用 `--nr-passes 1|2` 在受保护会话里临时指定 Pass 数；省略时保留原配置值。每轮均显式使用 `--restore-installation`；`--gpu-timers` 开启逐颜色／Pass GPU Timer，`--telemetry-seconds 1` 记录逐秒实际 Evaluate 计数。比较时保持模型 Preset、工作分辨率、Pass 数及 HDR 条件一致；结果会分别保留应用／显示 FPS、FG 服务 Evaluate 计数、遥测和 GPU Timer 行，视觉质量仍须独立人工判断。

单颜色适用条件还要求 UI Composition 关闭。`--xefg-ui-composition 0|1`（PowerShell：`-XeFGUiComposition 0|1`）仅在完整恢复的受保护会话中，临时修改 `OptiScaler.ini` 已有 `[XeFG] UIComposition`；省略保留用户值，非 AUTO 或启用 UI Composition 时可能保留双链。原文件先备份，退出后整文件校验恢复，不修改 SDK 行为。入口会给临时 NR 策略写入迁移标记，使明确的单／双链测试选择不被一次性升级覆盖；生产迁移由配置测试单独验证。纯合成变换检查为 `tests/run-fg-acceptance-config-tests.ps1`。

XeFG SDK 隔离候选使用 `--nr-off-control --disable-xefg-sdk-detours`，并且必须同时使用 `--start-nr-disabled`（PowerShell：`-StartNrDisabled -DisableXeFGSdkDetours`）和 `--restore-installation`。该开关在启动前决定，整个会话不安装 Nexus 的 XeFG SDK 导出／Swapchain Detour；不在运行中的 Context 上拆卸 Hook，也不通过关闭 XeFG 伪造视觉成功。隔离分支只验证候选加载、NR Evaluate=0、退出和恢复；SDK 资源／UI Composition 到达情况标记为不可观测，`routing_passed=null`，必须另做相同场景的动态人工对照。

正常 Detour 分支的 `ReShade.log` 可用有限样本 `resource_contract` 和 `ui_composition_contract` 核对：原始／转发 Tag、Present ID、有效期、尺寸、资源是否替换、底层 SDK 返回值及实际应用状态。日志中的 SDK 成功、配置值或 `ui_composition=1` 只证明调用／状态契约，不能替代小地图透影和连续运动画质验收。

逐窗口 Evaluate 必须读取 telemetry 的 nr[evals=…]，不能取 nr_stage[evals=…] 累计值。前者用于匹配本窗口的应用帧数和 NR On／Off，后者仅为阶段累计诊断；修正解析后可用已保存的完整日志重新验证，保留修正前派生结果和原始会话记录，不必重新启动游戏。
