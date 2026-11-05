#pragma once

#include "common/QtLogSink.hpp"

#include <QWidget>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;

namespace lumora::editor {

/// Live engine log with level filter and text search.
class ConsolePanel final : public QWidget {
    Q_OBJECT
public:
    explicit ConsolePanel(app::LogBridge& bridge, QWidget* parent = nullptr);

private:
    void append(const app::UiLogEntry& entry);
    void rebuild();
    [[nodiscard]] bool accepts(const app::UiLogEntry& entry) const;
    void updateCounts();

    app::LogBridge& bridge_;
    QPlainTextEdit* view_ = nullptr;
    QComboBox* level_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* counts_ = nullptr;
    int warnings_ = 0;
    int errors_ = 0;
};

} // namespace lumora::editor
