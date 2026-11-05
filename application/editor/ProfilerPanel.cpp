#include "editor/ProfilerPanel.hpp"

#include "common/Theme.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"

#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace lumora::editor {

FrameTimeGraph::FrameTimeGraph(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(70);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void FrameTimeGraph::push(double ms) {
    samples_.push_back(ms);
    while (samples_.size() > 240) samples_.pop_front();
    update();
}

void FrameTimeGraph::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRect(r, QColor(0x14, 0x16, 0x1a));
    p.setPen(QColor(0x2c, 0x30, 0x38));
    p.drawRect(r);
    const double maxMs = std::max(33.4, samples_.empty() ? 0.0 : *std::ranges::max_element(samples_) * 1.1);
    auto yFor = [&](double ms) { return r.bottom() - (ms / maxMs) * r.height(); };
    // Budget lines: 60 and 30 FPS.
    for (const auto& [ms, label] : {std::pair{16.67, "60 fps"}, std::pair{33.33, "30 fps"}}) {
        p.setPen(QPen(QColor(0x3a, 0x40, 0x4a), 1, Qt::DashLine));
        p.drawLine(QPointF(r.left(), yFor(ms)), QPointF(r.right(), yFor(ms)));
        p.setPen(QColor(0x6b, 0x71, 0x7c));
        p.drawText(QPointF(r.left() + 4, yFor(ms) - 2), QString::fromLatin1(label));
    }
    if (samples_.size() < 2) return;
    QPainterPath path;
    const double step = r.width() / 239.0;
    double x = r.right() - (samples_.size() - 1) * step;
    path.moveTo(x, yFor(samples_.front()));
    for (double s : samples_) {
        path.lineTo(x, yFor(s));
        x += step;
    }
    p.setPen(QPen(app::theme::kAccent, 1.4));
    p.drawPath(path);
}

ProfilerPanel::ProfilerPanel(render::RenderContext* context, StatsProvider stats, QWidget* parent)
    : QWidget(parent), context_(context), stats_(std::move(stats)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    graph_ = new FrameTimeGraph;
    layout->addWidget(graph_);

    auto* form = new QFormLayout;
    form->setVerticalSpacing(4);
    fps_ = new QLabel(QStringLiteral("-"));
    cpu_ = new QLabel(QStringLiteral("-"));
    gpu_ = new QLabel(QStringLiteral("-"));
    scene_ = new QLabel(QStringLiteral("-"));
    fps_->setStyleSheet(QStringLiteral("font-size: 13pt; font-weight: 600;"));
    form->addRow(tr("Frame rate"), fps_);
    form->addRow(tr("CPU record"), cpu_);
    form->addRow(tr("GPU frame"), gpu_);
    form->addRow(tr("Scene"), scene_);
    layout->addLayout(form);

    auto* passesTitle = new QLabel(tr("GPU passes"));
    passesTitle->setProperty("section", true);
    layout->addWidget(passesTitle);
    passes_ = new QTreeWidget;
    passes_->setColumnCount(2);
    passes_->setHeaderLabels({tr("Pass"), tr("ms")});
    passes_->setRootIsDecorated(false);
    passes_->header()->setStretchLastSection(false);
    passes_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    passes_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    passes_->setMinimumHeight(120);
    layout->addWidget(passes_, 1);

    auto* memTitle = new QLabel(tr("Video memory"));
    memTitle->setProperty("section", true);
    layout->addWidget(memTitle);
    vram_ = new QProgressBar;
    vram_->setTextVisible(false);
    vramLabel_ = new QLabel;
    vramLabel_->setProperty("muted", true);
    layout->addWidget(vram_);
    layout->addWidget(vramLabel_);

    if (context_ != nullptr) {
        const auto& caps = context_->caps();
        auto* device = new QLabel(QStringLiteral("%1\n%2 · Vulkan %3\nTimestamps: %4 · RT: %5 · Mesh shaders: %6")
                                      .arg(QString::fromStdString(caps.name), QString::fromStdString(caps.driverName),
                                           QString::fromStdString(rhi::versionString(caps.apiVersion)),
                                           caps.timestamps ? tr("yes") : tr("no"),
                                           caps.rayTracingPipeline && caps.accelerationStructure ? tr("yes") : tr("no"),
                                           caps.meshShader ? tr("yes") : tr("no")));
        device->setProperty("muted", true);
        device->setWordWrap(true);
        layout->addWidget(device);
    }

    auto* timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, &ProfilerPanel::refresh);
    timer->start();
}

void ProfilerPanel::onFrame(double frameIntervalMs) {
    graph_->push(frameIntervalMs);
    intervalAccum_ += frameIntervalMs;
    ++intervalCount_;
}

void ProfilerPanel::refresh() {
    if (intervalCount_ > 0) {
        lastIntervalMs_ = intervalAccum_ / intervalCount_;
        intervalAccum_ = 0.0;
        intervalCount_ = 0;
    }
    if (lastIntervalMs_ > 0.0) {
        fps_->setText(QStringLiteral("%1 fps  (%2 ms)").arg(1000.0 / lastIntervalMs_, 0, 'f', 0).arg(lastIntervalMs_, 0, 'f', 2));
    }
    const render::FrameStats* s = stats_ ? stats_() : nullptr;
    if (s == nullptr) return;
    cpu_->setText(QStringLiteral("%1 ms").arg(s->cpuRecordMs, 0, 'f', 3));
    gpu_->setText(s->gpuFrameMs > 0.0 ? QStringLiteral("%1 ms").arg(s->gpuFrameMs, 0, 'f', 3) : tr("n/a"));
    scene_->setText(tr("%1 draws · %2 tris · %3 visible · %4 culled · %5 lights · %6×%7")
                        .arg(s->scene.drawCalls)
                        .arg(s->scene.triangles)
                        .arg(s->scene.visibleObjects)
                        .arg(s->scene.culledObjects)
                        .arg(s->scene.lights)
                        .arg(s->extent.width)
                        .arg(s->extent.height));
    scene_->setWordWrap(true);

    if (passes_->topLevelItemCount() != static_cast<int>(s->gpuPasses.size())) {
        passes_->clear();
        for (std::size_t i = 0; i < s->gpuPasses.size(); ++i) passes_->addTopLevelItem(new QTreeWidgetItem);
    }
    for (std::size_t i = 0; i < s->gpuPasses.size(); ++i) {
        QTreeWidgetItem* item = passes_->topLevelItem(static_cast<int>(i));
        item->setText(0, QString::fromStdString(s->gpuPasses[i].name));
        item->setText(1, QString::number(s->gpuPasses[i].milliseconds, 'f', 3));
    }

    const double usedMiB = static_cast<double>(s->vram.usage) / (1024.0 * 1024.0);
    const double budgetMiB = static_cast<double>(s->vram.budget) / (1024.0 * 1024.0);
    vram_->setRange(0, 1000);
    vram_->setValue(budgetMiB > 0 ? static_cast<int>(usedMiB / budgetMiB * 1000.0) : 0);
    vramLabel_->setText(tr("%1 MiB used of %2 MiB budget (device-local heaps)").arg(usedMiB, 0, 'f', 1).arg(budgetMiB, 0, 'f', 0));
}

} // namespace lumora::editor
