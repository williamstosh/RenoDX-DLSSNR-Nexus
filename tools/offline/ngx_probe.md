# 独立 NGX bounded prototype

这是独立离屏探针，不接入生产插件，不启动游戏。入口是 `ngx_probe.cpp`；复用统一
`build.ps1 -Backend ngx` 构建，不另设构建／运行包装器。

## 接口与依赖

```text
offline_ngx_tests.exe --runtime-dir <显式本地目录> --output <新空目录> [--timeout-ms 20000]
```

输出 `ngx-probe-result.json`，schema 为 `nr-nexus-offline-ngx-probe/1`。退出码
`2` 为已生成证据但 Feature18 可行性仍 blocked，`1` 为参数／探针故障；本原型不会
把流程正常退出标成 Feature18 pass。新目录或已有空目录均可，已有非空目录拒绝覆盖。
结果只保留 Runtime 的 basename、SHA-256、版本和导出名，不记录私有路径或异常原文。
Runtime 自己可能写入的日志属于本地证据，未经检查不能公开。

依赖：Windows SDK D3D12／DXGI／WRL、既有 NVIDIA NGX 头文件和 nlohmann JSON
头文件。链接 `d3d12.lib dxgi.lib bcrypt.lib version.lib`。无 NGX import library、
新增安装或 Runtime 复制／发布。使用 `tests/test-support.ps1` 的
`Initialize-TestBuild` 可直接准备既有 MSVC 环境。

## 阶段与边界

1. 只检查显式目录中 `nvngx_dlssnr.dll`、`nvngx.dll`、`_nvngx.dll` 三个文件。
   哈希／版本及 named export inspection 不执行 DLL 入口或导出，不解析二进制实现。
2. 创建实际 NVIDIA D3D12 硬件设备，不创建窗口、交换链或游戏进程，不回退 WARP。
3. 公共初始化候选须同时提供 `Init_with_ProjectID`、capability、DestroyParameters
   和 Shutdown1。导出名检查仅是候选筛选，不能证明 SDK/Core ABI 匹配；同名函数在
   SDK/Core/Snippet 层可能有不同签名。任何文件名均不作为 ABI 证据，候选只标 STATIC。
4. 已移除按导出名自动调用公共初始化／capability 的分支。当前没有经过验证的 SDK
   provider，因此 initialize／capability 均未尝试，原因是 `sdk_provider_abi_unverified`；
   此探针不完成 NGX 初始化，也不调用 Runtime 导出。后续路线是链接真实官方 SDK 并
   通过有明确类型和来源的适配实现初始化，另行验证 ABI 与输入契约；不以用户确认勾选
   代替证明，不使用游戏 AppID。没有公开 Feature18 capability key，availability 保留 null。
5. Feature18 Create／Evaluate 明确 blocked。未猜测私有初始化参数、颜色／深度／运动
   输入格式、资源状态、回调、模型／历史与授权规则。生产代码中的 `DLSSNR.*` 参数与
   私有 Init_Ext 路由只是现存实现事实，不等于 NVIDIA 公开保证。
6. 独立 worker 进程由探针自身施加 100–60000 ms 总时限，默认 20000 ms，覆盖文件检查、
   导出映射检查与实际 D3D12 设备创建。执行阶段先落盘 running，超时／崩溃保留最后阶段
   并标 blocked。强制终止不等于正常清理成功，也不能保证驱动内部工作已停止。

`created` 只有非空 handle 与成功 Create 返回才能成立；`evaluated` 须实际成功 API
返回；`gpu_completed` 还须将对应命令提交到非 copy 队列，并获得真实 GPU fence 完成
且无 device removal 的证据。这三项分别表达不同层次，本原型均为 false。设备创建或
capability 成功不代表模型可用，API 接受也不代表 GPU 已完成或输出正确。

## 定向测试

设置 `NR_NEXUS_NGX_PROBE_EXE` 为已构建探针，执行
`python -m unittest discover -s tests -p test_offline_ngx_probe.py -v`。可选显式设置
`NR_NEXUS_NGX_RUNTIME_DIR` 以核对已有本地 Runtime 身份与实际导出、短 deadline。
没有 Runtime 时这两项跳过，不能报告已测。每轮回归在 `tmp/offline/offline_ngx_test_*`
下创建唯一证据目录，按测试项保存输入和结果，打印实际路径并保留，不自动清理；父目录
按需创建。不模拟 NGX 成功，不安装或复制 DLL。

## 官方来源与摘要（核验于 2026-11-05）

- NVIDIA 固定 SDK 头文件：
  <https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/include/nvsdk_ngx.h>
  SDK／Core／snippet 可能具有同名不同签名；ProjectID 允许自有 GUID、CUSTOM engine。
  D3D12 Create 要求 open recording list、正确 device node、非 copy queue；具体
  feature 参数仍须查该 feature 文档。此次没有找到 Feature18 专用公开契约。
- NVIDIA feature enum：
  <https://github.com/NVIDIA/DLSS/blob/a291cc7d2cc642a51566f3dfd5376f635cd1b284/include/nvsdk_ngx_defs.h>
  18 仍名为 `NVSDK_NGX_Feature_Reserved18`。这不证明不可实现，也不授权推断私有契约。
- Microsoft DLL loader：
  <https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw>
  `DONT_RESOLVE_DLL_REFERENCES` 不执行 DllMain 或加载导入；仅用于独立 worker 的导出
  检查，检查后释放映射；当前不会再以可执行方式加载 NGX DLL 或调用其导出。
- Microsoft D3D12 queue signal：
  <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal>
  queue Signal 在 GPU 侧设置 fence，须另外等待／核实完成；CPU API 返回值不提供该证明。

当前结论仅为“本地 DLL＋固定 SDK 的公开契约路线 BLOCKED，未 Create／Evaluate”，
不判断独立调用在原理上不可实现。以上只引用既有 SDK 头文件及官方 API，没有复制第三方
实现或随附 Runtime。
