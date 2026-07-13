# DiskCleaner

Windows 磁盘清理工具 — 使用 C++ 和 Direct2D 构建的原生 GUI 应用，高效扫描并清理系统垃圾文件。

## 功能一览

支持扫描并清理 **20+ 类** 垃圾文件：

| 类别 | 说明 | 风险 |
|------|------|------|
| 临时文件 | Windows 和用户临时目录 | 低 |
| 回收站 | 清空所有驱动器回收站 | 低 |
| 预读取文件 | 系统预读缓存（保留 7 天内） | 低 |
| 缩略图缓存 | 图片和图标缓存 | 低 |
| Windows 缓存 | 系统缓存和兼容性缓存 | 低 |
| 浏览器缓存 | Chrome / Edge / Firefox / Opera / Brave | 低 |
| 应用缓存 | VS Code、Cursor、Discord、Adobe、Steam 等 | 低 |
| 系统日志 | Windows 日志文件（7 天以上） | 低 |
| 最近文档 | 文件访问历史记录 | 低 |
| 错误报告 | Windows 错误报告文件 | 低 |
| DirectX 缓存 | 显卡着色器缓存 | 低 |
| 显卡缓存 | NVIDIA / AMD / Intel 着色器缓存 | 低 |
| Internet 缓存 | Windows WinINet 缓存 | 低 |
| 崩溃转储 | 应用 CrashDumps 文件 | 低 |
| 安装残留 | Windows 安装和升级残留文件 | 低 |
| 字体缓存 | 系统字体缓存 | 低 |
| 更新缓存 | Windows Update 下载文件 | ⚠️ |
| 休眠文件 | 禁用休眠并删除 hiberfil.sys | ⚠️ |
| 系统还原点 | 清理旧的还原点 | ⚠️ |
| Windows.old | 旧系统备份文件夹 | ⚠️ |
| 转储文件 | 蓝屏分析转储文件 | ⚠️ |
| 传递优化 | 更新分发缓存 | ⚠️ |
| 事件日志 | 系统事件日志 | ⚠️ |
| 安装缓存 | Installer 补丁缓存（慎用） | ⚠️ |

> ⚠️ 标注项为高风险操作，请谨慎勾选。

## 使用

1. **以管理员身份运行**（部分清理项需要管理员权限）
2. 勾选要清理的项目
3. 点击「扫描」查看可释放空间
4. 点击「清理」执行清理

## 构建

- Visual Studio 2019 或更高版本
- Windows SDK 10.0+
- 依赖：Direct2D、DirectWrite、WIC、Shell API

打开 `DiskCleanerCpp.sln` 直接编译即可；Release 输出为 `build\Release\DiskCleaner.exe`。

## 技术栈

- C++17
- Win32 API + Direct2D 渲染
- 无第三方依赖
