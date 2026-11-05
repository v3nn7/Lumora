#pragma once

#include "common/ModuleExport.hpp"

#include <QWidget>

#include <deque>
#include <functional>

class QLabel;
class QTreeWidget;
class QProgressBar;

namespace lumora::render {
struct FrameStats;
class RenderContext;
}

namespace lumora::editor {

/// Rolling frame-time graph (CPU frame interval).
class LUMORA_EDITOR_API FrameTimeGraph final : public QWidget {
    Q_OBJECT
public:
    explicit FrameTimeGraph(QWidget* parent = nullptr);
    void push(double ms);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    std::deque<double> samples_;
};

/// CPU/GPU timings (GPU per render-graph pass from timestamp queries), draw statistics and VRAM.
class LUMORA_EDITOR_API ProfilerPanel final : public QWidget {
    Q_OBJECT
public:
    using StatsProvider = std::function<const render::FrameStats*()>;
    ProfilerPanel(render::RenderContext* context, StatsProvider stats, QWidget* parent = nullptr);

    /// Called every rendered frame with the CPU frame interval.
    void onFrame(double frameIntervalMs);

private:
    void refresh();

    render::RenderContext* context_;
    StatsProvider stats_;
    FrameTimeGraph* graph_ = nullptr;
    QLabel* fps_ = nullptr;
    QLabel* cpu_ = nullptr;
    QLabel* gpu_ = nullptr;
    QLabel* scene_ = nullptr;
    QTreeWidget* passes_ = nullptr;
    QProgressBar* vram_ = nullptr;
    QLabel* vramLabel_ = nullptr;
    double intervalAccum_ = 0.0;
    int intervalCount_ = 0;
    double lastIntervalMs_ = 0.0;
};

} // namespace lumora::editor
