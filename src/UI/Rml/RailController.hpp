#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>
#include <QString>
#include <QTimer>

namespace Acheron {
namespace UI {
namespace Rml {

class RailController final : public Controller
{
    Q_OBJECT
public:
    RailController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);

private slots:
    void rebuild();

private:
    void renderRail();
    QString renderFolderGrid(Core::Snowflake accountId, Core::Snowflake folderId);
    static QString acronym(const QString &name);

    QTimer rebuildTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Acheron
