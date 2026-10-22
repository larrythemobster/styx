#pragma once

#include "Controller.hpp"
#include <QString>
#include <functional>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
class Shell;
namespace Rml {

struct ConfirmOptions {
    QString title;
    QString body;
    QString confirmText = QStringLiteral("Confirm");
    QString cancelText = QStringLiteral("Cancel");
    bool danger = false;
    std::function<void()> onConfirm;
    std::function<void()> onCancel;
};

class ConfirmModalController final : public Controller
{
    Q_OBJECT
public:
    explicit ConfirmModalController(Shell &shell, QObject *parent = nullptr);
    ~ConfirmModalController() override;

    void show(const ConfirmOptions &options);
    void hide();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void updateContent();

    ::Rml::ElementDocument *modalDocument = nullptr;
    ConfirmOptions currentOptions;
    bool visible = false;
};

// Generic RmlUi confirm modal entry points
void showConfirmModal(Shell &shell, const ConfirmOptions &options);
void showConfirmModal(const ConfirmOptions &options);

inline void showConfirmModal(Shell &shell,
                             const QString &title,
                             const QString &body,
                             const QString &confirmText,
                             std::function<void()> onConfirm,
                             std::function<void()> onCancel = nullptr,
                             bool danger = false)
{
    ConfirmOptions opt;
    opt.title = title;
    opt.body = body;
    opt.confirmText = confirmText;
    opt.onConfirm = std::move(onConfirm);
    opt.onCancel = std::move(onCancel);
    opt.danger = danger;
    showConfirmModal(shell, opt);
}

inline void showConfirmModal(const QString &title,
                             const QString &body,
                             const QString &confirmText,
                             std::function<void()> onConfirm,
                             std::function<void()> onCancel = nullptr,
                             bool danger = false)
{
    ConfirmOptions opt;
    opt.title = title;
    opt.body = body;
    opt.confirmText = confirmText;
    opt.onConfirm = std::move(onConfirm);
    opt.onCancel = std::move(onCancel);
    opt.danger = danger;
    showConfirmModal(opt);
}

} // namespace Rml
} // namespace UI
} // namespace Styx
