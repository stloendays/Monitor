#pragma once

#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <vector>

namespace monitor_hub {

struct WorkspaceSearchDocument {
    QString kind;
    QString title;
    QString subtitle;
    QString project_label;
    QString project_id;
    QString task_id;
    QString issue_id;
    QString event_id;
    QString path;
    int tab_index = -1;
    qint64 order = 0;
};

std::vector<WorkspaceSearchDocument> search_workspace_documents(
    const std::vector<WorkspaceSearchDocument>& documents,
    const QString& query,
    std::size_t limit = 80);

}  // namespace monitor_hub
