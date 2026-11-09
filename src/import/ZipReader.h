// Reads entries of a zip archive (a .docx) as streams, on miniz
// (third_party/miniz). The archive is read through QFile (Unicode paths on
// every platform) and an entry is inflated through fixed buffers (64 KiB in,
// 32 KiB window, ~110 KiB per open entry), never whole; the bytes actually
// inflated are counted, so the sizes the archive claims are never trusted.
#pragma once

#include <QIODevice>
#include <QString>

#include <memory>

namespace folia::import {

class ZipReader {
public:
    static constexpr int kMaxEntries = 20000;

    ZipReader();
    ~ZipReader();
    ZipReader(const ZipReader &) = delete;
    ZipReader &operator=(const ZipReader &) = delete;

    // Reads the central directory. False (with a reason) for anything that
    // is not a readable zip, or one with more than kMaxEntries entries.
    bool open(const QString &path, QString *error);
    int entryCount() const;
    // An entry by name (case-insensitive, as OPC part names are), or -1.
    int find(const QString &name) const;
    QString nameAt(int index) const;

    // A sequential device over the inflated entry. Reading past `maxBytes`
    // inflated bytes fails (errorString() says so). Valid while the reader
    // lives; one at a time keeps memory at one set of buffers.
    std::unique_ptr<QIODevice> openEntry(int index, qint64 maxBytes) const;
    // A small entry in one piece (empty, with `error` set, past maxBytes).
    QByteArray readAll(int index, qint64 maxBytes, QString *error = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace folia::import
