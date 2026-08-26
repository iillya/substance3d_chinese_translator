# Substance 下拉框纯翻译层尝试记录

> 本文档记录把 Substance 3D Painter/Designer 的界面翻译改为"纯显示层翻译"
> 这一方向的尝试过程、进度与遇到的问题。本分支保存的是实验代码，**不直接
> 用于发布**；main 分支保持稳定的发布代码。

## 目标

让翻译只发生在"绘制层"，不改动控件 / model 的原始文本：
- 英文原文始终存在控件 `text()` 与 model 的 `DisplayRole` 里。
- 翻译只在 `QPainter` 绘制文本那一刻替换为中文。
- 关闭翻译开关 = 恢复原文 = 触发重绘，不依赖任何"把 unicode 存回属性再恢复"。

已经成功应用到普通控件（QLabel / 按钮 / 菜单 / 选项卡 / 下拉框闭合区等），
它们通过 hook `QPainter::drawText` 实现，样式与 Painter 原生完全一致。

## 已完成

### 1. 撤销 setText 侵入式翻译（已落地）

- `translateWidget` 不再对按钮 / 标签 / 分组 / 下拉 / 选项卡 / 停靠 / 行编辑
  `setText` / `setTitle` / `setPlaceholderText`，也不再写 `kSourceProperty`。
- 删除 `translateMenu` / `translateMenuBar`（setText 菜单翻译）。
- 删除 `restoreTranslatedWidget` / `restoreLayersPanelOriginals` /
  `restoreAllTranslatedWidgets`（setText 恢复链）。
- `sp_delegate_set_enabled` 改为：翻转 `g_enabled` + 重绘；不卸载 drawText 钩子
  时靠绘制层停翻，不 `restoreAllTranslatedWidgets`。
- `setTranslateLayersPanel` 改为仅刷新（重绘），不再恢复 setText。
- `generalPainterTranslation` 已加 `g_enabled` 检查，关闭开关即停翻。

### 2. drawText 钩子增强（已落地）

为覆盖 Painter 大量控件的绘制，补齐了 `QPainter::drawText` 的缺失重载：
- `drawText(const QRectF &, int, const QString &, QRectF *)`
- `drawText(const QPointF &, const QString &, int, int)`

加上原有的 QPoint / QPointF / QRect / QRectF+QTextOption / XY / XYWH，
共覆盖 Painter Qt6Gui.dll 导出的全部 8 个 `drawText` 重载。

### 3. 资源树 / 列表继续走显示层 delegate（保留）

`TranslationItemDelegate`（QStyledItemDelegate 子类）仍在资源树 / 图层面板 /
资源列表上工作，`displayText()` 翻译，不改 model。

### 4. Painter 下拉框显示层翻译（已落地）

- Painter 的下拉框 `QComboBox` 使用 `QComboBoxListView`（QAbstractItemView）。
- 实际 Delegate 类型为 `Alg::DefaultComboBoxDelegate` 和
  `Alg::SectionComboBoxDelegate`；它们未声明自己的 Qt 元信息，因此
  `metaObject()` 只会显示基类 `QItemDelegate`，必须通过 RTTI 区分。
- model 用 `Qt::AccessibleDescriptionRole` 区分行：
  - `section_title`：分组标题，Painter 画成斜体（不是灰色），不可选（disabled）。
  - `section_child`：可选中子项。
- `ComboPaintProxyModel` 仅覆盖绘制用索引的 `DisplayRole`，不替换
  `QComboBox` 或 popup view 的真实 model。
- `ComboPaintDelegate` 不自行绘图，而是把代理索引交给 Painter 原 Delegate
  的 `paint()`；其它角色、分组斜体、缩进、行高、悬停和选中样式全部沿用原版。
- `sizeHint()` 也直接转发给原 Delegate，避免改变布局计算。
- 已在 Painter 11.1.3 / Qt 6.8.6 的 `JadeToad.spp` 中验证：工程可正常加载，
  下拉框中文与原生样式同时保留。

### 已排除的方案

- 替换 popup 的真实 model：破坏 `QComboBox` 与 popup view 的内部模型关系，
  导致下拉框无法打开。
- 用标准 `QItemDelegate` 重绘：能显示中文，但会丢失 Painter 私有 Delegate
  的分组斜体与子项缩进。
- 继承标准 `QComboBoxDelegate`：它只处理 `separator`，不认 `section_title`，
  且存在私有头 `qcombobox_p.h` 依赖。
- 直接改 Painter 的原版 delegate 的 vtable（`drawDisplay` 槽）：
  即使运行时确认 `drawDisplay` 位于槽位 21，复制单对象虚表仍会令 Painter
  11.1.3 启动时报严重错误，因此不采用。
- 广播 `FontChange` 并强制布局/重绘：不能让 `QTextLayout` 改用译文计算。

## 索引

- 主引擎源码：`source/cpp/translation_ui_delegate.cpp`
- 构建脚本：`source/cpp/build_package.py`
- 本次涉及文件：`translation_ui_delegate.cpp`、`CMakeLists.txt`、
  `__init__.py`（删除样式探测 `_style_probe`）、`dist/*.zip`
