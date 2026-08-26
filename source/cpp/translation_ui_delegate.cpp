#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QIdentityProxyModel>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QHelpEvent>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QCursor>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QAction>
#else
#include <QtWidgets/QAction>
#endif
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QTextOption>
#include <QtGui/QTextDocument>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractItemDelegate>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QAbstractSlider>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QGraphicsObject>
#include <QtWidgets/QItemDelegate>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

#include <typeinfo>
#include <vector>
#include "extraction_rules.h"


namespace {
QHash<QString, QString> g_translations;
QHash<QString, QString> g_originals;
QHash<QString, QString> g_idTranslations;
QString g_fallbackPath;
QString g_idTranslationPath;
QPointer<QWidget> g_originalTooltipOwner;

struct AssetTooltipContext {
    QPointer<QAbstractItemView> view;
    QPersistentModelIndex index;
    QString source;
    QString translation;
    QPoint globalPosition;
    qint64 createdAt = 0;
    quint64 generation = 0;

    bool isValid() const {
        return view && index.isValid() && !source.isEmpty() &&
               !translation.isEmpty() && generation != 0;
    }
};

AssetTooltipContext g_assetTooltipContext;
quint64 g_assetTooltipGeneration = 0;
bool g_enabled = true;
bool g_translateDesignerGraph = false;
bool g_translateLayersPanel = true;
bool g_fuzzyMatchEnabled = true;
// Keep the mouse trigger as canonical text.  On Qt 6.5,
// QKeySequence("Ctrl").isEmpty() is false but toString() returns an empty
// string, while "Shift" and normal key sequences round-trip correctly.  A
// QKeySequence round-trip therefore makes Ctrl+mouse impossible to match.
QString g_editKey = QStringLiteral("Ctrl");
int g_heldEditKey = 0;
Qt::MouseButton g_editButton = Qt::RightButton;
QKeySequence g_enableShortcut;
// 快捷键改为“松开时触发一次”：按下时只记录，KeyRelease 时再触发，
// 避免按住 F10/F9 等键时自动重复事件让回调风暴式反复执行。
int g_enableShortcutArmed = 0;
qint64 g_lastEnableFireMs = 0;
// 编辑弹窗打开期间忽略新的编辑触发，避免按住组合键连点鼠标时
// 叠出多个“更改翻译”窗口。
bool g_editDialogOpen = false;
using ShortcutCallback = void (*)(int);
ShortcutCallback g_shortcutCallback = nullptr;
using DictionaryReloadCallback = int (*)();
DictionaryReloadCallback g_dictionaryReloadCallback = nullptr;

bool shortcutMatches(const QKeySequence &target, int key,
                     Qt::KeyboardModifiers modifiers) {
    if (target.isEmpty())
        return false;
    return target.matches(
               QKeySequence(key | static_cast<int>(modifiers)))
           == QKeySequence::ExactMatch;
}

bool appClosingDown();

void shortcutDiag(const QString &line) {
#if defined(SD_TRANSLATION_SHORTCUT_DIAGNOSTICS)
    QFile out(QDir::temp().filePath(QStringLiteral("sp_shortcut_diag.log")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&out);
        stream << QDateTime::currentMSecsSinceEpoch() << " " << line << "\n";
    }
#else
    Q_UNUSED(line);
#endif
}

void tooltipDiag(const QString &line) {
#if defined(SD_TRANSLATION_TOOLTIP_DIAGNOSTICS)
    QFile out(QDir::temp().filePath(QStringLiteral("sp_tooltip_diag.log")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&out);
        stream << QDateTime::currentMSecsSinceEpoch() << " " << line << "\n";
    }
#else
    Q_UNUSED(line);
#endif
}

void fireShortcut() {
    // 应用正在关闭时，即使有排队中的回调也不再调用 Python，
    // 避免 Python 模块卸载过程中被回调触发导致 shiboken 崩溃。
    if (appClosingDown())
        return;
    // 防抖：无论事件如何重复，同一快捷键 100ms 内只触发一次。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - g_lastEnableFireMs < 100)
        return;
    g_lastEnableFireMs = now;
    shortcutDiag(QStringLiteral("FIRE enable-shortcut"));
    if (g_shortcutCallback)
        g_shortcutCallback(0);
}

// 判断 Qt 按键是否在物理上仍处于按下状态。用于“按键+鼠标”组合触发：
// 只凭最后一次 KeyPress 的记录判断会产生残留状态，松开后的普通单击
// 也会被误判为组合键（例如设置 Z+左键后，单个左键误弹“更改翻译”）。
bool heldKeyIsDown(int qtKey) {
    UINT vk = 0;
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        vk = static_cast<UINT>('A' + (qtKey - Qt::Key_A));
    } else if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        vk = static_cast<UINT>('0' + (qtKey - Qt::Key_0));
    } else if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        vk = VK_F1 + static_cast<UINT>(qtKey - Qt::Key_F1);
    } else {
        switch (qtKey) {
        case Qt::Key_Escape: vk = VK_ESCAPE; break;
        case Qt::Key_Tab:
        case Qt::Key_Backtab: vk = VK_TAB; break;
        case Qt::Key_Return:
        case Qt::Key_Enter: vk = VK_RETURN; break;
        case Qt::Key_Backspace: vk = VK_BACK; break;
        case Qt::Key_Delete: vk = VK_DELETE; break;
        case Qt::Key_Insert: vk = VK_INSERT; break;
        case Qt::Key_Home: vk = VK_HOME; break;
        case Qt::Key_End: vk = VK_END; break;
        case Qt::Key_PageUp: vk = VK_PRIOR; break;
        case Qt::Key_PageDown: vk = VK_NEXT; break;
        case Qt::Key_Left: vk = VK_LEFT; break;
        case Qt::Key_Right: vk = VK_RIGHT; break;
        case Qt::Key_Up: vk = VK_UP; break;
        case Qt::Key_Down: vk = VK_DOWN; break;
        case Qt::Key_Space: vk = VK_SPACE; break;
        case Qt::Key_Minus: vk = VK_OEM_MINUS; break;
        case Qt::Key_Equal: vk = VK_OEM_PLUS; break;
        case Qt::Key_BracketLeft: vk = VK_OEM_4; break;
        case Qt::Key_BracketRight: vk = VK_OEM_6; break;
        case Qt::Key_Semicolon: vk = VK_OEM_1; break;
        case Qt::Key_Apostrophe: vk = VK_OEM_7; break;
        case Qt::Key_Comma: vk = VK_OEM_COMMA; break;
        case Qt::Key_Period: vk = VK_OEM_PERIOD; break;
        case Qt::Key_Slash: vk = VK_OEM_2; break;
        case Qt::Key_Backslash: vk = VK_OEM_5; break;
        case Qt::Key_QuoteLeft: vk = VK_OEM_3; break;
        default:
            // 无法映射的按键不额外拦截，保留 Qt 事件状态判断。
            return true;
        }
    }
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

QHash<QString, QString> g_fuzzyResolved;
QHash<QString, QString> g_translationsFolded;
constexpr auto kSourceProperty = "_sp_translation_source";
constexpr auto kTranslatingComboProperty = "_sp_translation_combo_busy";
// 下拉选项的原文存在每个选项自己的 itemData 里（而不是按索引的列表），
// 模型增删/重排后各选项仍携带自己的原文，还原时不会错位。
constexpr int kComboSourceRole = Qt::UserRole + 0x4A0;

// One Qt6 delegate serves both Painter and Designer. Designer-only features
// (graph-view painting hooks, Designer resource widgets) are always compiled
// in but only activated when the host process is Designer.

void translateWidget(QWidget *widget);
QString controlUniqueId(QWidget *widget, const QString &sourceText);

// 翻译路径的控件 ID：词库为空时直接返回空，省去每次绘制都计算 ID 的开销；
// 更改翻译窗口的 Ctrl+右键路径仍始终计算完整 ID。
QString translationControlId(QWidget *widget, const QString &sourceText) {
    return g_idTranslations.isEmpty()
               ? QString()
               : controlUniqueId(widget, sourceText);
}

bool containsCjk(const QString &text) {
    const QVector<uint> codePoints = text.toUcs4();
    for (const uint code : codePoints) {
        if (code == 0x3007 ||
            (code >= 0x3400 && code <= 0x4DBF) ||
            (code >= 0x4E00 && code <= 0x9FFF) ||
            (code >= 0xF900 && code <= 0xFAFF) ||
            (code >= 0x20000 && code <= 0x2FA1F))
            return true;
    }
    return false;
}

bool isAsciiLetter(QChar character) {
    const uint code = character.unicode();
    return (code >= 0x41 && code <= 0x5A) ||
           (code >= 0x61 && code <= 0x7A);
}

bool containsAsciiLetter(const QString &text) {
    for (const QChar character : text) {
        if (isAsciiLetter(character))
            return true;
    }
    return false;
}

// Normalization pipeline shared by the dictionary index and every fuzzy
// lookup, following common i18n folding practice:
//   whitespace variants -> collapse  |  invisible chars dropped
//   elision / " *" markers stripped  |  '_' == ' ' (identifier vs label)
//   Unicode NFC                       |  full-width ASCII -> half-width
//   quote/dash variants unified       |  case folding
//   diacritics decomposed and dropped (é -> e)
QString normalizeForMatch(QString text) {
    // Unify whitespace variants (NBSP, figure space, ideographic space, CRLF).
    text.replace(QChar(0x00A0), QLatin1Char(' '));
    text.replace(QChar(0x2007), QLatin1Char(' '));
    text.replace(QChar(0x202F), QLatin1Char(' '));
    text.replace(QChar(0x3000), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text = text.simplified();

    // Drop invisible characters that never belong to a display name.
    text.remove(QChar(0x200B));  // zero width space
    text.remove(QChar(0x200C));  // zero width non-joiner
    text.remove(QChar(0x200D));  // zero width joiner
    text.remove(QChar(0xFEFF));  // byte order mark
    text.remove(QChar(0x00AD));  // soft hyphen

    // UI state and elision suffixes ("Name …", "Name...", "Name *").
    while (text.endsWith(QChar(0x2026)))
        text.chop(1);
    while (text.endsWith(QLatin1String("...")))
        text.chop(3);
    if (text.endsWith(QLatin1String(" *")))
        text.chop(2);
    text = text.simplified();

    // Identifier <-> display label equivalence ("Color_Dodge" vs "Color dodge").
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text = text.simplified();

    // Unicode normalization so composed and decomposed forms match.
    text = text.normalized(QString::NormalizationForm_C);

    // Full-width ASCII (U+FF01..U+FF5E) -> half-width.
    for (int i = 0; i < text.size(); ++i) {
        const uint code = text.at(i).unicode();
        if (code >= 0xFF01 && code <= 0xFF5E)
            text[i] = QChar(code - 0xFEE0);
    }

    // Quote and dash variants.
    text.replace(QChar(0x2018), QLatin1Char('\''));
    text.replace(QChar(0x2019), QLatin1Char('\''));
    text.replace(QChar(0x201C), QLatin1Char('"'));
    text.replace(QChar(0x201D), QLatin1Char('"'));
    text.replace(QChar(0x2010), QLatin1Char('-'));
    text.replace(QChar(0x2011), QLatin1Char('-'));
    text.replace(QChar(0x2012), QLatin1Char('-'));
    text.replace(QChar(0x2013), QLatin1Char('-'));
    text.replace(QChar(0x2014), QLatin1Char('-'));
    text.replace(QChar(0x2015), QLatin1Char('-'));

    // Case folding.
    text = text.toCaseFolded();

    // Diacritics folding: decompose, drop combining marks, recompose.
    text = text.normalized(QString::NormalizationForm_D);
    QString stripped;
    stripped.reserve(text.size());
    for (const QChar ch : text) {
        const QChar::Category category = ch.category();
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing)
            continue;
        stripped.append(ch);
    }
    // Identifier concatenation equivalence: drop every space so that
    // "ScatteringColor" matches "Scattering color" (and "Color_Dodge" too).
    stripped.remove(QLatin1Char(' '));
    return stripped.normalized(QString::NormalizationForm_C);
}

// Fuzzy dictionary lookup through the shared normalization pipeline. Callers
// always try the exact map first; this map only exists to catch casing and
// formatting differences ("3D Perlin Noise" vs "3D Perlin noise", "Color_Dodge"
// vs "Color dodge"). Very short strings are too ambiguous to fuzzy-match.
QString fuzzyTranslation(const QString &key) {
    const QString normalized = normalizeForMatch(key);
    if (normalized.size() < 2)
        return {};
    return g_translationsFolded.value(normalized);
}

QComboBox *owningComboBox(QAbstractItemView *view) {
    if (!view)
        return nullptr;
    for (QObject *current = view; current; current = current->parent()) {
        if (auto *combo = qobject_cast<QComboBox *>(current))
            return combo;
    }
    // Qt places a combo popup inside a private top-level container on some
    // styles, so its QObject parent chain does not necessarily reach the
    // QComboBox. Comparing view pointers is stable across those styles.
    for (QWidget *widget : QApplication::allWidgets()) {
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            if (combo->view() == view)
                return combo;
        }
    }
    return nullptr;
}

// Paint 等高频路径只沿父链找宿主，不做 allWidgets 全量扫描，避免每次
// 重绘列表都遍历全部控件；Ctrl+右键的精确归属仍走 owningComboBox()。
QComboBox *owningComboBoxFast(QAbstractItemView *view) {
    if (!view)
        return nullptr;
    for (QObject *current = view; current; current = current->parent()) {
        if (auto *combo = qobject_cast<QComboBox *>(current))
            return combo;
    }
    return nullptr;
}

QString translated(const QString &text, bool removeMnemonic = false,
                   const QString &controlId = QString()) {
    // translated() 查找顺序：
    //   1. 控件 ID 专属词库（control_ids_zh.json）精确查找；
    //   2. 全局词库精确查找；
    //   3. 全局模糊匹配兜底。
    if (!g_enabled)
        return {};
    QString key = text.trimmed();
    if (key.isEmpty())
        return {};
    // 1. 控件 ID 专属词库（id_types_zh.json，键为完整 ID 字符串）。
    if (!controlId.isEmpty()) {
        const auto idHit = g_idTranslations.constFind(controlId);
        if (idHit != g_idTranslations.cend()) {
            // 精确 ID 也可能映射为 _skip_（表示该控件下任何原文都不翻译），
            // 不能把 _skip_ 本身当作译文显示。
            if (idHit.value() == QStringLiteral("_skip_"))
                return {};
            return idHit.value();
        }
        // 跳过翻译标记：键为“上级类名||自身类名||objectName||*”
        // （* 表示任意原文）、值为 "_skip_"，
        // 表示该控件下任何原文都不翻译（例如导入对话框的
        // QListWidget||QMenu||None||*，避免把 texture 改成纹理破坏导入类型键）。
        // ID 固定为“上级类名||自身类名||objectName||原文”，
        // 原文在最后一段，通配替换最后一段即可。
        const int lastSeparator =
            controlId.lastIndexOf(QStringLiteral("||"));
        if (lastSeparator > 0) {
            const QString wildcard =
                controlId.left(lastSeparator) + QStringLiteral("||*");
            const auto globalWildcard = g_translations.constFind(wildcard);
            if (globalWildcard != g_translations.cend() &&
                globalWildcard.value() == QStringLiteral("_skip_"))
                return {};
            const auto idWildcard = g_idTranslations.constFind(wildcard);
            if (idWildcard != g_idTranslations.cend() &&
                idWildcard.value() == QStringLiteral("_skip_"))
                return {};
        }
    }
    // 全局词库：允许用户映射覆盖官方中文。
    const auto exact = g_translations.constFind(key);
    if (exact != g_translations.cend())
        return exact.value();
    if (containsCjk(key))
        return {};
    // Designer appends " *" to an instance-parameter title as soon as the
    // user overrides its inherited/default value.  The marker is UI state,
    // not part of the translatable source string.  Match the stable title and
    // then preserve the marker in the translated result.
    QString stateSuffix;
    if (key.endsWith(u'*')) {
        key.chop(1);
        key = key.trimmed();
        stateSuffix = QStringLiteral(" *");
    }
    if (removeMnemonic) {
        // Prefer the exact dictionary key (e.g. "R&D"); fall back to the
        // mnemonic-stripped form that Painter actually displays.
        key.remove(u'&');
    }
    // 全局词库（处理 " *" / 助记符后）。
    const auto found = g_translations.constFind(key);
    if (found != g_translations.cend())
        return found.value() + stateSuffix;
    if (g_fuzzyMatchEnabled) {
        const QString fuzzy = fuzzyTranslation(key);
        if (!fuzzy.isNull())
            return fuzzy + stateSuffix;
    }
    return {};
}

bool isInsideLayersPanel(QWidget *widget) {
    if (!widget)
        return false;
    for (QObject *parent = widget; parent; parent = parent->parent()) {
        const QString className = QString::fromLatin1(parent->metaObject()->className());
        const QString objectName = parent->objectName();
        if (className.contains("LayerStack") || className.contains("LayerTree") ||
            objectName.contains("DockLayers"))
            return true;
        if (auto *dock = qobject_cast<QDockWidget *>(parent)) {
            const QString title = dock->windowTitle().trimmed();
            if (title == QStringLiteral("Layers") || title == QStringLiteral("图层"))
                return true;
        }
    }
    return false;
}

QObject *painterElidedLabelOwner(QObject *object) {
    int depth = 0;
    for (QObject *parent = object ? object->parent() : nullptr;
         parent && depth < 4; parent = parent->parent(), ++depth) {
        if (QString::fromLatin1(parent->metaObject()->className()) ==
            QStringLiteral("Alg::ElidedLabel"))
            return parent;
    }
    return nullptr;
}

// Alg::ElidedLabel keeps the complete Painter parameter label in its "text"
// property and lets its child QLabel draw an elided version.  Reading the
// child text alone therefore loses dictionary lookup information whenever a
// narrow panel produces (for example) "Specular edg…".
QString sourceFromPainterElidedLabel(QObject *object,
                                     const QString &displayedText) {
    const QString displayed = displayedText.trimmed();
    QString prefix = displayed;
    if (prefix.endsWith(QChar(0x2026)))
        prefix.chop(1);
    else if (prefix.endsWith(QLatin1String("...")))
        prefix.chop(3);
    else
        return {};
    prefix = prefix.trimmed();
    if (prefix.isEmpty() || !object)
        return {};

    int depth = 0;
    for (QObject *parent = object->parent(); parent && depth < 4;
         parent = parent->parent(), ++depth) {
        const QString className =
            QString::fromLatin1(parent->metaObject()->className());
        if (className != QStringLiteral("Alg::ElidedLabel") &&
            className != QStringLiteral("Alg::EditLabel"))
            continue;
        const QString full = parent->property("text").toString().trimmed();
        // Do not treat an unrelated parent text property as the label's
        // source. Painter's complete value must extend the visible prefix.
        if (full.size() > prefix.size() &&
            full.startsWith(prefix, Qt::CaseInsensitive))
            return full;
    }
    return {};
}

QString sourceForObject(QObject *object, const QString &displayedText) {
    QString displayed = displayedText.trimmed();
    displayed.remove(u'&');
    if (!object)
        return displayed;
    // This must run before the per-object source check: a freshly created
    // child QLabel has no saved source yet, which is exactly when Painter may
    // already have elided its displayed text.
    const QString fullElidedSource =
        sourceFromPainterElidedLabel(object, displayed);
    const QString stored = object->property(kSourceProperty).toString();
    if (stored.isEmpty())
        return fullElidedSource.isEmpty() ? displayed : fullElidedSource;
    if (displayed == stored || g_translations.value(stored) == displayed ||
        g_originals.value(displayed) == stored)
        return stored;
    // Painter reused the object for a different value; ignore stale metadata.
    return fullElidedSource.isEmpty() ? displayed : fullElidedSource;
}

bool shouldExcludeLayersPanel(QWidget *widget) {
    return !g_translateLayersPanel && isInsideLayersPanel(widget);
}

QString comboStoredSource(QComboBox *combo, int index) {
    if (!combo || index < 0 || index >= combo->count())
        return {};
    const QVariant value = combo->itemData(index, kComboSourceRole);
    return value.isValid() ? value.toString() : QString();
}

QString comboSourceAt(QComboBox *combo, int index) {
    if (!combo || index < 0 || index >= combo->count())
        return {};
    const QString stored = comboStoredSource(combo, index);
    const QString displayed = combo->itemText(index).trimmed();
    if (!stored.isEmpty()) {
        if (displayed == stored || g_translations.value(stored) == displayed ||
            g_originals.value(displayed) == stored)
            return stored;
    }
    const auto original = g_originals.constFind(displayed);
    return original == g_originals.cend() ? displayed : original.value();
}

bool isLayerBlendModeButton(QToolButton *button) {
    return button && button->objectName() == QStringLiteral("blendingMode") &&
           isInsideLayersPanel(button);
}

bool isLayerChannelSelector(QComboBox *combo) {
    return combo && combo->objectName() == QStringLiteral("channelSelector") &&
           isInsideLayersPanel(combo);
}

void lockLayerChannelPopupWidth(QComboBox *combo) {
    if (!isLayerChannelSelector(combo) || !combo->view())
        return;
    const int width = combo->width();
    combo->view()->setMinimumWidth(width);
    combo->view()->setMaximumWidth(width);
    QWidget *popup = combo->view()->window();
    if (popup && popup != combo->window() && popup != combo->view()) {
        popup->setMinimumWidth(width);
        popup->setMaximumWidth(width);
    }
}

QString actionSource(QAction *action) {
    if (!action)
        return {};
    const QString displayed = action->text().trimmed();
    const QString instanceSource = sourceForObject(action, displayed);
    if (instanceSource != displayed)
        return instanceSource;
    const auto original = g_originals.constFind(displayed);
    return original == g_originals.cend() ? displayed : original.value();
}

bool isLayerBlendModeMenu(QMenu *menu) {
    if (!menu)
        return false;
    if (auto *button = qobject_cast<QToolButton *>(menu->parentWidget())) {
        if (isLayerBlendModeButton(button))
            return true;
    }
    for (QAction *action : menu->actions()) {
        const QString source = actionSource(action);
        if (source == QStringLiteral("Passthrough") ||
            source == QStringLiteral("Normal map combine"))
            return true;
    }
    return false;
}

QString menuTranslation(QMenu *menu, const QString &source) {
    // 统一顺序：控件 ID（id_types_zh.json）→ 全局词库 → 模糊兜底。
    return translated(source, false,
                      translationControlId(menu, source));
}

class TranslationItemDelegate final : public QStyledItemDelegate {
public:
    explicit TranslationItemDelegate(QAbstractItemView *view,
                                     bool compactGrid = false,
                                     bool layersPanel = false)
        : QStyledItemDelegate(view), compactGrid_(compactGrid),
          layersPanel_(layersPanel), view_(view) {}

    QString displayText(const QVariant &value, const QLocale &locale) const override {
        if (g_enabled && (!layersPanel_ || g_translateLayersPanel) &&
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            value.metaType().id() == QMetaType::QString) {
#else
            value.userType() == QMetaType::QString) {
#endif
            const QString text = value.toString();
            const QString result = translated(
                text, false, translationControlId(view_, text));
            if (!result.isNull())
                return result;
        }
        return QStyledItemDelegate::displayText(value, locale);
    }

    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!compactGrid_)
            return;
        option->features |= QStyleOptionViewItem::WrapText;
        option->textElideMode = Qt::ElideRight;
        QFont font = option->font;
        const qreal currentSize = font.pointSizeF();
        if (font.pixelSize() > 0)
            font.setPixelSize(qMax(9, font.pixelSize() - 2));
        else if (currentSize > 0.0)
            font.setPointSizeF(qMax<qreal>(7.0, currentSize - 2.0));
        option->font = font;
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (compactGrid_) {
            QStyleOptionViewItem adjusted(option);
            initStyleOption(&adjusted, index);
            size.setHeight(size.height() + QFontMetrics(adjusted.font).lineSpacing() + 6);
        }
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (!compactGrid_) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);
        const QString text = adjusted.text;

        // Let the native style draw only selection/background/focus. Painter's
        // style ignores transparent text colors, so both display and decoration
        // are removed here; the icon and text are drawn explicitly below.
        QStyleOptionViewItem nativePart(adjusted);
        nativePart.text.clear();
        nativePart.icon = QIcon();
        nativePart.features &= ~QStyleOptionViewItem::HasDisplay;
        nativePart.features &= ~QStyleOptionViewItem::HasDecoration;
        const QWidget *widget = adjusted.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &nativePart, painter, widget);

        painter->save();
        const QSize iconSize = adjusted.decorationSize.isValid()
                                   ? adjusted.decorationSize
                                   : QSize(48, 48);
        QRect iconRect(QPoint(0, 0), iconSize);
        iconRect.moveCenter(QPoint(adjusted.rect.center().x(),
                                   adjusted.rect.top() + 4 + iconSize.height() / 2));
        const QIcon::Mode iconMode = (adjusted.state & QStyle::State_Enabled)
                                         ? QIcon::Normal
                                         : QIcon::Disabled;
        const QIcon::State iconState = (adjusted.state & QStyle::State_Open)
                                           ? QIcon::On
                                           : QIcon::Off;
        adjusted.icon.paint(painter, iconRect, Qt::AlignCenter, iconMode, iconState);

        painter->setFont(adjusted.font);
        const bool selected = adjusted.state & QStyle::State_Selected;
        painter->setPen(adjusted.palette.color(
            selected ? QPalette::HighlightedText : QPalette::Text));
        const QFontMetrics metrics(adjusted.font);
        const int twoLines = metrics.lineSpacing() * 2;
        QRect textRect = adjusted.rect.adjusted(2, 0, -2, -2);
        textRect.setTop(iconRect.bottom() + 3);
        textRect.setHeight(twoLines + 2);
        painter->setClipRect(textRect);
        painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop |
                                        Qt::TextWordWrap,
                          text);
        painter->restore();
    }

private:
    bool compactGrid_ = false;
    bool layersPanel_ = false;
    QAbstractItemView *view_ = nullptr;
};

class ComboPaintProxyModel final : public QIdentityProxyModel {
public:
    ComboPaintProxyModel(QComboBox *combo, QObject *parent)
        : QIdentityProxyModel(parent), combo_(combo) {}

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override {
        const QVariant sourceValue = QIdentityProxyModel::data(index, role);
        if (role != Qt::DisplayRole || !g_enabled || !combo_)
            return sourceValue;
        const QString source = sourceValue.toString();
        const QString result = translated(
            source, false, translationControlId(combo_, source));
        return result.isNull() ? sourceValue : QVariant(result);
    }

private:
    QPointer<QComboBox> combo_;
};

// The wrapper never draws an item itself. It calls Painter's original private
// delegate with an identity-proxy index whose DisplayRole alone is translated.
// All other roles and Painter's section styling remain untouched.
class ComboPaintDelegate final : public QAbstractItemDelegate {
public:
    ComboPaintDelegate(QAbstractItemView *view, QComboBox *combo,
                       QAbstractItemDelegate *original)
        : QAbstractItemDelegate(view), original_(original),
          proxy_(combo, this) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (!original_ || !index.isValid())
            return;
        ensureSourceModel(index.model());
        original_->paint(painter, option, proxy_.mapFromSource(index));
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override {
        return original_ ? original_->sizeHint(option, index) : QSize();
    }

private:
    void ensureSourceModel(const QAbstractItemModel *model) const {
        if (proxy_.sourceModel() != model)
            proxy_.setSourceModel(const_cast<QAbstractItemModel *>(model));
    }

    QPointer<QAbstractItemDelegate> original_;
    mutable ComboPaintProxyModel proxy_;
};

struct ComboPaintBinding {
    QPointer<QAbstractItemView> view;
    QPointer<QAbstractItemDelegate> original;
    QPointer<ComboPaintDelegate> installed;
};

std::vector<ComboPaintBinding> g_comboPaintBindings;

int installComboDisplayDelegate(QAbstractItemView *view, QComboBox *combo) {
    if (!view || !combo)
        return 0;
    if (dynamic_cast<ComboPaintDelegate *>(view->itemDelegate())) {
        view->viewport()->update();
        return 2;
    }
    QAbstractItemDelegate *original = view->itemDelegate();
    if (!dynamic_cast<QItemDelegate *>(original))
        return 0;
    const QString rtti = QString::fromLatin1(typeid(*original).name());
    if (rtti != QStringLiteral("class Alg::DefaultComboBoxDelegate") &&
        rtti != QStringLiteral("class Alg::SectionComboBoxDelegate"))
        return 0;
    auto *installed = new ComboPaintDelegate(view, combo, original);
    g_comboPaintBindings.push_back({view, original, installed});
    view->setItemDelegate(installed);
    view->viewport()->update();
    return 1;
}

void restoreComboDisplayDelegates() {
    for (auto it = g_comboPaintBindings.rbegin();
         it != g_comboPaintBindings.rend(); ++it) {
        if (it->view && it->view->itemDelegate() == it->installed)
            it->view->setItemDelegate(it->original);
        delete it->installed.data();
    }
    g_comboPaintBindings.clear();
}

struct DelegateBinding {
    QPointer<QAbstractItemView> view;
    QPointer<QAbstractItemDelegate> original;
    QPointer<TranslationItemDelegate> installed;
    bool compactGrid = false;
    bool originalWordWrap = false;
    QSize originalGridSize;
    Qt::TextElideMode originalElideMode = Qt::ElideRight;
};

std::vector<DelegateBinding> g_delegateBindings;

int installAssetDelegate(QAbstractItemView *view, bool compactGrid = false,
                         bool layersPanel = false) {
    if (!view)
        return 0;
    if (dynamic_cast<TranslationItemDelegate *>(view->itemDelegate())) {
        view->viewport()->update();
        return 2;
    }
    DelegateBinding binding;
    binding.view = view;
    binding.original = view->itemDelegate();
    binding.compactGrid = compactGrid;
    binding.originalElideMode = view->textElideMode();
    if (compactGrid) {
        if (auto *listView = qobject_cast<QListView *>(view)) {
            binding.originalWordWrap = listView->wordWrap();
            binding.originalGridSize = listView->gridSize();
            listView->setWordWrap(true);
            const QSize grid = listView->gridSize();
            const int extraLine = QFontMetrics(listView->font()).lineSpacing();
            if (grid.isValid())
                listView->setGridSize(QSize(grid.width(), grid.height() + extraLine + 6));
        }
        view->setTextElideMode(Qt::ElideRight);
    }
    auto *delegate =
        new TranslationItemDelegate(view, compactGrid, layersPanel);
    binding.installed = delegate;
    g_delegateBindings.push_back(binding);
    view->setItemDelegate(delegate);
    view->viewport()->update();
    return 1;
}

void restoreAssetDelegates() {
    for (auto it = g_delegateBindings.rbegin();
         it != g_delegateBindings.rend(); ++it) {
        QAbstractItemView *view = it->view.data();
        TranslationItemDelegate *installed = it->installed.data();
        if (view) {
            const bool stillInstalled = view->itemDelegate() == installed;
            if (stillInstalled)
                view->setItemDelegate(it->original.data());
            if (stillInstalled && it->compactGrid) {
                if (auto *listView = qobject_cast<QListView *>(view)) {
                    listView->setWordWrap(it->originalWordWrap);
                    listView->setGridSize(it->originalGridSize);
                }
                view->setTextElideMode(it->originalElideMode);
            }
            if (view->viewport())
                view->viewport()->update();
        }
        delete installed;
    }
    g_delegateBindings.clear();
}

// Painter 11 does not expose a QSortFilterProxyModel for the main Assets
// list.  The search field updates Alg::NewResourceListModel through Painter's
// private resource database, whose labels and tags are read-only from the
// public plug-in API.  Keep that native model installed and, for CJK queries,
// ask Painter for the complete current category before hiding non-matching
// rows in the native QListView.  This preserves Painter-owned QModelIndex,
// drag/drop, activation and selection semantics and needs no reverse
// Chinese-to-English dictionary.
class AssetRowFilter final : public QObject {
public:
    explicit AssetRowFilter(QObject *parent = nullptr)
        : QObject(parent), timer_(new QTimer(this)) {
        timer_->setSingleShot(true);
        timer_->setInterval(40);
        QObject::connect(timer_, &QTimer::timeout, this,
                         [this] { applyFilter(); });
    }

    void observe(QWidget *widget) {
        QWidget *container = resourcesContainer(widget);
        if (!container)
            return;
        // One filter instance owns exactly one search surface. The manager
        // creates another instance for every main shelf or resource picker,
        // so opening a generator/filter picker cannot steal the shelf state.
        if (container_ && container_ != container)
            return;
        bindContainer(container);
    }

    void setActive(bool active) {
        active_ = active;
        if (!active_) {
            deactivateLocalQuery(true);
            return;
        }
        if (field_ && containsCjk(field_->text()))
            activateLocalQuery(field_->text());
    }

    void translationsChanged() {
        if (localQuery_)
            scheduleFilter();
    }

    void shutdown(bool restoreNative = true) {
        active_ = false;
        unbind(restoreNative);
    }

private:
    enum class HostKind { None, Painter, PainterPicker, Designer };

    static QString className(const QObject *object) {
        return object
                   ? QString::fromLatin1(object->metaObject()->className())
                   : QString();
    }

    static HostKind containerKind(const QObject *object) {
        const QString type = className(object);
        if (type == QStringLiteral("Alg::NewResourcesView"))
            return HostKind::Painter;
        if (type == QStringLiteral("Alg::ResourcePickerWidget"))
            return HostKind::PainterPicker;
        if (type == QStringLiteral("Pfx::DataBase::ResourceTableWidget") &&
            object->objectName() == QStringLiteral("mResourceTableWidget"))
            return HostKind::Designer;
        return HostKind::None;
    }

    static QWidget *resourcesContainer(QWidget *widget) {
        int depth = 0;
        for (QObject *current = widget; current && depth < 14;
             current = current->parent(), ++depth) {
            if (containerKind(current) != HostKind::None)
                return qobject_cast<QWidget *>(current);
        }
        return nullptr;
    }

    static bool isSupportedModel(QAbstractItemModel *model, HostKind kind) {
        const QString type = className(model);
        if (kind == HostKind::Painter)
            return type == QStringLiteral("Alg::NewResourceListModel");
        // Picker model class names vary between Painter releases. The view is
        // identified strictly by its nearest ResourcePickerWidget ancestor;
        // filtering only reads DisplayRole and hides rows on the native view.
        if (kind == HostKind::PainterPicker)
            return model != nullptr;
        if (kind == HostKind::Designer)
            return type == QStringLiteral(
                       "Pfx::DataBase::ResourcesListModel");
        return false;
    }

    static bool isMainAssetView(QListView *view, QWidget *container) {
        if (!view || !container || resourcesContainer(view) != container ||
            !view->model())
            return false;
        const HostKind kind = containerKind(container);
        if (!isSupportedModel(view->model(), kind))
            return false;
        if (kind == HostKind::Painter)
            return view->objectName() == QStringLiteral("resources") &&
                   className(view) == QStringLiteral("Alg::ResourceListView");
        if (kind == HostKind::PainterPicker)
            return className(view) == QStringLiteral("Alg::ResourceListView");
        if (kind == HostKind::Designer)
            return className(view) == QStringLiteral(
                       "Pfx::DataBase::ResourceTableWidget::CustomListView");
        return false;
    }

    static bool isAssetSearchField(QLineEdit *field, QWidget *container) {
        if (!field || !container || resourcesContainer(field) != container)
            return false;
        const HostKind kind = containerKind(container);
        if (kind == HostKind::Painter)
            return field->objectName() == QStringLiteral("search_field") &&
                   className(field) ==
                       QStringLiteral("Alg::SearchFieldLineEdit");
        if (kind == HostKind::PainterPicker)
            return className(field) ==
                   QStringLiteral("Alg::SearchFieldLineEdit");
        if (kind == HostKind::Designer)
            return field->objectName() == QStringLiteral("globalSearch") &&
                   className(field) == QStringLiteral("QLineEdit");
        return false;
    }

    void bindContainer(QWidget *container) {
        if (!container)
            return;

        QLineEdit *field = nullptr;
        const QList<QLineEdit *> fields = container->findChildren<QLineEdit *>();
        for (QLineEdit *candidate : fields) {
            if (isAssetSearchField(candidate, container)) {
                field = candidate;
                if (candidate->isVisible())
                    break;
            }
        }

        QListView *view = nullptr;
        const QList<QListView *> views = container->findChildren<QListView *>();
        for (QListView *candidate : views) {
            if (isMainAssetView(candidate, container)) {
                view = candidate;
                if (candidate->isVisible())
                    break;
            }
        }

        if (!field || !view || !view->model())
            return;
        const HostKind kind = containerKind(container);
        if (field_ == field && view_ == view && model_ == view->model() &&
            hostKind_ == kind)
            return;

        // Painter may replace only the model while preserving the same search
        // field and view. In that case the visible CJK query is still locally
        // owned, so do not transiently send it to the host's native search.
        const bool sameSearchSurface =
            container_ == container && field_ == field && view_ == view &&
            hostKind_ == kind;
        unbind(!sameSearchSurface);
        container_ = container;
        field_ = field;
        view_ = view;
        model_ = view->model();
        hostKind_ = kind;

        fieldConnection_ = QObject::connect(
            field, &QLineEdit::textChanged, this,
            [this](const QString &query) { onTextChanged(query); });
        connectModel();

        if (active_ && containsCjk(field_->text()))
            activateLocalQuery(field_->text());
        else
            clearHiddenRows();
    }

    void connectModel() {
        if (!model_)
            return;
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::modelReset, this,
            [this] { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int, int) { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &, int, int) { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::layoutChanged, this,
            [this] { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &,
                   const QVector<int> &) {
                if (localQuery_)
                    scheduleFilter();
            }));
    }

    void disconnectModel() {
        for (const QMetaObject::Connection &connection : modelConnections_)
            QObject::disconnect(connection);
        modelConnections_.clear();
    }

    void onTextChanged(const QString &query) {
        if (applying_ || !active_ || !field_ || !view_)
            return;
        if (containsCjk(query)) {
            activateLocalQuery(query);
        } else {
            // Painter has already received this text change. Remove the local
            // row mask and leave the English/empty query entirely native.
            deactivateLocalQuery(false);
        }
    }

    void activateLocalQuery(const QString &query) {
        if (!active_ || applying_ || !field_ || !view_ || !model_ ||
            field_->signalsBlocked())
            return;

        const QString visibleQuery = query;
        const int cursor = field_->cursorPosition();
        const int selectionStart = field_->selectionStart();
        const int selectionLength = field_->selectedText().size();

        applying_ = true;
        localQuery_ = true;
        query_ = visibleQuery.trimmed();

        // Signals stay enabled here: Painter receives an empty text query and
        // repopulates the original model with every resource in the currently
        // selected native category. setText() emits textChanged(), but the
        // applying_ guard above prevents recursion into this handler.
        field_->setText(QString());

        // A host-side textChanged handler is allowed to rebuild the resource
        // widget synchronously. Never dereference stale QPointers afterward.
        if (!field_ || !view_) {
            applying_ = false;
            localQuery_ = false;
            query_.clear();
            return;
        }

        // Restore the user's CJK text only for presentation. QSignalBlocker
        // restores the previous signal state even if Qt code throws/returns.
        {
            const QSignalBlocker blocker(field_);
            field_->setText(visibleQuery);
            if (selectionStart >= 0) {
                const int start = qMin(selectionStart, visibleQuery.size());
                const int length = qMin(selectionLength,
                                        visibleQuery.size() - start);
                field_->setSelection(start, qMax(0, length));
            } else {
                field_->setCursorPosition(qMin(cursor, visibleQuery.size()));
            }
        }
        applying_ = false;
        scheduleFilter();
    }

    void deactivateLocalQuery(bool restoreNative) {
        if (timer_)
            timer_->stop();
        const bool wasLocal = localQuery_;
        localQuery_ = false;
        query_.clear();
        clearHiddenRows();
        if (restoreNative && wasLocal)
            restoreNativeQuery();
    }

    void restoreNativeQuery() {
        if (!field_ || appClosingDown())
            return;
        // The visible string was restored while signals were blocked. Emit
        // Painter's normal notification once so disabling/unloading the plug-in
        // cannot leave a hidden empty query behind the visible CJK text.
        QMetaObject::invokeMethod(field_, "textChanged", Qt::DirectConnection,
                                  Q_ARG(QString, field_->text()));
    }

    void scheduleFilter() {
        if (!active_ || !localQuery_ || !timer_)
            return;
        timer_->start();
    }

    void refreshRowMask() {
        if (active_ && localQuery_)
            scheduleFilter();
        else
            clearHiddenRows();
    }

    QStringList normalizedTerms() const {
        const QString normalized = normalizeForMatch(query_);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        return normalized.split(QLatin1Char(' '), Qt::SkipEmptyParts);
#else
        return normalized.split(QLatin1Char(' '), QString::SkipEmptyParts);
#endif
    }

    void applyFilter() {
        if (!active_ || !localQuery_ || !view_ || !model_)
            return;
        // Painter may rebuild the model asynchronously after the user clears
        // the field. A stale timer must always fail open instead of applying
        // the previous CJK row mask to an empty/native query.
        const QString visibleQuery = field_ ? field_->text().trimmed()
                                            : QString();
        if (visibleQuery.isEmpty() || !containsCjk(visibleQuery)) {
            deactivateLocalQuery(false);
            return;
        }
        if (view_->model() != model_) {
            QWidget *container = container_.data();
            if (container &&
                isSupportedModel(view_->model(), hostKind_)) {
                // bindContainer() recognizes a model-only replacement and
                // preserves ownership of the visible CJK query.
                bindContainer(container);
            } else {
                unbind(true);
            }
            return;
        }
        if (!isSupportedModel(model_, hostKind_)) {
            unbind(true);
            return;
        }

        const QStringList terms = normalizedTerms();
        if (terms.isEmpty()) {
            clearHiddenRows();
            return;
        }

        for (int row = 0; row < model_->rowCount(); ++row) {
            const QModelIndex index = model_->index(row, 0);
            const QString source =
                index.data(Qt::DisplayRole).toString().trimmed();
            const QString target = translated(
                source, false, translationControlId(view_, source));
            const QString searchable =
                normalizeForMatch(source + QLatin1Char(' ') + target);

            bool matches = true;
            for (const QString &term : terms) {
                if (!searchable.contains(term)) {
                    matches = false;
                    break;
                }
            }
            view_->setRowHidden(row, !matches);
        }

        const QModelIndex current = view_->currentIndex();
        if (current.isValid() && view_->isRowHidden(current.row())) {
            view_->clearSelection();
            view_->setCurrentIndex(QModelIndex());
        }
        view_->doItemsLayout();
        if (view_->viewport())
            view_->viewport()->update();
    }

    void clearHiddenRows() {
        if (!view_ || !model_ || view_->model() != model_)
            return;
        for (int row = 0; row < model_->rowCount(); ++row)
            view_->setRowHidden(row, false);
        view_->doItemsLayout();
        if (view_->viewport())
            view_->viewport()->update();
    }

    void unbind(bool restoreNative) {
        if (timer_)
            timer_->stop();
        QObject::disconnect(fieldConnection_);
        fieldConnection_ = {};
        disconnectModel();
        deactivateLocalQuery(restoreNative);
        field_.clear();
        view_.clear();
        model_.clear();
        container_.clear();
        hostKind_ = HostKind::None;
        applying_ = false;
    }

    QTimer *timer_ = nullptr;
    QPointer<QWidget> container_;
    QPointer<QLineEdit> field_;
    QPointer<QListView> view_;
    QPointer<QAbstractItemModel> model_;
    QMetaObject::Connection fieldConnection_;
    QList<QMetaObject::Connection> modelConnections_;
    QString query_;
    HostKind hostKind_ = HostKind::None;
    bool active_ = true;
    bool localQuery_ = false;
    bool applying_ = false;
};

// Owns one independent AssetRowFilter per resource-search container. Pointer
// identity is used only while the QWidget is alive; destroyed containers
// remove their entry immediately and their filter restores no dead widgets.
class AssetSearchManager final : public QObject {
public:
    explicit AssetSearchManager(QObject *parent = nullptr) : QObject(parent) {}

    void observe(QWidget *widget) {
        QWidget *container = containerFor(widget);
        if (!container)
            return;
        AssetRowFilter *filter = filters_.value(container, nullptr);
        if (!filter) {
            filter = new AssetRowFilter(this);
            filter->setActive(active_);
            filters_.insert(container, filter);
            QObject::connect(container, &QObject::destroyed, this,
                             [this, container] {
                AssetRowFilter *removed = filters_.take(container);
                if (removed) {
                    // QObject::destroyed is emitted during teardown. Do not
                    // re-enter Painter by emitting textChanged at this point.
                    removed->shutdown(false);
                    removed->deleteLater();
                }
            });
        }
        filter->observe(widget);
    }

    void setActive(bool active) {
        active_ = active;
        for (AssetRowFilter *filter : filters_)
            filter->setActive(active);
    }

    void translationsChanged() {
        for (AssetRowFilter *filter : filters_)
            filter->translationsChanged();
    }

    void shutdown() {
        const QList<AssetRowFilter *> filters = filters_.values();
        filters_.clear();
        for (AssetRowFilter *filter : filters) {
            filter->shutdown();
            delete filter;
        }
    }

private:
    static QWidget *containerFor(QWidget *widget) {
        int depth = 0;
        for (QObject *current = widget; current && depth < 14;
             current = current->parent(), ++depth) {
            const QString type =
                QString::fromLatin1(current->metaObject()->className());
            if (type == QStringLiteral("Alg::NewResourcesView") ||
                type == QStringLiteral("Alg::ResourcePickerWidget") ||
                (type == QStringLiteral("Pfx::DataBase::ResourceTableWidget") &&
                 current->objectName() == QStringLiteral("mResourceTableWidget")))
                return qobject_cast<QWidget *>(current);
        }
        return nullptr;
    }

    QHash<QWidget *, AssetRowFilter *> filters_;
    bool active_ = true;
};

AssetSearchManager *g_assetRowFilter = nullptr;

void observePainterAssetSearch(QWidget *widget) {
    if (g_assetRowFilter)
        g_assetRowFilter->observe(widget);
}

void scanAssetSearchWidgets() {
    if (!g_assetRowFilter)
        return;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget)
            g_assetRowFilter->observe(widget);
    }
}

bool isResourcePickerView(QAbstractItemView *view) {
    if (!view)
        return false;
    for (QObject *parent = view->parent(); parent; parent = parent->parent()) {
        const QString className = QString::fromLatin1(parent->metaObject()->className());
        if (className == QStringLiteral("Alg::ResourcePickerWidget"))
            return true;
        if (qobject_cast<QMenu *>(parent))
            break;
    }
    return false;
}

bool isDesignerGraphView(QGraphicsView *view) {
    if (!view)
        return false;
    const QString className =
        QString::fromLatin1(view->metaObject()->className());
    return className ==
               QStringLiteral("Pfx::Editor::Components::Graph::GraphView") ||
           className.endsWith(QStringLiteral("::GraphView"));
}

// The graph's node text is substituted while the view paints, so toggling the
// plug-in must schedule a repaint of every Designer graph view; otherwise the
// previously painted translation (or original) stays on screen.
bool appClosingDown() {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    return QCoreApplication::closingDown();
#else
    return false;
#endif
}

void refreshGraphViews() {
    if (appClosingDown())
        return;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (!widget || !widget->isVisible())
            continue;
        if (auto *view = qobject_cast<QGraphicsView *>(widget)) {
            if (isDesignerGraphView(view) && view->viewport())
                view->viewport()->update();
        }
    }
}

QGraphicsView *designerGraphViewForPainter(QPainter *painter) {
    if (!painter || !painter->device())
        return nullptr;
    // QGraphicsView paints its scene directly on the viewport widget. This
    // identifies that native paint pass without adding or moving scene items.
    auto *viewport = dynamic_cast<QWidget *>(painter->device());
    if (!viewport)
        return nullptr;
    auto *view = qobject_cast<QGraphicsView *>(viewport->parentWidget());
    return isDesignerGraphView(view) && view->viewport() == viewport
               ? view
               : nullptr;
}

bool isDesignerGraphPainter(QPainter *painter) {
    return designerGraphViewForPainter(painter) != nullptr;
}

// graphOwnerItem is defined below together with the other geometry helpers.
QGraphicsItem *graphOwnerItem(QPainter *painter,
                              qreal *differenceOut = nullptr);

// The graph paints node titles that may be elided ("Name …") or shown as the
// raw identifier. The owning item's tooltip carries the full display name on
// its first line, which is the reliable key for the dictionary.
QString stripHtmlTags(QString text) {
    QString plain;
    plain.reserve(text.size());
    bool inTag = false;
    for (const QChar ch : text) {
        if (ch == u'<') {
            inTag = true;
        } else if (ch == u'>') {
            inTag = false;
        } else if (!inTag) {
            plain.append(ch);
        }
    }
    plain.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    plain.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    plain.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    plain.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    plain.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    return plain;
}

QString graphFullTitleFromItem(QGraphicsItem *item) {
    for (QGraphicsItem *current = item; current;
         current = current->parentItem()) {
        const QString tip = current->toolTip().trimmed();
        if (tip.isEmpty())
            continue;
        // Designer's tooltip is rich text: "<b>Name</b><br>(ID : ...)…".
        // Take the first segment up to the first line break, then remove the
        // HTML tags so the plain display name can be matched to the dictionary.
        QString firstLine = tip;
        const int htmlBreak =
            firstLine.indexOf(QLatin1String("<br"), 0, Qt::CaseInsensitive);
        if (htmlBreak >= 0)
            firstLine = firstLine.left(htmlBreak);
        const int newline = firstLine.indexOf(QLatin1Char('\n'));
        if (newline >= 0)
            firstLine = firstLine.left(newline);
        firstLine = stripHtmlTags(firstLine).trimmed();
        if (!firstLine.isEmpty())
            return firstLine;
    }
    return {};
}

// Port labels may already be partially translated (mixed CJK + ASCII, e.g.
// "（主要）Background"). When the whole label has no dictionary entry, walk
// the remaining ASCII word segments and translate each one separately. Only
// mixed labels are touched so fully English labels keep their existing
// whole-string lookup behavior.
QString translateMixedPortLabel(const QString &source) {
    bool hasCjk = false;
    bool hasAsciiLetter = false;
    for (const QChar ch : source) {
        const uint code = ch.unicode();
        if ((code >= 0x3400 && code <= 0x4DBF) ||
            (code >= 0x4E00 && code <= 0x9FFF)) {
            hasCjk = true;
        } else if (isAsciiLetter(ch)) {
            hasAsciiLetter = true;
        }
    }
    if (!hasCjk || !hasAsciiLetter)
        return {};

    QString result = source;
    bool changed = false;
    int i = 0;
    while (i < result.size()) {
        if (!isAsciiLetter(result[i])) {
            ++i;
            continue;
        }
        int j = i;
        while (j < result.size()) {
            const uint c = result[j].unicode();
            const bool part =
                (c >= 0x30 && c <= 0x39) ||
                (c >= 0x41 && c <= 0x5A) ||
                (c >= 0x61 && c <= 0x7A) ||
                c == 0x5F;   // underscore keeps identifiers together
            if (!part)
                break;
            ++j;
        }
        const QString word = result.mid(i, j - i);
        QString target = g_translations.value(word);
        if (target.isNull() && g_fuzzyMatchEnabled)
            target = fuzzyTranslation(word);
        if (!target.isNull() && target != word) {
            result.replace(i, j - i, target);
            changed = true;
            i += target.size();
        } else {
            i = j;
        }
    }
    return changed ? result : QString();
}

QString graphPaintTranslation(QPainter *painter, const QString &source,
                              int portSide = 0) {
    if (!g_enabled || !g_translateDesignerGraph)
        return {};
    // 混合端口标签（已部分翻译 + 残留英文，如"（主要） Preview"）需要
    // 放行到分段翻译；纯中文标签才是已翻译完成、直接跳过。
    const bool hasCjk = containsCjk(source);
    const bool mixedPortLabel =
        portSide != 0 && hasCjk && containsAsciiLetter(source);
    if (hasCjk && !mixedPortLabel)
        return {};
    if (!isDesignerGraphPainter(painter))
        return {};
    // 1. Exact dictionary match always wins.
    QString target = g_translations.value(source);
    if (!target.isNull())
        return target;
    const auto cached = g_fuzzyResolved.constFind(source);
    if (cached != g_fuzzyResolved.cend())
        return cached.value();

    // 2. Tooltip full-name fallback (node titles only): the graph paints
    // elided titles ("Name …") and identifier forms. The item tooltip carries
    // the full display name on its first line. Port labels must not use this
    // tooltip match; only node titles are allowed to fall back to it.
    QString cacheKey = source;
    if (portSide == 0) {
        QGraphicsItem *owner = graphOwnerItem(painter);
        if (owner) {
            const QString full = graphFullTitleFromItem(owner);
            const QString fullNormalized = normalizeForMatch(full);
            const QString normalized = normalizeForMatch(source);
            if (!fullNormalized.isEmpty() && fullNormalized != normalized &&
                fullNormalized.startsWith(normalized)) {
                // 同一绘制文本可能属于不同节点（完整名不同），缓存键必须
                // 带上完整名，避免串用其他节点的 tooltip 匹配结果。
                cacheKey = source + QChar(0x01) + fullNormalized;
                const auto tooltipCached =
                    g_fuzzyResolved.constFind(cacheKey);
                if (tooltipCached != g_fuzzyResolved.cend())
                    return tooltipCached.value();
                target = g_translations.value(full);
                if (target.isNull())
                    target = g_translationsFolded.value(fullNormalized);
            }
        }
    }

    // 3. Global and scoped fuzzy matching on the drawn source (case,
    // full-width, underscore, diacritics and whitespace differences). This
    // step is gated by the plug-in option; the tooltip fallback always stays
    // active.
    if (target.isNull() && g_fuzzyMatchEnabled)
        target = fuzzyTranslation(source);

    // 4. Port labels that are mixed CJK/ASCII: translate the remaining
    // English word segments (e.g. "（主要）Background" -> "（主要）背景").
    if (target.isNull() && portSide != 0)
        target = translateMixedPortLabel(source);

    g_fuzzyResolved.insert(cacheKey, target);
    return target;
}

// ---------------------------------------------------------------------------
// 通用绘制层翻译(纯显示层,样式无关)。
// 在 QPainter::drawText 处翻译任何 UI 文本。此时拿到的是控件传给 drawText 的
// 完整文本(QStyle::drawItemText / QCommonStyle::drawControl 会把控件全文传进来),
// 与当前 QStyle 是 QCommonStyle/Fusion/自定义无关——因此**不需要替换 QStyle**,
// 也就不存在"换 QCommonStyle 破坏外观"的问题。
// 原文恢复按 SpeedTree 逻辑:只读控件自身的 text()/父控件 text 属性
// (Alg::ElidedLabel),绝不写 kSourceProperty 缓存。
// 仅当非节点图绘制时进入此处(节点图由 graphPaintTranslation 先行处理)。
// ---------------------------------------------------------------------------
QString generalPainterTranslation(QPainter *painter, const QString &text) {
    if (!g_enabled)
        return {};
    if (text.isEmpty())
        return {};
    const QString trimmed = text.trimmed();
    if (trimmed.size() <= 1)
        return {};
    if (containsCjk(trimmed))
        return {};

    // 廉价过滤:纯数值/单位数值几乎不可能是 UI 标签。
    {
        bool allValue = true;
        for (QChar ch : trimmed) {
            if (ch.isDigit() || ch == QLatin1Char('.') || ch == QLatin1Char(',') ||
                ch == QLatin1Char('%') || ch == QLatin1Char('-') ||
                ch == QLatin1Char('+') || ch == QLatin1Char('e') ||
                ch == QLatin1Char('E') || ch == QLatin1Char(' '))
                continue;
            allValue = false;
            break;
        }
        if (allValue)
            return {};
    }

    // 从正在绘制的控件恢复完整原文(SpeedTree 逻辑):普通 QLabel 的 text() 就是
    // 全文;Alg::ElidedLabel 把截断串塞给子 QLabel,全文在父控件的 text 属性里。
    if (QWidget *device = dynamic_cast<QWidget *>(painter->device())) {
        if (auto *lbl = qobject_cast<QLabel *>(device)) {
            const QString displayed = lbl->text().trimmed();
            const QString full = sourceFromPainterElidedLabel(lbl, displayed);
            const QString source = full.isEmpty() ? displayed : full;
            if (!source.isEmpty() && source != text) {
                const QString fullTr =
                    translated(source, false, translationControlId(lbl, source));
                if (!fullTr.isEmpty())
                    return fullTr;
            }
        }
    }

    // 直接精确/全局查词。
    const QString target = translated(trimmed, false, QString());
    if (!target.isEmpty())
        return target;

    // 省略前缀恢复:被截断的 "Mater…" 匹配词库里以 "Mater" 开头的最短原文。
    // 先用线性遍历(省略绘制不常见);后续可换成 g_sortedKeys + 二分以省耗时。
    if (text.endsWith(QChar(0x2026)) || text.endsWith(QLatin1String("..."))) {
        QString base = text;
        base.chop(text.endsWith(QChar(0x2026)) ? 1 : 3);
        base = base.trimmed();
        if (base.size() >= 2) {
            const QString baseLower = base.toCaseFolded();
            QString best;
            for (auto it = g_translations.cbegin(); it != g_translations.cend(); ++it) {
                const QString &key = it.key();
                if (key.toCaseFolded().startsWith(baseLower) &&
                    (best.isEmpty() || key.size() < best.size()))
                    best = key;
            }
            if (!best.isEmpty()) {
                const QString preTr = translated(best, false, QString());
                if (!preTr.isEmpty())
                    return preTr;
            }
        }
    }
    return {};
}

using DrawPoint = void (*)(QPainter *, const QPoint &, const QString &);
using DrawPointF = void (*)(QPainter *, const QPointF &, const QString &);
using DrawRect = void (*)(QPainter *, const QRect &, int, const QString &,
                          QRect *);
using DrawRectFOption = void (*)(QPainter *, const QRectF &, const QString &,
                                 const QTextOption &);
using DrawRectFAlign = void (*)(QPainter *, const QRectF &, int,
                                const QString &, QRectF *);
using DrawXY = void (*)(QPainter *, int, int, const QString &);
using DrawXYWH = void (*)(QPainter *, int, int, int, int, int,
                          const QString &, QRect *);
using DrawPointF2 = void (*)(QPainter *, const QPointF &, const QString &,
                             int, int);

DrawPoint g_drawPoint = nullptr;
DrawPointF g_drawPointF = nullptr;
DrawRect g_drawRect = nullptr;
DrawRectFOption g_drawRectFOption = nullptr;
DrawRectFAlign g_drawRectFAlign = nullptr;
DrawXY g_drawXY = nullptr;
DrawXYWH g_drawXYWH = nullptr;
DrawPointF2 g_drawPointF2 = nullptr;
bool g_graphPainterHooksInstalled = false;
QSet<QString> g_graphPaintDiagnosticKeys;
struct GraphHookSlot {
    void **slot = nullptr;
    void *original = nullptr;
    void *replacement = nullptr;
};
std::vector<GraphHookSlot> g_graphHookSlots;

qreal transformDifference(const QTransform &a, const QTransform &b) {
    return qAbs(a.m11() - b.m11()) + qAbs(a.m12() - b.m12()) +
           qAbs(a.m13() - b.m13()) + qAbs(a.m21() - b.m21()) +
           qAbs(a.m22() - b.m22()) + qAbs(a.m23() - b.m23()) +
           qAbs(a.m31() - b.m31()) + qAbs(a.m32() - b.m32()) +
           qAbs(a.m33() - b.m33());
}

QGraphicsItem *graphOwnerItem(QPainter *painter, qreal *differenceOut) {
    QGraphicsView *view = designerGraphViewForPainter(painter);
    if (!view || !view->scene()) {
        if (differenceOut)
            *differenceOut = 1.0e20;
        return nullptr;
    }
    QGraphicsItem *owner = nullptr;
    qreal bestDifference = 1.0e20;
    const QTransform current = painter->worldTransform();
    for (QGraphicsItem *item : view->scene()->items()) {
        if (!item)
            continue;
        const qreal difference = transformDifference(
            current, item->deviceTransform(view->viewportTransform()));
        if (difference < bestDifference) {
            bestDifference = difference;
            owner = item;
        }
    }
    if (differenceOut)
        *differenceOut = bestDifference;
    return owner;
}

#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
void recordGraphPaintType(QPainter *painter, const QString &text,
                          const char *overload, quintptr caller,
                          int flags = -1) {
    QGraphicsView *view = designerGraphViewForPainter(painter);
    if (!view || !view->scene() || text.isEmpty())
        return;

    qreal bestDifference = 1.0e20;
    QGraphicsItem *owner = graphOwnerItem(painter, &bestDifference);

    const QString ownerRtti = owner
        ? QString::fromLatin1(typeid(*owner).name())
        : QStringLiteral("<none>");
    const QString parentRtti = owner && owner->parentItem()
        ? QString::fromLatin1(typeid(*owner->parentItem()).name())
        : QStringLiteral("<none>");
    const quintptr moduleBase =
        reinterpret_cast<quintptr>(GetModuleHandleW(nullptr));
    const quintptr callerRva = caller >= moduleBase ? caller - moduleBase : 0;
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6")
                            .arg(text, QString::fromLatin1(overload), ownerRtti,
                                 parentRtti)
                            .arg(flags)
                            .arg(callerRva, 0, 16);
    if (g_graphPaintDiagnosticKeys.contains(key))
        return;
    g_graphPaintDiagnosticKeys.insert(key);

    QFile output(QDir::temp().filePath(
        QStringLiteral("sd_graph_paint_types.txt")));
    if (!output.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&output);
    stream << "text=" << text << "\toverload=" << overload
           << "\tflags=" << flags
           << "\tcaller_rva=0x" << QString::number(callerRva, 16)
           << "\towner_rtti=" << ownerRtti
           << "\towner_public_type=" << (owner ? owner->type() : -1)
           << "\towner_has_parent=" << (owner && owner->parentItem() ? 1 : 0)
           << "\towner_flags=" << (owner ? int(owner->flags()) : -1)
           << "\towner_z=" << (owner ? owner->zValue() : 0.0)
           << "\towner_children=" << (owner ? owner->childItems().size() : -1)
           << "\tparent_rtti=" << parentRtti
           << "\ttransform_difference=" << bestDifference
           << "\tpen=" << painter->pen().color().name(QColor::HexArgb)
           << "\tfont_size=" << painter->font().pointSizeF()
           << "\tfont_weight=" << painter->font().weight() << "\n";
#endif

// Designer's connector items draw each port label centered inside a rectangle
// whose size and position are computed from the original text. Swapping in a
// translation of a different width leaves the text centered inside that stale
// rectangle, so its outer edge no longer lines up with the other port labels.
// Port labels are anchored to the connector dot: input labels end at a fixed
// column just left of the dot (right edge anchored), output labels start at a
// fixed column just right of the dot (left edge anchored). Return +1 for an
// input label, -1 for an output label and 0 when the side cannot be
// determined.
int graphPortLabelSide(QPainter *painter, const QRectF &rect) {
    QGraphicsView *view = designerGraphViewForPainter(painter);
    QGraphicsItem *owner = graphOwnerItem(painter);
    if (!view || !owner || !owner->parentItem())
        return 0;
    const QString rtti = QString::fromLatin1(typeid(*owner).name());
    if (!rtti.contains(QStringLiteral("Connector")))
        return 0;
    const QRectF nodeRect = owner->parentItem()->sceneBoundingRect();
    if (nodeRect.width() <= 0.0)
        return 0;
    const qreal labelCenterX = painter->worldTransform().map(rect.center()).x();
    const qreal nodeCenterX = view->mapFromScene(nodeRect.center()).x();
    if (labelCenterX + 2.0 < nodeCenterX)
        return 1;   // label on the left half of the node: input port
    if (labelCenterX - 2.0 > nodeCenterX)
        return -1;  // label on the right half of the node: output port
    return 0;
}

Qt::Alignment graphPortLabelAlignment(Qt::Alignment alignment,
                                      int portSide) {
    alignment &= ~Qt::AlignHorizontal_Mask;
    if (portSide == 1)
        alignment |= Qt::AlignRight;   // input label: right edge meets the dot
    else if (portSide == -1)
        alignment |= Qt::AlignLeft;    // output label: left edge leaves the dot
    else
        alignment |= Qt::AlignHCenter;
    return alignment;
}

void hookedDrawPoint(QPainter *painter, const QPoint &point,
                     const QString &text) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QPoint",
                         reinterpret_cast<quintptr>(_ReturnAddress()));
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawPoint(painter, point, target.isEmpty() ? text : target);
}

void hookedDrawPointF(QPainter *painter, const QPointF &point,
                      const QString &text) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QPointF",
                         reinterpret_cast<quintptr>(_ReturnAddress()));
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawPointF(painter, point, target.isEmpty() ? text : target);
}

void hookedDrawPointF2(QPainter *painter, const QPointF &point,
                       const QString &text, int flags, int justification) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QPointF/Justify",
                         reinterpret_cast<quintptr>(_ReturnAddress()), flags);
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawPointF2(painter, point, target.isEmpty() ? text : target,
                  flags, justification);
}

void hookedDrawRect(QPainter *painter, const QRect &rect, int flags,
                    const QString &text, QRect *boundingRect) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QRect",
                         reinterpret_cast<quintptr>(_ReturnAddress()), flags);
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawRect(painter, rect, flags, target.isEmpty() ? text : target,
               boundingRect);
}

void hookedDrawRectFOption(QPainter *painter, const QRectF &rect,
                           const QString &text,
                           const QTextOption &option) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QRectF/QTextOption",
                         reinterpret_cast<quintptr>(_ReturnAddress()),
                         int(option.alignment()) |
                             (int(option.textDirection()) << 16));
#endif
    const int portSide = graphPortLabelSide(painter, rect);
    QString target = graphPaintTranslation(painter, text, portSide);
    if (target.isEmpty() && portSide == 0)
        target = generalPainterTranslation(painter, text);
    if (target.isEmpty()) {
        g_drawRectFOption(painter, rect, text, option);
        return;
    }
    if (portSide == 0) {
        g_drawRectFOption(painter, rect, target, option);
        return;
    }
    QTextOption drawOption(option);
    drawOption.setWrapMode(QTextOption::NoWrap);
    drawOption.setAlignment(graphPortLabelAlignment(option.alignment(),
                                                    portSide));
    g_drawRectFOption(painter, rect, target, drawOption);
}

void hookedDrawRectFAlign(QPainter *painter, const QRectF &rect, int flags,
                          const QString &text, QRectF *boundingRect) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QRectF/Align",
                         reinterpret_cast<quintptr>(_ReturnAddress()), flags);
#endif
    const int portSide = graphPortLabelSide(painter, rect);
    QString target = graphPaintTranslation(painter, text, portSide);
    if (target.isEmpty() && portSide == 0)
        target = generalPainterTranslation(painter, text);
    if (target.isEmpty()) {
        g_drawRectFAlign(painter, rect, flags, text, boundingRect);
        return;
    }
    if (portSide == 0) {
        g_drawRectFAlign(painter, rect, flags, target, boundingRect);
        return;
    }
    const Qt::Alignment adjusted =
        graphPortLabelAlignment(Qt::Alignment(flags), portSide);
    g_drawRectFAlign(painter, rect, int(adjusted), target, boundingRect);
}

void hookedDrawXY(QPainter *painter, int x, int y, const QString &text) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "XY",
                         reinterpret_cast<quintptr>(_ReturnAddress()));
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawXY(painter, x, y, target.isEmpty() ? text : target);
}

void hookedDrawXYWH(QPainter *painter, int x, int y, int width, int height,
                    int flags, const QString &text, QRect *boundingRect) {
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "XYWH",
                         reinterpret_cast<quintptr>(_ReturnAddress()), flags);
#endif
    QString target = graphPaintTranslation(painter, text);
    if (target.isEmpty())
        target = generalPainterTranslation(painter, text);
    g_drawXYWH(painter, x, y, width, height, flags,
               target.isEmpty() ? text : target, boundingRect);
}

// 在 *任意* 模块的导入表里,把所有指向 `original` 的槽替换为 `replacement`
// (SpeedTree 的文案:patch every import slot in `module` that points at `original`)。
bool replaceImportInModule(HMODULE module, std::vector<GraphHookSlot> &slotList,
                           void *original, void *replacement) {
    if (!module || !original || !replacement)
        return false;
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const DWORD importRva = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva)
        return false;
    bool replaced = false;
    auto *descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + importRva);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->FirstThunk)
            continue;
        auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(
            base + descriptor->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            auto **slot = reinterpret_cast<void **>(&thunk->u1.Function);
            if (*slot != original)
                continue;
            GraphHookSlot hook{slot, original, replacement};
            slotList.push_back(hook);
            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE,
                                &oldProtection)) {
                slotList.pop_back();
                continue;
            }
            *slot = replacement;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
            replaced = true;
        }
    }
    return replaced;
}

// 扫 *全部* 已加载模块,逐个 patch IAT。这样 QtWidgets.dll 里
// QStyle::drawItemText -> QPainter::drawText 的调用(在 QtWidgets.dll 的导入表里)
// 也能被拦截;而旧的 replaceMainModuleImportInto 只改宿主主 exe,拦不到它。
bool replaceImportAllModules(std::vector<GraphHookSlot> &slotList,
                             void *original, void *replacement) {
    bool any = false;
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                            &needed))
        return false;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        if (replaceImportInModule(modules[i], slotList, original, replacement))
            any = true;
    }
    return any;
}

bool restoreImportHooks(std::vector<GraphHookSlot> &slotList) {
    for (std::size_t index = slotList.size(); index > 0; --index) {
        const std::size_t current = index - 1;
        const GraphHookSlot hook = slotList[current];
        if (!hook.slot || *hook.slot != hook.replacement) {
            slotList.erase(slotList.begin() + current);
            continue;
        }
        DWORD oldProtection = 0;
        if (!VirtualProtect(hook.slot, sizeof(void *), PAGE_READWRITE,
                            &oldProtection))
            continue;
        *hook.slot = hook.original;
        DWORD ignored = 0;
        VirtualProtect(hook.slot, sizeof(void *), oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), hook.slot, sizeof(void *));
        slotList.erase(slotList.begin() + current);
    }
    return slotList.empty();
}

bool uninstallGraphPainterHooks() {
    restoreImportHooks(g_graphHookSlots);
    g_graphPainterHooksInstalled = !g_graphHookSlots.empty();
    return !g_graphPainterHooksInstalled;
}

bool graphHookEnvironmentCompatible() {
    // Painter 与 Designer 同属 Qt6 宿主,不再区分;只要求 64 位 + Qt6。
    // 幂等:drawText 的 MSVC mangled 名在 Qt6.x 内稳定。
    if constexpr (sizeof(void *) != 8)
        return false;
    const QStringList qtParts = QString::fromLatin1(qVersion()).split(u'.');
    if (qtParts.size() < 2)
        return false;
    bool majorOk = false;
    const int qtMajor = qtParts.at(0).toInt(&majorOk);
    if (!majorOk || qtMajor != 6)
        return false;
    return true;
}

template <typename Function>
bool hookModuleImportForSlots(std::vector<GraphHookSlot> &slotList,
                              HMODULE module, const char *symbol,
                              Function hook, Function &original) {
    original = reinterpret_cast<Function>(GetProcAddress(module, symbol));
    return original && replaceImportAllModules(
        slotList, reinterpret_cast<void *>(original),
        reinterpret_cast<void *>(hook));
}

template <typename Function>
bool hookQtGuiImport(HMODULE qtGui, const char *symbol, Function hook,
                     Function &original) {
    return hookModuleImportForSlots(g_graphHookSlots, qtGui, symbol, hook,
                                    original);
}

bool installGraphPainterHooks() {
    if (g_graphPainterHooksInstalled)
        return true;
    if (!graphHookEnvironmentCompatible())
        return false;
    HMODULE qtGui = GetModuleHandleW(L"Qt6Gui.dll");
    if (!qtGui)
        return false;
    bool installed = false;
    installed |= hookQtGuiImport(
        qtGui, "?drawText@QPainter@@QEAAXAEBVQPoint@@AEBVQString@@@Z",
        &hookedDrawPoint, g_drawPoint);
    installed |= hookQtGuiImport(
        qtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z",
        &hookedDrawPointF, g_drawPointF);
    installed |= hookQtGuiImport(
        qtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@HH@Z",
        &hookedDrawPointF2, g_drawPointF2);
    installed |= hookQtGuiImport(
        qtGui,
        "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z",
        &hookedDrawRect, g_drawRect);
    installed |= hookQtGuiImport(
        qtGui,
        "?drawText@QPainter@@QEAAXAEBVQRectF@@AEBVQString@@AEBVQTextOption@@@Z",
        &hookedDrawRectFOption, g_drawRectFOption);
    installed |= hookQtGuiImport(
        qtGui,
        "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z",
        &hookedDrawRectFAlign, g_drawRectFAlign);
    installed |= hookQtGuiImport(
        qtGui, "?drawText@QPainter@@QEAAXHHAEBVQString@@@Z",
        &hookedDrawXY, g_drawXY);
    installed |= hookQtGuiImport(
        qtGui,
        "?drawText@QPainter@@QEAAXHHHHHAEBVQString@@PEAVQRect@@@Z",
        &hookedDrawXYWH, g_drawXYWH);
    if (!installed || g_graphHookSlots.empty()) {
        uninstallGraphPainterHooks();
        return false;
    }
    g_graphPainterHooksInstalled = installed;
    return installed;
}

bool isDesignerResourceList(QAbstractItemView *view) {
    if (!view || !view->model())
        return false;
    const QString viewClass =
        QString::fromLatin1(view->metaObject()->className());
    const QString modelClass =
        QString::fromLatin1(view->model()->metaObject()->className());
    return viewClass ==
               QStringLiteral("Pfx::DataBase::ResourceTableWidget::CustomListView") &&
           modelClass == QStringLiteral("Pfx::DataBase::ResourcesListModel");
}

bool isDesignerLibraryTree(QAbstractItemView *view) {
    if (!view || !view->model() ||
        view->objectName() != QStringLiteral("mTreeWidget"))
        return false;
    if (QString::fromLatin1(view->model()->metaObject()->className()) !=
        QStringLiteral("QTreeModel"))
        return false;
    for (QObject *parent = view->parent(); parent; parent = parent->parent()) {
        if (QString::fromLatin1(parent->metaObject()->className()) ==
            QStringLiteral("Pfx::Editor::Components::Shelf::QueryExplorerWidget"))
            return true;
    }
    return false;
}

bool isResourceFolderTree(QAbstractItemView *view) {
    auto *tree = qobject_cast<QTreeView *>(view);
    if (!tree)
        return false;
    const QString name = tree->objectName();
    if (name != QStringLiteral("tree_view") &&
        name != QStringLiteral("filtered_tree_view"))
        return false;

    bool sawPathPanel = false;
    bool sawResourcesView = false;
    int depth = 0;
    for (QObject *parent = tree->parent(); parent && depth < 12;
         parent = parent->parent(), ++depth) {
        if (parent->objectName() == QStringLiteral("path_filter_panel"))
            sawPathPanel = true;
        if (QString::fromLatin1(parent->metaObject()->className()) ==
            QStringLiteral("Alg::NewResourcesView"))
            sawResourcesView = true;
    }
    return sawPathPanel && sawResourcesView;
}

bool isAssetPreviewView(QAbstractItemView *view) {
    if (!view || !view->model())
        return false;
    // Painter uses the same native preview popup for the resource shelf and
    // for the generator/filter/material/shader pickers.  Picker views do not
    // have the shelf's "resources" object name, so ancestry is the stable
    // discriminator for them.
    if (isResourcePickerView(view))
        return true;
    const QString className =
        QString::fromLatin1(view->metaObject()->className());
    if (view->objectName() == QStringLiteral("resources") &&
        className == QStringLiteral("Alg::ResourceListView"))
        return true;
    return isDesignerResourceList(view);
}

QString assetPreviewDisplayAt(QAbstractItemView *view,
                              const QPoint &globalPosition) {
    if (!view || !view->viewport())
        return {};
    const QPoint viewportPosition =
        view->viewport()->mapFromGlobal(globalPosition);
    const QModelIndex index = view->indexAt(viewportPosition);
    if (!index.isValid())
        return {};
    const QVariant value = index.data(Qt::DisplayRole);
    if (auto *styled = qobject_cast<QStyledItemDelegate *>(
            view->itemDelegate())) {
        const QString rendered = styled->displayText(value, QLocale()).trimmed();
        if (!rendered.isEmpty())
            return rendered;
    }
    return value.toString().trimmed();
}

QString assetPreviewRawDisplayAt(QAbstractItemView *view,
                                 const QPoint &globalPosition) {
    if (!view || !view->viewport())
        return {};
    const QPoint viewportPosition =
        view->viewport()->mapFromGlobal(globalPosition);
    const QModelIndex index = view->indexAt(viewportPosition);
    if (!index.isValid())
        return {};
    return index.data(Qt::DisplayRole).toString().trimmed();
}

QAbstractItemView *resourceListViewFromAncestry(QWidget *widget) {
    if (!widget)
        return nullptr;
    for (QObject *current = widget; current;
         current = current->parent()) {
        if (auto *view = qobject_cast<QAbstractItemView *>(current)) {
            if (isAssetPreviewView(view))
                return view;
        }
    }
    return nullptr;
}

void clearAssetTooltipContext() {
    g_assetTooltipContext = AssetTooltipContext{};
}

bool assetTooltipContextStillMatches(const AssetTooltipContext &context) {
    if (!g_enabled || !context.isValid() || !context.view->viewport())
        return false;
    if (!context.view->isVisible() || !context.view->viewport()->isVisible())
        return false;
    const QPoint viewportPosition =
        context.view->viewport()->mapFromGlobal(QCursor::pos());
    return context.view->indexAt(viewportPosition) == context.index;
}

QString assetTooltipTextWithTranslation(const QString &text,
                                        const QString &source,
                                        const QString &translation) {
    if (text.isEmpty() || source.isEmpty() || translation.isEmpty())
        return text;
    QString adjusted = text;
    if (!Qt::mightBeRichText(text)) {
        const int sourcePosition = adjusted.indexOf(source);
        if (sourcePosition >= 0) {
            adjusted.insert(sourcePosition + source.size(),
                            QLatin1Char('\n') + translation);
            return adjusted;
        }
        return adjusted + QLatin1Char('\n') + translation;
    }

    const QString escapedSource = source.toHtmlEscaped();
    const QString escapedTranslation = translation.toHtmlEscaped();
    const int sourcePosition = adjusted.indexOf(
        escapedSource, 0, Qt::CaseSensitive);
    if (sourcePosition >= 0) {
        adjusted.insert(sourcePosition + escapedSource.size(),
                        QStringLiteral("<br/>") + escapedTranslation);
        return adjusted;
    }

    int fallbackPosition = adjusted.lastIndexOf(
        QStringLiteral("</body>"), -1, Qt::CaseInsensitive);
    if (fallbackPosition < 0) {
        fallbackPosition = adjusted.lastIndexOf(
            QStringLiteral("</html>"), -1, Qt::CaseInsensitive);
    }
    const QString fallback = QStringLiteral("<br/>") + escapedTranslation;
    if (fallbackPosition >= 0)
        adjusted.insert(fallbackPosition, fallback);
    else
        adjusted += fallback;
    return adjusted;
}

bool injectAssetTranslationIntoLabel(QLabel *label,
                                     const AssetTooltipContext &context,
                                     bool allowHeightGrowth,
                                     QEvent::Type triggerType) {
    if (!label || !assetTooltipContextStillMatches(context))
        return false;
    const int beforeWidth = label->width();
    const int beforeHeight = label->height();
    QSize beforeHint = label->sizeHint();
    const bool containsOurTranslation =
        label->property("sp_asset_preview_source").toString() ==
            context.source &&
        label->property("sp_asset_preview_translation").toString() ==
            context.translation &&
        label->text().contains(context.translation);
    if (containsOurTranslation) {
        label->setProperty("sp_asset_preview_translation", context.translation);
        // Painter's first useful event can be Paint. The translation may
        // therefore already exist by the time Show arrives, while the widget
        // still has its cached native height. Complete the pending growth
        // before allowing that paint/show event to continue.
        if (allowHeightGrowth) {
            if (!label->property(
                    "sp_asset_preview_original_min_height").isValid())
                label->setProperty("sp_asset_preview_original_min_height",
                                   label->minimumHeight());
            label->setMinimumHeight(beforeHint.height());
            if (label->height() != beforeHint.height())
                label->resize(label->width(), beforeHint.height());
        }
        tooltipDiag(QStringLiteral(
            "GEOMETRY existing event=%1 size=%2x%3 hint=%4x%5 final=%6x%7 "
            "lines=%8")
                        .arg(int(triggerType))
                        .arg(beforeWidth)
                        .arg(beforeHeight)
                        .arg(beforeHint.width())
                        .arg(beforeHint.height())
                        .arg(label->width())
                        .arg(label->height())
                        .arg(label->text().count(QLatin1Char('\n')) +
                             label->text().count(QStringLiteral("<br"),
                                                 Qt::CaseInsensitive) + 1));
        return true;
    }
    // A minimum height installed by the previous native refresh also clamps
    // sizeHint(). Temporarily restore Painter's original minimum before
    // measuring the fresh English content, otherwise every mouse event adds
    // another line (333, 349, 365, ...).
    const QVariant savedOriginalMinimum = label->property(
        "sp_asset_preview_original_min_height");
    const int lockedMinimum = label->minimumHeight();
    if (savedOriginalMinimum.isValid()) {
        label->setMinimumHeight(savedOriginalMinimum.toInt());
        beforeHint = label->sizeHint();
    }
    const QString adjusted = assetTooltipTextWithTranslation(
        label->text(), context.source, context.translation);
    if (adjusted == label->text()) {
        if (savedOriginalMinimum.isValid())
            label->setMinimumHeight(lockedMinimum);
        return false;
    }
    label->setProperty("sp_asset_preview_translation", context.translation);
    label->setProperty("sp_asset_preview_source", context.source);
    label->setText(adjusted);
    const QSize adjustedHint = label->sizeHint();
    // QTipLabel caches its English-only sizeHint. After setText(), that stale
    // hint can still report the native two-line height and clip Painter's
    // metadata row. Reserve one explicit font line before the first paint,
    // while preserving the native width and every byte of native rich text.
    if (allowHeightGrowth) {
        label->ensurePolished();
        // After setText(), this hint is the stable natural height of the
        // complete image + English + Chinese + native metadata document.
        // QTipLabel's pre-injection hint is contaminated by its current
        // window height, so adding a line to that value grows forever.
        const int requiredHeight = adjustedHint.height();
        if (!label->property("sp_asset_preview_original_min_height").isValid())
            label->setProperty("sp_asset_preview_original_min_height",
                               label->minimumHeight());
        // Painter rewrites the native tooltip on every mouse move and calls
        // resize() with the English-only height. A temporary minimum keeps
        // those legitimate refreshes from shrinking the visible popup; it is
        // restored as soon as QTipLabel hides.
        label->setMinimumHeight(requiredHeight);
        if (label->height() != requiredHeight)
            label->resize(label->width(), requiredHeight);
    }
    tooltipDiag(QStringLiteral(
        "GEOMETRY injected event=%1 grow=%2 before=%3x%4 beforeHint=%5x%6 "
        "adjustedHint=%7x%8 final=%9x%10 lines=%11")
                    .arg(int(triggerType))
                    .arg(allowHeightGrowth ? 1 : 0)
                    .arg(beforeWidth)
                    .arg(beforeHeight)
                    .arg(beforeHint.width())
                    .arg(beforeHint.height())
                    .arg(adjustedHint.width())
                    .arg(adjustedHint.height())
                    .arg(label->width())
                    .arg(label->height())
                    .arg(adjusted.count(QLatin1Char('\n')) +
                         adjusted.count(QStringLiteral("<br"),
                                        Qt::CaseInsensitive) + 1));
    tooltipDiag(QStringLiteral("INJECT QTipLabel source=[%1] translation=[%2]")
                    .arg(context.source, context.translation));
    return true;
}

void restoreAssetTooltipDecoration(QWidget *popup) {
    if (!popup)
        return;
    if (auto *label = qobject_cast<QLabel *>(popup)) {
        if (QString::fromLatin1(label->metaObject()->className()) ==
            QStringLiteral("QTipLabel")) {
            const QVariant originalMinimum = label->property(
                "sp_asset_preview_original_min_height");
            if (originalMinimum.isValid())
                label->setMinimumHeight(originalMinimum.toInt());
            label->setProperty("sp_asset_preview_original_min_height",
                               QVariant());
            label->setProperty("sp_asset_preview_translation", QVariant());
            label->setProperty("sp_asset_preview_source", QVariant());
        }
    }
    const auto injectedLabels = popup->findChildren<QLabel *>(
        QStringLiteral("sp_asset_preview_translation"),
        Qt::FindDirectChildrenOnly);
    for (QLabel *injected : injectedLabels) {
        if (popup->layout())
            popup->layout()->removeWidget(injected);
        injected->deleteLater();
    }
}

void restoreAllAssetTooltipDecorations() {
    for (QWidget *widget : QApplication::topLevelWidgets())
        restoreAssetTooltipDecoration(widget);
}

bool injectAssetTranslationIntoCustomPreview(
    QWidget *popup, const AssetTooltipContext &context) {
    if (!popup || popup->windowType() != Qt::ToolTip ||
        !assetTooltipContextStillMatches(context))
        return false;
    if (auto *existing = popup->findChild<QLabel *>(
            QStringLiteral("sp_asset_preview_translation"))) {
        existing->setText(context.translation);
        return true;
    }
    auto *layout = qobject_cast<QBoxLayout *>(popup->layout());
    if (!layout)
        return false;
    auto *label = new QLabel(context.translation, popup);
    label->setObjectName(QStringLiteral("sp_asset_preview_translation"));
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(label);
    popup->adjustSize();
    tooltipDiag(QStringLiteral("INJECT custom class=%1 source=[%2] translation=[%3]")
                    .arg(QString::fromLatin1(popup->metaObject()->className()),
                         context.source, context.translation));
    return true;
}

bool isAssetPreviewCandidate(QWidget *widget) {
    if (!widget)
        return false;
    if (QString::fromLatin1(widget->metaObject()->className()) ==
        QStringLiteral("QTipLabel"))
        return true;
    return widget->isWindow() && widget->windowType() == Qt::ToolTip;
}

bool injectAssetTranslationIntoPreview(QWidget *widget,
                                       bool allowHeightGrowth,
                                       QEvent::Type triggerType) {
    const AssetTooltipContext context = g_assetTooltipContext;
    if (!context.isValid() || !isAssetPreviewCandidate(widget))
        return false;
    tooltipDiag(QStringLiteral(
        "PREVIEW CANDIDATE class=%1 name=%2 type=%3 window=%4 visible=%5 "
        "age=%6 match=%7 layout=%8")
                    .arg(widget
                             ? QString::fromLatin1(
                                   widget->metaObject()->className())
                             : QStringLiteral("<null>"),
                         widget ? widget->objectName() : QString())
                    .arg(widget ? int(widget->windowType()) : -1)
                    .arg(widget && widget->isWindow() ? 1 : 0)
                    .arg(widget && widget->isVisible() ? 1 : 0)
                    .arg(QDateTime::currentMSecsSinceEpoch() - context.createdAt)
                    .arg(assetTooltipContextStillMatches(context) ? 1 : 0)
                    .arg(widget && widget->layout()
                             ? QString::fromLatin1(
                                   widget->layout()->metaObject()->className())
                             : QStringLiteral("<none>")));
    if (!assetTooltipContextStillMatches(context))
        return false;
    bool injected = false;
    if (auto *label = qobject_cast<QLabel *>(widget)) {
        if (QString::fromLatin1(label->metaObject()->className()) ==
            QStringLiteral("QTipLabel"))
            injected = injectAssetTranslationIntoLabel(
                label, context, allowHeightGrowth, triggerType);
    } else if (widget && widget->isWindow()) {
        injected = injectAssetTranslationIntoCustomPreview(widget, context);
    }
    return injected;
}

void translateWidget(QWidget *widget) {
    if (!widget || !g_enabled)
        return;

    // 纯显示层：文本永存控件原文，翻译只发生在绘制层
    // （QPainter::drawText 钩子 generalPainterTranslation 与显示层 delegate）。
    // 这里只保留“安装显示层 delegate”这一项必要工作，普通控件的
    // 文字不再 setText/setTitle/setPlaceholderText，避免改写原文。
    if (widget->windowType() == Qt::ToolTip ||
        QString::fromLatin1(widget->metaObject()->className()) == QStringLiteral("QTipLabel") ||
        (widget->window() && widget->window()->windowType() == Qt::ToolTip))
        return;

    observePainterAssetSearch(widget);

    // Painter 下拉项经 QTextLayout 绘制，drawText 钩子覆盖不到。包装其原生
    // delegate，并只给 paint() 传入 DisplayRole 已翻译的代理索引；真实 model
    // 与 Painter 的分组字体、缩进、选中样式均保持不变。
    if (auto *combo = qobject_cast<QComboBox *>(widget))
        installComboDisplayDelegate(combo->view(), combo);

    auto *itemView = qobject_cast<QAbstractItemView *>(widget);
    if (!itemView || shouldExcludeLayersPanel(itemView))
        return;

    if (QComboBox *combo = owningComboBox(itemView))
        installComboDisplayDelegate(itemView, combo);

    const QString className = QString::fromLatin1(itemView->metaObject()->className());
    if (isDesignerResourceList(itemView) || isDesignerLibraryTree(itemView)) {
        installAssetDelegate(itemView);
        return;
    }
    if (QComboBox *combo = owningComboBox(itemView);
        isLayerChannelSelector(combo)) {
        lockLayerChannelPopupWidth(combo);
        return;
    }
    if (g_translateLayersPanel && isInsideLayersPanel(itemView)) {
        installAssetDelegate(itemView, false, true);
        return;
    }
    const bool mainResourceView =
        className == QStringLiteral("Alg::ResourceListView") &&
        itemView->objectName() == QStringLiteral("resources");
    const bool pickerView = isResourcePickerView(itemView);
    const bool folderTree = isResourceFolderTree(itemView);
    if (mainResourceView || pickerView || folderTree)
        installAssetDelegate(itemView, pickerView);
}

QString originalTextAt(QWidget *widget, const QPoint &position) {
    if (!widget || !g_enabled)
        return {};

    // Color editors are composite controls and may deliver the tooltip event
    // through an internal QLabel/QWidget rather than Alg::ColorButton itself.
    // Never replace Painter's color-editing hover behaviour with an English
    // original-text hint.
    for (QObject *current = widget; current; current = current->parent()) {
        const QString className =
            QString::fromLatin1(current->metaObject()->className());
        const QString objectName = current->objectName();
        if (className == QStringLiteral("Alg::ColorButton") ||
            className.contains(QStringLiteral("ColorPicker"),
                               Qt::CaseInsensitive) ||
            className.contains(QStringLiteral("ColorEditor"),
                               Qt::CaseInsensitive) ||
            objectName == QStringLiteral("colorZone"))
            return {};
        if (className == QStringLiteral("Alg::AbstractDataView"))
            break;
    }

    // QMenu paints all entries itself, so there is no child label from which
    // the generic tooltip path can recover the source. Only actions actually
    // translated by this plug-in carry this marker; Painter's native entries
    // and their own help text remain untouched.
    if (auto *menu = qobject_cast<QMenu *>(widget)) {
        QAction *action = menu->actionAt(position);
        if (!action || action->isSeparator())
            return {};
        const QString source = action->property(kSourceProperty).toString().trimmed();
        if (source.isEmpty())
            return {};
        QString displayed = action->text().trimmed();
        displayed.remove(u'&');
        return menuTranslation(menu, source) == displayed ? source : QString();
    }

    // The open part of a QComboBox is an independent item-view viewport.
    // Resolve the hovered row through its owning combo instead of treating it
    // as an asset view (whose thumbnail tooltip must remain untouched).
    if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget())) {
        if (widget == view->viewport()) {
            if (QComboBox *combo = owningComboBox(view)) {
                const QModelIndex index = view->indexAt(position);
                if (!index.isValid())
                    return {};
                const int row = index.row();
                const QString displayed = index.data(Qt::DisplayRole).toString().trimmed();
                const QString source = comboStoredSource(combo, row);
                if (!source.isEmpty()) {
                    const QString expected = translated(
                        source, false,
                        translationControlId(combo, source));
                    if (expected == displayed)
                        return source;
                }
                const auto original = g_originals.constFind(displayed);
                return original == g_originals.cend() ? QString() : original.value();
            }
        }
    }

    // Preserve existing tooltips on non-label controls. Parameter labels are
    // handled specially: replace Painter's long description with the concise
    // English source name from our reverse dictionary.
    if (!widget->toolTip().isEmpty() && !qobject_cast<QLabel *>(widget))
        return {};

    // Original-text hints are useful for named UI concepts, not for values or
    // direct-manipulation controls. Painter also has custom subclasses whose
    // names identify the same editor categories.
    const QString widgetClass = QString::fromLatin1(widget->metaObject()->className());
    if (qobject_cast<QAbstractSlider *>(widget) ||
        qobject_cast<QAbstractSpinBox *>(widget) ||
        qobject_cast<QLineEdit *>(widget) ||
        widgetClass.contains(QStringLiteral("Slider"), Qt::CaseInsensitive) ||
        widgetClass.contains(QStringLiteral("SpinBox"), Qt::CaseInsensitive) ||
        widgetClass.contains(QStringLiteral("Numeric"), Qt::CaseInsensitive) ||
        widgetClass.contains(QStringLiteral("Number"), Qt::CaseInsensitive) ||
        widgetClass.contains(QStringLiteral("Color"), Qt::CaseInsensitive))
        return {};

    // Painter builds a slider from several plain QWidget/QLabel children.
    // Inspecting only the leaf type therefore misses its title, value field
    // and track. Reject the entire value-editor family through its parent chain.
    if (!qobject_cast<QLabel *>(widget)) {
        for (QObject *current = widget; current; current = current->parent()) {
            const QString className = QString::fromLatin1(current->metaObject()->className());
            const QString objectName = current->objectName();
            if (className == QStringLiteral("Alg::Slider") ||
                className == QStringLiteral("Alg::SliderHeader") ||
                className == QStringLiteral("Alg::InternalSlider") ||
                className == QStringLiteral("Alg::CustomLineEdit") ||
                className == QStringLiteral("Alg::ColorButton") ||
                objectName == QStringLiteral("colorZone") ||
                objectName == QStringLiteral("value"))
                return {};
            if (className == QStringLiteral("Alg::AbstractDataView"))
                break;
        }
    }

    // Painter owns asset-view tooltips and uses them for thumbnail/large
    // previews, so those events must remain untouched. The resource folder
    // tree has no asset preview; keep the useful English-original tooltip only
    // there. Translation itself still happens in the delegate paint path.
    if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget())) {
        if (widget == view->viewport()) {
            if (isResourceFolderTree(view)) {
                const QModelIndex index = view->indexAt(position);
                if (index.isValid()) {
                    const QString source =
                        index.data(Qt::DisplayRole).toString().trimmed();
                    if (g_translations.contains(source))
                        return source;
                }
            }
            if (g_translateLayersPanel && isInsideLayersPanel(view)) {
                const QModelIndex index = view->indexAt(position);
                if (index.isValid()) {
                    const QString source =
                        index.data(Qt::DisplayRole).toString().trimmed();
                    if (g_translations.contains(source))
                        return source;
                }
            }
            return {};
        }
    }

    QString displayed;
    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        displayed = button->text();
    else if (auto *label = qobject_cast<QLabel *>(widget))
        displayed = label->text();
    else if (auto *group = qobject_cast<QGroupBox *>(widget))
        displayed = group->title();
    else if (auto *menuBar = qobject_cast<QMenuBar *>(widget)) {
        QAction *action = menuBar->actionAt(position);
        if (!action)
            return {};
        displayed = action->text();
    }
    else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        displayed = combo->currentText();
        const QString source = comboStoredSource(combo, combo->currentIndex());
        if (!source.isEmpty() &&
            translated(source, false, translationControlId(combo, source)) == displayed)
            return source;
    }
    else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
        const int tab = tabs->tabAt(position);
        if (tab >= 0) {
            displayed = tabs->tabText(tab);
            const QVariant storedVariant = tabs->tabData(tab);
            const QString source = storedVariant.isValid()
                                       ? storedVariant.toString()
                                       : QString();
            if (!source.isEmpty() &&
                translated(source, false, translationControlId(tabs, source)) == displayed)
                return source;
        }
    }
    // A QDockWidget covers its complete panel, including large blank content
    // areas. Treating its window title as text under the cursor therefore
    // produced an unrelated tooltip (for example "Properties - Fill") almost
    // anywhere inside the properties panel. The real title-bar label is a
    // separate child widget and is handled by the QLabel path above.
    else if (qobject_cast<QDockWidget *>(widget))
        return {};

    displayed.remove(u'&');
    displayed = displayed.trimmed();
    const QString storedSource = widget->property(kSourceProperty).toString();
    if (!storedSource.isEmpty() &&
        translated(storedSource, false,
                   translationControlId(widget, storedSource)) == displayed)
        return storedSource;
    const auto found = g_originals.constFind(displayed);
    return found == g_originals.cend() ? QString() : found.value();
}

bool shouldSuppressTooltip(QWidget *widget) {
    if (!widget)
        return false;

    // Apply this test before the QLabel exception below. Painter's color
    // control contains label-like children, and those children must not revive
    // the plug-in's original-text tooltip while the color swatch is hovered.
    for (QObject *current = widget; current; current = current->parent()) {
        const QString className =
            QString::fromLatin1(current->metaObject()->className());
        const QString objectName = current->objectName();
        if (className == QStringLiteral("Alg::ColorButton") ||
            className.contains(QStringLiteral("ColorPicker"),
                               Qt::CaseInsensitive) ||
            className.contains(QStringLiteral("ColorEditor"),
                               Qt::CaseInsensitive) ||
            objectName == QStringLiteral("colorZone"))
            return true;
        if (className == QStringLiteral("Alg::AbstractDataView"))
            break;
    }

    // A parameter title is meaningful translated text even when it is a child
    // of Alg::Slider or owns Painter's long descriptive tooltip. The tooltip
    // event will be replaced by the concise English source label.
    if (qobject_cast<QLabel *>(widget))
        return false;

    if (qobject_cast<QAbstractSlider *>(widget) ||
        qobject_cast<QAbstractSpinBox *>(widget) ||
        qobject_cast<QLineEdit *>(widget))
        return true;

    for (QObject *current = widget; current; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        const QString objectName = current->objectName();
        if (className == QStringLiteral("Alg::Slider") ||
            className == QStringLiteral("Alg::SliderHeader") ||
            className == QStringLiteral("Alg::InternalSlider") ||
            className == QStringLiteral("Alg::CustomLineEdit") ||
            className == QStringLiteral("Alg::ColorButton") ||
            objectName == QStringLiteral("colorZone") ||
            objectName == QStringLiteral("value"))
            return true;
        if (className == QStringLiteral("Alg::AbstractDataView"))
            break;
    }
    return false;
}

// 宿主控件自带原生悬浮提示时不覆盖：沿父级（到窗口为止）检查 setToolTip。
bool hasNativeTooltip(QWidget *widget) {
    for (QWidget *current = widget; current;
         current = current->parentWidget()) {
        if (!current->toolTip().isEmpty())
            return true;
        if (current->isWindow())
            break;
    }
    return false;
}

QString contextSourceAt(QWidget *widget, const QPoint &position) {
    if (!widget || shouldExcludeLayersPanel(widget))
        return {};

    if (auto *graphView = qobject_cast<QGraphicsView *>(widget->parentWidget())) {
        if (widget == graphView->viewport() && isDesignerGraphView(graphView)) {
            QGraphicsItem *item = graphView->itemAt(position);
            for (QGraphicsItem *current = item; current;
                 current = current->parentItem()) {
                const QString title = graphFullTitleFromItem(current).trimmed();
                if (!title.isEmpty())
                    return title;
            }
        }
    }

    if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget())) {
        if (widget == view->viewport()) {
            const QModelIndex index = view->indexAt(position);
            if (index.isValid()) {
                const QString displayed =
                    index.data(Qt::DisplayRole).toString().trimmed();
                if (QComboBox *combo = owningComboBox(view)) {
                    const QString source = comboStoredSource(combo, index.row());
                    if (!source.isEmpty())
                        return source;
                    const auto original = g_originals.constFind(displayed);
                    if (original != g_originals.cend())
                        return original.value();
                    // 原文已是中文（或未命中词库）时也允许弹窗，
                    // 否则下拉框弹出项 Ctrl+右键会没有反应。
                    return displayed;
                }
                return displayed;
            }
            return {};
        }
    }

    QString displayed;
    if (auto *menu = qobject_cast<QMenu *>(widget)) {
        QAction *action = menu->actionAt(position);
        if (!action || action->isSeparator())
            return {};
        displayed = action->text();
        const QString storedSource = action->property(kSourceProperty).toString();
        if (!storedSource.isEmpty() &&
            menuTranslation(menu, storedSource) == displayed)
            return storedSource;
        // Chinese text without our marker is official localization. Allow the
        // editor to inspect it; editTranslation() will show a warning.
        if (containsCjk(displayed))
            return displayed.trimmed();
    } else if (auto *button = qobject_cast<QAbstractButton *>(widget))
        displayed = button->text();
    else if (auto *label = qobject_cast<QLabel *>(widget))
        displayed = label->text();
    else if (auto *group = qobject_cast<QGroupBox *>(widget))
        displayed = group->title();
    else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        displayed = combo->currentText();
        const QString source = comboStoredSource(combo, combo->currentIndex());
        if (!source.isEmpty())
            return source;
    }
    else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
        const int tab = tabs->tabAt(position);
        if (tab >= 0) {
            displayed = tabs->tabText(tab);
            const QVariant storedVariant = tabs->tabData(tab);
            const QString source = storedVariant.isValid()
                                       ? storedVariant.toString()
                                       : QString();
            if (!source.isEmpty())
                return source;
        }
    }
    // Dock widgets span the whole panel. Their window title is not a discrete
    // text control under the cursor, so editing it would also trigger when the
    // user Ctrl+right-clicks an unrelated blank area of the panel.
    else if (qobject_cast<QDockWidget *>(widget))
        return {};

    displayed.remove(u'&');
    displayed = displayed.trimmed();
    const QString storedSource = widget->property(kSourceProperty).toString();
    if (!storedSource.isEmpty() && g_translations.value(storedSource) == displayed)
        return storedSource;
    const auto original = g_originals.constFind(displayed);
    if (original != g_originals.cend())
        return original.value();
    if (containsCjk(displayed))
        return displayed;
    return displayed;
}

QString contextSourceAtHierarchy(QWidget *widget, const QPoint &position) {
    if (!widget)
        return {};
    const QPoint globalPosition = widget->mapToGlobal(position);
    for (QWidget *current = widget; current;
         current = current->parentWidget()) {
        const QString source = contextSourceAt(
            current, current->mapFromGlobal(globalPosition)
        );
        if (!source.isEmpty())
            return source;
        if (current->isWindow())
            break;
    }
    return {};
}

// 控件所属面板（广义：QDockWidget 或 QDialog/顶层窗口）：
// 同时返回 objectName 和窗口标题，缺省用 None 占位。
QString controlPanelName(QWidget *widget) {
    if (!widget)
        return {};
    QWidget *target = nullptr;
    for (QWidget *current = widget; current;
         current = current->parentWidget()) {
        if (auto *dock = qobject_cast<QDockWidget *>(current)) {
            target = dock;
            break;
        }
        if (current->isWindow()) {
            // 弹出菜单/下拉弹出层本身是无标题的临时窗口，
            // 继续沿父级往上找真正的宿主窗口（QDialog/QMainWindow）。
            const bool isPopup =
                current->windowType() == Qt::Popup ||
                current->windowType() == Qt::ToolTip;
            const bool hasTitle =
                !current->windowTitle().trimmed().isEmpty();
            if (!isPopup &&
                (hasTitle || qobject_cast<QDialog *>(current) ||
                 qobject_cast<QMainWindow *>(current))) {
                target = current;
                break;
            }
            if (!current->parentWidget())
                break;
        }
    }
    if (!target)
        target = widget->window();
    if (!target)
        return {};
    const QString objectName = target->objectName();
    const QString windowTitle = target->windowTitle().trimmed();
    return QStringLiteral("objectName：%1，窗口标题：%2")
        .arg(objectName.isEmpty() ? QStringLiteral("None") : objectName)
        .arg(windowTitle.isEmpty() ? QStringLiteral("None") : windowTitle);
}

QString controlUniqueId(QWidget *widget, const QString &sourceText) {
    // 返回用于生成稳定 ID 的规范字符串：
// 上级类名||自身类名||自身 objectName||原文，
// 上级类名指被点击控件上级控件（parentWidget）的类名，
    // 自身指被点击控件本身；哪一项没有就用 None 占位。
    if (!widget)
        return {};

    const QString parentClassName =
        widget->parentWidget()
            ? QString::fromLatin1(
                  widget->parentWidget()->metaObject()->className())
            : QString();
    const QString ownClassName =
        QString::fromLatin1(widget->metaObject()->className());
    const QString ownObjectName = widget->objectName();
    // 菜单/按钮文本可能带助记符 "&"，词库键统一用去掉助记符的原文。
    QString normalizedSource = sourceText;
    normalizedSource.remove(u'&');

    auto orNone = [](const QString &value) {
        return value.isEmpty() ? QStringLiteral("None") : value;
    };
return orNone(parentClassName) + QStringLiteral("||")
    + orNone(ownClassName) + QStringLiteral("||")
    + orNone(ownObjectName) + QStringLiteral("||")
    + orNone(normalizedSource);
}

bool saveTranslation(const QString &source, const QString &target,
                     QString *error,
                     const QString &fixedPath = QString()) {
    const QString translationPath = fixedPath.isEmpty()
        ? g_fallbackPath : fixedPath;
    if (translationPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("未配置可写入的翻译文件。");
        return false;
    }

    QJsonObject root;
    QFile existing(translationPath);
    const bool existed = existing.exists();
    if (existed && existing.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(existing.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error)
                *error = QStringLiteral("Translation JSON is invalid: %1")
                             .arg(translationPath);
            existing.close();
            return false;
        }
        root = document.object();
        existing.close();
    } else if (existed) {
        if (error)
            *error = existing.errorString();
        return false;
    }

    if (!existed) {
        root.insert(QStringLiteral("$schema"), QStringLiteral("sp-translation-v1"));
        root.insert(QStringLiteral("id"), translationPath == g_fallbackPath
            ? QStringLiteral("user-added-translations")
            : QStringLiteral("plugin-edited-translations"));
        root.insert(QStringLiteral("language"), QStringLiteral("zh-CN"));
        root.insert(QStringLiteral("description"),
                    translationPath == g_fallbackPath
            ? QStringLiteral("Translations added from Substance 3D Painter")
            : QStringLiteral("Translations edited from Substance 3D Painter"));
    } else if (root.value(QStringLiteral("$schema")).toString() !=
               QStringLiteral("sp-translation-v1")) {
        if (error)
            *error = QStringLiteral("原始翻译文件格式无效：%1").arg(translationPath);
        return false;
    }
    QJsonObject translations =
        root.value(QStringLiteral("translations")).toObject();
    translations.insert(source, target);
    root.insert(QStringLiteral("translations"), translations);

    const QFileInfo info(translationPath);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error)
            *error = QStringLiteral("无法访问翻译文件目录：%1").arg(info.absolutePath());
        return false;
    }
    QSaveFile output(translationPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = output.errorString();
        return false;
    }
    output.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!output.commit()) {
        if (error)
            *error = output.errorString();
        return false;
    }
    return true;
}

// 将“译文等于原文”作为撤销全局自定义翻译处理。尤其不能把这种恒等映射
// 留在 user_added_zh.json 中：该文件的加载优先级最高，会遮蔽官方词库。
bool removeFallbackTranslation(const QString &source, QString *error,
                               bool *removed) {
    if (removed)
        *removed = false;
    if (g_fallbackPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("未配置用户翻译文件。");
        return false;
    }
    QFile existing(g_fallbackPath);
    if (!existing.exists())
        return true;
    if (!existing.open(QIODevice::ReadOnly)) {
        if (error)
            *error = existing.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(existing.readAll(), &parseError);
    existing.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Translation JSON is invalid: %1")
                         .arg(g_fallbackPath);
        return false;
    }
    QJsonObject root = document.object();
    if (root.value(QStringLiteral("$schema")).toString() !=
        QStringLiteral("sp-translation-v1")) {
        if (error)
            *error = QStringLiteral("原始翻译文件格式无效：%1")
                         .arg(g_fallbackPath);
        return false;
    }
    QJsonObject translations =
        root.value(QStringLiteral("translations")).toObject();
    if (!translations.contains(source))
        return true;
    translations.remove(source);
    root.insert(QStringLiteral("translations"), translations);
    QSaveFile output(g_fallbackPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error)
            *error = output.errorString();
        return false;
    }
    output.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!output.commit()) {
        if (error)
            *error = output.errorString();
        return false;
    }
    if (removed)
        *removed = true;
    return true;
}

void refreshTranslatedViews() {
    if (g_assetRowFilter)
        g_assetRowFilter->translationsChanged();
    for (QWidget *widget : QApplication::allWidgets()) {
        if (!widget)
            continue;
        if (auto *view = qobject_cast<QAbstractItemView *>(widget))
            view->viewport()->update();
        if (widget->isVisible())
            translateWidget(widget);
    }
}

// 右键弹窗的修饰键是否匹配（可自定义；NoModifier 表示禁用右键弹窗）。
QString modifierOnlySequence(Qt::KeyboardModifiers modifiers) {
    QStringList parts;
    if (modifiers & Qt::ControlModifier) {
        parts << QStringLiteral("Ctrl");
    }
    if (modifiers & Qt::AltModifier) {
        parts << QStringLiteral("Alt");
    }
    if (modifiers & Qt::ShiftModifier) {
        parts << QStringLiteral("Shift");
    }
    return parts.join(QLatin1Char('+'));
}

Qt::KeyboardModifiers effectiveMouseModifiers(
    Qt::KeyboardModifiers eventModifiers) {
    // Designer consumes Ctrl for several graph/navigation interactions and
    // some synthesized mouse/context-menu events consequently omit
    // ControlModifier even while the key is physically held. Merge the Qt
    // snapshot with Windows' current key state so Ctrl+right-click remains
    // reliable without weakening the configured shortcut check.
    Qt::KeyboardModifiers result = eventModifiers;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        result |= Qt::ControlModifier;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        result |= Qt::ShiftModifier;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        result |= Qt::AltModifier;
    return result;
}

bool editKeyActive(Qt::KeyboardModifiers modifiers) {
    if (g_editKey.isEmpty())
        return false;
    const Qt::KeyboardModifiers eventModifiers = modifiers;
    modifiers = effectiveMouseModifiers(modifiers);
    shortcutDiag(QStringLiteral(
        "EDIT modifiers event=0x%1 effective=0x%2 target=%3")
        .arg(int(eventModifiers), 0, 16)
        .arg(int(modifiers), 0, 16)
        .arg(g_editKey));
    const QString modifierOnly = modifierOnlySequence(modifiers);
    const QString pressed = g_heldEditKey
        ? QKeySequence(g_heldEditKey | int(modifiers)).toString()
        : modifierOnly;
    if (pressed.isEmpty() || pressed != g_editKey) {
        // 记录的最后按键可能早已松开（残留状态，例如从 Z+左键 切回
        // Ctrl+右键 后 Z 的松开事件丢失）：残留键会把组合拼成
        // “Ctrl+Z” 导致 Ctrl+右键 失效。确认该键已不在物理按下状态时
        // 清掉残留，并按纯修饰键重新判定当前这一次触发。
        if (g_heldEditKey != 0 && !heldKeyIsDown(g_heldEditKey)) {
            g_heldEditKey = 0;
            if (!modifierOnly.isEmpty() &&
                modifierOnly == g_editKey)
                return true;
        }
        return false;
    }
    if (g_heldEditKey) {
        // 残留的“最后按下键”会导致松开后的单击误触发：只有配置键
        // 在物理上仍处于按下状态时才认为组合成立；否则清掉残留状态。
        if (!heldKeyIsDown(g_heldEditKey)) {
            g_heldEditKey = 0;
            return false;
        }
    }
    return true;
}

void setTranslateLayersPanel(bool enabled) {
    if (g_translateLayersPanel == enabled)
        return;
    g_translateLayersPanel = enabled;
    // 纯显示层：开/关图层面板翻译只是切换显示层 delegate 的生效条件，
    // 无需改写任何文本，直接用 refreshTranslatedViews 重绘即可。
    refreshTranslatedViews();
}

// 主窗口怎么设置 UI 风格，所有插件弹窗就怎么设置：统一继承主窗口的调色板、
// 样式表、字体和图标，保证与软件主界面外观一致。
QWidget *hostStyleWindow(QWidget *reference) {
    QWidget *host = reference ? reference->window() : nullptr;
    if (!host)
        host = QApplication::activeWindow();
    if (!host) {
        const QWidgetList topLevel = QApplication::topLevelWidgets();
        for (QWidget *widget : topLevel) {
            if (widget->isVisible()) {
                host = widget;
                break;
            }
        }
    }
    return host;
}

void applyHostWindowStyle(QWidget *window, QWidget *reference) {
    if (!window)
        return;
    QWidget *host = hostStyleWindow(reference);
    if (host) {
        window->setPalette(host->palette());
        window->setStyleSheet(host->styleSheet());
        window->setFont(host->font());
        window->setWindowIcon(host->windowIcon());
    }
    if (window->windowIcon().isNull())
        window->setWindowIcon(QApplication::windowIcon());
}

void showHostMessage(QWidget *parent, const QString &title,
                     const QString &text,
                     QMessageBox::Icon icon = QMessageBox::Information) {
    QMessageBox box(parent);
    box.setWindowTitle(title);
    box.setText(text);
    box.setIcon(icon);
    applyHostWindowStyle(&box, parent);
    box.exec();
}

// 自定义 ID 的悬停注释：原生 ToolTip 会按宽度自动换行，这里用自绘
// QLabel（默认不换行）保证注释始终显示在一行。
class IdTipFilter final : public QObject {
public:
    explicit IdTipFilter(QObject *parent = nullptr) : QObject(parent) {}

    QLabel *tip = nullptr;
    QWidget *field = nullptr;

    bool eventFilter(QObject *object, QEvent *event) override {
        if (object != field)
            return false;
        if (event->type() == QEvent::Enter) {
            if (tip) {
                tip->adjustSize();
                tip->move(field->mapToGlobal(
                    QPoint(0, -tip->height() - 4)));
                tip->show();
                tip->raise();
            }
        } else if (event->type() == QEvent::Leave) {
            if (tip)
                tip->hide();
        }
        return false;
    }
};

void editTranslation(const QString &source, const QString &uniqueId,
                     const QString &panelName, QWidget *parent) {
    if (source.isEmpty())
        return;
    QString current = g_idTranslations.value(uniqueId);
    if (current.isNull())
        current = g_translations.value(source);
    if (current.isNull())
        current = source;

    QWidget *dialogParent = QApplication::activeWindow();
    if (!dialogParent)
        dialogParent = parent;
    if (qobject_cast<QMenu *>(dialogParent))
        dialogParent = nullptr;
    QDialog dialog(dialogParent);
    dialog.setWindowTitle(QStringLiteral("更改翻译"));
    dialog.setMinimumWidth(460);
    applyHostWindowStyle(&dialog, dialogParent);

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();
    auto *sourceEdit = new QLineEdit(source, &dialog);
    auto *currentEdit = new QLineEdit(current, &dialog);
    auto *targetEdit = new QLineEdit(current, &dialog);
    auto *uniqueIdEdit = new QLineEdit(uniqueId, &dialog);
    sourceEdit->setReadOnly(true);
    currentEdit->setReadOnly(true);
    uniqueIdEdit->setReadOnly(true);
    sourceEdit->setObjectName(QStringLiteral("sp_translation_source"));
    currentEdit->setObjectName(QStringLiteral("sp_translation_current"));
    targetEdit->setObjectName(QStringLiteral("sp_translation_target"));
    uniqueIdEdit->setObjectName(QStringLiteral("sp_translation_unique_id"));
    auto *idTip = new QLabel(
        QStringLiteral(
            "自定义 ID 格式：上级控件类名||自身控件类名||自身控件 objectName||原文"),
        &dialog);
    idTip->setObjectName(QStringLiteral("sp_translation_id_tip"));
    idTip->setWindowFlags(Qt::ToolTip);
    idTip->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    idTip->setStyleSheet(QStringLiteral(
        "QLabel { background: #2b2b2b; color: #ffffff;"
        " padding: 4px 8px; border: 1px solid #555555; }"));
    idTip->hide();
    auto *idTipFilter = new IdTipFilter(idTip);
    idTipFilter->tip = idTip;
    idTipFilter->field = uniqueIdEdit;
    uniqueIdEdit->installEventFilter(idTipFilter);
    auto *panelEdit = new QLineEdit(
        panelName.isEmpty() ? QStringLiteral("None") : panelName, &dialog);
    panelEdit->setReadOnly(true);
    panelEdit->setObjectName(QStringLiteral("sp_translation_panel_name"));
    form->addRow(QStringLiteral("自定义 ID："), uniqueIdEdit);
    form->addRow(QStringLiteral("所属面板："), panelEdit);
    form->addRow(QStringLiteral("原文："), sourceEdit);
    form->addRow(QStringLiteral("当前翻译："), currentEdit);
    form->addRow(QStringLiteral("新翻译："), targetEdit);
    layout->addLayout(form);

    auto *idCheck = new QCheckBox(
        QStringLiteral("保存到专项词库（control_ids_zh.json）"), &dialog);
    idCheck->setObjectName(QStringLiteral("sp_translation_save_to_id"));
    layout->addWidget(idCheck);

    if (containsCjk(source)) {
        auto *warning = new QLabel(
            QStringLiteral("这是软件官方提供的中文，不建议更改。保存后将由插件词库覆盖官方中文。"),
            &dialog);
        warning->setObjectName(QStringLiteral("sp_translation_official_warning"));
        warning->setWordWrap(true);
        warning->setStyleSheet(QStringLiteral(
            "QLabel { color: #ff5c5c; font-weight: 600; padding: 4px 0; }"));
        layout->addWidget(warning);
    }

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    // 输入框始终获得焦点并全选，保持原有体验；触发键若仍按着，
    // 由应用级过滤器在松开前只拦截该键的字符/输入法事件。
    targetEdit->selectAll();
    targetEdit->setFocus();

    g_editDialogOpen = true;
    const int dialogResult = dialog.exec();
    g_editDialogOpen = false;
    if (dialogResult != QDialog::Accepted)
        return;
    const QString target = targetEdit->text().trimmed();
    if (target.isEmpty()) {
        showHostMessage(parent, QStringLiteral("提示"),
                        QStringLiteral("翻译未改变，未写入词库"));
        return;
    }

    QString error;
    const bool saveToId = idCheck->isChecked() && !uniqueId.isEmpty();
    if (!saveToId && target == source) {
        bool removed = false;
        if (!removeFallbackTranslation(source, &error, &removed)) {
            showHostMessage(parent, QStringLiteral("删除翻译失败"), error,
                            QMessageBox::Critical);
            return;
        }
        if (removed) {
            // 词包合并只由 Python 实现；回调会执行既有的加载与同步流程，
            // 避免在原生层重复词包优先级规则。
            const bool reloadOk = g_dictionaryReloadCallback &&
                                  g_dictionaryReloadCallback() == 1;
            if (!reloadOk) {
                showHostMessage(parent, QStringLiteral("词库重载失败"),
                                QStringLiteral("已从 user_added_zh.json 删除该自定义词条，但当前界面未能重载词库。请重新加载插件。"),
                                QMessageBox::Warning);
                return;
            }
            refreshTranslatedViews();
            showHostMessage(parent, QStringLiteral("提示"),
                            QStringLiteral("已从 user_added_zh.json 删除该自定义词条，并已恢复默认翻译。"));
        } else {
            showHostMessage(parent, QStringLiteral("提示"),
                            QStringLiteral("没有可删除的用户自定义词条。"));
        }
        return;
    }
    if (target == current) {
        showHostMessage(parent, QStringLiteral("提示"),
                        QStringLiteral("翻译未改变，未写入词库"));
        return;
    }
    if (saveToId) {
        // 保存到专项词库：以完整控件 ID 为键写入 control_ids_zh.json。
        if (!saveTranslation(uniqueId, target, &error,
                             g_idTranslationPath)) {
            showHostMessage(parent, QStringLiteral("保存翻译失败"), error,
                            QMessageBox::Critical);
            return;
        }
        g_idTranslations.insert(uniqueId, target);
    } else {
        if (!saveTranslation(source, target, &error)) {
            showHostMessage(parent, QStringLiteral("保存翻译失败"), error,
                            QMessageBox::Critical);
            return;
        }
        // Keep historical reverse entries until restart so widgets currently
        // showing an older translation can still resolve back to the source.
        g_translations.insert(source, target);
        g_translationsFolded.insert(normalizeForMatch(source), target);
        g_originals.insert(target, source);
    }
    // 新词条可能让此前缓存为空结果的模糊/工具提示匹配立即生效。
    g_fuzzyResolved.clear();
    refreshTranslatedViews();
}

class TranslationUiFilter final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        const auto type = event->type();
        // Show covers a newly created tooltip. Painter reuses one visible
        // QTipLabel for later assets: QLabel::setText posts layout/update
        // work before the next paint, so inject there to avoid one frame of
        // untranslated content. Paint remains only as a compatibility
        // fallback for host versions that skip those preparation events.
        if ((type == QEvent::Show || type == QEvent::Polish ||
             type == QEvent::ShowToParent ||
             type == QEvent::LayoutRequest ||
             type == QEvent::UpdateRequest || type == QEvent::Paint) &&
            g_enabled) {
            QWidget *candidate = qobject_cast<QWidget *>(object);
            if (isAssetPreviewCandidate(candidate)) {
                const bool allowHeightGrowth =
                    type == QEvent::Show || type == QEvent::Polish ||
                    type == QEvent::ShowToParent ||
                    type == QEvent::LayoutRequest ||
                    type == QEvent::Paint;
                injectAssetTranslationIntoPreview(candidate,
                                                  allowHeightGrowth, type);
            }
        }
        // 快捷键识别：只“看见”组合键，动作延后一拍执行且不吞按键，
        // 避免在事件过滤器中同步弹出模态窗口，也不抢占宿主同名快捷键。
        if (type == QEvent::ShortcutOverride) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const int key = keyEvent->key();
            const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
            if (shortcutMatches(g_enableShortcut, key, modifiers)) {
                // 只记录按下，待 KeyRelease 时触发一次。
                g_enableShortcutArmed = key;
                shortcutDiag(QStringLiteral("ARM0 key=%1").arg(key));
            }
            return false;
        }
        // 记录当前按住的非修饰键，供“键盘序列+鼠标按键”触发判定。
        if (type == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const int key = keyEvent->key();
            if (key != Qt::Key_Control && key != Qt::Key_Shift &&
                key != Qt::Key_Alt && key != Qt::Key_Meta)
                g_heldEditKey = key;
            return false;
        }
        if (type == QEvent::KeyRelease) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (g_heldEditKey && keyEvent->key() == g_heldEditKey)
                g_heldEditKey = 0;
            if (g_enableShortcutArmed != 0 &&
                keyEvent->key() == g_enableShortcutArmed) {
                if (heldKeyIsDown(g_enableShortcutArmed)) {
                    // 物理上仍按着：宿主（F10 是菜单键）会合成重复的
                    // “松开”事件，忽略它们，等真正松开再触发。
                    shortcutDiag(QStringLiteral("KR0 spurious key=%1")
                                     .arg(keyEvent->key()));
                } else {
                    g_enableShortcutArmed = 0;
                    shortcutDiag(QStringLiteral("KR0 real key=%1")
                                     .arg(keyEvent->key()));
                    QTimer::singleShot(0, this, [] { fireShortcut(); });
                }
            }
            return false;
        }
        // 应用/窗口失焦时按键松开事件可能丢失，直接清掉残留按键，
        // 避免之后一次普通鼠标单击被误判为“按键+鼠标”组合。
        if (type == QEvent::WindowDeactivate) {
            g_heldEditKey = 0;
            g_enableShortcutArmed = 0;
            return false;
        }
        // Cleanup must run even after translation has been disabled; otherwise
        // a reused QTipLabel can retain the resource preview's minimum height.
        if (type == QEvent::Hide)
            restoreAssetTooltipDecoration(qobject_cast<QWidget *>(object));
        if (!g_enabled)
            return false;
        if (type == QEvent::Leave || type == QEvent::Hide) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && g_originalTooltipOwner == widget) {
                QToolTip::hideText();
                g_originalTooltipOwner.clear();
            }
            if (widget && g_assetTooltipContext.view &&
                (widget == g_assetTooltipContext.view ||
                 widget == g_assetTooltipContext.view->viewport()))
                clearAssetTooltipContext();
        }
        if (type == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            auto *menu = qobject_cast<QMenu *>(object);
            if (menu && mouse->button() == g_editButton &&
                g_editButton == Qt::RightButton &&
                editKeyActive(mouse->modifiers())) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint mousePos = mouse->position().toPoint();
#else
                const QPoint mousePos = mouse->pos();
#endif
                const QString source = contextSourceAt(menu, mousePos);
                if (!source.isEmpty()) {
                    if (g_editDialogOpen)
                        return false;
                    const QString uniqueId = controlUniqueId(menu, source);
                    const QString panelName = controlPanelName(menu);
                    QPointer<QWidget> safeWindow(menu->window());
                    QTimer::singleShot(
                        0, this,
                        [source, uniqueId, panelName, safeWindow]() {
                        QWidget *parent = safeWindow.data();
                        if (!parent)
                            parent = QApplication::activeWindow();
                        editTranslation(source, uniqueId, panelName, parent);
                    });
                    event->accept();
                    return true;
                }
            } else if (!menu && mouse->button() == g_editButton &&
                       g_editButton == Qt::RightButton &&
                       editKeyActive(mouse->modifiers())) {
                auto *widget = qobject_cast<QWidget *>(object);
                if (widget) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const QPoint mousePos = mouse->position().toPoint();
#else
                    const QPoint mousePos = mouse->pos();
#endif
                    const QString source =
                        contextSourceAtHierarchy(widget, mousePos);
                    if (!source.isEmpty()) {
                        if (g_editDialogOpen)
                            return false;
                        const QString uniqueId =
                            controlUniqueId(widget, source);
                        const QString panelName = controlPanelName(widget);
                        QPointer<QWidget> safeWidget(widget);
                        QTimer::singleShot(
                            0, this,
                            [source, uniqueId, panelName, safeWidget]() {
                            QWidget *parent = safeWidget
                                ? safeWidget->window()
                                : QApplication::activeWindow();
                            editTranslation(source, uniqueId, panelName,
                                            parent);
                        });
                        shortcutDiag(QStringLiteral(
                            "EDIT mouse-right source-length=%1")
                            .arg(source.size()));
                        event->accept();
                        return true;
                    }
                    shortcutDiag(QStringLiteral(
                        "EDIT mouse-right no-source class=%1")
                        .arg(QString::fromLatin1(
                            widget->metaObject()->className())));
                }
            } else if (!menu && mouse->button() == g_editButton &&
                       (g_editButton == Qt::LeftButton ||
                        g_editButton == Qt::MiddleButton) &&
                       editKeyActive(mouse->modifiers())) {
                // 左键/中键组合：对普通控件同样弹出“更改翻译”。
                auto *widget = qobject_cast<QWidget *>(object);
                if (widget) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                    const QPoint mousePos = mouse->position().toPoint();
#else
                    const QPoint mousePos = mouse->pos();
#endif
                    const QString source = contextSourceAt(widget, mousePos);
                    if (!source.isEmpty()) {
                        if (g_editDialogOpen)
                            return false;
                        const QString uniqueId =
                            controlUniqueId(widget, source);
                        const QString panelName = controlPanelName(widget);
                        QPointer<QWidget> safeWidget(widget);
                        QTimer::singleShot(
                            0, this,
                            [source, uniqueId, panelName, safeWidget]() {
                            if (safeWidget)
                                editTranslation(source, uniqueId, panelName,
                                                safeWidget.data());
                        });
                        event->accept();
                        return true;
                    }
                }
            }
        }
        if (type == QEvent::ContextMenu) {
            auto *context = static_cast<QContextMenuEvent *>(event);
            // A plain right-click belongs entirely to Painter. Translation
            // editing is an explicit Ctrl+right-click shortcut so it cannot
            // alter or compete with Painter's native context menus.
            if (g_editButton != Qt::RightButton ||
                !editKeyActive(context->modifiers()))
                return false;
            auto *widget = qobject_cast<QWidget *>(object);
            const QString source = contextSourceAtHierarchy(
                widget, context->pos()
            );
            if (source.isEmpty())
                return false;
            if (g_editDialogOpen)
                return false;
            const QString uniqueId = controlUniqueId(widget, source);
            const QString panelName = controlPanelName(widget);
            QPointer<QWidget> safeWidget(widget);
            QTimer::singleShot(
                0, this,
                [source, uniqueId, panelName, safeWidget]() {
                if (safeWidget)
                    editTranslation(source, uniqueId, panelName,
                                    safeWidget.data());
            });
            event->accept();
            return true;
        }
        if (type == QEvent::ToolTip) {
            auto *widget = qobject_cast<QWidget *>(object);
            auto *help = static_cast<QHelpEvent *>(event);
            if (QAbstractItemView *assetView =
                    resourceListViewFromAncestry(widget)) {
                const QString english =
                    assetPreviewRawDisplayAt(assetView, help->globalPos());
                const QString display =
                    assetPreviewDisplayAt(assetView, help->globalPos());
                if (!english.isEmpty() && !display.isEmpty() &&
                    display != english && containsCjk(display)) {
                    // Never consume Painter's ToolTip event. The host must be
                    // free to refresh its native preview image and metadata;
                    // we only remember the matching item and append Chinese
                    // during the preview widget's own update events below.
                    const QPoint viewportPosition =
                        assetView->viewport()->mapFromGlobal(help->globalPos());
                    const QModelIndex index =
                        assetView->indexAt(viewportPosition);
                    if (index.isValid()) {
                        const quint64 generation = ++g_assetTooltipGeneration;
                        g_assetTooltipContext = {
                            assetView,
                            QPersistentModelIndex(index),
                            english,
                            display,
                            help->globalPos(),
                            QDateTime::currentMSecsSinceEpoch(),
                            generation,
                        };
                        tooltipDiag(QStringLiteral(
                            "ASSET CONTEXT generation=%1 source=[%2] translation=[%3]")
                                        .arg(generation)
                                        .arg(english, display));
                    }
                } else {
                    clearAssetTooltipContext();
                }
            }
            // 插件自己的“更改翻译”弹窗放行原生 ToolTip（自定义 ID 的
            // 悬停注释），其余控件按原有规则抑制。
            const bool isPluginDialogWidget =
                widget && widget->objectName().startsWith(
                    QStringLiteral("sp_translation_"));
            if (shouldSuppressTooltip(widget) && !isPluginDialogWidget) {
                QToolTip::hideText();
                if (g_originalTooltipOwner == widget)
                    g_originalTooltipOwner.clear();
                event->accept();
                return true;
            }
            const QString source = originalTextAt(widget, help->pos());
            // 宿主控件自带原生悬浮提示时不覆盖，保留宿主自己的提示。
            if (!source.isNull() && !hasNativeTooltip(widget)) {
                QToolTip::showText(help->globalPos(), source, widget);
                g_originalTooltipOwner = widget;
                event->accept();
                return true;
            }
        }
        // The host rewrites some parameter labels while a value is edited.
        // Paint is the last safe interception point before that English text
        // reaches the screen. translateWidget is idempotent and only calls a
        // setter when the current text actually has a dictionary replacement.
        if (type == QEvent::Paint) {
            if (auto *widget = qobject_cast<QWidget *>(object)) {
                if (qobject_cast<QLabel *>(widget) ||
                    qobject_cast<QAbstractButton *>(widget) ||
                    qobject_cast<QComboBox *>(widget)) {
                    translateWidget(widget);
                } else {
                    auto *view = qobject_cast<QAbstractItemView *>(widget);
                    if (!view)
                        view = qobject_cast<QAbstractItemView *>(
                            widget->parentWidget());
                    // 下拉框 popup 的 QComboBoxListView 父链可能不直接到达
                    // QComboBox（经 QComboBoxPrivateContainer），用全量
                    // owningComboBox 找到所属 combo 并确保装上显示层 delegate。
                    if (view) {
                        if (QComboBox *combo = owningComboBox(view))
                            installComboDisplayDelegate(view, combo);
                    }
                }
            }
        } else if (type == QEvent::Show || type == QEvent::Polish ||
                   type == QEvent::LayoutRequest ||
                   type == QEvent::ActionAdded) {
            if (auto *widget = qobject_cast<QWidget *>(object))
                translateWidget(widget);
        }
        return false;
    }
};

TranslationUiFilter *g_filter = nullptr;
QTimer *g_fallbackTimer = nullptr;
bool g_fallbackScanEnabled = false;

void scanVisibleWidgets() {
    if (!g_enabled)
        return;
    if (appClosingDown())
        return;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget && widget->isVisible())
            translateWidget(widget);
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl sp_delegate_api_version() { return 14; }

extern "C" __declspec(dllexport) const wchar_t *__cdecl sp_delegate_build_id() {
    // 构建标识：用于确认正在运行的 DLL 是否包含最新搜索逻辑。
    return L"20260818-v1.3.6-sp-sd-cjk-search";
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_fallback_path(
    const wchar_t *path) {
    g_fallbackPath = path ? QString::fromWCharArray(path) : QString();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_id_path(
    const wchar_t *path) {
    g_idTranslationPath = path ? QString::fromWCharArray(path) : QString();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_clear_translations() {
    g_translations.clear();
    g_originals.clear();
    g_idTranslations.clear();
    g_fuzzyResolved.clear();
    g_translationsFolded.clear();
    if (g_assetRowFilter)
        g_assetRowFilter->translationsChanged();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_fuzzy_match(
    int enabled) {
    g_fuzzyMatchEnabled = enabled != 0;
    g_fuzzyResolved.clear();
    if (g_assetRowFilter)
        g_assetRowFilter->translationsChanged();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_fallback_scan(
    int enabled) {
    g_fallbackScanEnabled = enabled != 0;
    if (!g_fallbackTimer)
        return;
    if (g_fallbackScanEnabled && !g_fallbackTimer->isActive())
        g_fallbackTimer->start();
    else if (!g_fallbackScanEnabled && g_fallbackTimer->isActive())
        g_fallbackTimer->stop();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_edit_modifier(
    int mask) {
    // 旧版兼容：修饰键掩码转成键盘序列。
    g_editKey = modifierOnlySequence(Qt::KeyboardModifiers(mask));
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_edit_key(
    const wchar_t *sequence) {
    g_editKey = sequence
        ? QString::fromWCharArray(sequence).trimmed()
        : QString();
    // 触发方式变更后，旧的“最后按下键”状态不再有意义，立即清掉，
    // 避免从“Z+左键”切回“Ctrl+右键”后残留 Z 导致右键触发失效。
    g_heldEditKey = 0;
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_edit_button(
    int button) {
    g_editButton = Qt::MouseButton(button);
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_shortcut_callback(
    void *callback) {
    g_shortcutCallback = reinterpret_cast<ShortcutCallback>(callback);
}

extern "C" __declspec(dllexport) void __cdecl
sp_delegate_set_dictionary_reload_callback(void *callback) {
    g_dictionaryReloadCallback =
        reinterpret_cast<DictionaryReloadCallback>(callback);
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_enable_shortcut(
    const wchar_t *sequence) {
    g_enableShortcut = sequence
        ? QKeySequence(QString::fromWCharArray(sequence))
        : QKeySequence();
    g_enableShortcutArmed = 0;
    // 每次插件启动/配置快捷键时清空诊断日志，便于观察单次测试。
    QFile::remove(QDir::temp().filePath(QStringLiteral("sp_shortcut_diag.log")));
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_reserve_translations(
    int count) {
    if (count > 0) {
        g_translations.reserve(count);
        g_originals.reserve(count);
        g_translationsFolded.reserve(count);
    }
}

extern "C" __declspec(dllexport) int __cdecl sp_delegate_is_extractable(
    const wchar_t *text) {
    if (!text)
        return 0;
    const QString value = QString::fromWCharArray(text).trimmed();
    if (!extraction_rules::validSource(value.toStdString()))
        return 0;
    if (g_translations.contains(value))
        return 0;
    // ID translations are deliberately scoped to one control.  A matching
    // label in the asset library is still untranslated unless it is present
    // in g_translations, so it must remain exportable.
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_add_translation(
    const wchar_t *source, const wchar_t *target) {
    if (source && target) {
        const QString sourceString = QString::fromWCharArray(source);
        const QString targetString = QString::fromWCharArray(target);
        g_translations.insert(sourceString, targetString);
        g_translationsFolded.insert(normalizeForMatch(sourceString),
                                    targetString);
        g_originals.insert(targetString, sourceString);
        if (g_assetRowFilter)
            g_assetRowFilter->translationsChanged();
    }
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_enabled(int enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled) {
        clearAssetTooltipContext();
        restoreAllAssetTooltipDecorations();
    }
    if (g_assetRowFilter)
        g_assetRowFilter->setActive(g_enabled);
    if (g_enabled) {
        scanAssetSearchWidgets();
        try {
            if (!installGraphPainterHooks())
                g_translateDesignerGraph = false;
        } catch (...) {
            g_translateDesignerGraph = false;
            uninstallGraphPainterHooks();
        }
    }
    refreshGraphViews();
    // 纯显示层：切换语言不修改任何文本，只强制全量重绘，
    // 让 drawText 钩子 / 显示层 delegate 按 g_enabled 重新翻译或恢复原文。
    for (QWidget *widget : QApplication::allWidgets()) {
        if (!widget)
            continue;
        if (auto *view = qobject_cast<QAbstractItemView *>(widget))
            view->viewport()->update();
        widget->update();
    }
}

extern "C" __declspec(dllexport) int __cdecl
sp_delegate_set_translate_designer_graph(int enabled) {
    if (!enabled) {
        g_translateDesignerGraph = false;
        refreshGraphViews();
        return 1;
    }
    if (!graphHookEnvironmentCompatible()) {
        g_translateDesignerGraph = false;
        uninstallGraphPainterHooks();
        return 0;
    }
    if (g_enabled) {
        try {
            if (!installGraphPainterHooks()) {
                g_translateDesignerGraph = false;
                uninstallGraphPainterHooks();
                return 0;
            }
        } catch (...) {
            g_translateDesignerGraph = false;
            uninstallGraphPainterHooks();
            return 0;
        }
    }
    g_translateDesignerGraph = true;
    refreshGraphViews();
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_add_id_translation(
    const wchar_t *id, const wchar_t *target) {
    if (id && target) {
        g_idTranslations.insert(QString::fromWCharArray(id),
                                QString::fromWCharArray(target));
        if (g_assetRowFilter)
            g_assetRowFilter->translationsChanged();
    }
}

extern "C" __declspec(dllexport) void __cdecl
sp_delegate_set_translate_layers(int enabled) {
    setTranslateLayersPanel(enabled != 0);
}

extern "C" __declspec(dllexport) int __cdecl sp_delegate_install(void *viewPointer) {
    return installAssetDelegate(static_cast<QAbstractItemView *>(viewPointer));
}

extern "C" __declspec(dllexport) int __cdecl sp_delegate_install_ui(void *applicationPointer) {
    auto *application = static_cast<QApplication *>(applicationPointer);
    if (!application)
        application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!application)
        return 0;

    // Designer's private graph item paints its title directly; there is no
    // public text child to edit. Patch only the host's imported QPainter
    // drawText calls and substitute exact, currently visible node titles.
    // Geometry, font, clipping and z-order therefore remain entirely native.
    // Graph painting hooks are opt-in and are installed only through
    // sp_delegate_set_translate_designer_graph(). Merely loading the plug-in
    // never patches the host executable's import table.

    // Do not install a QTranslator: translators receive the original English
    // source before Painter's own translator and could therefore override an
    // official Chinese translation. The widget/model display layer below sees
    // Painter's final text and only fills strings that remain untranslated.
    if (!g_filter) {
        g_filter = new TranslationUiFilter(application);
        application->installEventFilter(g_filter);
    }
    if (!g_assetRowFilter) {
        g_assetRowFilter = new AssetSearchManager(application);
        g_assetRowFilter->setActive(g_enabled);
    }
    scanAssetSearchWidgets();
    if (!g_fallbackTimer) {
        g_fallbackTimer = new QTimer(application);
        g_fallbackTimer->setInterval(10000);
        QObject::connect(g_fallbackTimer, &QTimer::timeout, application, [] { scanVisibleWidgets(); });
        // 全量扫描兜底默认关闭，由 Python 侧开关控制（sp_delegate_set_fallback_scan）。
        if (g_fallbackScanEnabled)
            g_fallbackTimer->start();
    }
    // 纯显示层:让 drawText 钩子在两个宿主(含 Painter)都常开安装,
    // 使 generalPainterTranslation 能翻译通用界面文本(样式无关)。
    // 这些钩子内部会先走 graphPaintTranslation(仅当 g_translateDesignerGraph),
    // 再走 generalPainterTranslation,因此对 Painter 不会误触节点图逻辑。
    if (graphHookEnvironmentCompatible())
        installGraphPainterHooks();
    scanVisibleWidgets();
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl
sp_delegate_uninstall_ui(void *applicationPointer) {
    auto *application = static_cast<QApplication *>(applicationPointer);
    if (!application)
        application = qobject_cast<QApplication *>(QCoreApplication::instance());

    if (g_filter && application)
        application->removeEventFilter(g_filter);
    if (g_assetRowFilter) {
        g_assetRowFilter->shutdown();
        delete g_assetRowFilter;
        g_assetRowFilter = nullptr;
    }
    if (g_fallbackTimer) {
        g_fallbackTimer->stop();
        delete g_fallbackTimer;
        g_fallbackTimer = nullptr;
    }
    if (g_filter) {
        delete g_filter;
        g_filter = nullptr;
    }
    clearAssetTooltipContext();
    restoreAllAssetTooltipDecorations();
    restoreComboDisplayDelegates();
    restoreAssetDelegates();
    uninstallGraphPainterHooks();
}
