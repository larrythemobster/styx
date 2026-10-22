#pragma once

class QKeyEvent;
class QTextEdit;

namespace Styx {
namespace UI {

[[nodiscard]] bool moveCursorToTextEdgeFromOuterLine(QTextEdit *edit, const QKeyEvent *e);

} // namespace UI
} // namespace Styx
