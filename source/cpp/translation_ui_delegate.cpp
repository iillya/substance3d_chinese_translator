#include <QtCore/QCoreApplication>
#include <QtCore/QCache>
#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QIdentityProxyModel>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtCore/QThread>
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
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtCore/QtMath>
#include <QtWidgets/QStyleOption>
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
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QPlainTextEdit>
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
#include <algorithm>
#include <atomic>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>
#include "extraction_rules.h"


namespace {
QHash<QString, QString> g_translations;
QHash<QString, QString> g_idTranslations;
QString g_fallbackPath;
QString g_idTranslationPath;
QPointer<QWidget> g_originalTooltipOwner;

struct AssetTooltipContext {
    QPointer<QAbstractItemView> view;
    QPersistentModelIndex index;
    QString source;
    QString translation;
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
std::atomic<int> g_activeHookCalls{0};
thread_local int g_hookDepth = 0;
// A delegate has already resolved this item using its original model value.
// Generic drawing must not translate that presentation a second time.
thread_local QString g_delegatePaintText;

class HookCallScope final {
public:
    HookCallScope() { ++g_activeHookCalls; ++g_hookDepth; }
    ~HookCallScope() { --g_hookDepth; --g_activeHookCalls; }
    HookCallScope(const HookCallScope &) = delete;
    HookCallScope &operator=(const HookCallScope &) = delete;
};
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

bool onUiThread() {
    return qApp && QThread::currentThread() == qApp->thread();
}

#if defined(SD_TRANSLATION_SHORTCUT_DIAGNOSTICS)
void shortcutDiag(const QString &line) {
    QFile out(QDir::temp().filePath(QStringLiteral("sp_shortcut_diag.log")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&out);
        stream << QDateTime::currentMSecsSinceEpoch() << " " << line << "\n";
    }
}
#else
#define shortcutDiag(...) ((void)0)
#endif

#if defined(SD_TRANSLATION_TOOLTIP_DIAGNOSTICS)
void tooltipDiag(const QString &line) {
    QFile out(QDir::temp().filePath(QStringLiteral("sp_tooltip_diag.log")));
    if (out.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&out);
        stream << QDateTime::currentMSecsSinceEpoch() << " " << line << "\n";
    }
}
#else
#define tooltipDiag(...) ((void)0)
#endif

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
constexpr int kGraphCacheLimit = 4096;

void cacheGraphTranslation(const QString &key, const QString &target) {
    if (g_fuzzyResolved.size() >= kGraphCacheLimit)
        g_fuzzyResolved.clear();
    g_fuzzyResolved.insert(key, target);
}
QHash<QString, QString> g_translationsFolded;
quint64 g_dictionaryRevision = 1;
QCache<QString, QString> g_normalizedText(1024 * 1024);
QCache<QString, QString> g_lookupResults(2 * 1024 * 1024);
void notifyDictionaryChanged();

void cacheText(QCache<QString, QString> &cache, const QString &key,
               const QString &value) {
    // Account for UTF-16 storage plus a conservative per-entry overhead.
    // Long tooltip/user strings must not evict the entire small-label cache.
    if (key.size() <= 1024 && value.size() <= 4096)
        cache.insert(key, new QString(value),
                     128 + int((key.size() + value.size()) * sizeof(QChar)));
}

void invalidateDictionaryCaches() {
    ++g_dictionaryRevision;
    g_lookupResults.clear();
    g_fuzzyResolved.clear();
    notifyDictionaryChanged();
}
// One source builds separate Qt5 and Qt6 delegates for Painter and Designer. Designer-only features
// (graph-view painting hooks, Designer resource widgets) are always compiled
// in but only activated when the host process is Designer.

void translateWidget(QWidget *widget, bool observeSearch = true);
QString controlUniqueId(QWidget *widget, const QString &sourceText);

// 翻译路径的控件 ID：词库为空时直接返回空，省去每次绘制都计算 ID 的开销；
// 更改翻译窗口的 Ctrl+右键路径仍始终计算完整 ID。
QString translationControlId(QWidget *widget, const QString &sourceText) {
    return g_idTranslations.isEmpty()
               ? QString()
               : controlUniqueId(widget, sourceText);
}

bool containsCjk(const QString &text) {
    for (int i = 0; i < text.size(); ++i) {
        uint code = text.at(i).unicode();
        if (QChar::isHighSurrogate(code) && i + 1 < text.size() &&
            text.at(i + 1).isLowSurrogate()) {
            code = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            ++i;
        }
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
QString computeNormalizedText(QString text) {
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

QString normalizeForMatch(const QString &text) {
    if (const QString *cached = g_normalizedText.object(text))
        return *cached;
    const QString result = computeNormalizedText(text);
    cacheText(g_normalizedText, text, result);
    return result;
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

QString translatedUncached(const QString &text, bool removeMnemonic,
                           const QString &controlId) {
    // translated() 查找顺序：
    //   1. 控件 ID 专属词库（control_ids_zh.json）精确查找；
    //   2. 全局词库精确查找；
    //   3. 全局模糊匹配兜底。
    if (!g_enabled)
        return {};
    QString key = text.trimmed();
    if (key.isEmpty())
        return {};
    // 1. 控件 ID 专属词库（control_ids_zh.json，键为完整 ID 字符串）。
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
        // 前三段为控件信息，其后的完整原文可能也包含 ||。
        int sourceSeparator = -2;
        for (int part = 0; part < 3; ++part) {
            sourceSeparator = controlId.indexOf(QStringLiteral("||"), sourceSeparator + 2);
            if (sourceSeparator < 0) break;
        }
        if (sourceSeparator > 0) {
            const QString wildcard =
                controlId.left(sourceSeparator) + QStringLiteral("||*");
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

QString translated(const QString &text, bool removeMnemonic = false,
                   const QString &controlId = QString()) {
    if (!g_enabled)
        return {};
    if (controlId.isEmpty()) {
        const auto exact = g_translations.constFind(text.trimmed());
        if (exact != g_translations.cend())
            return exact.value();
    }
    const QString key = QString::number(controlId.size()) + QLatin1Char(':') +
        controlId + QChar(removeMnemonic ? 1 : 0) + text;
    if (const QString *cached = g_lookupResults.object(key))
        return *cached;
    const QString result = translatedUncached(text, removeMnemonic, controlId);
    cacheText(g_lookupResults, key, result);
    return result;
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

bool shouldExcludeLayersPanel(QWidget *widget) {
    return !g_translateLayersPanel && isInsideLayersPanel(widget);
}

bool isLayerBlendModeButton(QToolButton *button) {
    return button && button->objectName() == QStringLiteral("blendingMode") &&
           isInsideLayersPanel(button);
}

bool isLayerChannelSelector(QComboBox *combo) {
    return combo && combo->objectName() == QStringLiteral("channelSelector") &&
           isInsideLayersPanel(combo);
}

struct PopupWidthBinding {
    QPointer<QWidget> widget;
    int minimum, maximum, applied;
};
std::vector<PopupWidthBinding> g_popupWidths;

void lockPopupWidth(QWidget *widget, int width) {
    g_popupWidths.erase(std::remove_if(g_popupWidths.begin(), g_popupWidths.end(),
        [](const PopupWidthBinding &binding) { return !binding.widget; }), g_popupWidths.end());
    auto found = std::find_if(g_popupWidths.begin(), g_popupWidths.end(),
        [widget](const PopupWidthBinding &binding) { return binding.widget == widget; });
    if (found == g_popupWidths.end()) {
        g_popupWidths.push_back({widget, widget->minimumWidth(), widget->maximumWidth(), width});
    } else {
        // Preserve constraints changed by the host since our previous lock.
        if (widget->minimumWidth() != found->applied) found->minimum = widget->minimumWidth();
        if (widget->maximumWidth() != found->applied) found->maximum = widget->maximumWidth();
        found->applied = width;
    }
    widget->setFixedWidth(width);
}

void restoreLayerPopupWidths() {
    for (const auto &binding : g_popupWidths) {
        if (!binding.widget) continue;
        if (binding.widget->minimumWidth() == binding.applied)
            binding.widget->setMinimumWidth(binding.minimum);
        if (binding.widget->maximumWidth() == binding.applied)
            binding.widget->setMaximumWidth(binding.maximum);
    }
    g_popupWidths.clear();
}

void lockLayerChannelPopupWidth(QComboBox *combo) {
    if (!isLayerChannelSelector(combo) || !combo->view())
        return;
    const int width = combo->width();
    lockPopupWidth(combo->view(), width);
    QWidget *popup = combo->view()->window();
    if (popup && popup != combo->window() && popup != combo->view()) {
        lockPopupWidth(popup, width);
    }
}

QString actionSource(QAction *action) {
    if (!action)
        return {};
    QString source = action->text().trimmed();
    source.remove(u'&');
    return source;
}

QString menuTranslation(QMenu *menu, const QString &source) {
    // 统一顺序：控件 ID（control_ids_zh.json）→ 全局词库 → 模糊兜底。
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
        QScopedValueRollback<QString> presentation(
            g_delegatePaintText, displayText(index.data(Qt::DisplayRole), QLocale()));
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

    bool setData(const QModelIndex &, const QVariant &, int) override { return false; }
    bool setItemData(const QModelIndex &, const QMap<int, QVariant> &) override { return false; }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override {
        const QVariant sourceValue = QIdentityProxyModel::data(index, role);
        if (role != Qt::DisplayRole || !g_enabled || !combo_ ||
            shouldExcludeLayersPanel(combo_))
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
          proxy_(combo, this), fallback_(this) {
        connect(original, &QAbstractItemDelegate::commitData,
                this, &QAbstractItemDelegate::commitData);
        connect(original, &QAbstractItemDelegate::closeEditor,
                this, &QAbstractItemDelegate::closeEditor);
        connect(original, &QAbstractItemDelegate::sizeHintChanged,
                this, &QAbstractItemDelegate::sizeHintChanged);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (!index.isValid())
            return;
        ensureSourceModel(index.model());
        const QModelIndex displayIndex = proxy_.mapFromSource(index);
        QScopedValueRollback<QString> presentation(
            g_delegatePaintText, displayIndex.data(Qt::DisplayRole).toString());
        activeDelegate()->paint(painter, option, displayIndex);
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override {
        if (!index.isValid())
            return {};
        ensureSourceModel(index.model());
        return activeDelegate()->sizeHint(option, proxy_.mapFromSource(index));
    }

    // Only painting and measurement see the display proxy. All interactions
    // retain the host's real index/model and the original delegate's behavior.
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override {
        return activeDelegate()->createEditor(parent, option, index);
    }
    void destroyEditor(QWidget *editor, const QModelIndex &index) const override {
        activeDelegate()->destroyEditor(editor, index);
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        activeDelegate()->setEditorData(editor, index);
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override {
        activeDelegate()->setModelData(editor, model, index);
    }
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override {
        activeDelegate()->updateEditorGeometry(editor, option, index);
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        return activeDelegate()->editorEvent(event, model, option, index);
    }
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
        return activeDelegate()->helpEvent(event, view, option, index);
    }

private:
    QAbstractItemDelegate *activeDelegate() const {
        return original_ ? original_.data() : &fallback_;
    }

    void ensureSourceModel(const QAbstractItemModel *model) const {
        if (proxy_.sourceModel() != model)
            proxy_.setSourceModel(const_cast<QAbstractItemModel *>(model));
    }

    QPointer<QAbstractItemDelegate> original_;
    mutable ComboPaintProxyModel proxy_;
    mutable QStyledItemDelegate fallback_;
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
        return 2;
    }
    g_comboPaintBindings.erase(std::remove_if(g_comboPaintBindings.begin(),
        g_comboPaintBindings.end(), [](const ComboPaintBinding &binding) {
            return !binding.view || !binding.installed;
        }), g_comboPaintBindings.end());
    QAbstractItemDelegate *original = view->itemDelegate();
    if (!original)
        return 0;
    const QString rtti = QString::fromLatin1(typeid(*original).name());
    if (typeid(*original) != typeid(QItemDelegate) &&
        typeid(*original) != typeid(QStyledItemDelegate) &&
        rtti != QStringLiteral("class QComboBoxDelegate") &&
        rtti != QStringLiteral("class QComboMenuDelegate") &&
        rtti != QStringLiteral("class Alg::DefaultComboBoxDelegate") &&
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
            it->view->setItemDelegate(it->original ? it->original.data()
                : new QStyledItemDelegate(it->view));
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
        return 2;
    }
    g_delegateBindings.erase(std::remove_if(g_delegateBindings.begin(),
        g_delegateBindings.end(), [](const DelegateBinding &binding) {
            return !binding.view || !binding.installed;
        }), g_delegateBindings.end());
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
                view->setItemDelegate(it->original ? it->original.data()
                    : new QStyledItemDelegate(view));
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
        observeContainer(widget, resourcesContainer(widget));
    }

private:
    friend class AssetSearchManager;
    void observeContainer(QWidget *widget, QWidget *container) {
        if (!container)
            return;
        // One filter instance owns exactly one search surface. The manager
        // creates another instance for every main shelf or resource picker,
        // so opening a generator/filter picker cannot steal the shelf state.
        if (container_ && container_ != container)
            return;
        // Ordinary paints need no repeated descendant search. Rebind when a
        // search field/view appears, disappears or receives a different model.
        if (container_ == container && field_ && view_ && model_ == view_->model() &&
            (!qobject_cast<QLineEdit *>(widget) || widget == field_) &&
            (!qobject_cast<QAbstractItemView *>(widget) || widget == view_))
            return;
        bindContainer(container);
    }

public:
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
        clearSearchText();
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
            model_, &QAbstractItemModel::rowsMoved, this,
            [this] { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::layoutChanged, this,
            [this] { refreshRowMask(); }));
        modelConnections_.push_back(QObject::connect(
            model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &last,
                   const QVector<int> &roles) {
                // Thumbnail, size and hover-help updates do not change the
                // DisplayRole searched below. Empty/unknown role lists remain
                // conservative so host-specific text dependencies still work.
                const bool textMayHaveChanged = roles.isEmpty() ||
                    std::any_of(roles.cbegin(), roles.cend(), [](int role) {
                        return role != Qt::DecorationRole &&
                               role != Qt::SizeHintRole &&
                               role != Qt::ToolTipRole;
                    });
                if (textMayHaveChanged) {
                    if (first.parent().isValid() || last.parent().isValid())
                        clearSearchText();
                    else
                        invalidateSearchRows(first.row(), last.row());
                    if (localQuery_)
                        scheduleFilter();
                }
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

        applying_ = true;
        localQuery_ = true;
        query_ = query.trimmed();
        // Route the host search to the complete native category without
        // rewriting the user's text, cursor, selection or undo history.
        QMetaObject::invokeMethod(field_, "textChanged", Qt::DirectConnection,
                                  Q_ARG(QString, QString()));
        if (!field_ || !view_) {
            applying_ = false;
            localQuery_ = false;
            query_.clear();
            return;
        }
        applying_ = false;
        scheduleFilter();
    }

    void deactivateLocalQuery(bool restoreNative) {
        clearSearchText();
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
        // Re-submit the unchanged visible query so disabling/unloading the plug-in
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
        clearSearchText();
        if (active_ && localQuery_)
            scheduleFilter();
        else
            clearHiddenRows();
    }

    QStringList normalizedTerms() const {
        QStringList terms;
        const QStringList words = query_.simplified().split(QLatin1Char(' '));
        for (const QString &word : words) {
            const QString normalized = normalizeForMatch(word);
            if (!normalized.isEmpty()) terms.append(normalized);
        }
        return terms;
    }

    void clearSearchText() {
        searchText_.clear();
        searchTextCost_ = 0;
    }

    void invalidateSearchRows(int first, int last) {
        if (first < 0 || last < first) {
            clearSearchText();
            return;
        }
        const int end = qMin(last, int(searchText_.size()) - 1);
        for (int row = first; row <= end; ++row) {
            searchTextCost_ -= int(searchText_[row].size() * sizeof(QChar));
            searchText_[row] = QString();
        }
    }

    QString searchableRow(int row) {
        if (row < searchText_.size() && !searchText_.at(row).isNull())
            return searchText_.at(row);
        const QString source = model_->index(row, 0).data(Qt::DisplayRole)
                                   .toString().trimmed();
        const QString target = translated(
            source, false, translationControlId(view_, source));
        const QString text = normalizeForMatch(source + QLatin1Char(' ') + target);
        const int cost = int(text.size() * sizeof(QChar));
        if (row < searchText_.size() && cost <= 4 * 1024 * 1024 - searchTextCost_) {
            searchText_[row] = text.isNull() ? QStringLiteral("") : text;
            searchTextCost_ += cost;
        }
        return text;
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

        for (auto it = hiddenRows_.begin(); it != hiddenRows_.end();) {
            if (!it->isValid()) it = hiddenRows_.erase(it);
            else ++it;
        }
        const QStringList terms = normalizedTerms();
        if (terms.isEmpty()) {
            clearHiddenRows();
            return;
        }

        bool maskChanged = false;
        const int rowCount = model_->rowCount();
        const QString scope = translationControlId(view_, QString());
        if (searchRevision_ != g_dictionaryRevision || searchScope_ != scope) {
            clearSearchText();
            searchRevision_ = g_dictionaryRevision;
            searchScope_ = scope;
        }
        searchText_.resize(qMin(rowCount, 20000));
        for (int row = 0; row < rowCount; ++row) {
            const QString searchable = searchableRow(row);

            bool matches = true;
            for (const QString &term : terms) {
                if (!searchable.contains(term)) {
                    matches = false;
                    break;
                }
            }
            const bool hidden = view_->isRowHidden(row);
            if (!matches && !hidden) {
                hiddenRows_.insert(QPersistentModelIndex(model_->index(row, 0)));
                view_->setRowHidden(row, true);
                maskChanged = true;
            } else if (matches && hidden &&
                       hiddenRows_.remove(QPersistentModelIndex(model_->index(row, 0)))) {
                view_->setRowHidden(row, false);
                maskChanged = true;
            }
        }

        const QModelIndex current = view_->currentIndex();
        if (current.isValid() && view_->isRowHidden(current.row())) {
            view_->clearSelection();
            view_->setCurrentIndex(QModelIndex());
        }
        // Model/view changes already schedule native layout and repaint work.
        // Only a changed row mask needs an additional layout from this filter.
        if (maskChanged) {
            view_->doItemsLayout();
            if (view_->viewport())
                view_->viewport()->update();
        }
    }

    void clearHiddenRows() {
        bool changed = false;
        if (view_ && model_ && view_->model() == model_) {
            for (const QPersistentModelIndex &index : hiddenRows_) {
                if (index.isValid() && index.model() == model_) {
                    view_->setRowHidden(index.row(), false);
                    changed = true;
                }
            }
        }
        hiddenRows_.clear();
        if (changed && view_) {
            view_->doItemsLayout();
            if (view_->viewport()) view_->viewport()->update();
        }
    }

    void unbind(bool restoreNative) {
        clearSearchText();
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
    QSet<QPersistentModelIndex> hiddenRows_;
    QVector<QString> searchText_;
    QString searchScope_;
    quint64 searchRevision_ = 0;
    int searchTextCost_ = 0;
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
        QWidget *container = AssetRowFilter::resourcesContainer(widget);
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
        filter->observeContainer(widget, container);
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
    QHash<QWidget *, AssetRowFilter *> filters_;
    bool active_ = true;
};

AssetSearchManager *g_assetRowFilter = nullptr;

void notifyDictionaryChanged() {
    if (g_assetRowFilter)
        g_assetRowFilter->translationsChanged();
}

void observePainterAssetSearch(QWidget *widget) {
    if (g_assetRowFilter)
        g_assetRowFilter->observe(widget);
}

void synchronizeWidgets(bool attach, bool repaint) {
    for (QWidget *widget : QApplication::allWidgets()) {
        if (!widget)
            continue;
        if (attach) {
            observePainterAssetSearch(widget);
            if (widget->isVisible())
                translateWidget(widget, false);
        }
        if (repaint) {
            if (auto *area = qobject_cast<QAbstractScrollArea *>(widget))
                area->viewport()->update();
            widget->update();
        }
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

struct GraphOwnerInfo {
    QTransform transform;
    QString fullTitle;
    QRectF nodeRect;
    bool connector = false;
    bool valid = false;
};
GraphOwnerInfo graphOwnerInfo(QPainter *painter);

// Only shared between port classification and title lookup in ONE drawText
// hook, before forwarding to Qt. Never retained across draws, event dispatch,
// scene changes or frames: QGraphicsItem is not necessarily a QObject.
class GraphPaintContext final {
public:
    explicit GraphPaintContext(QPainter *painter) : painter_(painter) {}
    const GraphOwnerInfo &owner() {
        if (!resolved_) {
            owner_ = graphOwnerInfo(painter_);
            resolved_ = true;
        }
        return owner_;
    }

private:
    QPainter *painter_;
    GraphOwnerInfo owner_;
    bool resolved_ = false;
};

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
                              int portSide = 0,
                              GraphPaintContext *context = nullptr) {
    if (!onUiThread() || g_hookDepth > 1 || !g_enabled || !g_translateDesignerGraph)
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

    // 2. Tooltip full-name fallback (node titles only): the graph paints
    // elided titles ("Name …") and identifier forms. The item tooltip carries
    // the full display name on its first line. Port labels must not use this
    // tooltip match; only node titles are allowed to fall back to it.
    QString cacheKey = QString::number(portSide) + QChar(0x01) + source;
    if (portSide == 0) {
        const GraphOwnerInfo owner = context ? context->owner() : graphOwnerInfo(painter);
        if (owner.valid) {
            const QString &full = owner.fullTitle;
            const QString fullNormalized = normalizeForMatch(full);
            const QString normalized = normalizeForMatch(source);
            if (!fullNormalized.isEmpty() && fullNormalized != normalized &&
                fullNormalized.startsWith(normalized)) {
                // 同一绘制文本可能属于不同节点（完整名不同），缓存键必须
                // 带上完整名，避免串用其他节点的 tooltip 匹配结果。
                cacheKey += QChar(0x01) + fullNormalized;
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

    const auto cached = g_fuzzyResolved.constFind(cacheKey);
    if (cached != g_fuzzyResolved.cend())
        return cached.value();

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

    cacheGraphTranslation(cacheKey, target);
    return target;
}

// ---------------------------------------------------------------------------
// Translation at QPainter text calls preserves the host's source properties.
// QTextLayout-backed item views use display delegates instead. Full source
// recovery is restricted to the actual owner; dictionary-prefix guesses are
// ambiguous and must never run inside painting.
// ---------------------------------------------------------------------------
// Resolve policy from the actual paint owner, never from application focus.
// A focused editor must not suppress unrelated labels in the same panel.
bool isInputPaintOwner(QWidget *widget) {
    for (QWidget *current = widget; current; current = current->parentWidget()) {
        if (qobject_cast<QLineEdit *>(current) ||
            qobject_cast<QTextEdit *>(current) ||
            qobject_cast<QPlainTextEdit *>(current) ||
            qobject_cast<QAbstractSpinBox *>(current))
            return true;
    }
    return false;
}

QString paintSource(QWidget *owner, const QString &displayed) {
    QString prefix = displayed;
    if (prefix.endsWith(QChar(0x2026))) prefix.chop(1);
    else if (prefix.endsWith(QLatin1String("..."))) prefix.chop(3);
    else return displayed;
    if (prefix.isEmpty()) return displayed;
    QStringList candidates;
    if (auto *combo = qobject_cast<QComboBox *>(owner))
        candidates.append(combo->currentText());
    else if (auto *button = qobject_cast<QAbstractButton *>(owner))
        candidates.append(button->text());
    else if (auto *edit = qobject_cast<QLineEdit *>(owner)) {
        if (edit->text().isEmpty()) candidates.append(edit->placeholderText());
    } else if (auto *tabs = qobject_cast<QTabBar *>(owner)) {
        for (int i = 0; i < tabs->count(); ++i) candidates.append(tabs->tabText(i));
    }
    QString full;
    for (const QString &candidate : candidates) {
        if (!candidate.startsWith(prefix) || candidate.size() <= prefix.size()) continue;
        if (!full.isEmpty() && full != candidate) return displayed;
        full = candidate;
    }
    return full.isEmpty() ? displayed : full;
}

QString generalPainterTranslation(QPainter *painter, const QString &text) {
    if (!onUiThread() || g_hookDepth > 1 || !g_enabled || !painter ||
        isDesignerGraphPainter(painter))
        return {};
    QWidget *owner = dynamic_cast<QWidget *>(painter->device());
    if (!owner || shouldExcludeLayersPanel(owner) ||
        owner->window()->windowType() == Qt::ToolTip ||
        isLayerBlendModeButton(qobject_cast<QToolButton *>(owner)))
        return {};
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed == g_delegatePaintText)
        return {};
    QString source = paintSource(owner, trimmed);
    if (isInputPaintOwner(owner)) {
        // QLineEdit paints its placeholder with drawText, while its editable
        // content uses QTextLayout. Require both an empty value and an exact
        // placeholder match; focus is irrelevant to this decision.
        auto *edit = qobject_cast<QLineEdit *>(owner);
        if (!edit || !edit->text().isEmpty() ||
            edit->placeholderText().trimmed() != source)
            return {};
    }
    if (auto *label = qobject_cast<QLabel *>(owner)) {
        // Recover only an actual truncated label, never substitute a full
        // sentence for a single fragment from rich-text rendering.
        if (label->text().trimmed() == trimmed) {
            const QString full = sourceFromPainterElidedLabel(label, trimmed);
            if (!full.isEmpty()) source = full;
        }
    }
    return translated(source, true, translationControlId(owner, source));
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
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
QSet<QString> g_graphPaintDiagnosticKeys;
#endif
struct GraphHookSlot {
    void **slot = nullptr;
    void *original = nullptr;
    void *replacement = nullptr;
};
std::vector<GraphHookSlot> g_graphHookSlots;
// Keep patched modules mapped until their import slots have been restored.
std::vector<HMODULE> g_hookModuleRefs;

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
    const QTransform viewportTransform = view->viewportTransform();
    for (QGraphicsItem *item : view->scene()->items()) {
        if (!item)
            continue;
        const qreal difference = transformDifference(
            current, item->deviceTransform(viewportTransform));
        if (difference < bestDifference) {
            bestDifference = difference;
            owner = item;
            // Zero is the minimum possible difference. Preserve the original
            // scene order (including ties), but stop once its first match wins.
            if (difference == 0.0)
                break;
        }
    }
    if (differenceOut)
        *differenceOut = bestDifference;
    return owner;
}

GraphOwnerInfo snapshotGraphOwner(QGraphicsItem *item, const QTransform &viewport) {
    GraphOwnerInfo result;
    if (!item)
        return result;
    result.valid = true;
    result.transform = item->deviceTransform(viewport);
    result.fullTitle = graphFullTitleFromItem(item);
    result.connector = item->parentItem() &&
        QByteArray(typeid(*item).name()).contains("Connector");
    if (result.connector)
        result.nodeRect = item->parentItem()->sceneBoundingRect();
    return result;
}

using TransformKey = std::array<qreal, 9>;
TransformKey transformKey(const QTransform &t) {
    return {t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(),
            t.m31(), t.m32(), t.m33()};
}
struct TransformHash {
    size_t operator()(const TransformKey &key) const {
        size_t hash = 0;
        for (qreal value : key)
            hash ^= std::hash<qreal>{}(value) + size_t(0x9e3779b9) +
                    (hash << 6) + (hash >> 2);
        return hash;
    }
};

// A value-only index belongs to one native QPainter begin/end lifetime. It
// never stores QGraphicsItem pointers: plain graphics items have no destroyed
// signal. Nested paints have separate indexes, released on end/destruction.
struct GraphPaintIndex {
    QPointer<QGraphicsView> view;
    QPointer<QGraphicsScene> scene;
    QTransform viewport;
    std::vector<GraphOwnerInfo> items;
    std::unordered_map<TransformKey, size_t, TransformHash> exact;
    QMetaObject::Connection changed;
    bool ready = false;
    bool tooLarge = false;
    int queries = 0;
    ~GraphPaintIndex() { QObject::disconnect(changed); }

    void prepare() {
        const QTransform current = view->viewportTransform();
        if (ready && current == viewport && scene == view->scene())
            return;
        ready = true;
        tooLarge = false;
        viewport = current;
        scene = view->scene();
        items.clear();
        exact.clear();
        if (!scene)
            return;
        const auto nativeItems = scene->items();
        // Bound temporary memory independently of arbitrary scene size.
        if (nativeItems.size() > 30000) {
            tooLarge = true;
            return;
        }
        items.reserve(size_t(nativeItems.size()));
        exact.reserve(size_t(nativeItems.size()));
        size_t textBytes = 0;
        for (QGraphicsItem *item : nativeItems) {
            GraphOwnerInfo info = snapshotGraphOwner(item, viewport);
            textBytes += size_t(info.fullTitle.size()) * sizeof(QChar);
            if (textBytes > 4 * 1024 * 1024) {
                tooLarge = true;
                items.clear();
                exact.clear();
                return;
            }
            // emplace preserves the first item at an identical transform,
            // exactly as the exhaustive resolver's strict '<' comparison.
            exact.emplace(transformKey(info.transform), items.size());
            items.push_back(std::move(info));
        }
    }

    GraphOwnerInfo resolve(const QTransform &transform) {
        const auto hit = exact.find(transformKey(transform));
        if (hit != exact.end())
            return items[hit->second];
        const GraphOwnerInfo *best = nullptr;
        qreal distance = 1.0e20;
        for (const auto &item : items) {
            const qreal candidate = transformDifference(transform, item.transform);
            if (candidate < distance) {
                best = &item;
                distance = candidate;
            }
        }
        return best ? *best : GraphOwnerInfo{};
    }
};

std::unordered_map<QPainter *, std::unique_ptr<GraphPaintIndex>> g_graphPaintIndexes;
bool g_trackPainterConstruction = false;
bool g_trackPainterBegin = false;

void beginGraphPaint(QPainter *painter) {
    if (!onUiThread())
        return;
    g_graphPaintIndexes.erase(painter);
    if (!g_enabled || !g_translateDesignerGraph || !painter->isActive() ||
        g_graphPaintIndexes.size() >= 8)
        return;
    auto *view = designerGraphViewForPainter(painter);
    if (!view || !view->scene())
        return;
    auto index = std::make_unique<GraphPaintIndex>();
    index->view = view;
    index->scene = view->scene();
    index->changed = QObject::connect(view->scene(), &QGraphicsScene::changed,
        qApp, [painter](const QList<QRectF> &) {
            const auto found = g_graphPaintIndexes.find(painter);
            if (found != g_graphPaintIndexes.end())
                found->second->ready = false;
        });
    g_graphPaintIndexes.emplace(painter, std::move(index));
}

void endGraphPaint(QPainter *painter) {
    if (onUiThread())
        g_graphPaintIndexes.erase(painter);
}

GraphOwnerInfo graphOwnerInfo(QPainter *painter) {
    const auto found = g_graphPaintIndexes.find(painter);
    if (found != g_graphPaintIndexes.end() && found->second->view) {
        auto &index = *found->second;
        // A tiny graph paint should not pay to snapshot a huge offscreen scene.
        // Build only after the live resolver has been needed four times.
        if (++index.queries > 4) {
            index.prepare();
            if (!index.tooLarge)
                return index.resolve(painter->worldTransform());
        }
    }
    auto *view = designerGraphViewForPainter(painter);
    return view ? snapshotGraphOwner(graphOwnerItem(painter), view->viewportTransform())
                : GraphOwnerInfo{};
}

using PainterConstruct = QPainter *(*)(QPainter *, QPaintDevice *);
using PainterDestruct = void (*)(QPainter *);
using PainterBegin = bool (*)(QPainter *, QPaintDevice *);
using PainterEnd = bool (*)(QPainter *);
PainterConstruct g_painterConstruct = nullptr;
PainterDestruct g_painterDestruct = nullptr;
PainterBegin g_painterBegin = nullptr;
PainterEnd g_painterEnd = nullptr;

QPainter *hookedPainterConstruct(QPainter *painter, QPaintDevice *device) {
    HookCallScope call;
    QPainter *result = g_painterConstruct(painter, device);
    if (onUiThread() && g_trackPainterConstruction)
        beginGraphPaint(painter);
    return result;
}
void hookedPainterDestruct(QPainter *painter) {
    HookCallScope call;
    endGraphPaint(painter);
    g_painterDestruct(painter);
}
bool hookedPainterBegin(QPainter *painter, QPaintDevice *device) {
    HookCallScope call;
    const bool result = g_painterBegin(painter, device);
    if (result && onUiThread() && g_trackPainterBegin)
        beginGraphPaint(painter);
    return result;
}
bool hookedPainterEnd(QPainter *painter) {
    HookCallScope call;
    endGraphPaint(painter);
    return g_painterEnd(painter);
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
int graphPortLabelSide(QPainter *painter, const QRectF &rect,
                       GraphPaintContext *context = nullptr) {
    if (!onUiThread() || g_hookDepth > 1 || !g_enabled || !g_translateDesignerGraph) return 0;
    QGraphicsView *view = designerGraphViewForPainter(painter);
    if (!view)
        return 0;
    const GraphOwnerInfo owner = context ? context->owner() : graphOwnerInfo(painter);
    if (!owner.valid || !owner.connector)
        return 0;
    const QRectF &nodeRect = owner.nodeRect;
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
    HookCallScope call;
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
    HookCallScope call;
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
    HookCallScope call;
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
    HookCallScope call;
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
    HookCallScope call;
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QRectF/QTextOption",
                         reinterpret_cast<quintptr>(_ReturnAddress()),
                         int(option.alignment()) |
                             (int(option.textDirection()) << 16));
#endif
    GraphPaintContext context(painter);
    const int portSide = graphPortLabelSide(painter, rect, &context);
    QString target = graphPaintTranslation(painter, text, portSide, &context);
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
    HookCallScope call;
#if defined(SD_TRANSLATION_GRAPH_DIAGNOSTICS)
    recordGraphPaintType(painter, text, "QRectF/Align",
                         reinterpret_cast<quintptr>(_ReturnAddress()), flags);
#endif
    GraphPaintContext context(painter);
    const int portSide = graphPortLabelSide(painter, rect, &context);
    QString target = graphPaintTranslation(painter, text, portSide, &context);
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
    HookCallScope call;
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
    HookCallScope call;
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
            const bool changed = InterlockedCompareExchangePointer(
                slot, replacement, original) == original;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
            if (changed) replaced = true;
            else slotList.pop_back();
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
    const DWORD count = qMin<DWORD>(needed / sizeof(HMODULE),
                                   sizeof(modules) / sizeof(HMODULE));
    for (DWORD i = 0; i < count; ++i) {
        const bool retained = std::find(g_hookModuleRefs.begin(),
            g_hookModuleRefs.end(), modules[i]) != g_hookModuleRefs.end();
        HMODULE reference = nullptr;
        if (!retained && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(modules[i]), &reference))
            continue;
        const bool replaced = replaceImportInModule(modules[i], slotList,
                                                    original, replacement);
        if (replaced) {
            any = true;
            if (reference) g_hookModuleRefs.push_back(reference);
        } else if (reference) {
            FreeLibrary(reference);
        }
    }
    return any;
}

bool restoreImportHooks(std::vector<GraphHookSlot> &slotList) {
    for (std::size_t index = slotList.size(); index > 0; --index) {
        const std::size_t current = index - 1;
        const GraphHookSlot hook = slotList[current];
        if (!hook.slot || *hook.slot == hook.original) {
            slotList.erase(slotList.begin() + current);
            continue;
        }
        // Another hook may be chaining through ours. Do not overwrite it or
        // unload our code until the original import is demonstrably restored.
        if (*hook.slot != hook.replacement)
            continue;
        DWORD oldProtection = 0;
        if (!VirtualProtect(hook.slot, sizeof(void *), PAGE_READWRITE,
                            &oldProtection))
            continue;
        void *previous = InterlockedCompareExchangePointer(
            hook.slot, hook.original, hook.replacement);
        DWORD ignored = 0;
        VirtualProtect(hook.slot, sizeof(void *), oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), hook.slot, sizeof(void *));
        if (previous == hook.replacement || previous == hook.original)
            slotList.erase(slotList.begin() + current);
    }
    return slotList.empty();
}

bool uninstallGraphPainterHooks() {
    restoreImportHooks(g_graphHookSlots);
    g_graphPainterHooksInstalled = !g_graphHookSlots.empty();
    if (!g_graphPainterHooksInstalled && g_activeHookCalls == 0) {
        g_graphPaintIndexes.clear();
        g_trackPainterConstruction = false;
        g_trackPainterBegin = false;
        for (HMODULE module : g_hookModuleRefs) FreeLibrary(module);
        g_hookModuleRefs.clear();
    }
    return !g_graphPainterHooksInstalled;
}

bool graphHookEnvironmentCompatible() {
    // Each binary only hooks the Qt major against which it was compiled.
    // Every overload is resolved by export name; no fixed memory addresses.
    if constexpr (sizeof(void *) != 8)
        return false;
    const QStringList qtParts = QString::fromLatin1(qVersion()).split(u'.');
    if (qtParts.size() < 2)
        return false;
    bool majorOk = false;
    const int qtMajor = qtParts.at(0).toInt(&majorOk);
    if (!majorOk || qtMajor != QT_VERSION_MAJOR)
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
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    HMODULE qtGui = GetModuleHandleW(L"Qt6Gui.dll");
#else
    HMODULE qtGui = GetModuleHandleW(L"Qt5Gui.dll");
#endif
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
    // Optional public Qt lifetime hooks. If a matching cleanup hook cannot be
    // mounted, use the existing live resolver instead of retaining an index.
    const bool destruct = hookQtGuiImport(qtGui,
        "??1QPainter@@QEAA@XZ", &hookedPainterDestruct, g_painterDestruct);
    const bool end = hookQtGuiImport(qtGui,
        "?end@QPainter@@QEAA_NXZ", &hookedPainterEnd, g_painterEnd);
    g_trackPainterConstruction = destruct && end && hookQtGuiImport(qtGui,
        "??0QPainter@@QEAA@PEAVQPaintDevice@@@Z",
        &hookedPainterConstruct, g_painterConstruct);
    g_trackPainterBegin = destruct && end && hookQtGuiImport(qtGui,
        "?begin@QPainter@@QEAA_NPEAVQPaintDevice@@@Z", &hookedPainterBegin, g_painterBegin);
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
    // A plug-in-owned document holds the translated presentation. The native
    // QLabel and its HTML/image/metadata source remain untouched.
    auto *document = label->findChild<QTextDocument *>(
        QStringLiteral("sp_asset_preview_document"), Qt::FindDirectChildrenOnly);
    if (!document) {
        document = new QTextDocument(label);
        document->setObjectName(QStringLiteral("sp_asset_preview_document"));
    }
    if (document->defaultFont() != label->font()) document->setDefaultFont(label->font());
    if (document->documentMargin() != 0) document->setDocumentMargin(0);
    const QString nativeText = label->text();
    if (document->property("source_html").toString() != nativeText ||
        document->property("asset_source").toString() != context.source ||
        document->property("translation").toString() != context.translation) {
        const QString adjusted = assetTooltipTextWithTranslation(
            nativeText, context.source, context.translation);
        if (adjusted == nativeText) return false;
        if (Qt::mightBeRichText(nativeText))
            document->setHtml(adjusted);
        else
            document->setPlainText(adjusted);
        document->setProperty("source_html", nativeText);
        document->setProperty("asset_source", context.source);
        document->setProperty("translation", context.translation);
    }
    const int margin = label->margin() + label->frameWidth();
    const int textWidth = qMax(1, label->contentsRect().width() - 2 * margin);
    if (document->textWidth() != textWidth) document->setTextWidth(textWidth);
    if (allowHeightGrowth) {
        if (!label->property("sp_asset_preview_original_min_height").isValid())
            label->setProperty("sp_asset_preview_original_min_height", label->minimumHeight());
        const int height = qCeil(document->size().height()) + 2 * margin;
        label->setMinimumHeight(height);
        label->resize(label->width(), height);
    }
    if (triggerType == QEvent::Paint) {
        QPainter painter(label);
        QStyleOption option;
        option.initFrom(label);
        label->style()->drawPrimitive(QStyle::PE_PanelTipLabel, &option, &painter, label);
        painter.translate(label->contentsRect().topLeft() + QPoint(margin, margin));
        QAbstractTextDocumentLayout::PaintContext paintContext;
        paintContext.palette = label->palette();
        paintContext.palette.setColor(QPalette::Text,
                                       label->palette().color(QPalette::ToolTipText));
        document->documentLayout()->draw(&painter, paintContext);
    }
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
        }
    }
    delete popup->findChild<QTextDocument *>(
        QStringLiteral("sp_asset_preview_document"), Qt::FindDirectChildrenOnly);
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

void translateWidget(QWidget *widget, bool observeSearch) {
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

    if (observeSearch)
        observePainterAssetSearch(widget);

    // Painter 下拉项经 QTextLayout 绘制，drawText 钩子覆盖不到。包装其原生
    // delegate，并只给 paint() 传入 DisplayRole 已翻译的代理索引；真实 model
    // 与 Painter 的分组字体、缩进、选中样式均保持不变。
    if (auto *combo = qobject_cast<QComboBox *>(widget))
        installComboDisplayDelegate(combo->view(), combo);

    auto *itemView = qobject_cast<QAbstractItemView *>(widget);
    if (!itemView || shouldExcludeLayersPanel(itemView))
        return;

    QComboBox *ownerCombo = owningComboBox(itemView);
    if (QComboBox *combo = ownerCombo)
        installComboDisplayDelegate(itemView, combo);

    const QString className = QString::fromLatin1(itemView->metaObject()->className());
    if (isDesignerResourceList(itemView) || isDesignerLibraryTree(itemView)) {
        installAssetDelegate(itemView);
        return;
    }
    if (QComboBox *combo = ownerCombo;
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
        const QString source = actionSource(action);
        const QString result = menuTranslation(menu, source);
        return !result.isEmpty() && result != source ? source : QString();
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
                if (shouldExcludeLayersPanel(combo)) return {};
                const QString source = index.data(Qt::DisplayRole).toString().trimmed();
                const QString result = translated(
                    source, false, translationControlId(combo, source));
                return !result.isEmpty() && result != source ? source : QString();
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
    }
    else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
        const int tab = tabs->tabAt(position);
        if (tab >= 0) {
            displayed = tabs->tabText(tab);
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
    if (shouldExcludeLayersPanel(widget) || isInputPaintOwner(widget)) return {};
    if (auto *label = qobject_cast<QLabel *>(widget)) {
        const QString full = sourceFromPainterElidedLabel(label, displayed);
        if (!full.isEmpty()) displayed = full;
    }
    const QString result = translated(displayed, false,
                                      translationControlId(widget, displayed));
    return !result.isEmpty() && result != displayed ? displayed : QString();
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
    } else if (auto *button = qobject_cast<QAbstractButton *>(widget))
        displayed = button->text();
    else if (auto *label = qobject_cast<QLabel *>(widget))
        displayed = label->text();
    else if (auto *group = qobject_cast<QGroupBox *>(widget))
        displayed = group->title();
    else if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        displayed = combo->currentText();
    }
    else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
        const int tab = tabs->tabAt(position);
        if (tab >= 0) {
            displayed = tabs->tabText(tab);
        }
    }
    // Dock widgets span the whole panel. Their window title is not a discrete
    // text control under the cursor, so editing it would also trigger when the
    // user Ctrl+right-clicks an unrelated blank area of the panel.
    else if (qobject_cast<QDockWidget *>(widget))
        return {};

    displayed.remove(u'&');
    displayed = displayed.trimmed();
    if (auto *label = qobject_cast<QLabel *>(widget)) {
        const QString full = sourceFromPainterElidedLabel(label, displayed);
        if (!full.isEmpty()) return full;
    }
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

bool validTranslationPackage(const QJsonObject &root) {
    if (root.value(QStringLiteral("$schema")).toString() != QStringLiteral("sp-translation-v1") ||
        root.value(QStringLiteral("language")).toString() != QStringLiteral("zh-CN") ||
        !root.value(QStringLiteral("translations")).isObject())
        return false;
    const auto entries = root.value(QStringLiteral("translations")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (!it.value().isString()) return false;
    return true;
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
            ? QStringLiteral("Translations added from Substance 3D")
            : QStringLiteral("Translations edited from Substance 3D"));
    } else if (!validTranslationPackage(root)) {
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
    if (!validTranslationPackage(root)) {
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
    synchronizeWidgets(g_enabled, true);
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
    if (!enabled) restoreLayerPopupWidths();
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
    if (source.isEmpty() || g_editDialogOpen)
        return;
    // Keep the DLL mapped through saving, reload callbacks and result dialogs,
    // all of which may enter another event loop after the editor closes.
    QScopedValueRollback<bool> editing(g_editDialogOpen, true);
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

    const int dialogResult = dialog.exec();
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
        g_translations.insert(source, target);
        g_translationsFolded.insert(normalizeForMatch(source), target);
    }
    // Invalidate cached hits as well as misses, including scoped rules.
    invalidateDictionaryCaches();
    refreshTranslatedViews();
}

class TranslationUiFilter final : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        const auto type = event->type();
        // A nested event loop may change the scene while its painter is alive.
        // Discard that batch's value snapshot before control returns to paint.
        if (!g_graphPaintIndexes.empty()) {
            for (auto &entry : g_graphPaintIndexes) {
                if (type != QEvent::Paint ||
                    (entry.second->view && entry.second->view->viewport() == object))
                    entry.second->ready = false;
            }
        }
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
                const bool painted = injectAssetTranslationIntoPreview(
                    candidate, allowHeightGrowth, type);
                if (painted && type == QEvent::Paint &&
                    qobject_cast<QLabel *>(candidate))
                    return true;
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
        if (type == QEvent::Hide) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (isAssetPreviewCandidate(widget)) restoreAssetTooltipDecoration(widget);
        }
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
        // Paint is a final opportunity to attach a missing display delegate.
        // Existing delegates must not schedule another update here.
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
                    // Only a parent-chain lookup on the paint path. The full
                    // scan attaches unusual detached popups outside painting.
                    if (view) {
                        if (QComboBox *combo = owningComboBoxFast(view))
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
    synchronizeWidgets(true, false);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl sp_delegate_api_version() { return 16; }

extern "C" __declspec(dllexport) const wchar_t *__cdecl sp_delegate_build_id() {
    // 构建标识：用于确认正在运行的 DLL 是否包含最新搜索逻辑。
    return L"20260925-complete-performance-api16";
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
    g_idTranslations.clear();
    g_translationsFolded.clear();
    invalidateDictionaryCaches();
}

// Replace both dictionaries atomically, with one Python/native transition and
// one invalidation notification. A malformed payload leaves the old maps live.
extern "C" __declspec(dllexport) int __cdecl sp_delegate_replace_translations(
    const char *json, int length) {
    if (!json || length <= 0 || (qApp && !onUiThread()))
        return 0;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(json, length), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return 0;
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("translations")).isArray() ||
        !root.value(QStringLiteral("control_ids")).isObject())
        return 0;
    QHash<QString, QString> translations, folded, ids;
    const auto entries = root.value(QStringLiteral("translations")).toArray();
    translations.reserve(int(entries.size()));
    folded.reserve(int(entries.size()));
    for (const auto &entry : entries) {
        const auto pair = entry.toArray();
        if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString())
            return 0;
        const QString source = pair.at(0).toString();
        const QString target = pair.at(1).toString();
        translations.insert(source, target);
        folded.insert(computeNormalizedText(source), target);
    }
    const auto scoped = root.value(QStringLiteral("control_ids")).toObject();
    for (auto it = scoped.begin(); it != scoped.end(); ++it) {
        if (!it.value().isString())
            return 0;
        ids.insert(it.key(), it.value().toString());
    }
    g_translations.swap(translations);
    g_translationsFolded.swap(folded);
    g_idTranslations.swap(ids);
    invalidateDictionaryCaches();
    return 1;
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_fuzzy_match(
    int enabled) {
    g_fuzzyMatchEnabled = enabled != 0;
    invalidateDictionaryCaches();
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_fallback_scan(
    int enabled) {
    g_fallbackScanEnabled = enabled != 0;
    if (!g_fallbackTimer)
        return;
    if (g_fallbackScanEnabled && g_enabled && !g_fallbackTimer->isActive())
        g_fallbackTimer->start();
    else if ((!g_fallbackScanEnabled || !g_enabled) && g_fallbackTimer->isActive())
        g_fallbackTimer->stop();
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
#if defined(SD_TRANSLATION_SHORTCUT_DIAGNOSTICS)
    QFile::remove(QDir::temp().filePath(QStringLiteral("sp_shortcut_diag.log")));
#endif
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
        g_translationsFolded.insert(computeNormalizedText(sourceString),
                                    targetString);
        invalidateDictionaryCaches();
    }
}

extern "C" __declspec(dllexport) void __cdecl sp_delegate_set_enabled(int enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled) {
        restoreLayerPopupWidths();
        clearAssetTooltipContext();
        restoreAllAssetTooltipDecorations();
        restoreComboDisplayDelegates();
        restoreAssetDelegates();
    }
    if (g_assetRowFilter)
        g_assetRowFilter->setActive(g_enabled);
    if (g_fallbackTimer) {
        if (g_enabled && g_fallbackScanEnabled) g_fallbackTimer->start();
        else g_fallbackTimer->stop();
    }
    if (g_enabled && g_filter) {
        try {
            if (!installGraphPainterHooks())
                g_translateDesignerGraph = false;
        } catch (...) {
            g_translateDesignerGraph = false;
            uninstallGraphPainterHooks();
        }
    }
    // One pass attaches delegates/search surfaces and repaints both widgets
    // and graphics viewports. Initial dictionary loading defers this to install.
    if (g_filter)
        synchronizeWidgets(g_enabled, true);
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
        invalidateDictionaryCaches();
    }
}

extern "C" __declspec(dllexport) void __cdecl
sp_delegate_set_translate_layers(int enabled) {
    setTranslateLayersPanel(enabled != 0);
}

extern "C" __declspec(dllexport) int __cdecl sp_delegate_install_ui(void *applicationPointer) {
    auto *application = static_cast<QApplication *>(applicationPointer);
    if (!application)
        application = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!application || !onUiThread())
        return 0;
    if (!installGraphPainterHooks())
        return 0;

    // Designer's private graph item paints its title directly; there is no
    // public text child to edit. Patch only the host's imported QPainter
    // drawText calls and substitute exact, currently visible node titles.
    // Geometry, font, clipping and z-order therefore remain entirely native.
    // Generic display hooks are shared by both hosts. Graph translation is
    // independently gated by sp_delegate_set_translate_designer_graph().

    // Do not install a QTranslator: translators receive the original English
    // source before Painter's own translator and could therefore override an
    // official Chinese translation. The widget/model display layer below sees
    // Painter's final text. Existing Chinese is changed only by an explicit
    // dictionary override, as in the normal translation lookup.
    if (!g_filter) {
        g_filter = new TranslationUiFilter(application);
        application->installEventFilter(g_filter);
    }
    if (!g_assetRowFilter) {
        g_assetRowFilter = new AssetSearchManager(application);
        g_assetRowFilter->setActive(g_enabled);
    }
    if (!g_fallbackTimer) {
        g_fallbackTimer = new QTimer(application);
        g_fallbackTimer->setInterval(10000);
        QObject::connect(g_fallbackTimer, &QTimer::timeout, application, [] { scanVisibleWidgets(); });
        // 全量扫描兜底默认关闭，由 Python 侧开关控制（sp_delegate_set_fallback_scan）。
        if (g_fallbackScanEnabled && g_enabled)
            g_fallbackTimer->start();
    }
    // 纯显示层:让 drawText 钩子在两个宿主(含 Painter)都常开安装,
    // 使 generalPainterTranslation 能翻译通用界面文本(样式无关)。
    // 这些钩子内部会先走 graphPaintTranslation(仅当 g_translateDesignerGraph),
    // 再走 generalPainterTranslation,因此对 Painter 不会误触节点图逻辑。
    synchronizeWidgets(true, false);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
sp_delegate_uninstall_ui(void *applicationPointer) {
    if (!onUiThread()) return 0;
    g_enabled = false;
    g_translateDesignerGraph = false;
    g_shortcutCallback = nullptr;
    g_dictionaryReloadCallback = nullptr;
    // A modal translation editor can run a nested event loop during unload.
    // Keep its native code mapped until it returns.
    if (g_editDialogOpen || g_activeHookCalls != 0 || !g_graphPaintIndexes.empty()) return 0;
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
    restoreLayerPopupWidths();
    const bool restored = uninstallGraphPainterHooks();
    return restored && g_activeHookCalls == 0 ? 1 : 0;
}
