// Validated command bus shared by native controls, menus, key bindings and
// Lua (ctx.run). Every command declares its arguments; unknown commands,
// unknown arguments, missing required arguments and wrong types are rejected
// before the handler runs.
#pragma once

#include "core/Api.h"

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QStringList>

namespace folia {

struct ArgSpec {
    QString name;
    QMetaType::Type type = QMetaType::QString; // QString, LongLong, Bool, Int, Double
    bool required = false;
};

struct CommandSpec {
    QString name;          // "page.new"
    QString title;         // "New Page"
    QString description;
    QVector<ArgSpec> args;
    std::function<CommandResult(const QVariantMap &)> run;
    std::function<bool()> enabled; // optional
};

class CommandRegistry : public QObject, public ICommandBus {
    Q_OBJECT
public:
    explicit CommandRegistry(QObject *parent = nullptr) : QObject(parent) {}

    void add(CommandSpec spec);
    bool has(const QString &name) const { return m_specs.contains(name); }
    const CommandSpec *spec(const QString &name) const;
    QStringList names() const;

    CommandResult dispatchCommand(const QString &name, const QVariantMap &args) override;
    // Validation only (used by the shell to reject bad bindings at load).
    CommandResult validate(const QString &name, const QVariantMap &args) const;

signals:
    void dispatched(const QString &name, bool ok, const QString &error);

private:
    QHash<QString, CommandSpec> m_specs;
    QStringList m_order;
    int m_depth = 0;
};

} // namespace folia
