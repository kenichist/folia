// In-app benchmark driver (performance-and-QA). Drives the real app through
// synthetic Qt events and native commands, records raw samples plus process
// counters, writes JSON (schema in docs/BENCHMARKS.md) and quits. Only active
// with --bench; normal runs never construct it.
#pragma once

#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantMap>
#include <functional>

namespace folia {

class AppController;

class BenchDriver : public QObject {
    Q_OBJECT
public:
    BenchDriver(AppController *app, const QString &scenario, const QString &out, const QVariantMap &args);
    void run();

    // Process start -> main() (ms), measured from the kernel's process start time.
    static void markMainEntered();

private:
    using Samples = QVector<double>;
    struct Metric {
        QString unit = QStringLiteral("us");
        QString proxy;
        Samples samples;
    };

    bool waitFor(const std::function<bool()> &pred, int timeoutMs);
    void settle(int ms);
    bool openAndWait(PageId id, double *openUs, bool *cached);
    void typeText(const QString &text, int count, bool ime);
    void add(const QString &metric, double v, const QString &unit = QStringLiteral("us"), const QString &proxy = {});
    void addInstrSamples();
    QJsonObject envJson() const;
    QJsonObject processJson() const;
    PageId manifestPage(const QString &key) const;
    QVector<PageId> manifestList(const QString &key) const;

    void scenarioSnapshot();
    void scenarioLaunch();
    void scenarioOpen();
    void scenarioCreate();
    void scenarioExpand();
    void scenarioTyping();
    void scenarioScroll();
    void scenarioStress();
    void scenarioPresentation();
    void scenarioIdle();
    void scenarioNavigate();
    void scenarioIntegration();
    void scenarioRecovery();
    void scenarioSettings();
    void scenarioImages(); // BenchImages.cpp
    void scenarioPdf();    // BenchPdf.cpp
    void scenarioSplit();  // BenchSplit.cpp
    void finish();

    AppController *m_app;
    QString m_scenario;
    QString m_out;
    QVariantMap m_args;
    QJsonObject m_manifest;
    QHash<QString, Metric> m_metrics;
    QJsonObject m_counters;
    QJsonArray m_notes;
    QJsonObject m_extra;
    int m_paints = 0;
    int m_exposureSamples = 0;
    int m_exposedSamples = 0;
    double m_firstPaintProcMs = -1;
    double m_firstPaintMainMs = -1;
    PageId m_firstPaintPage = 0;
};

} // namespace folia
