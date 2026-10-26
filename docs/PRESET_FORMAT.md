# NR Nexus 预设格式

预设保存插件配置选择，不包含运行时 DLL、模型、游戏数据或测量结果。扩展名为 `.nrnexus.json`，内容为 UTF-8 JSON。例如：

```json
{
  "format": "nr-nexus-preset",
  "schema_version": 1,
  "metadata": {"name": "Example", "description": "A shared look"},
  "scope": "quality",
  "settings": {"NRIntensity": 0.8, "NRStyle": 1, "NRAutoMask": true}
}
```

示例中的数值只是选择示例，不代表内置默认值。`metadata` 可省略；支持字符串 `name`、`author`、`description`、`source_game`、`created_with` 及字符串数组 `tags`。`created_with` 是来源说明，不代替 schema，也不指定要加载的 DLL。文件最多 256 KiB、嵌套最多 8 层、配置最多 512 项；具体字符串长度限制见 [存储实现](../preset_store.hpp)。

## 范围与字段规则

[preset_fields.hpp](../preset_fields.hpp) 是字段名称、分类和数值边界清单；键名区分大小写。`FieldRule::Category` 包含下表五类；`label` 为可交给现有翻译器的英文键，`group` 为显示分组，二者均不是持久化标识。`pass=0` 表示全局或全链，`1` 表示基础层，`2..4` 表示附加层。全局 `NRSkinIndependent` 影响所有层。

| `scope` | 包含内容 |
| --- | --- |
| `quality` | `QUALITY`：基础模型强度、模型、style/tone/structure/skin/mask/UI，以及基础层的全局 Look 结果 |
| `quality-multipass` | 上述画质，加 `MULTIPASS`：`NRPasses`（含基础层的总数）以及当前启用层的模型、强度、Transfer/Color、steering、独立结果和两组继承开关 |
| `full` | 所有非 `EXCLUDED` 字段，包括未启用层保存的独立值、`COMPATIBILITY` 和 `GENERAL`；仅已接入读取／应用入口的字段能够实际恢复 |

画质范围不包含其他 compatibility fixes、Hook/stage、分辨率、快捷键或 diagnostics。基础 `NRTransferStrength`／`NRColorStrength` 归 `QUALITY`，与附加层 `Transfer`／`Color` 形成完整的逐层选择；后者归 `MULTIPASS`。全链 `NRLookUpsample` 随分辨率归兼容组，不是逐层结果参数。当前没有独立 pass 分辨率或 stage，不能合成这类键。

| 画质字段 | loader 数值范围／类型 |
| --- | --- |
| `NRPreset` | 整数 `0..3`；保留历史 `3`，菜单不新增该选项；保存请求值，不替换为运行时回退值 |
| `NRStyle` | 整数 `0..2` |
| `NRIntensity`、`NRLocalTone`、`NRLocalStructure` | 数值 `0..2` |
| `NRTransferStrength`、`NRColorStrength` | 数值 `0..1` |
| `NRSkinStructure` | 数值 `-1..1`，包括 UI 滑条不显示的负值语义 |
| `NRSkinIndependent`、`NRAutoMask`、`NRUICorrection` | boolean |
| `NRLookMode`、`NRLookStabilizeDetail` | boolean |
| `NRLookStrength`、`NRLookBrighten`、`NRLookDarken`、`NRLookColour`、`NRLookHue`、`NRLookMaxColour`、`NRLookShadows`、`NRLookMidtones`、`NRLookHighlights`、`NRLookTone`、`NRLookDetail` | 数值 `0..2` |
| `NRLookMaxBrighten`、`NRLookMaxDarken` | 数值 `0..4` |
| `NRLookHalo`、`NRLookHaloBrightSide` | 数值 `0..1` |
| `NRLookDetailRadius` | 数值 `0.25..4` |
| `NRLookStabilize`、`NRDetailStability` | 整数 `0..2` |
| `NRLookStabilizeMs` | 数值 `10..250`，毫秒 |

`NRPasses` 是整数 `1..4`。每个 `NRPass{2,3,4}` 均展开同一组字段：

| 后缀 | loader 数值范围／类型 |
| --- | --- |
| `Preset` | 整数 `0..2`；`0` 继承主模型，与 steering／结果继承独立 |
| `Intensity`、`LocalTone`、`Structure` | 数值 `0..2` |
| `Transfer`、`Color` | 数值 `0..1` |
| `Style` | 整数 `0..2` |
| `Skin` | 数值 `-1..1` |
| `FollowPass1`、`AutoMask`、`UICorrection`、`LookFollowPass1` | boolean；继承不删除保存的独立值 |
| `LookMode` 至 `DetailStability` 的结果字段 | 与上表对应基础层字段同类型、同范围；没有逐层 `LookUpsample` |

## 完整备份分类

注册表内的兼容项归 `COMPATIBILITY`：Hook/Present 策略、共同分辨率和上采样、HDR codec／来源编码／白点／传递模式／归一化、Depth/MVec、chained history；基础 Transfer/Color 强度按上述规则归画质。其逐键范围以规则表及 [生产 loader](../dlssnr.hpp) 为准。`GENERAL` 包含 `NeuralUplift`、两个快捷键、截图格式／来源／布局／JPEG 质量／大小限制，以及 `NRGpuTimers`、`NRCostMeter`。

以下为 `LoadConfiguration` 在 Setting registry 之外读取的完整清单；不因缺少 `Setting` 而静默当作普通 float：

| 类别 | 字段 | 类型／合法域 |
| --- | --- | --- |
| `COMPATIBILITY` | `EnableHooks` | 整数 `0..2`：关闭、NGX + Streamline、NGX；loader 对其他非零值仍启用 NGX，预设仅接受已定义枚举 |
| `COMPATIBILITY` | `DX11Source` | 字符串 `auto`、`native`、`foreign`、`off` |
| `COMPATIBILITY` | `ForeignNr` | 字符串 `yield`、`observe` |
| `COMPATIBILITY` | `DedupeMode`、`ListStateMode` | 整数 `0..1` |
| `COMPATIBILITY` | `NRMaxWorksets` | 整数 `1..8` |
| `COMPATIBILITY` | `NRWorksetFreePolicy`、`NRDisableRelease`、`NRVkStateRestore` | 整数 `0..1`，不能因只有两个值而改存 boolean |
| `COMPATIBILITY` | `NRShutdownQuiesce`、`NRPresentBlockHudless` | boolean |
| `COMPATIBILITY` | `NRPresentWhiteSource`、`NRPresentPreFgContract`、`NRAutoFgFallback` | 整数 `0..1` |
| `COMPATIBILITY` | `NRPresentPreFg` | 整数 `0..3` |
| `COMPATIBILITY` | `NRPresentMVecUnits`、`NROutputRect` | 整数 `0..2` |
| `COMPATIBILITY` | `NRPresentHdrPedestalPolicy` | 整数 `0..2`；经 HDR policy 入口处理 |
| `COMPATIBILITY` | `NRPresentRestoreScope` | 整数 `0..1`；经 scope 成对写入和回读验证入口处理 |
| `GENERAL` | `NRCrashDump`、`UiWelcomed`、`UiIssueNotice` | boolean；后者控制 HUD 通知 |
| `GENERAL` | `UiHudX`、`UiHudY` | 有限数值 `0..100`，百分比 |
| `GENERAL` | `UiTheme` | 字符串，使用 `ui::kThemes` 的 slug；当前为 `cyber_neon`、`nexus_dark`、`fluent_light`、`midnight_oled` |
| `GENERAL` | `UiLanguage` | 字符串 `auto` 或 `ui::kLanguages` 的语言码，不复制另一份语言枚举 |
| `GENERAL` | `UiMode` | 字符串 `modern`、`classic` |
| `EXCLUDED` | `NRDebugBypassHdrPedestal`、`NRNormTrace`、`NRScreenshotPlanes`、`NRTelemetrySeconds` | 会话诊断，原因见下表 |
| `EXCLUDED` | `NRReportHost` | 网络／第三方上传信息，禁止预设携带 |
| `EXCLUDED` | `NRPreUpscale`、`NRGlobalTone`、`NRSdrBlackRestore` | 旧键，原因见下表 |
| `EXCLUDED` | `ConfigVersion`、`NRPresentSdrRestoreConfigVersion`、`NRPresentHdrPedestalConfigVersion`、`NRPresentRestoreScopeConfigVersion` | 配置 schema 标记，禁止直接导入 |

字符串规则的 `minimum`／`maximum` 为 NaN，表示不适用，不是数值范围。必须先走字符串适配器和枚举白名单；该 NaN 只属于 C++ 元数据，不能序列化到 JSON。加载旧 INI 时允许的未知字符串回退，不代表预设可接受任意字符串。

## 禁止导入与导出

`EXCLUDED` 即使出现在 full 中也不写入或应用。注册表中的 debug 也必须有规则，以区别“已知但禁用”与未知键。

| 字段／对象 | 原因 |
| --- | --- |
| `NRDebugBypassSdrRestore`、`NRDebugBypassHdrPedestal`、`NRDebugBypassHostLook`、`NRPresentModelUI`、`NRDebugGuideMode` | 临时诊断会改变正式处理行为；不能随分享配置传播 |
| `NRDebugHdrCaptureMode`、`NRDebugCapturePlanes`、`NRScreenshotPlanes` | 临时捕获／阶段平面选择，可能产生诊断数据；前者是 UI-only Setting，不由 loader 读取 |
| `NREditTrace`、`NRNormTrace`、`NRTelemetrySeconds` | 调试追踪／遥测输出，不属于可恢复画质 |
| `NRPreUpscale` | 已由 `NRHookPoint` 替代，仅为旧配置迁移读取 |
| `NRGlobalTone` | 已退役、无运行效果，仅读取以告知旧工具用户 |
| `NRSdrBlackRestore` | 仍影响 Legacy／旧路径，但现有生产配置只读、无正式写入入口；不能绕过新 restore policy 约束。full 不保证无损复刻此旧值，需旧 INI 备份或明确的迁移支持 |
| 四个 `*ConfigVersion`／`ConfigVersion` 标记 | 由当前迁移／策略持久化入口生成；不是用户调节值，也不能用导入文件的戳解除只读保护 |
| `NRReportHost`、网络地址、上传目标／凭据／链接 | 不携带上传信息，也不因恢复预设触发网络操作 |
| 路径、宿主配置、环境变量、未知或过时键 | 不属于此白名单；当前 loader 没有可备份的路径字段，不虚构 `NRRuntimePath` 等键 |

## 验证与接入约定

数值必须有限且在闭区间内。枚举和整数不接受小数、boolean 或字符串，不将非法枚举钳制成另一个有效选择；boolean 只接受 JSON `true`／`false`。注册项按现有 `Setting.f/u/b` 判断类型。INI loader 的有限 float 钳制和非法值回退是旧配置修复行为；导入预设应报告非法值，不静默修复后应用。

未知 `settings` 键可作为未应用数据保留并在预览中说明，不能按名称猜测类型或写入 INI；缺失字段保持当前值，不做重置或补默认。未知顶层字段／metadata、重复键、非法类型／数值及未来 `schema_version` 拒绝；未来游戏配置 schema 或只读策略不能被预设覆盖。预设 schema 与 INI `ConfigVersion` 独立。

真实默认只从现有 `Setting::fallback` 读取，当前值从 `Setting::Get()`／实际状态读取。不要用“Defaults view”临时显示的值冒充用户配置；未注册字段复用生产 loader 的现有常量、解析器和状态，不在规则表建立第二套默认值。尤其 `UiTheme` 的缺失值依赖新安装／已有配置分类，不能简单写死一种主题。

应用应复用现有 Store／Persist／SettingEffect 与配置锁、epoch 机制：模型选择与 feature 重建需保持同一 runtime turn；批量变更只触发所需的 history／host-history／recreate，不能逐行反复重建。`NeuralUplift` 启用、共同分辨率、语言和主题分别复用其现有入口；`UiTheme` 应同步主题选择及 `ui::SetTheme`，语言码用现有语言表与 `StoreUiLanguage`，布局写入 `UiMode`。

正式 `NRPresentSdrRestorePolicy`、HDR policy 和 scope 必须遵守各自的 writable／版本保护、锁及 scope 成对回读验证；导入内容不直接提供版本戳。scope 的“键缺失／继承”和 `EnableHooks` 等 attach 策略的“未显式配置”都有语义，完整备份不能把它们错误固化为显式选择。`EnableHooks`、`DX11Source`、`ForeignNr` 属启动期策略，恢复配置后应提示重启，不能声称钩子已热切换。

规则表本身只提供元数据，不实现上述适配器。主线必须为表内非注册字段提供类型、读取、验证、应用和持久化；未接入项应明确显示不支持，不能宣称 full 已完整恢复。完整备份仍只覆盖允许的插件设置，不保证跨游戏、API、SDR/HDR 或运行时版本兼容；建议在预览中明确显示兼容项和重启要求。
