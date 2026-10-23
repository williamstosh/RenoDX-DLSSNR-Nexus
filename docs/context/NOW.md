# 当前状态

最后核验：2026-10-24，范围为新仓库源码状态、文档职责及语言规则。本页只维护 NR Nexus 状态。

## 源码与集成

- 源码基线为 Lecram V8.5 RC10 导入提交 `9bb6c0f`，独立项目标识及版本工具由 `db747cc` 引入。
  独立版本以 [version.h](../../version.h) 为准；仓库设有 `main` 和 `dev`。
- 文档已按中英双语 README、中文项目文档和原文许可声明划分职责，语言维护规则见 [AGENTS](../../AGENTS.md)。文档整理不改变插件源码、运行时标识或版本。
- 旧项目的共享兼容接口、Present SDR/HDR 修复、后续 NR+FG 集成、对应测试和游戏自动化尚未迁入，原候选的构建或验收不代表本源码已支持。

## 验证状态与缺口

未经修改的 RenoDX 宿主
[`374d07bf0e2fcef112ed1dfee98541bcf16487dd`](https://github.com/clshortfuse/renodx/commit/374d07bf0e2fcef112ed1dfee98541bcf16487dd)
缺少 RC10 调用的五个共享接口：

- `directx::NativeIdentity`
- `directx::SameNativeObject`
- `vtable::TransactionMutex`
- `vtable::BeginTransaction`
- `vtable::transaction_contended`

兼容实现尚未作为已提交成果集成，因此完整插件构建仍被阻塞；不能用任意上游 HEAD 替代兼容宿主。
[迁移记录](../MIGRATION.md#初始化验证)保存初始化历史结果：着色器及独立版本资源探针通过，完整 C++ 构建失败。
当前无合格 Nexus 插件二进制、安装、游戏／视觉验收、标签或 Release；文档修改不改变这些结论。

## 下一里程碑

待旧开发达到认可的交接边界，将已提交、审查和验收的成果及所需宿主兼容接口／测试集成到 `dev`，
再满足[开发目录切换条件](../MIGRATION.md#switching-the-development-directory)。
旧工作区继续维护其进行中任务、详细设计与证据，本仓库不复制动态进度；需要时通过被忽略的本地来源索引定位。
缺少来源或测试工具时先明确缺口，不把恢复资料入口当作功能实现或验证通过。
