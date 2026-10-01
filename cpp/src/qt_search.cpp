#include "monitor_hub/qt_search.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <utility>
#include <vector>

namespace monitor_hub {
namespace {

QString folded(const QString& value) {
    return value.simplified().toCaseFolded();
}

int field_score(const QString& field, const QString& token) {
    if (field.isEmpty()) return 0;
    if (field == token) return 80;
    if (field.startsWith(token)) return 45;
    if (field.contains(token)) return 20;
    return 0;
}

}  // namespace

std::vector<WorkspaceSearchDocument> search_workspace_documents(
    const std::vector<WorkspaceSearchDocument>& documents,
    const QString& query,
    std::size_t limit) {

    struct ScoredDocument {
        int score = 0;
        std::size_t index = 0;
    };

    const auto trimmed = query.trimmed();
    const auto tokens =
        trimmed.isEmpty()
            ? QStringList{}
            : folded(trimmed).split(
                  QRegularExpression(QStringLiteral("\\s+")),
                  Qt::SkipEmptyParts);

    std::vector<ScoredDocument> matches;
    matches.reserve(documents.size());

    for (std::size_t index = 0; index < documents.size(); ++index) {
        const auto& document = documents[index];

        const auto title = folded(document.title);
        const auto subtitle = folded(document.subtitle);
        const auto project = folded(document.project_label);
        const auto kind = folded(document.kind);
        const auto path = folded(document.path);
        const auto ids = folded(
            document.project_id + QLatin1Char(' ') +
            document.task_id + QLatin1Char(' ') +
            document.issue_id + QLatin1Char(' ') +
            document.event_id);

        int score = 0;
        bool accepted = true;

        for (const auto& token : tokens) {
            const bool present =
                title.contains(token) ||
                subtitle.contains(token) ||
                project.contains(token) ||
                kind.contains(token) ||
                path.contains(token) ||
                ids.contains(token);
            if (!present) {
                accepted = false;
                break;
            }

            score += field_score(title, token);
            score += field_score(project, token) / 2;
            score += field_score(kind, token) / 3;
            score += field_score(ids, token) / 2;
            if (subtitle.contains(token)) score += 8;
            if (path.contains(token)) score += 6;
        }

        if (!accepted) continue;
        if (tokens.isEmpty()) score = 1;

        matches.push_back({score, index});
    }

    std::stable_sort(
        matches.begin(),
        matches.end(),
        [&](const ScoredDocument& lhs, const ScoredDocument& rhs) {
            if (lhs.score != rhs.score) return lhs.score > rhs.score;
            const auto& left = documents[lhs.index];
            const auto& right = documents[rhs.index];
            if (left.order != right.order) return left.order > right.order;
            return left.title.localeAwareCompare(right.title) < 0;
        });

    if (limit > 0 && matches.size() > limit)
        matches.resize(limit);

    std::vector<WorkspaceSearchDocument> result;
    result.reserve(matches.size());
    for (const auto& match : matches)
        result.push_back(documents[match.index]);
    return result;
}

}  // namespace monitor_hub
