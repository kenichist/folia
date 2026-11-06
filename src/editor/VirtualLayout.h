// Virtualized document layout for the native editor.
//
//  * Only blocks that are painted, hit-tested or asked for geometry are laid
//    out (QTextLayout); every other block carries an estimated height.
//  * Per-block heights live in a flat vector indexed by block number; top
//    offsets are a lazily extended prefix sum, so an edit invalidates only
//    the blocks it touched and the prefix after them.
//  * Laid-out blocks far from the viewport are evicted (their glyph data is
//    freed, their measured height kept), so scrolling a 10 MiB note does not
//    accumulate layouts. The caret block and an active IME preedit are pinned.
//  * Role formats (headings, code, math source, inline markup, syntax colors)
//    are applied as QTextLayout additional formats at layout time, so the
//    document itself stays plain source text.
#pragma once

#include "editor/BlockData.h"
#include "editor/EditorContext.h"
#include "editor/MediaService.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTextBlock>
#include <QTimer>
#include <vector>

namespace folia {

class VirtualLayout : public QAbstractTextDocumentLayout {
    Q_OBJECT
public:
    VirtualLayout(QTextDocument *doc, EditorContext *ctx);
    ~VirtualLayout() override;

    void draw(QPainter *painter, const PaintContext &context) override;
    int hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const override;
    int pageCount() const override { return 1; }
    QSizeF documentSize() const override;
    QRectF frameBoundingRect(QTextFrame *frame) const override;
    QRectF blockBoundingRect(const QTextBlock &block) const override;

    // Column geometry (document coordinates).
    qreal columnLeft() const;
    qreal columnWidth() const;
    // Rect of the "Copy" affordance on a code opener line (empty otherwise).
    QRectF codeCopyRect(const QTextBlock &block) const;
    // Rect of the rendered preview of a math close/single line (empty otherwise).
    QRectF previewRect(const QTextBlock &block) const;

    void relayoutAll();
    void relayoutBlock(const QTextBlock &block);
    // Document position of the caret. Inline $…$ formulas are drawn rendered
    // in place except the one the caret touches, which shows its source.
    void setCaret(int position);
    // Viewport (document coordinates) used to decide what to keep laid out.
    void setViewport(qreal top, qreal bottom);
    void setLayoutCap(int maxLaidOutBlocks) { m_cap = maxLaidOutBlocks; }
    int laidOutCount() const { return m_laidOut; }
    int blockCount() const { return int(m_h.size()); }
    qint64 estimatedLayoutBytes() const;
    // Apply a preview result to visible math blocks waiting for it.
    void previewArrived(quint64 hash);
    // Identity of a display formula's source at a given size: a preview is
    // shown only when its hash equals the hash of the block's current source.
    static quint64 previewHash(const QString &source, qreal pointSize);
    QString mathSource(const QTextBlock &closeOrSingle) const;
    qreal previewPointSize() const;

    // Images (docs/ATTACHMENTS.md §3). Display size comes from the stored
    // dimensions (no decode needed for layout): min(column, native) wide.
    QSizeF imageDisplaySize(const QString &hash) const;
    // The drawn object of an Image or Pdf line (the PDF card).
    QRectF imageRect(const QTextBlock &block) const;
    // PDF lines: a fixed-height card; the thumbnail's width follows the
    // stored first-page geometry.
    QSizeF pdfCardSize() const;
    QRectF pdfThumbRect(const QRectF &card, const QString &hash) const;
    // This layout is no longer shown: withdraw its decode requests.
    void releaseMedia();
    // Recompute which bitmaps this layout wants (visible first, then one
    // viewport ahead in the scroll direction). Normally done after a paint.
    void updateMediaWants();
    qreal devicePixelRatio() const { return m_dpr; }
protected:
    void documentChanged(int from, int charsRemoved, int charsAdded) override;
    void resizeInlineObject(QTextInlineObject item, int posInDocument, const QTextFormat &format) override;
    void positionInlineObject(QTextInlineObject item, int posInDocument, const QTextFormat &format) override;
    void drawInlineObject(QPainter *painter, const QRectF &rect, QTextInlineObject object,
                          int posInDocument, const QTextFormat &format) override;

private:
    enum : uint8_t { StLaidOut = 1 };

    void fullInvalidate();
    // A width change on an unchanged document (window, sidebar, the split's
    // divider): only lines already laid out are dropped (at most the cap);
    // every height stays as it was, so a resize step costs the visible
    // lines, not the document. The heights of lines not laid out are
    // estimated again once the width has settled (reestimate), and only if
    // the text column changed width.
    void resizeInvalidate(bool columnChanged);
    void reestimate();
    void layoutBlock(const QTextBlock &block, int number) const;
    float estimateHeight(const QTextBlock &block) const;
    void ensureTops(int upTo) const;
    int blockAtY(qreal y) const;
    qreal topOf(int number) const;
    void setHeight(int number, float h) const;
    void evictFarBlocks(int firstVisible, int lastVisible);
    void applyFormats(const QTextBlock &block, LineRole role, QTextLayout *layout) const;
    void requestPreview(const QTextBlock &block, BlockData *data, LineRole role) const;
    std::shared_ptr<const MathDisplayList> inlineFormula(const QString &source, qreal pointSize, bool *pending) const;
    void inlineFormulaArrived(quint64 key);
    QString mathSourceFor(const QTextBlock &closeOrSingle, LineRole role) const;
    void paintBlockDecorations(QPainter *p, const QTextBlock &block, LineRole role,
                               const QRectF &rect, const PaintContext &ctx) const;
    void drawImageObject(QPainter *p, const QRectF &rect, int pos, const QTextFormat &format);
    void drawPdfObject(QPainter *p, const QRectF &rect, int pos, const QTextFormat &format);
    MediaService::Request pdfThumbRequest(const QString &hash, const QRectF &thumb) const;
    qreal objectHeight(const QTextBlock &block, LineRole role) const;
    void paintImagePlaceholder(QPainter *p, const QRectF &rect, const QString &label) const;
    qreal extraBelow(const QTextBlock &block) const; // import placeholder
    void mediaArrived(const MediaKey &key);
    QFont roleFont(const QTextBlock &block, LineRole role) const;
    qreal indentFor(LineRole role) const;
    qreal topPadFor(const QTextBlock &block, LineRole role) const;
    qreal bottomPadFor(LineRole role) const;

    EditorContext *m_ctx;
    qreal m_width = 0;
    mutable std::vector<float> m_h;
    mutable std::vector<uint8_t> m_st;
    mutable std::vector<double> m_top; // valid for [0, m_topValid)
    mutable int m_topValid = 0;
    mutable double m_sumH = 0;
    mutable int m_laidOut = 0;
    int m_cap = 600;
    qreal m_viewTop = 0, m_viewBottom = 0;
    mutable bool m_inPaint = false;
    mutable bool m_heightChangedInPaint = false;
    // Inline formulas: caret position, renders in flight (deduplicated) and
    // a generation bumped per arrival so waiting lines re-probe once.
    int m_caret = -1;
    mutable QSet<quint64> m_inlineRequested;
    mutable quint32 m_inlineGen = 0;
    // Main-thread render time still allowed in this paint (µs): formulas are
    // drawn in the paint that first needs them instead of a second one.
    mutable qint64 m_inlineBudgetUs = 0;
    QTimer m_inlineRepaint;
    // Images: device pixel ratio of the last paint, whether the wanted set
    // must be recomputed, scroll direction (prefetch goes that way) and the
    // resize settle (decodes at a new width wait until it stops changing).
    qreal m_dpr = 1;
    bool m_mediaDirty = true;
    int m_scrollDir = 1;
    QElapsedTimer m_widthChanged;
    QTimer m_mediaSettle;
    QTimer m_widthSettle;         // re-estimates heights once a resize stops
    bool m_estimatesStale = false; // heights of lines not laid out are for an older width
};

} // namespace folia
