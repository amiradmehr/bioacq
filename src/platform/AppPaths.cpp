#include "AppPaths.h"

#include "BuildConfig.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#if !BIOACQ_PACKAGED
namespace
{

// A folder counts as writable if it exists and is writable, or if it does not
// exist yet and its nearest existing ancestor is (it is created on first use).
bool writableOrCreatable (const QString &path)
{
    QFileInfo fi (QDir::cleanPath (path));
    while (!fi.exists ())
    {
        const QString parent = fi.absolutePath ();
        if (parent == fi.absoluteFilePath ())
            return false; // reached the root without finding anything
        fi = QFileInfo (parent);
    }
    return fi.isDir () && fi.isWritable ();
}

} // namespace
#endif

QString defaultRecordDir ()
{
#if !BIOACQ_PACKAGED
    const QString baked = QString::fromUtf8 (BIOACQ_RECORD_DIR);
    if (!baked.isEmpty () && writableOrCreatable (baked))
        return baked;
#endif
    QString docs = QStandardPaths::writableLocation (QStandardPaths::DocumentsLocation);
    if (docs.isEmpty ())
        docs = QDir::homePath ();
    return docs + QStringLiteral ("/BioAcq Recordings");
}

QString fallbackRecordDir ()
{
    return QDir::homePath () + QStringLiteral ("/bioacq_recordings");
}
