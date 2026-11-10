#include "import/ZipReader.h"

#include <QFile>

#include <miniz.h>

#include <cstring>

namespace folia::import {

struct ZipReader::Impl {
    QFile file;
    mz_zip_archive zip;
    bool open = false;

    Impl() { std::memset(&zip, 0, sizeof(zip)); }
    ~Impl()
    {
        if (open)
            mz_zip_reader_end(&zip);
    }

    static size_t read(void *opaque, mz_uint64 ofs, void *buf, size_t n)
    {
        auto *f = static_cast<QFile *>(opaque);
        if (!f->seek(qint64(ofs)))
            return 0;
        const qint64 got = f->read(static_cast<char *>(buf), qint64(n));
        return got < 0 ? 0 : size_t(got);
    }
};

namespace {

class EntryDevice : public QIODevice {
public:
    EntryDevice(mz_zip_archive *zip, int index, qint64 maxBytes) : m_max(maxBytes)
    {
        m_iter = mz_zip_reader_extract_iter_new(zip, mz_uint(index), 0);
        if (m_iter)
            QIODevice::open(QIODevice::ReadOnly);
        else
            setErrorString(QString::fromLatin1(mz_zip_get_error_string(mz_zip_peek_last_error(zip))));
    }
    ~EntryDevice() override
    {
        if (m_iter)
            mz_zip_reader_extract_iter_free(m_iter);
    }
    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxlen) override
    {
        if (!m_iter || m_failed)
            return -1;
        const size_t n = mz_zip_reader_extract_iter_read(m_iter, data, size_t(maxlen));
        if (n == 0 && m_iter->status < 0) {
            m_failed = true;
            setErrorString(QStringLiteral("the archive is damaged"));
            return -1;
        }
        m_total += qint64(n);
        if (m_total > m_max) {
            m_failed = true;
            setErrorString(QStringLiteral("a part of the archive is larger than %1 MB").arg(m_max >> 20));
            return -1;
        }
        return qint64(n);
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    mz_zip_reader_extract_iter_state *m_iter = nullptr;
    qint64 m_max;
    qint64 m_total = 0;
    bool m_failed = false;
};

} // namespace

ZipReader::ZipReader() : d(std::make_unique<Impl>()) {}

ZipReader::~ZipReader() = default;

bool ZipReader::open(const QString &path, QString *error)
{
    d->file.setFileName(path);
    if (!d->file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = d->file.errorString();
        return false;
    }
    d->zip.m_pRead = &Impl::read;
    d->zip.m_pIO_opaque = &d->file;
    if (!mz_zip_reader_init(&d->zip, mz_uint64(d->file.size()), 0)) {
        if (error)
            *error = QStringLiteral("not a readable zip archive (%1)")
                         .arg(QString::fromLatin1(mz_zip_get_error_string(mz_zip_peek_last_error(&d->zip))));
        return false;
    }
    d->open = true;
    if (entryCount() > kMaxEntries) {
        if (error)
            *error = QStringLiteral("the archive has more than %1 parts").arg(kMaxEntries);
        return false;
    }
    return true;
}

int ZipReader::entryCount() const
{
    return d->open ? int(mz_zip_reader_get_num_files(&d->zip)) : 0;
}

int ZipReader::find(const QString &name) const
{
    if (!d->open || name.isEmpty())
        return -1;
    return mz_zip_reader_locate_file(&d->zip, name.toUtf8().constData(), nullptr, 0);
}

QString ZipReader::nameAt(int index) const
{
    if (!d->open || index < 0 || index >= entryCount())
        return {};
    char buf[1024];
    const mz_uint n = mz_zip_reader_get_filename(&d->zip, mz_uint(index), buf, sizeof(buf));
    return n > 0 ? QString::fromUtf8(buf, int(n) - 1) : QString();
}

std::unique_ptr<QIODevice> ZipReader::openEntry(int index, qint64 maxBytes) const
{
    if (!d->open || index < 0 || index >= entryCount())
        return nullptr;
    auto dev = std::make_unique<EntryDevice>(&d->zip, index, maxBytes);
    if (!dev->isOpen())
        return nullptr;
    return dev;
}

QByteArray ZipReader::readAll(int index, qint64 maxBytes, QString *error) const
{
    std::unique_ptr<QIODevice> dev = openEntry(index, maxBytes);
    if (!dev) {
        if (error)
            *error = QStringLiteral("missing part");
        return {};
    }
    QByteArray out;
    char buf[64 * 1024];
    for (;;) {
        const qint64 n = dev->read(buf, sizeof(buf));
        if (n < 0) {
            if (error)
                *error = dev->errorString();
            return {};
        }
        if (n == 0)
            break;
        out.append(buf, n);
    }
    return out;
}

} // namespace folia::import
