// math_init_race: the very first math::render() calls of a process happen
// concurrently on 8 threads (racing the one-time font resolution and the
// glyph cache); all results must match a later single-threaded render.
#include "MathCorpus.h"
#include "math/MathRenderer.h"

#include <QGuiApplication>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <vector>

using namespace folia;

static bool same(const MathDisplayList &a, const MathDisplayList &b)
{
    if (a.width != b.width || a.ascent != b.ascent || a.descent != b.descent
        || a.runs.size() != b.runs.size() || a.rules.size() != b.rules.size()
        || a.paths.size() != b.paths.size() || a.fonts.size() != b.fonts.size())
        return false;
    for (int i = 0; i < a.runs.size(); ++i)
        if (a.runs[i].pos != b.runs[i].pos || a.runs[i].text != b.runs[i].text
            || a.runs[i].font != b.runs[i].font)
            return false;
    for (int i = 0; i < a.fonts.size(); ++i)
        if (a.fonts[i].family() != b.fonts[i].family() || a.fonts[i].pixelSize() != b.fonts[i].pixelSize())
            return false;
    for (int i = 0; i < a.paths.size(); ++i)
        if (a.paths[i].elementCount() != b.paths[i].elementCount()
            || a.paths[i].boundingRect() != b.paths[i].boundingRect())
            return false;
    return true;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    constexpr int kThreads = 8;
    std::vector<std::vector<math::Result>> got(kThreads);
    std::atomic<int> ready{0};
    std::vector<QThread *> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.push_back(QThread::create([&, t] {
            ready++;
            while (ready.load() < kThreads)
                QThread::yieldCurrentThread();
            for (int i = 0; i < kMathCorpusSize; ++i) {
                const int idx = (i + t * 5) % kMathCorpusSize;
                math::Style st;
                st.display = (idx + t) % 2 == 0;
                got[size_t(t)].push_back(math::render(QString::fromUtf8(kMathCorpus[idx].tex), st));
            }
        }));
        threads.back()->start();
    }
    for (QThread *th : threads) {
        th->wait();
        delete th;
    }
    int bad = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kMathCorpusSize; ++i) {
            const int idx = (i + t * 5) % kMathCorpusSize;
            math::Style st;
            st.display = (idx + t) % 2 == 0;
            const math::Result ref = math::render(QString::fromUtf8(kMathCorpus[idx].tex), st);
            const math::Result &r = got[size_t(t)][size_t(i)];
            if (!ref.ok || !r.ok || !same(ref.list, r.list)) {
                ++bad;
                std::fprintf(stderr, "mismatch thread %d formula %d (%s): ok=%d/%d %s\n", t, idx,
                             kMathCorpus[idx].label, int(r.ok), int(ref.ok), qPrintable(r.error));
            }
        }
    }
    std::printf("math_init_race: %d threads x %d formulas, %d mismatches, font %s\n", kThreads,
                kMathCorpusSize, bad, qPrintable(math::resolvedFontFamily()));
    return bad == 0 ? 0 : 1;
}
