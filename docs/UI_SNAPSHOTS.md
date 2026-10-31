# UI 离屏预览与文档配图

预览直接调用生产 `OnOverlay`／ImGui 控件，并由现有 `render-ui-captures.py` 栅格化 DrawData 和字体图集。状态、方案库和剪贴板为隔离的合成夹具，不读取个人配置，不启动游戏。图片不能代替游戏内焦点、GPU、画质或性能验收。

## 使用

先按 README 准备 Windows 构建环境、固定宿主及生成的 Shader include。Python 需 NumPy、Pillow；可传 `-PythonExecutable <python.exe>`，或设置本进程 `NR_NEXUS_PYTHON`，也支持 PATH 中的真实 `python.exe`（不支持 WindowsApps 占位程序）。

```powershell
./tools/preview-ui.ps1                                      # Quick
./tools/preview-ui.ps1 -Mode Full                           # 重要布局／主题重构
./tools/preview-ui.ps1 -Scenario quality                    # 单页的登记场景
./tools/preview-ui.ps1 -Scenario model-dropdown,preset-save # 精确场景
./tools/preview-ui.ps1 -Documentation Check                 # 候选＋比较，不替换
./tools/preview-ui.ps1 -Documentation Update                # 校验后只替换有意义变化
./tools/preview-ui.ps1 -Documentation Update -DocumentationImage quality,quality-zh # 仅替换选中的候选
./tools/preview-ui.ps1 -ArchiveName layout-milestone         # 显式保存重要快照
```

本地总览为 `artifacts/ui-snapshots/latest/index.html`，直接用浏览器打开，无需服务器；原图在 `latest/images/`。`previous/` 保留上一成功快照，页面提供同场景对照；`history/` 仅由显式归档产生。`manifest.json` 记录源码提交、工作树文件哈希、字体／构建输入、主题语言、场景、区域和图片哈希。文档同步后的工作树状态另记，捕获时输入身份保留。

指定 `-Scenario` 的定向结果独立轮换于 `artifacts/ui-snapshots/selected/latest/` 与 `selected/previous/`，不会替换完整总览；页面及元数据明确标记 selected 范围。

`tools/run-tests.ps1 -Suite ui` 通过后，若工作树中 `ui/` 或混合实现头的 UI 相关差异命中，默认生成 Quick；`-UiPreview Quick|Full|None` 可显式选择。普通业务修改不强制触发。自动检测是差异启发式；已提交的 UI 修改或间接布局影响可显式选 Quick。快照失败单独报错，不算视觉验收通过。Full 预览与测试产物的 `-RetentionMode Full` 是不同维度。

## 场景与长图

登记入口是 `tests/ui_snapshot_cases.hpp` 的 `Scenario` 列表及同文件动作分支。新增交互时复用 frame／find／click、真实鼠标和键盘事件，补充动作断言及语义区域；不要复制玩家 UI。`captures.json` 的 expected 集合必须与实际结果一致。

Quick 使用 Cyber Neon／简体中文，包含四页真实“展开全部／折叠全部”、模型选择、方案库任务、确认窗、辅助输入入口及代表性 Pass／诊断状态。Full 另作四主题、中英与长文本、多宽度／字号的正交覆盖；原有完整菜单回归矩阵仍独立运行。

完整页面保持真实宽度、字体及间距，按实际内容增高虚拟视口，稳定后要求内容区无未捕获滚动范围且底部在界内，不拉伸、不拼接。下拉、Tooltip、模态窗及滚动检查使用独立真实视口。原生文件选择器、Unicode 编辑窗口及 IME 候选不属于 ImGui DrawData，沿用原有专项测试；离屏预览只展示其 ImGui 入口。

夹具使用本机系统字体，中文已覆盖；没有加载 ReShade 的 ForkAwesome 图标字体，状态卡的部分图标可能显示替代字形，覆盖报告保留此限制。控件布局与文字的离屏检查不认证该图标或宿主实际字体的保真。

## 文档同步与质量判断

文档模式使用同一场景执行器，额外生成中英文文档场景。`regions.doc` 来自当前帧真实控件／子窗口几何，裁剪越界、空图、语言或元数据不匹配均失败；不再用历史固定像素坐标。旧 `NR_NEXUS_DOC_CAPTURES` 仍能生成 `doc-*.json`，并可用原 renderer 的 `--documentation --format webp` 导出。

旧环境变量入口仍先执行原菜单回归矩阵，再生成文档场景；新的 preview 入口才使用独立快速捕获路径。

候选在 `latest/documentation/`；`docs-report.json` 提供新旧哈希、尺寸、像素变化率。Update 仅复制达到变化阈值的候选到稳定的 `docs/images/ui/*.webp`。阈值用于避免细微像素噪声，不能代替 Codex 检查原图：应核对中文、中英文对应、完整控件组、字形、遮挡、说明及版本边界。无法确认时用 Check 保留旧图。README 只选特色区域；User Guide 补充操作图，不使用整套开发长图替代文档。新增图片引用及相邻说明由 Codex 同步维护，工具不会改写项目定位、来源或发布声明。

## 产物与故障

仅完整成功且源码输入未变化的候选可进入 latest。失败保留 `.work-*`／`.pending-*` 和 `last-failure.json`；旧 latest 不受影响，部分渲染的总览标记 partial 并列出失败项。

默认 Standard 成功后，登记的编译中间文件由现有测试 helper 清理；已退出且成功的本次预览工作目录在成品、日志和输入身份保存后按拥有权清单精确清理，原始 JSON／RGBA／可执行文件不进入长期成品。轮换只清理本系统拥有、未被修改的旧 Standard 成品；额外文件、哈希变化、重解析点、清理拒绝均保留并报告。Full／Acceptance 保留工作证据；Acceptance 不自动清理。失败证据或受保护的旧版本需按原项目归档规范人工选择，不自动扫描历史。

验证入口：`tests/test_capture_artifacts.py`、`tests/test_ui_snapshot_artifacts.py`、`tests/run-ui-preview-artifact-tests.ps1`，以及原有 `tools/run-tests.ps1 -Suite ui`。
