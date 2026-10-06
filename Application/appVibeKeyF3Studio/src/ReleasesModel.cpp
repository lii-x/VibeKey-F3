#include "ReleasesModel.h"

ReleasesModel::ReleasesModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ReleasesModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant ReleasesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const QVariantMap &m = m_items.at(index.row());
    switch (role) {
    case TagRole:  return m.value(QStringLiteral("tag"));
    case NameRole: return m.value(QStringLiteral("name"));
    case DateRole: return m.value(QStringLiteral("date"));
    case BodyRole: return m.value(QStringLiteral("body"));
    case FileRole: return m.value(QStringLiteral("file"));
    case UrlRole:  return m.value(QStringLiteral("url"));
    case SizeRole: return m.value(QStringLiteral("size"));
    default:       return {};
    }
}

QHash<int, QByteArray> ReleasesModel::roleNames() const
{
    return {
        { TagRole,  "tag"  },
        { NameRole, "name" },
        { DateRole, "date" },
        { BodyRole, "body" },
        { FileRole, "file" },
        { UrlRole,  "url"  },
        { SizeRole, "size" },
    };
}

void ReleasesModel::setReleases(const QList<QVariantMap> &releases)
{
    beginResetModel();
    m_items = releases;
    endResetModel();
    emit countChanged();
}

QVariantMap ReleasesModel::at(int index) const
{
    if (index < 0 || index >= m_items.size())
        return {};
    return m_items.at(index);
}
