#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace lumora::app::theme {

// Palette shared by every Lumora window.
inline const QColor kBackground{0x12, 0x13, 0x16};
inline const QColor kPanel{0x1a, 0x1c, 0x21};
inline const QColor kRaised{0x22, 0x25, 0x2b};
inline const QColor kBorder{0x2c, 0x30, 0x38};
inline const QColor kText{0xd7, 0xda, 0xe0};
inline const QColor kTextMuted{0x8a, 0x90, 0x9c};
inline const QColor kAccent{0x4f, 0x8c, 0xff};
inline const QColor kSuccess{0x5f, 0xcf, 0x8a};
inline const QColor kWarning{0xe8, 0xb8, 0x4a};
inline const QColor kError{0xec, 0x6a, 0x5e};

/// Applies the dark Fusion palette, typography and stylesheet.
void apply(QApplication& app);
/// Icon from the embedded SVG set (":/icons/<name>.svg").
[[nodiscard]] QIcon icon(const QString& name);

} // namespace lumora::app::theme
