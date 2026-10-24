# 巫师3本地测试入口

这里只迁入既有 Steam／巫师3 自动化：原存档保护、唯一时间戳选档、焦点核验、所属进程监控、菜单退出、恢复与有界重试。没有新增游戏适配，不包含悟空无人选档。

迁入后已完成语法检查、游戏预检和 90 项 Python 单测（89 项通过、1 项符号链接权限跳过）。新候选已完成巫师3加载、NR 开关、短移动、插件菜单与安全退出／完整恢复；原配置下的 NR 可见效果和 HUD 获用户确认。性能与视觉结论仅适用于实际配置及已测场景，自动化通过不等于完整画质验收。

## 本地配置与依赖

将 `target.example.json` 复制到仓库根 `.local/game-test/witcher3-steam.json`，填入原有的 `default_save`、`scene_patterns`、`expected_hdr_monitor` 和 `performance_target_fps`。该目录被 Git 忽略；不要提交实际存档标识、任务条件、显示器身份或运行凭据。已有本地配置无需重新生成。

`--target`／`-TargetPath` 指定该文件。适配器仍限于巫师3 Steam；换配置不表示支持其他游戏。`--output-root`／`-OutputRoot` 只接受本仓库 `tmp/` 或 `artifacts/` 下的目录；工作区互斥锁不随输出目录改变。

多显示环境可同时配置 `expected_hdr_adapter_luid`（查询返回的16位十六进制字符串）和 `expected_hdr_target_id`（JSON uint32 整数）。入口只接受唯一匹配该身份的 active 对象，并继续核对 `expected_hdr_monitor` 精确名称与 HDR enabled；不按同名对象的顺序猜测。仅配置一个字段、身份缺失／不匹配、重复身份或 HDR 未启用均在安装前拒绝。两字段省略或均为 null 时，保留原有“恰好一个 active 输出”的检查。先只读确认物理输出映射，再填写本地配置；该选择不会改变系统显示设置。

需要实际 Python 3（含 Pillow），PowerShell 7、可交互的 Windows 桌面、中文 Windows OCR、已登录的 Steam。通过 `-Python` 明确传入解释器，不依赖 WindowsApps 占位命令。OCR 构建使用系统 .NET Framework64 编译器及 Windows SDK 10.0.26100.0 metadata。HDR 查询工具使用 MSVC 14.44／SDK 26100；不随源码分发其生成 EXE。

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

完整 FG 入口保持原 HDR 显示条件检查、NR 开关／移动及退出流程；显示器名称和性能目标来自本地 target。测试通过仅覆盖相应保护与编排合同，截图仍需单独判断。

性能对照前核对原 `NRPresentBlockHudless` 值：入口省略 `--block-optional-hudless` 时临时设为 `0`，传入时临时设为 `1`。应与既有验收条件一致，分别记录两个颜色面和单面路由，不能将不同路由的 FPS 直接比较；完整恢复开关会在退出后恢复原值。
