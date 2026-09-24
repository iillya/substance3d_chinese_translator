#pragma once
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QListView>
#include <QtWidgets/QLineEdit>

// Test-only public Qt widgets with the host class names used by the adapters.
// These exercise dispatch and lifecycle, not Adobe's private implementations.
namespace Alg {
class ResourcePickerWidget : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
};
class ResourceListView : public QListView {
    Q_OBJECT
public:
    using QListView::QListView;
};
class SearchFieldLineEdit : public QLineEdit {
    Q_OBJECT
public:
    using QLineEdit::QLineEdit;
};
}

namespace Pfx { namespace Editor { namespace Components { namespace Graph {
class GraphView : public QGraphicsView {
    Q_OBJECT
public:
    using QGraphicsView::QGraphicsView;
};
}}}}
