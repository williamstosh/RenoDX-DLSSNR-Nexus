# NVIDIA DLSS Neural Rendering ControlMask：P0 设计

> **适用性更新（2026-11-07）**：本页保留原P0设计、验收矩阵及当时结论。后续开发已取消“完整P0 PASS→全部P1→P2”的统一串行前置，区域算法与已有合法Stage／分辨率／Pass／API／FG路径可交错推进；首项区域功能为深度远景强度保护。原未测项不改记通过，现有源码门控也不因此自动开放。下文旧“P1不启动”“仅P0通过后”及完整矩阵要求只描述历史轮次，不作为新功能的统一准入要求。

## P0 实施结果（截至2026-11-07）

本节记录实现背景与历史验证；当前详细进度保存在本地状态记录中，公开源码只保留实现边界和可复核结论。ControlMask 工程、离线语义／性能验证及既有 B 模型 GRect／BRect 动态画质和稳定性验收按各自证据范围保留；自动前台与采集流程也完成了一次 B-GRect 集成运行。**P0 仍未 PASS**：原始验收矩阵仍有未覆盖项，历史启动异常根因未明。用户已明确不再重复实机测试；不将自动流程运行误记为新的运行时或视觉验收。

- **Gate A 通过（证据核验）**：独立复核 GrimsVerk 研究页和公开研究补丁，确认社区报告中的 R/G/B/A 语义、BlendMode 0–3、ControlMaskSubrectBaseX/Y/Width/Height 参数名和完整纹理 Subrect 用法。来源仍是社区逆向研究及作者游戏观测，不是 NVIDIA 官方 API 契约。现有 DLL 四段文件版本识别可复用，门控只接受 310.9.16.0。工作集 command list、状态转换和 Fence 退休复用现有契约。
- **Gate B 通过（原型代码）**：在 dlssnr.hpp 中新增默认关闭的本地配置开关 NRControlMaskEnabled，及 NRControlMaskMode、NRControlMaskRed/Green/Blue、NRControlMaskBlendMode 诊断键；没有新增 UI。只在 D3D12 Upscaled、非 mixed、单 Pass、slot 0 和精确 Runtime 条件下创建、派发及绑定遮罩。RGBA8 UAV 与 PSO 懒创建；shader 生成固定 RGB、全 1 或左上四分之一 RGB 置零的矩形测试遮罩，A 固定 255。Evaluate 参数包含资源、完整工作尺寸 Subrect 和 BlendMode，AutoMask 保持原配置；API 返回（含失败／异常）后清空 mask 资源与 Subrect，工作集仍依提交跟踪和 Fence 退休。生成 dispatch 计入现有每 Pass Evaluate GPU timer 区间。Setup 失败跳过本次 NR Evaluate、清理旧绑定并标记下一次 History reset，不在同帧重试。限频日志分别记录启用参数、拒绝、setup 失败和 Evaluate 返回／接受状态，模型标注为请求值。
- **Gate C 通过（工程验证范围）**：Release 插件和 FXC 编译通过。复用未改动的版本门控 9 项、Stage Plan 407,557 项及既有 stage-resolution WARP 证据。本轮专项验证固定 RGB、全 1、矩形字节，NGX 资源/Subrect/BlendMode 与 AutoMask 保持；实际 ProcessInline 默认关闭零 mask dispatch、关闭／模式切换清理、非精确 Runtime 不派发；分配失败、NGX 返回失败及异常时保留游戏输出、清理参数且同帧不重试；已录制未提交的旧尺寸 mask 保留，提交完成后才释放。替身不执行 NVIDIA NR Runtime。D3D12 Debug Layer 当前不可用，验证层不记通过；PSO、Execute/Signal 和设备移除等每种失败分支尚未全部独立注入。
- **历史实机状态（截至2026-11-05）**：旧冻结候选结果只绑定对应旧 SHA。候选 C6FE4107…3320E 在《巫师3》DX12／310.9.16.0／RTX 5090D 上完成 B-T1 与 B-GRect：请求模型、模块身份、Evaluate 返回和 GPU 完成按当时证据记录；B-T1 原始 GPU 采样 256/256 valid。B-BRect 的 retry6 未及时响应前台交接；retry7 有 Evaluate/API 返回和模型对，但 GPU 未核实；retry8／retry9 采集等待超时且没有有效 BRect Evaluate。后续运行和当前收尾状态见 NOW；本历史段落不代表最新矩阵或当前实机候选。

### 前台与采集自动化现状

早期流程依赖 Computer Use 前台交接，出现过确认等待和采集超时；AllowSetForegroundWindow 的返回值也不能代替对真实前台状态的核验。按用户要求，当前共享 runner 已改为自动核对本次游戏的 HWND、PID、创建时间和路径，并在首次输入及采集边界激活、验证实际前台；后台或最小化窗口可自动恢复，身份不符或激活失败时阻断输入。CaptureScript 在 CAPTURE_READY 后由父进程自动启动一次，须同时有有效完成凭据和脚本零码退出才释放采集门禁；失焦、失败及超时走受保护恢复。方法和约束见 tools/game-test/README.md 与本地 RUNBOOK。

2026-11-06 的 B-GRect 自动集成运行验证了自动前台／采集、OFF／ON 采集、NR 状态回转、正常退出及文件恢复；该轮没有确认运行时加载或视觉效果。已有 B 模型 GRect／BRect 动态画质和稳定性验收按所有者确认的原证据范围有效，不重复启动游戏。Python 回归和窗口探针的具体结果以 NOW 为准；构建、MOCK 或自动流程通过不提升为 IN_GAME／VISUAL 结论。

以下矩阵与方法描述的是 2026-11-05 前后的原始验收轮次，不是当前完成情况。原轮次在 B-BRect 未闭环时未进入模型 A；后续实机和离线证据及尚未覆盖范围见 NOW。原固定方法为《巫师3》DX12、310.9.16.0、Upscaled 单 Pass、100% 工作尺寸、FG/XeFG 关闭、AutoMask=0、BlendMode=0。受保护入口 tools/run-fg-acceptance.ps1 -ControlMaskCase T0..T6 固定 NR 配置并临时关闭 OptiScaler 独立 FrameGen；要求 -SrOnly -RestoreInstallation -NrRuntimePath <精确 Runtime 源文件> -NrPreset 2|1，并沿用原候选 manifest、目标和 Python 参数。叠加层用现有 -IsolateOverlayKey 避开 Home/HUD 冲突；诊断源使用模型缓冲区以保持 Upscaled，诊断图明确不等于最终呈现。

| 用例 | Enabled | Mode | RGB | 实机状态 |
| --- | --- | --- | --- | --- |
| T0 | 0 | 0 | 1,1,1 | B、A 授权重试已执行 |
| T1 | 1 | 1 | 1,1,1，全 1 | B、A 已执行 |
| T2 | 1 | 0 | 0,1,1 | B、A 已执行；A 同帧抑制成立 |
| T3 | 1 | 0 | 0.5,1,1 | B 已执行，A 未测 |
| T4 | 1 | 0 | 1,0,1 | B 已执行；工具收尾 OCR failed，恢复通过；A 未测 |
| T5 | 1 | 0 | 1,1,0 | B 已执行，A 未测 |
| T6 | 1 | 2 | 外部 1,1,1；左上四分之一 RGB=0 | B、A 同帧局部证据成立 |
| GRect | 1 | 3 | 全图 R=B=A=1；左上四分之一 G=0，其余 G=1 | 新候选模型 B 实机完成；模型 A 未测 |
| BRect | 1 | 4 | 全图 R=G=A=1；左上四分之一 B=0，其余 B=1 | 新候选模型 B 未完成；有界重试均未收束 |

每轮在替换前重新备份插件、ReShade 配置、Runtime 和临时游戏设置，保护原存档直至进程退出，核验实际加载模块、Runtime、请求模型、Mask 参数、NGX 返回和 GPU 完成证据，退出后逐文件核对恢复哈希。参数接受、GPU 实际执行和视觉语义分别验收；GPU 错误、设备移除或保护异常时停止后续轮次。以上是原始轮次的验收约束；当前已按用户决定停止重复实机测试，现有证据范围和未覆盖边界见 NOW。

性能口径：现有每 Pass timer 同时包含 mask 生成与 NR Evaluate；周期 GPU 日志可报告原口径观测，不能替代逐帧 GPU 中位数／P95。全帧均值与 P95 来自生产 telemetry 的 Present 间隔，合并窗口时按帧数加权并保留 0.25 ms 分桶上沿口径。完整验收仍需至少 1000 个稳态帧与可核验输入；缺少的原始样本标为未测。RGBA8 的纹理 payload／GetResourceAllocationInfo 字节不等于驱动实际驻留显存增量，须另列可验证工作集与 GPU／驱动／模型／尺寸。不套用社区数值。

核验基线：2026-11-04，实施分支 `codex/controlmask-p0` 从 `dev@1e725d0` 起步，原型提交为 `0e59fbd`，最小诊断提交为 `e4331b5`；无推送、合并或发布。旧实机 DLL SHA 仍为 BCA331…70AD76；新诊断 DLL 为 C6FE4107…11E3320E，构建时输入为 0e59fbd 加本轮源码，随后保存到诊断提交，未将新提交称为先前构建动作。本文保留设计、证据来源与实施结果。证据区分**源码事实**、**作者实测**、**WARP 合同验证**和**未验证项**；社区报告作者明确说明研究与补丁由 Claude Code 制作、GrimsVerk 仅快速审阅，因此不把它当 NVIDIA 官方契约。

### 定向收尾的方法与前提

用户已批准最小诊断开发与离线测试，未授权游戏／部署。现有 G/B 用例实际发送 Tone=1、Structure=1、Style=2、AutoMask=0／BlendMode=0，强度没有关闭；G=0 后仍有改图不能据此推断无效。旧 RGB 矩形可由 R=0 解释，新 GRect／BRect 保持 R=1，以最小 Shader 扩展排除此混淆并观察空间边界。新模式忽略配置 RGB，复用原 RGBA8／队列／资源寿命及单次 Evaluate，不改旧 mode0/1/2、默认关闭、版本／Stage 门控或 History，不加正式 UI。矩形坐标采用半开区间 `[0,width/2) × [0,height/2)`，奇数尺寸向下取整。

严格中性比较的是相同完整输入与进入 Evaluate 前 History 下的**无 mask 输出对全 1 输出**，不是模型输入对输出。现有 RGB8 预览不能还原原生颜色、深度／运动和 History，也没有低成本完整回放入口。本轮按用户最新决定**暂不开发完整输入／History 回放**，先做受控静态场景 ABA（无 mask／全 1／无 mask）与单通道局部效果；采集重复噪声底、固定机位／预热、实际模型参数及原图，报告实用等价程度和不能归因的扰动，不能升级为严格数值等价。需要严格数值证明时再另行评估完整输入／独立 Feature 回放，禁止同帧双重 Evaluate 生产 Feature。G0 在 Style=2 时不预期等同 Tone=0。

GPU 原始出口复用 `gpu_timers.hpp` 已完成、未平均的 Stats，通过 recording token 绑定 Evaluate 当时的 Runtime、模型请求、mask／AutoMask／Style／强度；不增加 Query、队列或 Evaluate，不改变 HUD／趋势图／分辨率比较。`NRControlMaskGpuCaptureSamples=0` 默认关闭；非零目标上限 4096，事件缓冲上限 8192；`NRControlMaskGpuCaptureSeconds` 默认 120 秒（含预热），`NRControlMaskGpuCaptureWarmupSeconds` 默认 6 秒。要求现有 `NRGpuTimers=1`，采样器不自动增加查询。窗口从首个符合门控的 NR Evaluate 起算；时长／预热输入有界，满缓冲、超时及停止显式终止，不补造缺样。

导出在现有持模块引用的截图线程执行，目录为 `NR Diagnostics`：先写独立 incomplete 收据，再以拒绝覆盖的独立终态 JSON 保存有效／拒绝／超时／丢弃／溢出及未完成记录、实际样本数和原 Pass 计时。进程硬退出或写失败可能只有 incomplete／没有终态，均不能报完成；正常 worker 收尾保留 pending_stop。所有导出 JSON 经既有脱敏函数。Evaluate 区间含 mask 生成与 NR Evaluate，不能称独立 mask 耗时。受保护工具只收集本轮新文件并核对原始有效记录数，不用汇总声称缺失样本存在。

原始计划顺序为必要启动时观察异常→B 的 GRect／BRect→A 的 GRect／BRect→同机位 ABA 实用中性→R0.5 受控定量→B AutoMask／BlendMode 代表边界及必要 A 确认→动态／逐帧 GPU 性能。该计划已因用户决定停止重复实机测试而停止；单通道局部和受控静态结果不能替代严格因果，完整 P0 标准与 P1 门槛仍以 NOW 和 DECISIONS 为准。

## 原P0收尾结论（历史）

**CONDITIONAL GO；P0 未 PASS，P1 不启动。** 离线及历史运行结论按各自输入和证据范围保留；B 模型 GRect／BRect 动态画质与稳定性验收按原证据有效，自动化也完成一次集成流程运行。剩余未覆盖矩阵与启动异常根因未知；详细范围保留在本地状态记录中。依用户决定不重复实机测试，也不把未执行项写成通过。

## Nexus 当前事实与适配位置

- `dlssnr.hpp` 通过 NGX Feature 18 的 D3D12 Create/Evaluate/Release 代理维护 Nexus 自有 NR slot、参数块和逐槽 History；游戏创建的 DLSS feature 与 Nexus feature 状态分别管理。310.9.16 双模型选择已经按确切 Runtime 版本门控，未知版本不因“更高版本”而推定兼容。见 [`SetEvaluationParameters`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L7234)、[`ProcessInline`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L10155) 与 [`FeatureState`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L432)。
- Upscaled 经 `ProcessInline` 执行；`SetEvaluationParameters` 每帧设置 `DLSSNR.Color/Output/Depth/MVec` 与其 subrect、强度、AutoMask 等。P0 仅在显式启用且兼容时写入 ControlMask 参数。单层 Upscaled 不触碰 `ProcessInlinePreSR`（Render）、Present 后处理或 Vulkan。
- `FinalResources`/`WorksetKey`、`EnsureWorkset` 和 `CreateScratchTexture` 管理尺寸相关的 D3D12 Proxy、NR 输出、描述符和资源大小；工作集有 queue/fence lease、退休与 GPU 完成后释放机制。现有合同测试覆盖重建、录制引用、提交完成及退休。新资源应纳入同一工作集和退休账本，禁止另建无 fence 的裸缓存。
- 输入代理图、有效深度／运动矢量及其坐标和有效性由 `EvaluationContract` 与 Upscaled 路径解析；动态遮罩若以后读取这些输入，必须沿用同一帧解析出的资源、尺寸、反转深度标志及 motion scale，不自行猜测。P0 固定色不依赖深度和运动。
- 当前 `dev` 已包含 ControlMask P0 的生产路径、提交／资源门禁及定向回归；`tests/stage_execution_cases.hpp`、`tests/stage_gpu_runtime_cases.hpp`、`tests/same_stage_resolution_cases.hpp` 与 `tests/gpu_timer_cases.hpp` 继续提供真实 D3D12 command list、队列完成及时间戳夹具。替身 NGX 只验证参数／调用合同，不代表 NVIDIA Runtime 实测。

## 社区证据与边界

GrimsVerk 的 [ControlMask 研究](https://grimsverk.github.io/nr-runtime-research/controlmask.html)称参数在每次 Evaluate 读取，覆盖 310.8 与 310.9.16：R 调制最终 NR 混合强度，G 调制网络的 LocalTone 输入，B 调制 Structure，A 未使用。按其公式，R=0 抑制 NR 混合但仍执行网络；G=0 不取消 Style 色调映射；B=0 的结果受 AutoMask/BlendMode 影响。`ControlMaskSubrectBaseX/Y/Width/Height` 是 Evaluate 参数；研究实现按遮罩整张尺寸传 `(0,0,width,height)`。

同一研究称 310.8 有遮罩就强制 AutoMask 关闭；310.9.16 由 `DLSSNR.MaskBlendMode` 控制：0 关闭 AutoMask；1 保留 AutoMask 并对 `mask.b × LocalStructure` 与 NVIDIA skin/structure 值取平均；2 取 min；3 取 max。遮罩不存在则研究构建传空资源；全 1 遮罩在 mode 0/2 与无遮罩结果落在报告的捕获噪声范围内，不能据此假定 mode 1/3 下也完全中性。**“中性遮罩”定义为全 1 RGBA（alpha 任意），不是全 0。** 全零是有意强遮罩。

报告的游戏冻结画面对 R/G/B 和天空规则提供作者测量；运动规则明确标成未测。报告未证明 A/B 两模型分别对每个通道与 BlendMode 的响应一致，也没有 NVIDIA 发布的 ControlMask API 合同。公开[研究补丁](https://github.com/GrimsVerk/nr-runtime-research/blob/main/research-build-source.patch)在 OptiScaler NR shader 增加 mode 14，以 `R8G8B8A8_UNORM` 在模型工作尺寸每帧生成，再在同一 D3D12 command list 上转为 SRV、执行 NGX Evaluate、最后转回 UAV；尺寸变化时替换资源。其 0.13 ms／4K 测量是该作者机器、模型与计时口径，不作为 Nexus 成本预测。

[OptiScaler 分支](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)、[GITC Uplift](https://github.com/ghostinthecamera/GITC-Uplift) 与 [DLSS5 Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) 展示了各自较宽的管线、设置或 mask 概念；它们不证明 Nexus 路径兼容。GITC README 的跨 API 声明不等于 ControlMask 在每个 API 上均实测。Visual Enhancer README 只说明支持自定义 ControlMask，未提供本设计需要的独立原生语义证据。优先依据研究页、补丁和 Nexus 当前源码，不把其他产品的架构搬入 Nexus。

许可证：研究补丁标注 MIT，但其 diff 改动的是标为 [GPL-3.0 的 OptiScaler 分支](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/blob/main/LICENSE)中的文件；补丁标注不自动重新许可被修改的上游代码。P0 只借鉴公开行为描述，自行编写 shader 和资源代码，不复制补丁代码或 shader。若后续确需改编 OptiScaler/GPL 文件，须先逐文件确认来源、许可证兼容性、NOTICE 和再分发义务，再决定是否纳入。

## 方案比较与推荐

| 方案 | 做法 | 优点 | 代价／限制 |
| --- | --- | --- | --- |
| A：工作集内 GPU 生成（推荐） | 在 `ProcessInline` 当前 command list、Upscaled Evaluate 前，对工作集的一张 `R8G8B8A8_UNORM` 纹理运行小型生成 shader；P0 写全常量 RGB/全 1/诊断局部块。随后绑定 NGX，Evaluate 后恢复工作状态。 | 与现有资源、queue/fence lease 和 GPU timer 同源；无 CPU readback、无逐帧上传；可自然延伸 P1 深度/运动规则。 | 每帧 dispatch 和屏障；需为新 shader 加载、失败和 descriptor 资源做窄范围接线。固定 mask 可不变更内容，但仍需逐帧处理状态并绑定。 |
| B：CPU 生成并上传固定图 | CPU 建 RGBA 字节并在每帧通过 upload buffer/copy 更新 GPU mask。 | P0 固定图逻辑直观；不增加计算 shader。 | 上传、行距和 copy 状态增加实现路径；每帧复制浪费带宽；动态 P1 仍要重写 GPU 生成；upload buffer 与读资源的生命周期更复杂。1×1 广播/任意 subrect 未被验证，不能依赖。 |

采用 A，但 P0 shader 只生成固定/中性/通道探针图，不掺入深度、亮度或运动逻辑。遮罩宽高等于当帧 `resources.proxy` 的实际输入工作尺寸；完整纹理 subrect `(0,0,width,height)`，RGBA8 UNORM。固定配置在尺寸、模式或常量改变时更新；P0 为降低状态复杂度可每帧 dispatch 同一常量图。P1 再评估仅在源帧/参数变化时更新，不能因静态场景假设跳过动态数据依赖。

生成和 Evaluate 必须记录到 `ProcessInline` 当前 D3D12 command list，使用其当前 queue 顺序：遮罩 UAV 写入 → UAV barrier/转为 shader resource → NGX Evaluate → 归还可写状态。不得额外提交独立队列、CPU 等待或跨队列交接。评估确切状态转换应服从 Nexus 现有 workset/descriptor helper。Evaluate 已将遮罩交给 Runtime 后，CPU 不能覆写或释放它；逐帧同队列命令顺序保证 GPU 读完再处理后续写入。尺寸变化通过工作集 key 创建新尺寸资源，旧资源依现成 fence retirement 保留到所有 lease 完成；不要复用仍在飞行中的同一纹理去 CPU 更新。

新建遮罩只读 `resources.proxy`，不写 Proxy、NR 输出、游戏源输入、历史、HDR exposure 或 Result/Look 编辑输入。NR 仍只调用原有 Feature/slot，继续使用原 History reset epoch。遮罩只在最终 Evaluate 参数块绑定，绝不在 CreateFeature 保存为持久状态。创建、shader dispatch 或资源状态准备失败时，本帧不传半成品 mask：记录一次诊断、按现有失败安全路径保留原游戏 DLSS 输出/跳过 Nexus NR，并标记下一次 NR history reset；**不在同帧对同一 Feature 先带 mask 后无 mask 重试 Evaluate**，以免引入双推理及不确定 History 副作用。队列 Execute/Signal 失败沿用现有 GPU 提交失败/资源退休处理，不假报成功，停止使用该 mask 并回到现有安全行为。

## P0 参数与执行契约

- 新能力以独立、缺省 false 的本地试验开关启用；缺键等同关闭。现有玩家配置不迁移，不加 UI、预设编辑或逐 Pass 配置。只准 D3D12 Upscaled、单 Pass，Runtime 精确为 310.9.16.0；模型 A、B 分开轮测。其他版本/API/Stage/多 Pass 不传新参数，保持旧执行路径。版本信息缺失或不精确时拒绝启用并给一次性原因日志。
- 开启时：每次实际 Evaluate 为参数块设置本帧 mask resource、Subrect base `(0,0)` 和完整宽高，并设置诊断用 `DLSSNR.MaskBlendMode` 0–3。AutoMask 仍按当前 pass 配置显式发送；mode 0/1/2/3 由 Runtime 依上述报告解释。每个 RGB 探针应只改变对应通道，`A` 固定 255。
- 关闭或回退时：如果该可复用参数块曾绑定 mask，显式清空 resource 与 subrect，防止跨帧残留；否则保持原始键集合和参数完全不变。AutoMask 保留当前配置，不为“遮罩中性”暗改原配置。MaskBlendMode 未启用时不写。
- P0 输入种类：无输入遮罩（原生基线）、中性全 1、固定 RGB 可调常量、局部矩形置零/中间值探针。用 R/G/B 分别单通道探针验证语义；不引入贴图文件加载。
- 分配费用为每工作集一个 `width × height × 4` 字节的 RGBA8 图，额外临时资源仅沿用现有 shader descriptor/workset 分配。记录当前纹理尺寸、估算 allocation bytes、分配失败、提交/完成和 mask-enabled 状态；避免输出游戏图像或私人路径。

## 预计源码范围

- `dlssnr.hpp`：P0 缺省关闭开关（诊断/本地配置，不进 UI）、`FinalResources` 遮罩所有权、`EnsureWorkset` 尺寸分配、`ProcessInline` 生成/状态转换/失败分流、`SetEvaluationParameters` 绑定/清除参数与兼容门控。按实际结构最小接入，不新建跨 API 通用子系统。
- `shaders/`：一个专用常量遮罩生成 shader，输出 RGBA8 UNORM；复用现有编译和注册约定。P0 不加入天空/暗部/运动计算。
- `tests/stage_execution_cases.hpp`、`tests/same_stage_resolution_cases.hpp` 或相应 GPU fixture：真实 D3D12 资源、UAV/SRV 顺序、尺寸重建、提交失败和 fence 退休验证；`tests/gpu_timer_cases.hpp`/已有 GPU 时间戳入口测量开销。若测试接入需要新入口，再按既有 suite 选择最小改动。

## P0 自动化和人工验收矩阵

| 场景 | 自动化/证据 | 通过条件 |
| --- | --- | --- |
| 默认关闭／旧配置缺键 | 配置及 Evaluate 参数夹具 | 参数、GPU 提交数、输出路径与基线一致；无 mask 资源分配。
| 无 mask vs 全 1 | D3D12 参数夹具及 NVIDIA 310.9.16.0 实测 | 参数空值与全 1 完整 subrect 正确；重点比较 mode 0/2（研究报告的噪声范围结论），mode 1/3 单独记录，不预设中性。
| RGB 通道独立 | 全 1、中间值 0.5、单独 R/G/B=0 与局部矩形零值 | 资源读回确认字节、尺寸/坐标正确；NVIDIA 实测中 R 改混合量，G 改局部 tone 输入，B 在不同 BlendMode 下按预期影响 structure。A 固定不变。
| AutoMask × BlendMode | AutoMask 开/关 × mode 0/1/2/3，并测 A/B 各自 | 记录 Runtime、模型、参数与输出差；静态实测覆盖全部组合。替身只验正确写参，不断言 NVIDIA 图像语义。
| 尺寸改变/分配失败 | 同一 feature 输入工作尺寸改变、资源创建故障注入 | 新尺寸分配后旧资源等待 fence 退休；失败不绑定旧尺寸/半初始化资源，旧 DLSS 输出可见，history 正确 reset。
| GPU 录制/提交失败 | dispatch、barrier、Execute/Signal 故障注入 | 不宣称 NR 成功、不释放未完成资源、不在同帧重复 Evaluate；下一帧走既有恢复/退避规则。
| 能力门控 | 310.8、未知/缺版本、DX11、Vulkan、Render、Present、多 Pass | 不写 ControlMask/BlendMode、不建立 mask，明确一次性回退原因，旧路径结果不变。310.8 特别禁止 mask，因 AutoMask 交互不同。
| 成本 | 相同冻结输入的 mask off/on GPU timestamps、workset bytes、至少 1000 稳态帧 CPU frame-time 样本 | 报告 NR GPU 增量中位数/P95、显存字节增量和全帧 P95；按 GPU/分辨率/模型分别记录，不以第三方 0.13 ms 设门槛。
| 画质 | 所有者指定相同存档/静态机位截图，再做移动/转镜动态场景；Off/neutral/R/G/B/BlendMode 配对 | 由项目所有者判断色调、边缘、肤色、细节、闪烁/残影和稳定性。人工通过前不称视觉验收完成。

GPU 替身测试只能证明 Nexus 录制资源及顺序合同；NVIDIA Runtime 实测必须记录实际加载 DLL 身份、Runtime 版本、GPU/驱动、模型 A/B、参数组合和原始采样。P0 不要求自动执行游戏，不扩大现有游戏自动化授权。

## 原P0排除范围、风险与P1／P2规划（历史）

P0 排除 UI、用户遮罩文件、自动分类遮罩、深度/亮度/运动计算、Render/Present、DX11/Vulkan、FG/XeFG/Pre-FG、结果编辑/预设接入、跨队列、双 Pass 遮罩、Runtime 自动探测兼容矩阵及未经验证的 A/B 自动选择。

主要风险：① 当前精确 Runtime 已实测接受参数并确认部分 R／局部语义，但 G/B、AutoMask／BlendMode 及各模型全部组合未验证；② NVIDIA 实机完成与 WARP 生命周期证据已取得，D3D12 验证层仍不可用，启动异常尚未定位，不能据基础用例证明所有 GPU／启动安全条件；③ G 只改网络 tone 输入而不改 Style grade 的社区描述及 AutoMask BlendMode 对 B 的交互仍需专项，误读参数会导致画质变化／边缘闪烁。任何资源、格式、subrect 或 Runtime 行为不满足时关闭试验功能，恢复无 mask 基线。

- **P1（仅 P0 通过后）**：在相同工作集 shader 中逐步加入远景/天空（深度有效性、反转和软化）、暗部（Proxy 亮度）、运动区域（有效 motion 与尺度）；任何导引缺失、非有限/无效数据或尺寸不匹配时整帧回退到无 mask，清空绑定并保留已有 AutoMask 配置。人物/边缘保护需先证明现有输入能稳定区分语义，不能把深度边界直接当人物 mask。测静态和动态 flicker。
- **P2**：逐 Pass mask 参数和资源可在当前逐槽 Feature/History 结构下评估；模型 A/B 分测。Render、Present、DX11/Vulkan、FG/XeFG/Pre-FG 各需独立执行阶段、资源状态与 Runtime 兼容证据，不能从 Upscaled DX12 推广。随后才评估与 Result/Look 编辑及预设系统的用户配置交互。

## 参考

- [GrimsVerk ControlMask 研究及其样本、通道与限制](https://grimsverk.github.io/nr-runtime-research/controlmask.html)
- [研究构建完整源码补丁](https://github.com/GrimsVerk/nr-runtime-research/blob/main/research-build-source.patch)
- [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
- [GITC Uplift](https://github.com/ghostinthecamera/GITC-Uplift)
- [DLSS5 Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer)
- Nexus 当前事实：[`FinalResources` 与 `WorksetKey`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L782)、[`EnsureWorkset`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L6220)、[`CreateScratchTexture`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L5874)、[`DrainRetiredResources`](https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/1e725d0/dlssnr.hpp#L4901)、[`gpu_lease.hpp`](../../gpu_lease.hpp)、[`gpu_timers.hpp`](../../gpu_timers.hpp)、[`tests/stage_execution_cases.hpp`](../../tests/stage_execution_cases.hpp)、[`tests/same_stage_resolution_cases.hpp`](../../tests/same_stage_resolution_cases.hpp)、[`tests/gpu_timer_cases.hpp`](../../tests/gpu_timer_cases.hpp)。
