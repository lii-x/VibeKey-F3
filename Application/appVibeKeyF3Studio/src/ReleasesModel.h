#ifndef RELEASESMODEL_H
#define RELEASESMODEL_H

/* ReleasesModel —— 把 Gitee release 列表作为 QAbstractListModel 暴露给 QML。
 * 用模型(而非 QVariantList)作 ListView 数据源, 可避开 Qt 6.11 在
 * qarraydataops.h:45 (QPodArrayOps::copyAppend) 关于"共享数据上 range append"
 * 的 Debug 断言 —— QVariantList + ListView 在模型重置时反复 detach/共享, 是
 * 已知的边缘场景。
 */
#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QVariant>
#include <QVariantMap>
#include <QByteArray>

class ReleasesModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)   /* 属性而非 Q_INVOKABLE: 避免 QML 在基类指针上方法分发的边缘问题 */
public:
    enum Roles {
        TagRole = Qt::UserRole + 1,
        NameRole,
        DateRole,
        BodyRole,
        FileRole,
        UrlRole,
        SizeRole,
    };
    Q_ENUM(Roles)

    explicit ReleasesModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /* 整体替换内容(beginResetModel/endResetModel 触发 ListView 重建) */
    void setReleases(const QList<QVariantMap> &releases);

    /* QML 下载入口要按 index 取整条数据, 暴露 at() */
    Q_INVOKABLE QVariantMap at(int index) const;

    int count() const { return m_items.size(); }

signals:
    void countChanged();

private:
    QList<QVariantMap> m_items;
};

#endif
