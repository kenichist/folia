// Turns uploaded documents into pages (docs/ATTACHMENTS.md §8).
//
//  * Each file is converted on the attachment import worker (one thread,
//    shared with image and PDF imports, in order with them): nothing is read
//    or parsed on the main thread.
//  * Images it references are stored as attachments by that worker (a header
//    check first, so nothing that is not an image is copied) and stay pinned.
//  * On the main thread the page is then created and saved in one batch,
//    behind the attachment rows; `created` is emitted only once that save is
//    on disk, and the pins are released then. So a page never opens before
//    it exists, and a sweep can never remove its images first.
//  * The converted page is not loaded: it opens like any other page, when
//    its tab is shown.
#pragma once

#include "core/Types.h"
#include "import/ImportedDoc.h"

#include <QObject>

#include <memory>

namespace folia {

class AttachmentStore;
class Store;

class DocumentImporter : public QObject {
    Q_OBJECT
public:
    DocumentImporter(Store *store, AttachmentStore *attachments, QObject *parent = nullptr);

    // A new top-level page from `path` (see import::isDocumentFile()).
    void import(const QString &path);
    int pending() const { return m_pending; }
    import::Limits limits; // lowered by tests and the bench

signals:
    void created(folia::PageId id, const QString &title);
    void failed(const QString &fileName, const QString &error);
    void idle(); // nothing pending any more

private:
    void finish(const QString &path, const std::shared_ptr<import::ImportedDoc> &doc);

    Store *m_store;
    AttachmentStore *m_attachments;
    int m_pending = 0;
};

} // namespace folia
