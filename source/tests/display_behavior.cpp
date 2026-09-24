// Run with QT_QPA_PLATFORM=offscreen. No host application or mouse is used.
#include "../cpp/translation_ui_delegate.cpp"
#include "host_fixtures.h"
#include <QtCore/QEventLoop>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtGui/QStandardItemModel>
#include <iostream>
#include <stdexcept>

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

// Qt 5.12's Windows QMessageBox::showEvent dereferences the platform native
// interface, which qoffscreen does not provide. Skip that native-window step
// only in this test; the dialog's modal loop, callbacks and teardown still run.
class OffscreenMessageBoxFilter : public QObject {
public:
    bool eventFilter(QObject *object, QEvent *event) override {
        return QT_VERSION < QT_VERSION_CHECK(6, 0, 0) &&
            QGuiApplication::platformName() == QStringLiteral("offscreen") &&
            event->type() == QEvent::Show && qobject_cast<QMessageBox *>(object);
    }
};

class PaintProbe : public QLabel {
public:
    using QLabel::QLabel;
    QString input = QStringLiteral("Concrete");
    QString result;
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        result = generalPainterTranslation(&painter, input);
    }
    QString probe() {
        show();
        QPixmap image(size());
        render(&image);
        return result;
    }
};

class RecordingDelegate : public QStyledItemDelegate {
public:
    mutable QString measured;
    mutable QString painted;
    const QAbstractItemModel *eventModel = nullptr;
    void paint(QPainter *, const QStyleOptionViewItem &,
               const QModelIndex &index) const override {
        painted = index.data().toString();
    }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        measured = index.data().toString();
        return QSize(measured.size() * 10, 20);
    }
    bool editorEvent(QEvent *, QAbstractItemModel *model,
                     const QStyleOptionViewItem &, const QModelIndex &index) override {
        eventModel = model;
        return index.model() == model && index.data().toString() == QStringLiteral("Concrete");
    }
};

QImage renderWidget(QWidget &widget) {
    widget.ensurePolished();
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

class PreviewSourceView : public QListView {
public:
    QModelIndex indexAt(const QPoint &) const override {
        return model() ? model()->index(0, 0) : QModelIndex();
    }
};

void flushSearch() {
    QEventLoop loop;
    QTimer::singleShot(70, &loop, &QEventLoop::quit);
    loop.exec();
}

class GraphLabel : public QGraphicsItem {
public:
    QString source = QStringLiteral("Concrete");
    QString lastTranslation;
    QRectF boundingRect() const override { return QRectF(0, 0, 160, 30); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        lastTranslation = graphPaintTranslation(painter, source);
        painter->drawText(QPointF(2, 20), source);
    }
};

class GraphOwnerProbe : public QGraphicsItem {
public:
    double lookupUs = 0;
    double drawUs = 0;
    QRectF boundingRect() const override { return QRectF(0, 0, 100, 30); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 100; ++i)
            check(graphOwnerItem(painter) == this, "benchmark graph owner");
        lookupUs = timer.nsecsElapsed() / 100000.0;
        check(g_graphPaintIndexes.count(painter) == 1,
              "actual Qt painter lifetime enables the batch index");
        timer.restart();
        for (int i = 0; i < 100; ++i)
            hookedDrawRectFAlign(painter, boundingRect(), Qt::AlignLeft,
                                QStringLiteral("Graph benchmark missing 94017"), nullptr);
        drawUs = timer.nsecsElapsed() / 100000.0;
    }
};

class LayoutCountingResourceView : public Alg::ResourceListView {
public:
    using Alg::ResourceListView::ResourceListView;
    int layouts = 0;
    void doItemsLayout() override {
        ++layouts;
        Alg::ResourceListView::doItemsLayout();
    }
};

class CountingResourceModel : public QStandardItemModel {
public:
    mutable int displayReads = 0;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (role == Qt::DisplayRole) ++displayReads;
        return QStandardItemModel::data(index, role);
    }
};

class MovingResourceModel : public QAbstractListModel {
public:
    QStringList rows{QStringLiteral("Concrete"), QStringLiteral("Other")};
    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : int(rows.size());
    }
    QVariant data(const QModelIndex &index, int role) const override {
        return index.isValid() && role == Qt::DisplayRole ? QVariant(rows.at(index.row())) : QVariant();
    }
    void moveFirstToLast() {
        check(beginMoveRows({}, 0, 0, {}, 2), "resource move begins");
        rows.move(0, 1);
        endMoveRows();
    }
};

// Invoke the same single-shot filter callback without including its intentional
// 40 ms debounce delay in measurements. No mouse/keyboard or host is involved.
void runPendingSearch(AssetRowFilter &filter) {
    auto *timer = filter.findChild<QTimer *>();
    check(timer && timer->isSingleShot(), "search uses an owned single-shot timer");
    timer->stop();
    check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
          "search callback can be exercised offscreen");
}

class GraphResolutionProbe : public QGraphicsItem {
public:
    int paints = 0;
    QRectF boundingRect() const override { return QRectF(0, 0, 100, 30); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        ++paints;
        auto *view = designerGraphViewForPainter(painter);
        check(view && view->scene(), "graph resolver test uses an actual viewport");
        for (qreal offset : {0.0, 0.375, 0.0, 0.375, 0.0, 0.375}) {
            painter->save();
            painter->translate(offset, offset);
            // Reference the previous exhaustive algorithm, including first-tie
            // selection and nearest-transform fallback for non-exact matches.
            QGraphicsItem *expected = nullptr;
            qreal best = 1.0e20;
            for (auto *item : view->scene()->items()) {
                const qreal distance = transformDifference(
                    painter->worldTransform(),
                    item->deviceTransform(view->viewportTransform()));
                if (distance < best) {
                    expected = item;
                    best = distance;
                }
            }
            qreal distance = 0;
            check(graphOwnerItem(painter, &distance) == expected && distance == best,
                  "optimized lookup preserves exhaustive owner/distance semantics");
            GraphPaintContext context(painter);
            const int side = graphPortLabelSide(painter, boundingRect(), &context);
            check(context.owner().fullTitle == graphFullTitleFromItem(expected) &&
                  context.owner().transform == expected->deviceTransform(view->viewportTransform()),
                  "draw context resolves the same owner metadata");
            check(graphPaintTranslation(painter, QStringLiteral("Material …"), side, &context) ==
                      graphPaintTranslation(painter, QStringLiteral("Material …"), side),
                  "shared draw context preserves node translation");
            painter->restore();
        }
        const auto batch = g_graphPaintIndexes.find(painter);
        check(batch != g_graphPaintIndexes.end() && batch->second->ready,
              "regression covers the indexed path beyond the adaptive threshold");
    }
};

// A reproducible microbenchmark, not a claim about total SP/SD CPU usage.
int benchmark(QApplication &app, const QString &dictionaryDirectory) {
    PROCESS_MEMORY_COUNTERS_EX before{}, after{};
    GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before), sizeof(before));
    QElapsedTimer timer;
    timer.start();
    const QDir directory(dictionaryDirectory);
    for (const QString &name : directory.entryList({QStringLiteral("*_zh.json")}, QDir::Files)) {
        QFile file(directory.filePath(name));
        check(file.open(QIODevice::ReadOnly), "benchmark dictionary readable");
        QJsonParseError error;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &error).object();
        check(error.error == QJsonParseError::NoError, "benchmark dictionary valid JSON");
        const QJsonObject entries = root.value(QStringLiteral("translations")).toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (!it.value().isString()) continue;
            const std::wstring key = it.key().toStdWString();
            const std::wstring target = it.value().toString().toStdWString();
            if (name == QStringLiteral("control_ids_zh.json"))
                sp_delegate_add_id_translation(key.c_str(), target.c_str());
            else
                sp_delegate_add_translation(key.c_str(), target.c_str());
        }
    }
    const double loadMs = timer.nsecsElapsed() / 1.e6;
    GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after), sizeof(after));
    std::cout << "Qt=" << qVersion() << " entries=" << g_translations.size()
              << " scoped=" << g_idTranslations.size() << " load_ms=" << loadMs
              << " private_bytes_delta=" << (qint64(after.PrivateUsage) - qint64(before.PrivateUsage)) << '\n';
    QStringList exact, normalized;
    for (auto it = g_translations.cbegin(); it != g_translations.cend(); ++it) {
        if (it.key().contains(QStringLiteral("||")) || containsCjk(it.key())) continue;
        exact.append(it.key());
    }
    exact.sort();
    for (const QString &key : exact) normalized.append(normalizeForMatch(key));
    check(!exact.isEmpty(), "benchmark has terms");
    volatile qint64 checksum = 0;
    auto measureLookup = [&](const char *name, const QStringList &queries) {
        const int count = 200000;
        timer.restart();
        for (int i = 0; i < count; ++i)
            checksum += translated(queries.at(i % queries.size())).size();
        std::cout << name << "_us=" << timer.nsecsElapsed() / (1000.0 * count) << '\n';
    };
    measureLookup("exact", exact);
    measureLookup("normalized", normalized);
    measureLookup("miss", {QStringLiteral("This benchmark phrase is absent 94017")});
    // Keep the rendered text identical in all three modes so shorter Chinese
    // glyph runs do not misleadingly appear to be negative hook overhead.
    QLabel label(QStringLiteral("Untranslated benchmark label 94017"));
    label.resize(240, 40);
    label.show(); app.processEvents();
    QImage image(label.size(), QImage::Format_ARGB32_Premultiplied);
    auto measurePaint = [&](const char *name) {
        for (int i = 0; i < 100; ++i) label.render(&image);
        timer.restart();
        const int count = 4000;
        for (int i = 0; i < count; ++i) label.render(&image);
        std::cout << name << "_us=" << timer.nsecsElapsed() / (1000.0 * count) << '\n';
    };
    measurePaint("label_unhooked");
    check(sp_delegate_install_ui(&app) == 1, "benchmark installs engine");
    sp_delegate_set_enabled(0);
    measurePaint("label_disabled");
    sp_delegate_set_enabled(1);
    measurePaint("label_enabled");
    check(g_fallbackTimer && !g_fallbackTimer->isActive(), "default scan is idle");
    std::cout << "default_scan_timer_active=" << g_fallbackTimer->isActive()
              << " hook_slots=" << g_graphHookSlots.size() << " checksum=" << checksum << '\n';
    QGraphicsScene scene;
    auto *probe = new GraphOwnerProbe;
    scene.addItem(probe);
    Pfx::Editor::Components::Graph::GraphView graphView;
    graphView.setScene(&scene);
    graphView.setSceneRect(0, 0, 160, 60);
    graphView.resize(160, 60);
    graphView.show();
    g_translateDesignerGraph = true;
    for (int count : {100, 1000, 5000}) {
        while (scene.items().size() < count) {
            auto *item = scene.addRect(0, 0, 10, 10);
            item->setPos(10000 + scene.items().size(), 10000);
        }
        // First and last positions expose best/worst traversal order rather
        // than making an early-exit optimization look uniformly constant-time.
        for (int position : {-1, 1}) {
            probe->setZValue(position);
            renderWidget(graphView);
            const char *order = position < 0 ? "last" : "first";
            std::cout << "graph_owner_" << count << '_' << order << "_us=" << probe->lookupUs << '\n';
            std::cout << "graph_rect_" << count << '_' << order << "_us=" << probe->drawUs << '\n';
        }
    }
    check(sp_delegate_uninstall_ui(&app) == 1, "benchmark teardown");
    g_enabled = true;
    Alg::ResourcePickerWidget picker;
    LayoutCountingResourceView resources(&picker);
    Alg::SearchFieldLineEdit search(&picker);
    QStandardItemModel model;
    for (int i = 0; i < 5000; ++i)
        model.appendRow(new QStandardItem(i % 2 ? QStringLiteral("Concrete")
                                               : QStringLiteral("Other absent 94017")));
    resources.setModel(&model);
    AssetRowFilter filter;
    filter.observe(&resources);
    search.setText(QStringLiteral("混凝土"));
    runPendingSearch(filter);
    check(resources.isRowHidden(0) && !resources.isRowHidden(1),
          "benchmark actually applies the Chinese row mask");
    resources.layouts = 0;
    timer.restart();
    for (int i = 0; i < 20; ++i) runPendingSearch(filter);
    std::cout << "search_5000_unchanged_ms=" << timer.nsecsElapsed() / 20.e6 << '\n';
    std::cout << "search_unchanged_layouts=" << resources.layouts << '\n';
    filter.shutdown();
    return 0;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 12));
    try {
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--benchmark"))
            return benchmark(app, QString::fromLocal8Bit(argv[2]));
        check(containsCjk(QString::fromUtf8("\xf0\xa0\x80\x80")) &&
              !containsCjk(QString::fromUtf8("\xf0\x9f\x98\x80")),
              "allocation-free Han detection preserves supplementary characters");
        check(normalizeForMatch(QString::fromUtf8("Ｒóugh_Glass …")) == QStringLiteral("roughglass"),
              "normalization cache preserves width, accents and elision folding");
        sp_delegate_add_translation(L"Concrete", L"混凝土");
        check(translated(QStringLiteral("cONCrete")) == QStringLiteral("混凝土"), "warm fuzzy hit");
        sp_delegate_set_fuzzy_match(0);
        check(translated(QStringLiteral("cONCrete")).isNull(), "fuzzy option invalidates cached hits");
        sp_delegate_set_fuzzy_match(1);
        check(translated(QStringLiteral("Con&crete"), true) == QStringLiteral("混凝土") &&
              translated(QStringLiteral("Con&crete"), false).isNull(), "mnemonic policies have distinct caches");
        check(translated(QStringLiteral("Future term")).isNull(), "cache a miss");
        sp_delegate_add_translation(L"Future term", L"新词条");
        check(translated(QStringLiteral("Future term")) == QStringLiteral("新词条"), "new entry invalidates miss");
        const QString id = QStringLiteral("Test||QLabel||None||Concrete");
        sp_delegate_add_id_translation(L"Test||QLabel||None||Concrete", L"专用译文");
        check(translated(QStringLiteral("Concrete"), false, id) == QStringLiteral("专用译文"), "scoped cached hit");
        sp_delegate_add_id_translation(L"Test||QLabel||None||Concrete", L"_skip_");
        check(translated(QStringLiteral("Concrete"), false, id).isNull(), "skip rule invalidates cached translation");
        sp_delegate_add_id_translation(L"Test||QLabel||None||*", L"_skip_");
        sp_delegate_add_translation(L"Value || Other", L"错误译文");
        check(translated(QStringLiteral("Value || Other"), false,
              QStringLiteral("Test||QLabel||None||Value || Other")).isNull(),
              "wildcard excludes source text containing the ID separator");
        const QByteArray bulk = R"({"translations":[["roughglass","first"],["Rough Glass","last"]],"control_ids":{}})";
        check(sp_delegate_replace_translations(bulk.constData(), int(bulk.size())) == 1,
              "atomic dictionary replacement succeeds");
        check(translated(QStringLiteral("ROUGH_GLASS")) == QStringLiteral("last"),
              "bulk upload preserves normalization override order");
        const auto revision = g_dictionaryRevision;
        const QByteArray invalid = R"({"translations":[["broken",42]],"control_ids":{}})";
        check(sp_delegate_replace_translations(invalid.constData(), int(invalid.size())) == 0 &&
              g_dictionaryRevision == revision && translated(QStringLiteral("ROUGH_GLASS")) == QStringLiteral("last"),
              "invalid bulk upload preserves the previous dictionary and revision");
        for (int i = 0; i < 12000; ++i) {
            translated(QStringLiteral("absent text %1").arg(i));
            normalizeForMatch(QString::number(i));
        }
        check(g_lookupResults.totalCost() <= g_lookupResults.maxCost() &&
              g_normalizedText.totalCost() <= g_normalizedText.maxCost(), "text caches obey memory budgets");
        sp_delegate_clear_translations();
        const QString source = QStringLiteral("Concrete");
        const QString target = QStringLiteral("混凝土");
        g_translations.insert(source, target);
        g_translations.insert(QStringLiteral("Material"), QStringLiteral("材质"));
        PaintProbe label;
        label.resize(160, 30);
        label.setText(source);
        check(label.probe() == target, "plain label paint translation");
        check(label.text() == source, "host label source unchanged");
        check(originalTextAt(&label, QPoint()) == source, "original-text tooltip");
        g_idTranslations.insert(controlUniqueId(&label, source), QStringLiteral("专用译文"));
        check(label.probe() == QStringLiteral("专用译文"), "control ID wins over global dictionary");
        g_idTranslations.clear();
        g_translations.insert(target, QStringLiteral("自定义混凝土"));
        label.input = target;
        label.setText(target);
        check(label.probe() == QStringLiteral("自定义混凝土"),
              "explicit dictionary override of native Chinese is preserved");
        {
            QScopedValueRollback<QString> presentation(g_delegatePaintText, target);
            check(label.probe().isEmpty(), "resolved delegate text is not translated twice");
        }
        label.input = source;
        label.setText(source);
        g_translations.remove(target);
        g_enabled = false;
        check(label.probe().isEmpty(), "disabled paint falls through to English");
        g_enabled = true;
        label.input = QStringLiteral("Mater…");
        check(label.probe().isEmpty(), "ambiguous ellipsis is not guessed");
        label.input = source;

        QLineEdit editor;
        editor.setText(source);
        editor.setSelection(1, 3);
        PaintProbe editorChild(&editor);
        editorChild.resize(100, 20);
        check(editorChild.probe().isEmpty(), "editor paint is protected by actual owner");
        editor.show(); editor.setFocus(); app.processEvents();
        check(label.probe() == target, "focused editor does not suppress another label");
        check(editor.text() == source && editor.selectedText() == QStringLiteral("onc"),
              "input and selection unchanged");

        QWidget layers;
        layers.setObjectName(QStringLiteral("DockLayers"));
        PaintProbe layerLabel(&layers);
        layerLabel.resize(100, 20);
        g_translateLayersPanel = false;
        check(layerLabel.probe().isEmpty(), "layers toggle respected by generic paint");
        g_translateLayersPanel = true;
        check(layerLabel.probe() == target, "layers paint enabled");

        QComboBox combo(&layers);
        combo.addItem(source, QStringLiteral("business-id"));
        QAbstractItemModel *nativeModel = combo.model();
        const QModelIndex index = nativeModel->index(0, 0);
        ComboPaintProxyModel proxy(&combo, nullptr);
        proxy.setSourceModel(nativeModel);
        check(proxy.index(0, 0).data().toString() == target, "combo presentation translated");
        check(!proxy.setData(proxy.index(0, 0), target, Qt::EditRole),
              "display proxy cannot write translated text into the host model");
        check(proxy.index(0, 0).data(Qt::UserRole).toString() == QStringLiteral("business-id"),
              "business role preserved");
        check(combo.currentText() == source && combo.model() == nativeModel,
              "combo native text and model unchanged");
        g_translateLayersPanel = false;
        check(proxy.index(0, 0).data().toString() == source, "combo respects layers toggle");
        g_translateLayersPanel = true;
        RecordingDelegate original;
        ComboPaintDelegate delegate(combo.view(), &combo, &original);
        QStyleOptionViewItem option;
        delegate.paint(nullptr, option, index);
        delegate.sizeHint(option, index);
        check(original.painted == target && original.measured == target,
              "paint and size calculation use the same translation");
        QEvent event(QEvent::User);
        check(delegate.editorEvent(&event, nativeModel, option, index),
              "delegate interaction receives real source index");
        check(original.eventModel == nativeModel, "delegate model preserved");
        auto *temporaryDelegate = new QStyledItemDelegate;
        ComboPaintDelegate survivingDelegate(combo.view(), &combo, temporaryDelegate);
        delete temporaryDelegate;
        check(survivingDelegate.sizeHint(option, index).isValid(),
              "destroyed original delegate has a usable Qt fallback");
        for (int i = 0; i < kGraphCacheLimit + 100; ++i)
            cacheGraphTranslation(QString::number(i), QString());
        check(g_fuzzyResolved.size() <= kGraphCacheLimit, "graph cache has a hard bound");
        sp_delegate_add_translation(L"Concrete", L"混凝土");
        check(g_fuzzyResolved.isEmpty(), "dictionary update invalidates cached misses");

        Alg::ResourcePickerWidget resourcePicker;
        LayoutCountingResourceView resourceList(&resourcePicker);
        Alg::SearchFieldLineEdit resourceSearch(&resourcePicker);
        CountingResourceModel resourceModel;
        resourceModel.appendRow(new QStandardItem(QStringLiteral("Concrete")));
        resourceModel.appendRow(new QStandardItem(QStringLiteral("Native hidden")));
        resourceModel.appendRow(new QStandardItem(QStringLiteral("Other")));
        resourceList.setModel(&resourceModel);
        resourceList.setRowHidden(1, true);
        AssetRowFilter rowFilter;
        rowFilter.observe(&resourceList);
        check(resourceList.isRowHidden(1), "binding preserves host-hidden rows");
        resourceSearch.setText(QStringLiteral("混凝 土"));
        resourceSearch.setSelection(0, 2);
        flushSearch();
        check(!resourceList.isRowHidden(0) && resourceList.isRowHidden(2),
              "multi-term Chinese search uses each normalized term");
        check(resourceSearch.text() == QStringLiteral("混凝 土") &&
              resourceSearch.selectedText() == QStringLiteral("混凝"),
              "search preserves text and selection");
        resourceList.layouts = 0;
        resourceModel.displayReads = 0;
        runPendingSearch(rowFilter);
        check(resourceList.layouts == 0, "unchanged search mask does not force layout");
        check(resourceModel.displayReads == 0, "warm search does not reread model text");
        rowFilter.translationsChanged();
        runPendingSearch(rowFilter);
        check(resourceModel.displayReads > 0, "dictionary reload rebuilds search text");
        auto *searchTimer = rowFilter.findChild<QTimer *>();
        resourceModel.item(0)->setData(QColor(Qt::red), Qt::DecorationRole);
        resourceModel.item(0)->setData(QStringLiteral("Resource help"), Qt::ToolTipRole);
        resourceModel.item(0)->setData(QSize(48, 48), Qt::SizeHintRole);
        check(!searchTimer->isActive(), "thumbnail-only updates do not schedule search");
        resourceModel.item(0)->setData(1, Qt::UserRole);
        check(searchTimer->isActive(), "unknown host roles still trigger search");
        runPendingSearch(rowFilter);
        resourceModel.dataChanged(resourceModel.index(0, 0), resourceModel.index(2, 0));
        check(searchTimer->isActive(), "unspecified changed roles still trigger search");
        runPendingSearch(rowFilter);
        resourceModel.item(2)->setText(source);
        check(searchTimer->isActive(), "changed labels schedule search");
        runPendingSearch(rowFilter);
        check(!resourceList.isRowHidden(2) && resourceList.layouts > 0,
              "changed model still updates search visibility and layout");
        resourceModel.item(2)->setText(QStringLiteral("Other"));
        runPendingSearch(rowFilter);
        resourceSearch.clear(); flushSearch();
        check(resourceList.isRowHidden(1) && !resourceList.isRowHidden(2),
              "clearing search restores only plugin-hidden rows");
        rowFilter.shutdown();
        check(resourceList.model() == &resourceModel && resourceList.isRowHidden(1),
              "search teardown preserves the native model and row state");
        CountingResourceModel replacementModel;
        replacementModel.appendRow(new QStandardItem(QStringLiteral("Other")));
        replacementModel.appendRow(new QStandardItem(source));
        resourceList.setModel(&replacementModel);
        rowFilter.setActive(true);
        rowFilter.observe(&resourceList);
        resourceSearch.setText(target);
        runPendingSearch(rowFilter);
        check(resourceList.isRowHidden(0) && !resourceList.isRowHidden(1),
              "model replacement cannot reuse old row text");
        replacementModel.clear();
        replacementModel.appendRow(new QStandardItem(source));
        replacementModel.appendRow(new QStandardItem(QStringLiteral("Other")));
        runPendingSearch(rowFilter);
        check(!resourceList.isRowHidden(0) && resourceList.isRowHidden(1),
              "model reset invalidates cached row positions");
        resourceList.setObjectName(QStringLiteral("new_scope"));
        const std::wstring scopedId = controlUniqueId(&resourceList, source).toStdWString();
        sp_delegate_add_id_translation(scopedId.c_str(), L"_skip_");
        runPendingSearch(rowFilter);
        check(resourceList.isRowHidden(0), "new scoped skip rules invalidate search text");
        g_idTranslations.clear();
        invalidateDictionaryCaches();
        runPendingSearch(rowFilter);
        check(!resourceList.isRowHidden(0), "removed scope rules restore search results");
        resourceSearch.clear();
        rowFilter.shutdown();
        MovingResourceModel movingModel;
        resourceList.setModel(&movingModel);
        rowFilter.setActive(true);
        rowFilter.observe(&resourceList);
        resourceSearch.setText(target);
        runPendingSearch(rowFilter);
        check(!resourceList.isRowHidden(0) && resourceList.isRowHidden(1), "initial moving resource mask");
        movingModel.moveFirstToLast();
        check(rowFilter.findChild<QTimer *>()->isActive(), "moving rows schedules refilter");
        runPendingSearch(rowFilter);
        check(resourceList.isRowHidden(0) && !resourceList.isRowHidden(1), "moved rows use current cached text");
        rowFilter.shutdown();
        rowFilter.shutdown();
        check(graphHookEnvironmentCompatible(), "compiled Qt major supported");
        QLabel nativeLabel(source);
        nativeLabel.resize(180, 30);
        nativeLabel.show(); app.processEvents();
        const QImage english = renderWidget(nativeLabel);
        check(installGraphPainterHooks(), "real QPainter imports mounted");
        const QImage chinese = renderWidget(nativeLabel);
        check(chinese != english, "real QLabel render reaches the hook");
        check(nativeLabel.text() == source, "real hook leaves QLabel source untouched");
        QGraphicsScene scene;
        auto *graphLabel = new GraphLabel;
        scene.addItem(graphLabel);
        Pfx::Editor::Components::Graph::GraphView graphView;
        graphView.setScene(&scene);
        graphView.resize(220, 90);
        graphView.show(); app.processEvents();
        g_translateDesignerGraph = false;
        const QImage graphEnglish = renderWidget(graphView);
        check(graphLabel->lastTranslation.isEmpty(), "generic hook cannot bypass the graph switch");
        g_translateDesignerGraph = true;
        check(renderWidget(graphView) != graphEnglish && graphLabel->lastTranslation == target,
              "Designer graph adapter paints translations when enabled");
        g_translateDesignerGraph = false;
        check(renderWidget(graphView) == graphEnglish, "graph switch restores English pixels");
        g_translateDesignerGraph = true;
        graphLabel->source = QStringLiteral("Material …");
        graphLabel->setToolTip(QStringLiteral("Material One"));
        auto *otherGraphLabel = new GraphLabel;
        otherGraphLabel->source = graphLabel->source;
        otherGraphLabel->setToolTip(QStringLiteral("Material Two"));
        otherGraphLabel->setPos(190, 0);
        scene.addItem(otherGraphLabel);
        graphView.resize(500, 90);
        g_translations.insert(QStringLiteral("Material One"), QStringLiteral("材质一"));
        g_translations.insert(QStringLiteral("Material Two"), QStringLiteral("材质二"));
        renderWidget(graphView);
        check(g_graphPaintIndexes.empty(), "painter destruction releases all batch indexes");
        check(graphLabel->lastTranslation == QStringLiteral("材质一") &&
              otherGraphLabel->lastTranslation == QStringLiteral("材质二"),
              "same elided title uses each node's own tooltip cache entry");
        auto *resolverProbe = new GraphResolutionProbe;
        scene.addItem(resolverProbe);
        resolverProbe->setToolTip(QStringLiteral("Material One"));
        renderWidget(graphView);
        const int originalPaints = resolverProbe->paints;
        check(originalPaints > 0, "owner regression probe actually paints");
        resolverProbe->setPos(10, 5);
        resolverProbe->setZValue(2);
        graphView.scale(1.25, 0.9);
        renderWidget(graphView);
        delete otherGraphLabel;
        resolverProbe->setParentItem(graphLabel);
        graphLabel->setRotation(15);
        renderWidget(graphView);
        check(resolverProbe->paints >= originalPaints + 2,
              "owner checks run again after move, zoom, delete and reparent");
        delete resolverProbe;
        g_translateDesignerGraph = false;
        auto checkPaint = [&](QWidget &widget, const char *message) {
            widget.resize(240, 80);
            widget.show(); app.processEvents();
            g_enabled = false;
            const QImage before = renderWidget(widget);
            g_enabled = true;
            check(renderWidget(widget) != before, message);
        };
        QPushButton button(source);
        checkPaint(button, "native button paint");
        check(button.text() == source, "button source preserved");
        QGroupBox group(source);
        checkPaint(group, "native group title paint");
        QTabBar tabs;
        tabs.addTab(source);
        tabs.setTabData(0, QStringLiteral("host-owned-payload"));
        checkPaint(tabs, "native tab paint");
        check(contextSourceAt(&tabs, tabs.tabRect(0).center()) == source,
              "translation editor must not interpret host tabData as source");
        check(tabs.tabData(0).toString() == QStringLiteral("host-owned-payload"),
              "host tab metadata preserved");
        QComboBox nativeCombo;
        nativeCombo.addItem(source);
        check(paintSource(&nativeCombo, QStringLiteral("Con…")) == source,
              "elided combo uses its own full source");
        checkPaint(nativeCombo, "native closed combo paint");
        check(nativeCombo.currentText() == source, "closed combo source preserved");
        auto *nativeComboDelegate = nativeCombo.view()->itemDelegate();
        check(installComboDisplayDelegate(nativeCombo.view(), &nativeCombo) == 1,
              "standard combo popup delegate supported");
        check(installComboDisplayDelegate(nativeCombo.view(), &nativeCombo) == 2,
              "combo attachment is idempotent");
        restoreComboDisplayDelegates();
        check(nativeCombo.view()->itemDelegate() == nativeComboDelegate,
              "native combo delegate restored on teardown");
        QTabBar ambiguousTabs;
        ambiguousTabs.addTab(QStringLiteral("Material One"));
        ambiguousTabs.addTab(QStringLiteral("Material Two"));
        check(paintSource(&ambiguousTabs, QStringLiteral("Mater…")) == QStringLiteral("Mater…"),
              "ambiguous tabs must not choose an arbitrary source");
        QMenu menu;
        QAction *action = menu.addAction(source);
        checkPaint(menu, "native menu paint");
        check(action->text() == source, "menu action source preserved");
        QLineEdit placeholder;
        placeholder.setPlaceholderText(source);
        checkPaint(placeholder, "empty input placeholder paint");
        check(placeholder.text().isEmpty() && placeholder.placeholderText() == source,
              "placeholder source and empty value preserved");
        placeholder.setText(source);
        g_enabled = false;
        const QImage inputEnglish = renderWidget(placeholder);
        g_enabled = true;
        check(renderWidget(placeholder) == inputEnglish, "entered value never translated");
        QLabel originalTip(source, nullptr, Qt::ToolTip);
        originalTip.resize(180, 30);
        g_enabled = false;
        const QImage tipEnglish = renderWidget(originalTip);
        g_enabled = true;
        check(renderWidget(originalTip) == tipEnglish, "original tooltip never retranslated");
        PreviewSourceView previewView;
        QStandardItemModel previewModel;
        previewModel.appendRow(new QStandardItem(source));
        previewView.setModel(&previewModel);
        previewView.show(); app.processEvents();
        AssetTooltipContext context;
        context.view = &previewView;
        context.index = previewModel.index(0, 0);
        context.source = source;
        context.translation = target;
        context.generation = 1;
        QLabel preview;
        const QString previewHtml = QStringLiteral("<b>Concrete</b><br/>Native metadata");
        preview.setText(previewHtml);
        preview.resize(220, 80);
        check(injectAssetTranslationIntoLabel(&preview, context, true, QEvent::Show),
              "asset preview translation prepared");
        check(preview.text() == previewHtml, "preview HTML remains byte-for-byte unchanged");
        auto *document = preview.findChild<QTextDocument *>(QStringLiteral("sp_asset_preview_document"));
        check(document && document->toPlainText().contains(target) &&
              document->toPlainText().contains(QStringLiteral("Native metadata")),
              "preview document contains translation and native metadata");
        const int previewHeight = preview.height();
        injectAssetTranslationIntoLabel(&preview, context, true, QEvent::Show);
        check(preview.height() == previewHeight, "preview refresh must not grow repeatedly");
        restoreAssetTooltipDecoration(&preview);
        check(!preview.findChild<QTextDocument *>(QStringLiteral("sp_asset_preview_document")),
              "preview document removed on teardown");
        g_enabled = false;
        check(renderWidget(nativeLabel) == english, "real toggle restores English pixels");
        g_enabled = true;
        check(uninstallGraphPainterHooks(), "real hooks restored");
        check(renderWidget(nativeLabel) == english, "unloading restores original paint");
        g_editDialogOpen = true;
        check(sp_delegate_uninstall_ui(&app) == 0, "unload refuses active native dialog");
        g_editDialogOpen = false;
        check(sp_delegate_uninstall_ui(&app) == 1, "unload acknowledges complete cleanup");
        check(g_graphHookSlots.empty() && g_hookModuleRefs.empty(), "hook slots and module references released");
        // Another hook may retain our trampoline. Never report safe unload or
        // overwrite the third party's pointer in that case.
        void *originalPointer = reinterpret_cast<void *>(&hookedDrawPoint);
        void *replacementPointer = reinterpret_cast<void *>(&hookedDrawPointF);
        void *thirdPartyPointer = reinterpret_cast<void *>(&hookedDrawXY);
        void *slot = thirdPartyPointer;
        std::vector<GraphHookSlot> chained{{&slot, originalPointer, replacementPointer}};
        check(!restoreImportHooks(chained) && slot == thirdPartyPointer,
              "third-party chained import prevents unsafe unload");
        slot = replacementPointer;
        check(restoreImportHooks(chained) && slot == originalPointer, "owned import restores");
        check(sp_delegate_install_ui(&app) == 1, "engine can be reinstalled");
        sp_delegate_set_fallback_scan(1);
        sp_delegate_set_enabled(1);
        check(g_fallbackTimer->isActive(), "explicit fallback enabled");
        QListView compactList;
        compactList.setGridSize(QSize(80, 80));
        compactList.setWordWrap(false);
        installAssetDelegate(&compactList, true);
        check(compactList.gridSize().height() > 80, "compact translation adds label space");
        sp_delegate_set_enabled(0);
        check(!g_fallbackTimer->isActive(), "disabled engine stops fallback wakeups");
        check(compactList.gridSize() == QSize(80, 80) && !compactList.wordWrap(),
              "disabling restores original resource grid geometry");
        QWidget popupWidthProbe;
        popupWidthProbe.setMinimumWidth(20);
        popupWidthProbe.setMaximumWidth(600);
        lockPopupWidth(&popupWidthProbe, 150);
        lockPopupWidth(&popupWidthProbe, 200);
        sp_delegate_set_enabled(0);
        check(popupWidthProbe.minimumWidth() == 20 && popupWidthProbe.maximumWidth() == 600,
              "disabling restores popup constraints before repeated locks");
        lockPopupWidth(&popupWidthProbe, 150);
        popupWidthProbe.setMaximumWidth(500);
        setTranslateLayersPanel(false);
        check(popupWidthProbe.minimumWidth() == 20 && popupWidthProbe.maximumWidth() == 500,
              "layer toggle restores only constraints still owned by plugin");
        QTemporaryDir dictionaryDir;
        const QString invalidPath = dictionaryDir.filePath(QStringLiteral("bad_zh.json"));
        QFile invalidDictionary(invalidPath);
        check(invalidDictionary.open(QIODevice::WriteOnly), "create invalid dictionary fixture");
        const QByteArray invalidBytes = R"({"$schema":"sp-translation-v1","language":"zh-CN","translations":["retain me"]})";
        invalidDictionary.write(invalidBytes);
        invalidDictionary.close();
        QString saveError;
        check(!saveTranslation(source, target, &saveError, invalidPath), "editor rejects invalid existing entries");
        check(invalidDictionary.open(QIODevice::ReadOnly) && invalidDictionary.readAll() == invalidBytes,
              "editor preserves invalid dictionary bytes");
        invalidDictionary.close();
        bool protectedResultDialog = false;
        OffscreenMessageBoxFilter modalPlatformFilter;
        app.installEventFilter(&modalPlatformFilter);
        QTimer::singleShot(0, &app, [&] {
            auto *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            check(editor, "native translation editor is modal");
            auto *field = editor->findChild<QLineEdit *>(QStringLiteral("sp_translation_target"));
            check(field, "translation field present");
            field->clear();
            editor->accept();
            QTimer::singleShot(0, &app, [&] {
                auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                protectedResultDialog = message && g_editDialogOpen && sp_delegate_uninstall_ui(&app) == 0;
                if (message) message->accept();
            });
        });
        editTranslation(source, {}, {}, nullptr);
        check(protectedResultDialog && !g_editDialogOpen, "result dialogs retain native unload protection");
        check(sp_delegate_uninstall_ui(&app) == 1, "second teardown");
        std::cout << "Display behavior passed on Qt " << qVersion() << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
