#include "shell/CommandRegistry.h"

#include "core/Instrument.h"

namespace folia {

void CommandRegistry::add(CommandSpec spec)
{
    if (!m_specs.contains(spec.name))
        m_order << spec.name;
    m_specs.insert(spec.name, std::move(spec));
}

const CommandSpec *CommandRegistry::spec(const QString &name) const
{
    auto it = m_specs.find(name);
    return it == m_specs.end() ? nullptr : &*it;
}

QStringList CommandRegistry::names() const
{
    return m_order;
}

CommandResult CommandRegistry::validate(const QString &name, const QVariantMap &args) const
{
    CommandResult r;
    const CommandSpec *s = spec(name);
    if (!s) {
        r.error = QStringLiteral("unknown command '%1'").arg(name);
        return r;
    }
    for (auto it = args.cbegin(); it != args.cend(); ++it) {
        const ArgSpec *a = nullptr;
        for (const ArgSpec &x : s->args)
            if (x.name == it.key())
                a = &x;
        if (!a) {
            r.error = QStringLiteral("%1: unknown argument '%2'").arg(name, it.key());
            return r;
        }
        const QVariant &v = it.value();
        bool ok = true;
        switch (a->type) {
        case QMetaType::LongLong:
        case QMetaType::Int: {
            const int t = v.typeId();
            ok = t == QMetaType::LongLong || t == QMetaType::Int || t == QMetaType::Double
                || t == QMetaType::ULongLong || t == QMetaType::UInt;
            if (ok && t == QMetaType::Double)
                ok = v.toDouble() == double(v.toLongLong());
            break;
        }
        case QMetaType::Bool:
            ok = v.typeId() == QMetaType::Bool;
            break;
        case QMetaType::Double:
            ok = v.canConvert<double>() && v.typeId() != QMetaType::QString;
            break;
        default:
            ok = v.typeId() == QMetaType::QString;
            break;
        }
        if (!ok) {
            r.error = QStringLiteral("%1: argument '%2' has the wrong type").arg(name, it.key());
            return r;
        }
    }
    for (const ArgSpec &a : s->args) {
        if (a.required && !args.contains(a.name)) {
            r.error = QStringLiteral("%1: missing argument '%2'").arg(name, a.name);
            return r;
        }
    }
    if (s->enabled && !s->enabled()) {
        r.error = QStringLiteral("%1 is not available right now").arg(name);
        return r;
    }
    r.ok = true;
    return r;
}

CommandResult CommandRegistry::dispatchCommand(const QString &name, const QVariantMap &args)
{
    CommandResult r = validate(name, args);
    if (!r.ok) {
        emit dispatched(name, false, r.error);
        return r;
    }
    if (m_depth > 16) { // a composed action re-entering itself
        r.ok = false;
        r.error = QStringLiteral("%1: command nesting too deep").arg(name);
        emit dispatched(name, false, r.error);
        return r;
    }
    ++m_depth;
    instr::Scope scope("command_us");
    r = m_specs[name].run(args);
    --m_depth;
    emit dispatched(name, r.ok, r.error);
    return r;
}

} // namespace folia
