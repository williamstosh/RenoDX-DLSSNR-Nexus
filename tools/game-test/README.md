# 巫师3本地测试入口

这里只迁入既有 Steam／巫师3 自动化：原存档保护、唯一时间戳选档、焦点核验、所属进程监控、菜单退出、恢复与有界重试。没有新增游戏适配，不包含悟空无人选档。

迁入后已完成语法检查、游戏预检和 90 项 Python 单测（89 项通过、1 项符号链接权限跳过）。新候选已完成巫师3加载、NR 开关、短移动、插件菜单与安全退出／完整恢复；原配置下的 NR 可见效果和 HUD 获用户确认。性能与视觉结论仅适用于实际配置及已测场景，自动化通过不等于完整画质验收。

## 本地配置与依赖

将 `target.example.json` 复制到仓库根 `.local/game-test/witcher3-steam.json`，填入原有的 `default_save`、`scene_patterns`、`expected_hdr_monitor` 和 `performance_target_fps`。该目录被 Git 忽略；不要提交实际存档标识、任务条件、显示器身份或运行凭据。已有本地配置无需重新生成。

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

直接 wrapper 入口同样可使用 `-Python $pythonPath -TargetPath <配置> -OutputRoot <目录> -RestoreInstallation`。普通 smoke／menu／calibrate 入口为 `tools/run-game-test.ps1`，继续接受原 `-Game/-Platform/-Scenario/-Save/-CaptureHoldSeconds/-Retries/-Preflight` 参数。`-Preflight` 会构建 OCR 并写自检产物，不能与只读语法检查混淆。

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
仅指定的键会临时变化；Runtime 替换前核对版本，并将游戏原 Runtime 加入既有
备份／哈希核对和退出恢复清单。必须同时使用 `-RestoreInstallation`，不以创建
参数日志代替实际网络确认或画质／性能验收。

完整 FG 入口保持原 HDR 显示条件检查、NR 开关／移动及退出流程；显示器名称和性能目标来自本地 target。测试通过仅覆盖相应保护与编排合同，截图仍需单独判断。

性能对照前核对原 `NRPresentBlockHudless` 值：入口省略 `--block-optional-hudless` 时临时设为 `0`，传入时临时设为 `1`。应与既有验收条件一致，分别记录两个颜色面和单面路由，不能将不同路由的 FPS 直接比较；完整恢复开关会在退出后恢复原值。
