#include "import/DocxImport.h"

#include "import/ZipReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <utility>

namespace folia::import {

namespace {

bool isCancelled(const std::atomic<bool> *c)
{
    return c && c->load(std::memory_order_relaxed);
}

// Elements are matched by local name and namespace family, so Transitional
// and Strict OOXML read the same.
enum class Ns { Other, W, M, R, Mc, Wp, A, V };

Ns nsOf(QStringView uri)
{
    if (uri.contains(QLatin1String("wordprocessingml/2006/main")) || uri.contains(QLatin1String("ooxml/wordprocessingml/main")))
        return Ns::W;
    if (uri.contains(QLatin1String("officeDocument/2006/math")) || uri.contains(QLatin1String("ooxml/officeDocument/math")))
        return Ns::M;
    if (uri.contains(QLatin1String("markup-compatibility")))
        return Ns::Mc;
    if (uri.contains(QLatin1String("wordprocessingDrawing")))
        return Ns::Wp;
    if (uri.contains(QLatin1String("drawingml/2006/main")) || uri.contains(QLatin1String("ooxml/drawingml/main")))
        return Ns::A;
    if (uri.contains(QLatin1String("urn:schemas-microsoft-com:vml")))
        return Ns::V;
    if (uri.contains(QLatin1String("officeDocument/2006/relationships")) || uri.contains(QLatin1String("officeDocument/relationships")))
        return Ns::R;
    return Ns::Other;
}

QString attr(const QXmlStreamReader &x, QLatin1StringView local)
{
    for (const QXmlStreamAttribute &a : x.attributes())
        if (a.name() == local)
            return a.value().toString();
    return {};
}

bool onOff(const QXmlStreamReader &x)
{
    const QString v = attr(x, QLatin1StringView("val"));
    return v.isEmpty() || !(v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("off")
                            || v == QLatin1String("none"));
}

QString partPath(const QString &baseDir, QString target)
{
    if (target.startsWith(QLatin1Char('/')))
        return QDir::cleanPath(target.mid(1));
    return QDir::cleanPath(baseDir.isEmpty() ? target : baseDir + QLatin1Char('/') + target);
}

QString relsPathFor(const QString &part)
{
    const int slash = int(part.lastIndexOf(QLatin1Char('/')));
    return slash < 0 ? QStringLiteral("_rels/") + part + QStringLiteral(".rels")
                     : part.left(slash) + QStringLiteral("/_rels/") + part.mid(slash + 1) + QStringLiteral(".rels");
}

// Reads a whole small XML part with `fn(x)` per token; false on a DTD
// (refused: entity tricks) or an XML error.
bool walkSmall(const QByteArray &xml, const std::function<void(QXmlStreamReader &)> &fn, QString *error)
{
    QXmlStreamReader x(xml);
    while (!x.atEnd()) {
        const auto tok = x.readNext();
        if (tok == QXmlStreamReader::DTD) {
            *error = QStringLiteral("it declares a DTD, which Folia does not read");
            return false;
        }
        fn(x);
    }
    if (x.hasError()) {
        *error = QStringLiteral("a part is not valid XML (%1)").arg(x.errorString());
        return false;
    }
    return true;
}

struct Rel {
    QString type;
    QString target;
    bool external = false;
};

bool parseRels(const QByteArray &xml, QHash<QString, Rel> *out, QString *error)
{
    return walkSmall(xml, [out](QXmlStreamReader &x) {
        if (x.isStartElement() && x.name() == QLatin1String("Relationship")) {
            Rel r;
            r.type = attr(x, QLatin1StringView("Type"));
            r.target = attr(x, QLatin1StringView("Target"));
            r.external = attr(x, QLatin1StringView("TargetMode")).compare(QLatin1String("External"), Qt::CaseInsensitive) == 0;
            out->insert(attr(x, QLatin1StringView("Id")), r);
        }
    }, error);
}

struct Style {
    int heading = 0;     // 1..9
    bool title = false;
    bool quote = false;
    bool bold = false;   // character styles (Strong, Emphasis…)
    bool italic = false;
    QString numId;       // paragraph styles that number their paragraphs
    int ilvl = 0;
    QString basedOn;
};

bool parseStyles(const QByteArray &xml, QHash<QString, Style> *out, QString *error)
{
    QString id;
    Style cur;
    bool inPPr = false, inRPr = false;
    const bool ok = walkSmall(xml, [&](QXmlStreamReader &x) {
        const QStringView n = x.name();
        if (x.isStartElement()) {
            if (n == QLatin1String("style")) {
                id = attr(x, QLatin1StringView("styleId"));
                cur = Style();
            } else if (n == QLatin1String("name")) {
                const QString v = attr(x, QLatin1StringView("val")).toLower();
                if (v == QLatin1String("title")) {
                    cur.title = true;
                    cur.heading = 1;
                } else if (v.startsWith(QLatin1String("heading "))) {
                    cur.heading = qBound(1, v.mid(8).toInt(), 9);
                } else if (v == QLatin1String("quote") || v == QLatin1String("intense quote")) {
                    cur.quote = true;
                }
            } else if (n == QLatin1String("basedOn")) {
                cur.basedOn = attr(x, QLatin1StringView("val"));
            } else if (n == QLatin1String("pPr")) {
                inPPr = true;
            } else if (n == QLatin1String("rPr")) {
                inRPr = true;
            } else if (inPPr && n == QLatin1String("outlineLvl")) {
                const int l = attr(x, QLatin1StringView("val")).toInt();
                if (l >= 0 && l < 9 && !cur.heading)
                    cur.heading = l + 1;
            } else if (inPPr && n == QLatin1String("numId")) {
                cur.numId = attr(x, QLatin1StringView("val"));
            } else if (inPPr && n == QLatin1String("ilvl")) {
                cur.ilvl = attr(x, QLatin1StringView("val")).toInt();
            } else if (inRPr && n == QLatin1String("b")) {
                cur.bold = onOff(x);
            } else if (inRPr && n == QLatin1String("i")) {
                cur.italic = onOff(x);
            }
        } else if (x.isEndElement()) {
            if (n == QLatin1String("style") && !id.isEmpty())
                out->insert(id, cur);
            else if (n == QLatin1String("pPr"))
                inPPr = false;
            else if (n == QLatin1String("rPr"))
                inRPr = false;
        }
    }, error);
    // Inherit through basedOn (a few levels).
    for (auto it = out->begin(); it != out->end(); ++it) {
        QString b = it->basedOn;
        for (int depth = 0; depth < 8 && !b.isEmpty(); ++depth) {
            const auto p = out->constFind(b);
            if (p == out->cend())
                break;
            if (!it->heading)
                it->heading = p->heading;
            it->quote = it->quote || p->quote;
            if (it->numId.isEmpty()) {
                it->numId = p->numId;
                it->ilvl = p->ilvl;
            }
            b = p->basedOn;
        }
    }
    return ok;
}

struct Numbering {
    struct Level {
        QString fmt = QStringLiteral("decimal");
        int start = 1;
    };
    QHash<QString, QVector<Level>> abstracts;      // abstractNumId -> 9 levels
    QHash<QString, QString> nums;                  // numId -> abstractNumId
    QHash<QString, QHash<int, int>> startOverride; // numId -> ilvl -> start
    QHash<QString, QVector<int>> counters;         // numId -> current number per level (0: none yet)

    Level level(const QString &numId, int ilvl) const
    {
        const QVector<Level> lv = abstracts.value(nums.value(numId));
        Level l = ilvl >= 0 && ilvl < lv.size() ? lv[ilvl] : Level();
        if (const auto o = startOverride.value(numId); o.contains(ilvl))
            l.start = o.value(ilvl);
        return l;
    }
    // The marker for the next item at (numId, ilvl): "- " or "N. ".
    QString next(const QString &numId, int ilvl)
    {
        ilvl = qBound(0, ilvl, 8);
        const Level l = level(numId, ilvl);
        QVector<int> &c = counters[numId];
        if (c.isEmpty())
            c.fill(0, 9);
        c[ilvl] = c[ilvl] == 0 ? l.start : c[ilvl] + 1;
        for (int k = ilvl + 1; k < 9; ++k)
            c[k] = 0; // a shallower item restarts the deeper levels
        if (l.fmt == QLatin1String("bullet") || l.fmt == QLatin1String("none"))
            return QStringLiteral("- ");
        return QString::number(c[ilvl]) + QStringLiteral(". ");
    }
};

bool parseNumbering(const QByteArray &xml, Numbering *out, QString *error)
{
    QString abstractId, numId;
    int ilvl = -1;
    bool inAbstract = false, inNum = false;
    return walkSmall(xml, [&](QXmlStreamReader &x) {
        const QStringView n = x.name();
        if (x.isStartElement()) {
            if (n == QLatin1String("abstractNum")) {
                inAbstract = true;
                abstractId = attr(x, QLatin1StringView("abstractNumId"));
                out->abstracts[abstractId].resize(9);
            } else if (n == QLatin1String("num")) {
                inNum = true;
                numId = attr(x, QLatin1StringView("numId"));
            } else if (n == QLatin1String("lvl") || n == QLatin1String("lvlOverride")) {
                ilvl = attr(x, QLatin1StringView("ilvl")).toInt();
            } else if (inAbstract && ilvl >= 0 && ilvl < 9 && n == QLatin1String("start")) {
                out->abstracts[abstractId][ilvl].start = attr(x, QLatin1StringView("val")).toInt();
            } else if (inAbstract && ilvl >= 0 && ilvl < 9 && n == QLatin1String("numFmt")) {
                out->abstracts[abstractId][ilvl].fmt = attr(x, QLatin1StringView("val"));
            } else if (inNum && n == QLatin1String("abstractNumId")) {
                out->nums.insert(numId, attr(x, QLatin1StringView("val")));
            } else if (inNum && n == QLatin1String("startOverride")) {
                out->startOverride[numId].insert(ilvl, attr(x, QLatin1StringView("val")).toInt());
            }
        } else if (x.isEndElement()) {
            if (n == QLatin1String("abstractNum"))
                inAbstract = false;
            else if (n == QLatin1String("num"))
                inNum = false;
            else if (n == QLatin1String("lvl") || n == QLatin1String("lvlOverride"))
                ilvl = -1;
        }
    }, error);
}

// ------------------------------------------------------------- the body ----
struct Seg {
    QString text;
    bool bold = false;
    bool italic = false;
    QString link;    // external URL
    int image = -1;  // an image placeholder (index into DocxBody::m_images)
};

// Escapes characters that would pair up into markup the editor renders
// ($…$ math, `code`, *emphasis*), only where they would pair: a lone "$5"
// stays as typed.
QString escapePairs(const QString &raw, const QString &t)
{
    QString out = t;
    for (const QChar c : {QLatin1Char('$'), QLatin1Char('`'), QLatin1Char('*')}) {
        if (raw.count(c) >= 2)
            out.replace(c, QString(QLatin1Char('\\')) + c);
    }
    return out;
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber();
}

class DocxBody {
public:
    struct PendingImage {
        int block = -1;
        QString part;
        QString alt;
    };

    DocxBody(ImportedDoc *doc, DocBuilder *b, const QHash<QString, Rel> &rels, const QString &baseDir,
             const QHash<QString, Style> &styles, Numbering *numbering, const DocxLimits &lim)
        : m_doc(doc), m_b(b), m_rels(rels), m_baseDir(baseDir), m_styles(styles), m_num(numbering), m_lim(lim)
    {
    }

    QVector<PendingImage> images;
    QString title; // the first Title paragraph

    void token(QXmlStreamReader &x)
    {
        if (x.isStartElement()) {
            ++m_depth;
            if (m_skipAt)
                return;
            start(x);
        } else if (x.isEndElement()) {
            if (m_skipAt) {
                if (m_depth == m_skipAt)
                    m_skipAt = 0;
                --m_depth;
                return;
            }
            end(x);
            --m_depth;
        } else if (x.isCharacters() && !m_skipAt && (m_inText || m_inInstr) && !m_paras.isEmpty()) {
            addText(x.text().toString()); // field codes go to the field, not the text
        }
    }

private:
    struct Para {
        QString style;
        QString numId;
        int ilvl = 0;
        int outline = -1;
        QVector<Seg> segs;
        // Direct formatting, for documents written without heading styles
        // (TextEdit's "Word" files): every run bold, and the largest size.
        bool allBold = true;
        int maxSize = 0; // half-points
    };
    struct Field {
        QString instr;
        bool result = false;
        bool link = false;
    };
    struct Table {
        QStringList row;
        QString cell;
        int rows = 0;
        int span = 1;
        int columns = 0;
    };

    void skip() { m_skipAt = m_depth; }
    bool textAllowed() const
    {
        for (const Field &f : m_fields)
            if (!f.result)
                return false; // a field's code, not its displayed result
        return true;
    }

    void start(QXmlStreamReader &x)
    {
        const Ns ns = nsOf(x.namespaceUri());
        const QStringView n = x.name();
        if (ns == Ns::Mc) {
            if (n == QLatin1String("Fallback"))
                skip(); // the Choice branch was read
            return;
        }
        if (ns == Ns::Wp && n == QLatin1String("docPr")) {
            m_alt = attr(x, QLatin1StringView("descr"));
            return;
        }
        if ((ns == Ns::A && n == QLatin1String("blip")) || (ns == Ns::V && n == QLatin1String("imagedata"))) {
            const QString rid = attr(x, QLatin1StringView(ns == Ns::A ? "embed" : "id"));
            addImage(rid);
            return;
        }
        if (ns == Ns::M) {
            if (n == QLatin1String("t"))
                m_inText = true;
            return;
        }
        if (ns != Ns::W)
            return;
        if (n == QLatin1String("p")) {
            m_paras.push_back(Para());
        } else if (n == QLatin1String("pPr")) {
            m_inPPr = true;
        } else if (m_inPPr && n == QLatin1String("pStyle") && !m_paras.isEmpty()) {
            m_paras.last().style = attr(x, QLatin1StringView("val"));
        } else if (m_inPPr && n == QLatin1String("numId") && !m_paras.isEmpty()) {
            m_paras.last().numId = attr(x, QLatin1StringView("val"));
        } else if (m_inPPr && n == QLatin1String("ilvl") && !m_paras.isEmpty()) {
            m_paras.last().ilvl = attr(x, QLatin1StringView("val")).toInt();
        } else if (m_inPPr && n == QLatin1String("outlineLvl") && !m_paras.isEmpty()) {
            m_paras.last().outline = attr(x, QLatin1StringView("val")).toInt();
        } else if (m_inPPr && n == QLatin1String("rPr")) {
            skip(); // the paragraph mark's formatting, not the text's
        } else if (n == QLatin1String("r")) {
            m_bold = m_italic = false;
            m_size = 0;
        } else if (n == QLatin1String("sz")) {
            m_size = attr(x, QLatin1StringView("val")).toInt();
        } else if (n == QLatin1String("rStyle")) {
            const Style s = m_styles.value(attr(x, QLatin1StringView("val")));
            m_bold = m_bold || s.bold;
            m_italic = m_italic || s.italic;
        } else if (n == QLatin1String("b")) {
            m_bold = onOff(x);
        } else if (n == QLatin1String("i")) {
            m_italic = onOff(x);
        } else if (n == QLatin1String("t")) {
            m_inText = true;
        } else if (n == QLatin1String("tab") && !m_inPPr) {
            addText(QStringLiteral("\t")); // a space, except after a typed bullet or number
        } else if (n == QLatin1String("br") || n == QLatin1String("cr")) {
            const QString type = attr(x, QLatin1StringView("type"));
            if (type != QLatin1String("page") && type != QLatin1String("column"))
                addText(QStringLiteral("\n"));
        } else if (n == QLatin1String("noBreakHyphen")) {
            addText(QStringLiteral("-"));
        } else if (n == QLatin1String("hyperlink")) {
            const Rel r = m_rels.value(attr(x, QLatin1StringView("id")));
            m_linkStack.push_back(r.external ? r.target : QString());
        } else if (n == QLatin1String("fldSimple")) {
            m_linkStack.push_back(hyperlinkTarget(attr(x, QLatin1StringView("instr"))));
        } else if (n == QLatin1String("fldChar")) {
            const QString t = attr(x, QLatin1StringView("fldCharType"));
            if (t == QLatin1String("begin")) {
                m_fields.push_back(Field());
            } else if (t == QLatin1String("separate") && !m_fields.isEmpty()) {
                Field &f = m_fields.last();
                f.result = true;
                const QString url = hyperlinkTarget(f.instr);
                if (!url.isEmpty()) {
                    f.link = true;
                    m_linkStack.push_back(url);
                }
            } else if (t == QLatin1String("end") && !m_fields.isEmpty()) {
                if (m_fields.last().link && !m_linkStack.isEmpty())
                    m_linkStack.removeLast();
                m_fields.removeLast();
            }
        } else if (n == QLatin1String("instrText")) {
            m_inInstr = true;
        } else if (n == QLatin1String("del") || n == QLatin1String("moveFrom") || n == QLatin1String("sectPr")
                   || n == QLatin1String("footnoteReference") || n == QLatin1String("endnoteReference")
                   || n == QLatin1String("commentReference") || n == QLatin1String("sym")) {
            skip();
        } else if (n == QLatin1String("tbl")) {
            m_tables.push_back(Table());
        } else if (n == QLatin1String("tc") && !m_tables.isEmpty() && m_tables.size() == 1) {
            m_tables.last().cell.clear();
            m_tables.last().span = 1;
        } else if (n == QLatin1String("gridSpan") && m_tables.size() == 1) {
            m_tables.last().span = qBound(1, attr(x, QLatin1StringView("val")).toInt(), m_lim.maxTableColumns);
        }
    }

    void end(QXmlStreamReader &x)
    {
        const Ns ns = nsOf(x.namespaceUri());
        const QStringView n = x.name();
        if (ns == Ns::M && n == QLatin1String("t")) {
            m_inText = false;
            return;
        }
        if (ns != Ns::W)
            return;
        if (n == QLatin1String("t")) {
            m_inText = false;
        } else if (n == QLatin1String("instrText")) {
            m_inInstr = false;
        } else if (n == QLatin1String("pPr")) {
            m_inPPr = false;
        } else if (n == QLatin1String("hyperlink") || n == QLatin1String("fldSimple")) {
            if (!m_linkStack.isEmpty())
                m_linkStack.removeLast();
        } else if (n == QLatin1String("p") && !m_paras.isEmpty()) {
            Para p = m_paras.takeLast();
            finishParagraph(p);
            if (m_paras.isEmpty()) {
                // Text boxes inside the paragraph come after it.
                const QVector<Para> deferred = std::exchange(m_deferred, {});
                for (const Para &d : deferred)
                    emitParagraph(d);
            }
        } else if (n == QLatin1String("tc") && m_tables.size() == 1) {
            Table &t = m_tables.last();
            for (int s = 0; s < t.span; ++s) {
                if (t.row.size() >= m_lim.maxTableColumns)
                    break;
                t.row << (s == 0 ? t.cell : QString());
            }
            t.cell.clear();
        } else if (n == QLatin1String("tr") && m_tables.size() == 1) {
            Table &t = m_tables.last();
            if (!t.row.isEmpty()) {
                m_b->text(QStringLiteral("| ") + t.row.join(QStringLiteral(" | ")) + QStringLiteral(" |"));
                if (t.rows == 0) {
                    QStringList dash;
                    for (int c = 0; c < t.row.size(); ++c)
                        dash << QStringLiteral("---");
                    m_b->text(QStringLiteral("|") + dash.join(QLatin1Char('|')) + QStringLiteral("|"));
                }
                ++t.rows;
            }
            t.row.clear();
            flushTableImages();
        } else if (n == QLatin1String("tbl") && !m_tables.isEmpty()) {
            m_tables.removeLast();
        }
    }

    static QString hyperlinkTarget(const QString &instr)
    {
        const QString t = instr.trimmed();
        if (!t.startsWith(QLatin1String("HYPERLINK"), Qt::CaseInsensitive))
            return {};
        // HYPERLINK "url" [\switches]; \l "anchor" alone is internal.
        const int q1 = int(t.indexOf(QLatin1Char('"')));
        const int q2 = q1 < 0 ? -1 : int(t.indexOf(QLatin1Char('"'), q1 + 1));
        if (q1 < 0 || q2 < 0 || t.mid(9, q1 - 9).contains(QLatin1String("\\l")))
            return {};
        return t.mid(q1 + 1, q2 - q1 - 1);
    }

    void addText(const QString &s)
    {
        if (m_paras.isEmpty())
            return;
        if (m_inInstr) {
            if (!m_fields.isEmpty() && !m_fields.last().result)
                m_fields.last().instr += s;
            return;
        }
        if (!textAllowed())
            return;
        Para &para = m_paras.last();
        if (!s.trimmed().isEmpty()) {
            para.allBold = para.allBold && m_bold;
            para.maxSize = qMax(para.maxSize, m_size);
        }
        QVector<Seg> &segs = para.segs;
        const QString link = m_linkStack.isEmpty() ? QString() : m_linkStack.last();
        if (!segs.isEmpty() && segs.last().image < 0 && segs.last().bold == m_bold && segs.last().italic == m_italic
            && segs.last().link == link) {
            segs.last().text += s;
        } else {
            Seg g;
            g.text = s;
            g.bold = m_bold;
            g.italic = m_italic;
            g.link = link;
            segs.push_back(g);
        }
    }

    void addImage(const QString &rid)
    {
        const Rel r = m_rels.value(rid);
        if (r.target.isEmpty() || r.external || m_paras.isEmpty())
            return;
        Seg g;
        g.image = int(m_imageRefs.size());
        m_imageRefs.push_back({-1, partPath(m_baseDir, r.target), m_alt});
        m_alt.clear();
        m_paras.last().segs.push_back(g);
    }

    void finishParagraph(const Para &p)
    {
        if (!m_paras.isEmpty() && m_tables.isEmpty()) {
            m_deferred.push_back(p); // a text box inside a paragraph
            return;
        }
        if (!m_tables.isEmpty()) {
            // Cell text: the paragraphs joined; images after the row.
            const QString t = render(p.segs, false).trimmed().replace(QLatin1Char('|'), QStringLiteral("\\|"))
                                  .replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' '));
            Table &tb = m_tables.first(); // nested tables flatten into the outer cell
            if (!t.isEmpty()) {
                if (!tb.cell.isEmpty())
                    tb.cell += QLatin1Char(' ');
                tb.cell += t;
                if (tb.cell.size() > m_lim.maxCellChars)
                    tb.cell = tb.cell.left(m_lim.maxCellChars - 1) + QChar(0x2026);
            }
            for (const Seg &g : p.segs)
                if (g.image >= 0)
                    m_tableImages << g.image;
            return;
        }
        emitParagraph(p);
    }

    void flushTableImages()
    {
        for (int i : std::as_const(m_tableImages))
            placeImage(i);
        m_tableImages.clear();
    }

    void placeImage(int ref)
    {
        const int before = int(m_doc->blocks.size());
        m_b->text(QStringLiteral("\x01")); // replaced once the image is stored
        if (m_doc->blocks.size() > before) {
            PendingImage pi = m_imageRefs[ref];
            pi.block = before;
            images.push_back(pi);
        }
    }

    void emitParagraph(const Para &p)
    {
        const Style st = m_styles.value(p.style);
        int heading = st.heading;
        if (p.outline >= 0 && p.outline < 9)
            heading = p.outline + 1;
        if (st.title && title.isEmpty() && m_doc->blocks.isEmpty()) {
            title = render(p.segs, true).replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' ')).trimmed();
            if (!title.isEmpty())
                return;
        }
        QString prefix;
        const QString numId = !p.numId.isEmpty() ? p.numId : st.numId;
        const int ilvl = !p.numId.isEmpty() ? p.ilvl : st.ilvl;
        if (heading == 0 && p.style.isEmpty() && numId.isEmpty() && p.allBold && p.maxSize >= 28) {
            // No heading style, but set like one: short, bold, 14 pt or more.
            QString plain;
            for (const Seg &g : p.segs)
                plain += g.text;
            if (plain.trimmed().size() <= 200 && !plain.contains(QLatin1Char('\n')))
                heading = p.maxSize >= 40 ? 1 : p.maxSize >= 32 ? 2 : 3;
        }
        if (heading > 0) {
            prefix = QString(qMin(heading, 3), QLatin1Char('#')) + QLatin1Char(' ');
        } else if (!numId.isEmpty() && numId != QLatin1String("0") && m_num->nums.contains(numId)) {
            prefix = QString(2 * qBound(0, ilvl, 8), QLatin1Char(' ')) + m_num->next(numId, ilvl);
        } else if (st.quote) {
            prefix = QStringLiteral("> ");
        }
        // Text, split around images (each image is a line of its own).
        QVector<Seg> run;
        bool first = true;
        auto flushText = [&] {
            QString t = render(run, heading > 0);
            run.clear();
            const QStringList lines = t.split(QLatin1Char('\n'));
            for (int k = 0; k < lines.size(); ++k) {
                QString line = lines[k].trimmed();
                if (line.isEmpty())
                    continue;
                bool typed = false;
                if (first && prefix.isEmpty())
                    line = typedListItem(line, &typed);
                line.replace(QLatin1Char('\t'), QLatin1Char(' '));
                if (first && !prefix.isEmpty())
                    line = prefix + line;
                else if (!typed)
                    line = guardLineStart(line); // plain text that starts like markup stays text
                first = false;
                m_b->text(line);
            }
        };
        for (const Seg &g : p.segs) {
            if (g.image >= 0) {
                flushText();
                placeImage(g.image);
                continue;
            }
            run.push_back(g);
        }
        flushText();
    }

    // Bullets and numbers typed as text before a tab (TextEdit's "Word"
    // files): "•\tx" becomes "- x", "◦\tx" "  - x", "3\tx" "3. x".
    static QString typedListItem(const QString &line, bool *typed)
    {
        static const QRegularExpression bullet(
            QStringLiteral("^([\u2022\u25CF\u25AA\u25A0\u2023\u2043\u00B7\u25E6\u25CB\u25AB\u25A1])\\t+"));
        static const QRegularExpression number(QStringLiteral("^(\\d{1,3})[.)]?\\t+"));
        if (const QRegularExpressionMatch m = bullet.match(line); m.hasMatch()) {
            const QChar c = m.captured(1).front();
            const bool nested = c == QChar(0x25E6) || c == QChar(0x25CB) || c == QChar(0x25AB) || c == QChar(0x25A1);
            *typed = true;
            return (nested ? QStringLiteral("  - ") : QStringLiteral("- ")) + line.mid(m.capturedLength());
        }
        if (const QRegularExpressionMatch m = number.match(line); m.hasMatch()) {
            *typed = true;
            return m.captured(1) + QStringLiteral(". ") + line.mid(m.capturedLength());
        }
        return line;
    }

    // A plain line that starts like markup the editor would read as such.
    static QString guardLineStart(const QString &line)
    {
        static const QStringList starts = {QStringLiteral("#"), QStringLiteral("- "), QStringLiteral("* "),
                                           QStringLiteral("+ "), QStringLiteral("> "), QStringLiteral("```"),
                                           QStringLiteral("$$")};
        for (const QString &s : starts)
            if (line.startsWith(s))
                return QLatin1Char('\\') + line;
        return line;
    }

    // Markdown for a run of segments: adjacent runs with the same format
    // merge; bold+italic is bold (the editor does not nest); italic needs
    // word boundaries to render, so mid-word italic is dropped.
    QString render(const QVector<Seg> &segs, bool plain)
    {
        QString raw;
        for (const Seg &g : segs)
            raw += g.text;
        QVector<Seg> m;
        for (const Seg &g : segs) {
            if (g.image >= 0)
                continue;
            if (!m.isEmpty() && m.last().bold == g.bold && m.last().italic == g.italic && m.last().link == g.link)
                m.last().text += g.text;
            else
                m.push_back(g);
        }
        QString out;
        for (int k = 0; k < m.size(); ++k) {
            const Seg &g = m[k];
            const QString t = escapePairs(raw, g.text);
            if (t.isEmpty())
                continue;
            if (!g.link.isEmpty()) {
                const QString label = t.trimmed();
                out += label.isEmpty() ? t : QLatin1Char('[') + label + QStringLiteral("](") + g.link + QLatin1Char(')');
                continue;
            }
            if (plain || (!g.bold && !g.italic)) {
                out += t;
                continue;
            }
            const QString core = t.trimmed();
            if (core.isEmpty() || core.contains(QLatin1Char('\n'))) {
                out += t;
                continue;
            }
            const QChar before = out.isEmpty() ? QChar() : out.back();
            const QChar after = k + 1 < m.size() && !m[k + 1].text.isEmpty() ? m[k + 1].text.front() : QChar();
            const bool midWord = (!t.front().isSpace() && isWordChar(before)) || (!t.back().isSpace() && isWordChar(after));
            if (!g.bold && midWord) {
                out += t;
                continue;
            }
            const QString mark = g.bold ? QStringLiteral("**") : QStringLiteral("*");
            const int lead = int(t.indexOf(core));
            out += t.left(lead) + mark + core + mark + t.mid(lead + core.size());
        }
        return out;
    }

    ImportedDoc *m_doc;
    DocBuilder *m_b;
    const QHash<QString, Rel> &m_rels;
    QString m_baseDir;
    const QHash<QString, Style> &m_styles;
    Numbering *m_num;
    DocxLimits m_lim;

    int m_depth = 0;
    int m_skipAt = 0;
    bool m_inPPr = false;
    bool m_inText = false;
    bool m_inInstr = false;
    bool m_bold = false;
    bool m_italic = false;
    int m_size = 0;
    QString m_alt;
    QVector<Para> m_paras;
    QVector<Para> m_deferred;
    QVector<Field> m_fields;
    QStringList m_linkStack;
    QVector<Table> m_tables;
    QVector<int> m_tableImages;
    QVector<PendingImage> m_imageRefs;
};

bool isImagePart(const QString &part)
{
    static const QSet<QString> ok = {QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                     QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("tif"),
                                     QStringLiteral("tiff"), QStringLiteral("webp"), QStringLiteral("heic"),
                                     QStringLiteral("heif")};
    return ok.contains(QFileInfo(part).suffix().toLower());
}

} // namespace

ImportedDoc importDocx(const QString &path, const DocxImageImporter &images, const std::atomic<bool> *cancel,
                       const Limits &limits, const DocxLimits &lim)
{
    ImportedDoc doc;
    const QFileInfo fi(path);
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && f.peek(4) == QByteArray("\xD0\xCF\x11\xE0", 4)) {
            doc.error = QStringLiteral("it is a password-protected document or an old .doc file, which Folia cannot read");
            return doc;
        }
    }
    ZipReader zip;
    QString err;
    if (!zip.open(path, &err)) {
        doc.error = err;
        return doc;
    }
    // The main part, and the document properties, from the package's rels.
    QString main = QStringLiteral("word/document.xml");
    QString core;
    {
        QHash<QString, Rel> pkg;
        const int i = zip.find(QStringLiteral("_rels/.rels"));
        if (i >= 0 && !parseRels(zip.readAll(i, lim.maxRels, &err), &pkg, &err)) {
            doc.error = err;
            return doc;
        }
        for (const Rel &r : std::as_const(pkg)) {
            if (r.type.endsWith(QLatin1String("/officeDocument")))
                main = partPath(QString(), r.target);
            else if (r.type.endsWith(QLatin1String("/core-properties")))
                core = partPath(QString(), r.target);
        }
    }
    const int mainIndex = zip.find(main);
    if (mainIndex < 0) {
        doc.error = QStringLiteral("it is not a Word document (no %1)").arg(main);
        return doc;
    }
    const QString baseDir = QFileInfo(main).path() == QLatin1String(".") ? QString() : QFileInfo(main).path();
    QHash<QString, Rel> rels;
    if (const int i = zip.find(relsPathFor(main)); i >= 0 && !parseRels(zip.readAll(i, lim.maxRels, &err), &rels, &err)) {
        doc.error = err;
        return doc;
    }
    QHash<QString, Style> styles;
    Numbering numbering;
    for (const Rel &r : std::as_const(rels)) {
        if (r.external)
            continue;
        const int i = zip.find(partPath(baseDir, r.target));
        if (i < 0)
            continue;
        bool ok = true;
        if (r.type.endsWith(QLatin1String("/styles")))
            ok = parseStyles(zip.readAll(i, lim.maxSmallPart, &err), &styles, &err);
        else if (r.type.endsWith(QLatin1String("/numbering")))
            ok = parseNumbering(zip.readAll(i, lim.maxSmallPart, &err), &numbering, &err);
        if (!ok) {
            doc.error = err;
            return doc;
        }
    }

    // The body, streamed from the inflater into the XML reader.
    DocBuilder b(&doc, limits);
    DocxBody body(&doc, &b, rels, baseDir, styles, &numbering, lim);
    std::unique_ptr<QIODevice> dev = zip.openEntry(mainIndex, lim.maxDocumentXml);
    if (!dev) {
        doc.error = QStringLiteral("the archive is damaged");
        return doc;
    }
    QXmlStreamReader x;
    QByteArray buf(64 * 1024, Qt::Uninitialized);
    bool eof = false;
    auto feed = [&]() -> bool {
        const qint64 n = dev->read(buf.data(), buf.size());
        if (n < 0) {
            doc.error = dev->errorString();
            return false;
        }
        if (n == 0)
            eof = true;
        else
            x.addData(QByteArray(buf.constData(), int(n)));
        return true;
    };
    if (!feed())
        return doc;
    for (int count = 0;; ++count) {
        const QXmlStreamReader::TokenType tok = x.readNext();
        if (tok == QXmlStreamReader::Invalid) {
            if (x.error() == QXmlStreamReader::PrematureEndOfDocumentError && !eof) {
                if (!feed())
                    return doc;
                continue;
            }
            doc.error = QStringLiteral("the document is damaged (%1)").arg(x.errorString());
            return doc;
        }
        if (tok == QXmlStreamReader::DTD) {
            doc.error = QStringLiteral("it declares a DTD, which Folia does not read");
            return doc;
        }
        if (tok == QXmlStreamReader::EndDocument || b.full())
            break;
        body.token(x);
        if ((count & 4095) == 0 && isCancelled(cancel)) {
            doc.error = QStringLiteral("cancelled");
            return doc;
        }
    }
    dev.reset(); // one inflater at a time

    // Images, after the text: each part stored once, streamed to disk.
    QHash<QString, QString> stored;
    int skipped = 0;
    for (const DocxBody::PendingImage &pi : std::as_const(body.images)) {
        if (isCancelled(cancel)) {
            doc.error = QStringLiteral("cancelled");
            return doc;
        }
        QString line;
        if (auto it = stored.constFind(pi.part); it != stored.cend()) {
            line = *it;
        } else {
            const int i = zip.find(pi.part);
            if (i >= 0 && isImagePart(pi.part) && images && int(stored.size()) < limits.maxImages) {
                std::unique_ptr<QIODevice> img = zip.openEntry(i, lim.maxImageBytes);
                if (img)
                    line = images(img.get(), QFileInfo(pi.part).fileName(), pi.alt);
            }
            stored.insert(pi.part, line);
            if (line.isEmpty())
                ++skipped;
        }
        if (pi.block >= 0 && pi.block < doc.blocks.size())
            doc.blocks[pi.block].text = line; // empty: removed below
    }
    doc.blocks.erase(std::remove_if(doc.blocks.begin(), doc.blocks.end(),
                                    [](const BlockRecord &r) {
                                        return r.kind == BlockKind::Text
                                            && (r.text.isEmpty() || r.text == QLatin1String("\x01"));
                                    }),
                     doc.blocks.end());
    if (skipped > 0)
        doc.notes << (skipped == 1 ? QStringLiteral("1 image could not be added (only pictures such as PNG and JPEG are).")
                                   : QStringLiteral("%1 images could not be added (only pictures such as PNG and JPEG are).")
                                         .arg(skipped));

    // Title: the Title paragraph, else the document's properties, else the file name.
    doc.title = body.title;
    if (doc.title.isEmpty() && !core.isEmpty()) {
        if (const int i = zip.find(core); i >= 0) {
            bool inTitle = false;
            QString t;
            QString ignored;
            walkSmall(zip.readAll(i, lim.maxSmallPart, &ignored), [&](QXmlStreamReader &cx) {
                if (cx.isStartElement() && cx.name() == QLatin1String("title"))
                    inTitle = true;
                else if (cx.isEndElement() && cx.name() == QLatin1String("title"))
                    inTitle = false;
                else if (inTitle && cx.isCharacters())
                    t += cx.text();
            }, &ignored);
            doc.title = t.trimmed();
        }
    }
    // Else a leading "# " heading, as for Markdown, else the file name.
    if (doc.title.isEmpty() && !doc.blocks.isEmpty() && doc.blocks.first().kind == BlockKind::Text
        && doc.blocks.first().text.startsWith(QLatin1String("# "))) {
        doc.title = doc.blocks.first().text.mid(2).trimmed();
        doc.outputChars -= doc.blocks.first().text.size();
        doc.blocks.removeFirst();
    }
    if (doc.title.isEmpty())
        doc.title = fi.completeBaseName();
    return doc;
}

} // namespace folia::import
