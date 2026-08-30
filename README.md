# Substance 3D Painter / Designer 中文翻译补全插件

本插件用于为 Windows 版 Adobe Substance 3D Painter（SP）与 Adobe Substance 3D Designer（SD）补全中文界面。

插件会读取自带词库与用户添加的中文词库，对官方汉化尚未覆盖的菜单、按钮、标签、选项卡、下拉选项、参数名称、资源库目录树、资产名称以及 Designer 节点图文字进行补充翻译。

插件只改变界面显示，不覆盖软件已有的中文内容，也不修改资产本体或项目数据。

## 功能特性

- **界面补翻译**：自动翻译菜单、按钮、标签、选项卡、下拉选项、参数名称、资源库目录树和资源列表。
- **资源库中文搜索**：Painter 与 Designer 的资源库搜索框支持输入中文，可同时匹配英文原文和中文翻译。
- **动态刷新**：界面变化或参数拖动时实时补充翻译。
- **悬停原文**：鼠标悬停在插件翻译过的文字上时，可查看对应英文原文。
- **资源悬停预览翻译**：资源库中资源、修改器、滤镜、材质等悬停预览会同时显示中文翻译。
- **图层面板翻译**：Painter 专用，可翻译图层面板及用户创建的图层名称。
- **节点图翻译**：Designer 实验性功能，默认关闭，启用后翻译节点标题与输入、输出端口标签。
- **即时开关**：取消启用后可立即恢复全部界面原文。
- **直接改词**：可通过快捷方式打开“更改翻译”窗口，修改结果写入对应词库。
- **词条提取工具**：使用独立 C++ 提取器扫描资源文件与元数据，批量生成待翻译词库。
- **导出未翻译名称**：读取软件资源库中尚未翻译的资产名称，并导出为可编辑词库。
- **在线更新**：从 GitHub Releases 获取正式版本并原地更新，无需手动替换文件。

## 支持环境

- Windows 10 或 Windows 11，64 位。
- Substance 3D Painter 7.2 至最新版。
- Substance 3D Designer 14 及以上版本。

Painter 7.2 至 10.0 使用 Qt5 引擎，Painter 10.1 及以上版本与 Designer 14 及以上版本使用 Qt6 引擎。

## 安装方法

同一个发布包 `substance3d_chinese_translator.zip` 适用于两个软件。

### Substance 3D Painter

将发布包解压到以下目录：

```text
C:\Users\用户名\Documents\Adobe\Adobe Substance 3D Painter\python\plugins
```

解压后应存在以下文件夹：

```text
...\python\plugins\substance3d_chinese_translator
```

启动 Painter 后，打开菜单 `Python`，勾选“中文翻译补全插件”。

### Substance 3D Designer

将发布包解压到以下目录：

```text
C:\Users\用户名\Documents\Adobe\Adobe Substance 3D Designer\python\sduserplugins
```

解压后应存在以下文件夹：

```text
...\python\sduserplugins\substance3d_chinese_translator
```

启动 Designer 后插件会自动加载，并在菜单栏出现“中文翻译工具”。如果没有出现，请在工具或插件管理器中确认插件已勾选。

手动安装或替换插件目录前，请先完全退出对应软件。插件自带的“检查插件更新”功能无需退出软件。

## 快速使用

插件启用后会自动加载词库并翻译当前界面。

- **查看原文**：将鼠标悬停在已翻译文字上。
- **修改翻译**：按住 `Ctrl` 并点击鼠标右键，打开“更改翻译”窗口。
- **控制开关**：在“中文翻译工具”窗口中调整翻译总开关、图层面板翻译和模糊匹配等选项。

## 中文翻译工具

打开方式：

- Painter：菜单 `Python → 中文翻译工具`。
- Designer：菜单栏“中文翻译工具”。

工具窗口包含以下分区。

### 界面翻译设置

- **启用插件翻译**：总开关，默认开启。
- **翻译图层面板（包括用户创建的图层名称）**：Painter 专用。
- **启用节点图翻译**：Designer 实验性功能，默认关闭。
- **启用模糊匹配**：精确匹配优先，同时兼容大小写、全半角、下划线和空格等差异。
- **启用全量扫描兜底**：默认关闭，每 10 秒扫描一次可见控件，适合有漏翻时临时开启。
- **检查插件更新**：用于检查并下载新版本。

### 快捷键设置

- **启用、禁用插件翻译**：默认快捷键为 `F10`。
- **更改翻译弹窗**：默认触发方式为 `Ctrl + 鼠标右键`。

点击对应按钮后，按下新的快捷键或组合键即可修改。按 `Esc` 取消监听，按 `Backspace` 恢复默认设置。

### 词条提取

在“提取设置”中选择资产目录和输出 JSON 路径，在“提取选项”中选择需要提取的内容，然后点击“开始提取”。

可提取的词条属性包括：

- `label`
- `text`
- `group`
- `description`
- `category`
- `keywords`
- `values`
- `description_disabled`

还可以选择是否提取普通文件名和文件夹名。

提取完成后会生成可直接编辑的 `*_zh.json` 词库。如果输出文件已经存在，已有译文会被保留，新词条会以空译文加入。

### 提取规则

提取器会递归扫描资产目录，并跳过以下内容：

- 以 `.` 开头的隐藏目录，例如 `.git`。
- `__pycache__` 目录。
- `_unpacked_assets` 目录。

提取器支持以下格式：

- `.sbs`
- `.sbsar`
- `.spsm`
- `.sppr`
- `.spp`
- `.sbsprs`
- `.sbsasm`
- `.zip`
- `.7z`
- GLSL 相关文件，例如 `.glsl`、`.glslfx`、`.vert`、`.frag`

以下内容不会被提取：

- 已包含中文的原文。
- 纯整数、小数、科学计数法或百分比数值。
- 内部资源引用 URL。
- 插件现有词库中已经存在的原文。
- 超过大小上限的解析文件。

每个文件无论成功或失败，都会输出到面板日志。失败项还会写入输出文件旁的 `_failures.txt`。

### 导出资产库未翻译名称

点击“导出资产库未翻译名称”后，插件会读取当前软件资源库中尚未翻译的名称，并保存为可编辑的 `*_zh.json` 词库。

- Painter：遍历所有资产架资源。
- Designer：遍历界面资源库的目录树和资源列表。

已存在中文译文的名称不会重复导出。

## 在线更新

“检查插件更新”按钮通过 GitHub Releases API 查询最新正式版本。

更新流程如下：

1. 如果已是最新版本，会提示“已是最新版本”。
2. 如果正在提取词条，会提示先完成或取消提取。
3. 发现新版本后，会先确认，再显示可取消的下载进度窗口。
4. 下载完成后会校验 GitHub Release 资产摘要、文件清单、版本、CRC、路径和解压大小上限。
5. 校验通过后原地替换插件文件。
6. 应用完成后提示重启软件以启用新版本。
7. 重启后会提示更新成功，并自动清理临时备份和更新残留。

如果更新失败，插件会自动回滚旧版本，并保留安装包。用户自建词库会原样保留；更新包自带的以下词库会直接替换为最新版本：

- `official_assets_zh.json`
- `my_assets_zh.json`
- `control_ids_zh.json`

## 词库格式

插件会按文件名排序加载 `translations` 文件夹下所有 `*_zh.json` 词库。后加载的词库会覆盖同名词条。

全局词库格式如下：

```json
{
  "$schema": "sp-translation-v1",
  "id": "my-translations",
  "language": "zh-CN",
  "description": "My translations",
  "translations": {
    "English source": "中文翻译"
  }
}
```

控件 ID 专属词库放在 `control_ids_zh.json` 中，键为完整控件 ID：

```text
上级类名||自身类名||自身 objectName||原文
```

示例：

```json
{
  "$schema": "sp-translation-v1",
  "id": "control-id-specific-translations",
  "language": "zh-CN",
  "translations": {
    "QToolButton||QMenu||blendingModeMenu||Normal": "正常",
    "QToolButton||QMenu||blendingModeMenu||Multiply": "正片叠底"
  }
}
```

翻译查找顺序为：

1. 控件 ID 专属词库。
2. 全局词库。
3. 模糊匹配兜底。

词库要求如下：

- 文件名以 `_zh.json` 结尾。
- 文件编码为 UTF-8。
- `$schema` 为 `sp-translation-v1`。
- `language` 为 `zh-CN`。
- 空译文不会加载，可作为待翻译占位符。

修改词库后，重新加载插件或重启软件即可生效。

## 工作原理

插件由三层组成：

- **Python 层**：负责生命周期管理、词库加载、工具窗口、提取任务调度和在线更新。
- **原生翻译引擎**：以 Qt5 或 Qt6 DLL 形式运行，负责安装事件过滤器、列表项绘制委托和实时翻译。
- **独立词条提取器**：脱离宿主进程运行，负责解析容器与元数据格式。

三层之间通过 C API 和 JSON 文件通信。

## 开发与构建

### 仓库结构

```text
sp插件/
├─ sdks/                               随仓库捆绑的构建依赖
├─ source/
│  ├─ __init__.py                      Painter 入口
│  ├─ pluginInfo.json                  Designer 插件元数据
│  ├─ substance3d_chinese_translator/  合并后的 Python 模块
│  ├─ cpp/                             原生 C++ 引擎与构建脚本
│  ├─ native/                          Qt5、Qt6 翻译 DLL 与提取器 EXE
│  └─ translations/                    词库目录
├─ dist/                               构建输出目录
└─ README.md
```

### 构建要求

- Windows 64 位。
- CMake。
- Visual Studio 2022 Build Tools，包含 MSVC C++ 工具链。

Qt SDK 统一放在工作区 `_ThirdParty/Qt/`：Qt 5.12.5 使用 `msvc2017_64`，Qt 6.5.3 使用 `msvc2019_64`。提取器依赖仍随仓库提供，构建时无需 vcpkg。

### 构建步骤

在仓库根目录运行：

```text
python source/cpp/build_package.py
```

构建脚本会依次完成以下工作：

1. 校验源码与词库。
2. 编译 Qt5 翻译 DLL。
3. 编译 Qt6 翻译 DLL。
4. 编译独立词条提取器 EXE。
5. 生成 `dist/substance3d_chinese_translator.zip`。

任一编译失败都会中止打包，不会发布旧产物。

### 发布新版本

1. 修改 `source/substance3d_chinese_translator/__init__.py` 中的 `PLUGIN_VERSION`。
2. 修改 `source/pluginInfo.json` 中的 `version`，确保两者一致。
3. 运行构建脚本生成发布包。
4. 在 GitHub 创建版本号高于当前版本的新 Release。
5. 将 ZIP 文件作为 `substance3d_chinese_translator.zip` 或 `substance3d_chinese_translator_版本.zip` 上传为附件。

## 兼容性说明

- Painter 7.2 至 10.0 使用 Qt5 引擎。
- Painter 10.1 及以上版本与 Designer 14 及以上版本使用 Qt6 引擎。
- Designer 的资源库和节点图识别依赖软件内部控件类名。个别版本升级后，如果内部结构变化，相关面板翻译可能静默失效，但不会导致崩溃。
- 资源库搜索仍按英文原始名称匹配，翻译只作用于显示层。

## 卸载

完全退出对应软件后，删除其插件目录下的 `substance3d_chinese_translator` 文件夹，然后重新启动软件。

## 注意事项

- 手动替换 DLL 或整个插件目录前，必须先完全退出对应软件。
- `Ctrl + 鼠标右键` 会直接修改词库。大量手工调整前，建议先备份相关词库文件。
- 插件只改变界面显示，不修改资产本体或项目数据。
- 请勿对不信任的来源目录执行词条提取。提取器已有大小、嵌套层数和成员数量等限制，但处理恶意构造的压缩包仍可能存在风险。
