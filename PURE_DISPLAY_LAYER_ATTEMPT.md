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

### 4. 已确认 Painter 下拉框关键技术事实

- Painter 的下拉框 `QComboBox` 使用 `QComboBoxListView`（QAbstractItemView）。
- 平时 `view->itemDelegate()` 为 `QItemDelegate`；`resolution` / `opticalCamera`
  等个别下拉框为 `QComboBoxDelegate`。
- model 用 `Qt::AccessibleDescriptionRole` 区分行：
  - `section_title`：分组标题，Painter 画成斜体（不是灰色），不可选（disabled）。
  - `section_child`：可选中子项。
- Painter 运行 Qt **6.8.6**；我们的插件用 SDK Qt **6.5.3**。跨小版本 ABI。

## 未完成 / 卡住的问题

### 下拉框弹出列表项无法稳定翻译

目标：Painter 下拉框弹出列表的每个子项（`section_child`，如 "Base color"）
翻译成中文，同时保留 Painter 的分组标题（`section_title`）斜体样式。

遇到的障碍：

1. **drawText 钩子覆盖不到下班列表项**
   `QItemDelegate` / `QStyledItemDelegate` 绘制列表项文本时走
   `QCommonStylePrivate::viewItemDrawText` → `QTextLayout::draw`，
   它**不经过 `QPainter::drawText`**，因此 hook 不到。
   （qcommonstyle.cpp 的 `viewItemDrawText` 直接 `textLayout.draw(p, pos)`。）

2. **替换 delegate 会被 Painter 重置**
   尝试给 `combo->view()` 装自建 `ComboItemDelegate`（继承 QItemDelegate，
   覆写 `drawDisplay` 翻译，section_title 设斜体），但 Painter 在弹出下拉框时
   会把 delegate 换回它自己的 `QItemDelegate`。即使加了 800ms 定时重装，
   抓到的 delegate 仍总是 `QItemDelegate`。

3. **怀疑真正画列表的 view 不是 `combo->view()`**
   `enum_listviews` 曾抓到两个 `QComboBoxListView`（一个 QItemDelegate、一个
   QComboBoxDelegate）。而 popup 打开后 `allWidgets()` 里可见的含
   `section_title` 的 view 都是 `isVisible()==False`。怀疑弹出列表用的是
   另一个通过 `combo->view()` 拿不到、`allWidgets()` 也枚举不到的 view。

### 根本矛盾

Painter 的下拉框：
- 样式由 Painter 私有的 `QItemDelegate` / `QComboBoxDelegate` 绘制（认识
  `section_title` 斜体分组标题），标准 Qt 同类不认 `section_title`。
- 文本绘制走 `QTextLayout`（绕开 drawText 钩子）。

因此要用"纯显示层"翻译 Painter 下拉框，只有两条路：
- **替换 delegate**：能翻译，但丢失 Painter 的 `section_title` 斜体样式
  （或者 Painter 在 popup 时重置，替换不稳定）。
- **hook 更底层**：`QTextLayout::draw` 最终调 `QPainter::drawTextItem`，
  但它拿到的是只读的 `QTextItem`（单个字形片段），改不了文本。

### 已排除的方案

- 继承标准 `QComboBoxDelegate`：它只处理 `separator`，不认 `section_title`，
  且存在私有头 `qcombobox_p.h` 依赖。
- 直接改 Painter 的原版 delegate 的 vtable（`drawDisplay` 槽）：
  依赖跨 Qt 版本（6.8.6 vs 6.5.3）的 vtable 布局一致，风险高。

## 下一步可能的方向

1. **放弃纯显示层，用 `setItemText` 改 model 显示值**
   已验证 Painter 下拉框 model 是 `QStandardItemModel`，item `ItemIsEditable`，
   `setItemText` 可写。Painter 的 delegate 会照它自己的样式画中文（含
   section_title 斜体），这样翻译 + 样式都能保住；
   代价是它不是纯显示层（改 model 显示值），关开关需把原文恢复回 model。

2. **继续定位 Painter 真正画下拉列表的 view / delegate**
   需要确认 Painter 弹出列表用的是标准 `QComboBox` popup 还是自绘弹出窗，
   从而找到能稳定替换 / 包装的 delegate 实例。

## 索引

- 主引擎源码：`source/cpp/translation_ui_delegate.cpp`
- 构建脚本：`source/cpp/build_package.py`
- 本次涉及文件：`translation_ui_delegate.cpp`、`CMakeLists.txt`、
  `__init__.py`（删除样式探测 `_style_probe`）、`dist/*.zip`
