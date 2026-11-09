#include "app/DocumentImport.h"

#include "app/Platform.h"
#include "core/Instrument.h"
#include "import/MarkdownImport.h"
#include "storage/AttachmentStore.h"
#include "storage/Store.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>

namespace folia {

namespace {
constexpr qint64 kMaxImageBytes = 256ll << 20;
}

DocumentImporter::DocumentImporter(Store *store, AttachmentStore *attachments, QObject *parent)
    : QObject(parent), m_store(store), m_attachments(attachments)
{
}

void DocumentImporter::import(const QString &path)
{
    ++m_pending;
    auto doc = std::make_shared<import::ImportedDoc>();
    AttachmentStore *as = m_attachments;
    const import::Limits lim = limits;
    m_attachments->runJob([as, path, doc, lim] {
        QElapsedTimer t;
        t.start();
        // Local images: each file stored once, images only, within the limits.
        QHash<QString, QString> seen; // canonical path -> attachment line ("" = left out)
        QVector<AttachmentInfo> stored;
        int skipped = 0;
        auto images = [&](const QString &p, const QString &alt) -> QString {
            const QFileInfo fi(p);
            const QString key = fi.canonicalFilePath();
            if (key.isEmpty() || !fi.isFile())
                return {};
            if (auto it = seen.constFind(key); it != seen.cend())
                return *it;
            QString line;
            if (int(stored.size()) < lim.maxImages && fi.size() <= kMaxImageBytes && !as->cancelled()
                && platform::imageInfo(key).ok) {
                AttachmentStore::Source s;
                s.path = key;
                s.name = alt.isEmpty() ? fi.completeBaseName() : alt;
                const AttachmentStore::ImportResult r = as->importNow(s);
                if (r.ok) {
                    stored.push_back(r.info);
                    line = attachments::imageMarkdown(r.info.hash, s.name);
                }
            }
            if (line.isEmpty())
                ++skipped;
            seen.insert(key, line);
            return line;
        };
        *doc = import::importTextFile(path, images, as->cancelFlag(), lim);
        doc->attachments = stored;
        if (skipped > 0)
            doc->notes << (skipped == 1 ? QStringLiteral("1 image could not be added.")
                                        : QStringLiteral("%1 images could not be added.").arg(skipped));
        instr::record("document_import_worker_us", t.nsecsElapsed() / 1000);
    }, this, [this, path, doc] { finish(path, doc); });
}

void DocumentImporter::finish(const QString &path, const std::shared_ptr<import::ImportedDoc> &doc)
{
    QElapsedTimer t;
    t.start();
    QStringList pins;
    for (const AttachmentInfo &a : std::as_const(doc->attachments))
        pins << a.hash;
    if (!doc->error.isEmpty()) {
        for (const QString &h : std::as_const(pins))
            m_attachments->unpin(h);
        --m_pending;
        if (doc->error != QLatin1String("cancelled"))
            emit failed(QFileInfo(path).fileName(), doc->error);
        if (m_pending == 0)
            emit idle();
        return;
    }
    // Attachment rows go to the writer first, then the page that refers to them.
    for (const AttachmentInfo &a : std::as_const(doc->attachments)) {
        m_attachments->remember(a);
        m_store->putAttachment(a);
    }
    if (!doc->notes.isEmpty()) {
        BlockRecord note;
        note.text = QStringLiteral("> Imported from %1. %2").arg(QFileInfo(path).fileName(), doc->notes.join(QLatin1Char(' ')));
        doc->blocks.prepend(note);
    }
    const QString title = doc->title.trimmed();
    const PageId id = m_store->createPage(0, -1, title);
    SaveBatch b;
    b.page = id;
    b.rev = 1;
    b.hasTitle = true;
    b.title = title;
    b.hasOrder = true;
    b.order.reserve(doc->blocks.size());
    for (BlockRecord &r : doc->blocks) {
        r.id = m_store->allocateBlockId();
        b.order << r.id;
    }
    b.upserts = std::move(doc->blocks);
    m_store->commit(std::move(b));
    instr::record("document_import_main_us", t.nsecsElapsed() / 1000);
    // A no-op behind the save on the writer (jobs run in order): when it is
    // done, the page is on disk.
    m_store->writeJob([](sqlite3 *) { return 0; }, this, [this, id, title, pins] {
        for (const QString &h : pins)
            m_attachments->unpin(h);
        --m_pending;
        emit created(id, title);
        if (m_pending == 0)
            emit idle();
    });
}

} // namespace folia
