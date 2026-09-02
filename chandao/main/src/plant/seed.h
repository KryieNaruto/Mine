#pragma once

#include <QString>
#include <QVector>

// 种子描述符：后续新增种子只加一条 + 对应评价器。
struct SeedDescriptor {
    QString id;
    QString displayName;
    int timeStageCount = 5; // N 个时间阶段 → N+1 个视觉态，每段 1/(N+1)
};

namespace SeedRegistry {

inline const SeedDescriptor &smallTree()
{
    static const SeedDescriptor k{
        QStringLiteral("small_tree"),
        QStringLiteral("小树种子"),
        5,
    };
    return k;
}

inline QVector<SeedDescriptor> all()
{
    return { smallTree() };
}

inline const SeedDescriptor *find(const QString &id)
{
    if (id.isEmpty() || id == smallTree().id) {
        return &smallTree();
    }
    const auto seeds = all();
    for (const SeedDescriptor &s : seeds) {
        if (s.id == id) {
            // all() 返回临时副本，不能返回其元素指针；已知本期只有小树
            return &smallTree();
        }
    }
    return &smallTree();
}

} // namespace SeedRegistry
