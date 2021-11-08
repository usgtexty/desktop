/*
 * Copyright 2021 (c) Matthieu Gallien <matthieu.gallien@nextcloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "bulkpropagatorjob.h"

#include "putmultifilejob.h"
#include "owncloudpropagator_p.h"
#include "syncfileitem.h"
#include "syncengine.h"
#include "propagateupload.h"
#include "propagatorjobs.h"
#include "filesystem.h"
#include "account.h"
#include "common/utility.h"
#include "common/checksums.h"
#include "networkjobs.h"

#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace OCC {

Q_LOGGING_CATEGORY(lcBulkPropagatorJob, "nextcloud.sync.propagator.bulkupload", QtInfoMsg)

}

namespace {

/**
 * We do not want to upload files that are currently being modified.
 * To avoid that, we don't upload files that have a modification time
 * that is too close to the current time.
 *
 * This interacts with the msBetweenRequestAndSync delay in the folder
 * manager. If that delay between file-change notification and sync
 * has passed, we should accept the file for upload here.
 */
bool fileIsStillChanging(const OCC::SyncFileItem &item)
{
    const QDateTime modtime = OCC::Utility::qDateTimeFromTime_t(item._modtime);
    const qint64 msSinceMod = modtime.msecsTo(QDateTime::currentDateTimeUtc());

    return std::chrono::milliseconds(msSinceMod) < OCC::SyncEngine::minimumFileAgeForUpload
        // if the mtime is too much in the future we *do* upload the file
        && msSinceMod > -10000;
}

inline QByteArray getEtagFromJsonReply(const QJsonObject &reply)
{
    QByteArray ocEtag = OCC::parseEtag(reply.value("OC-ETag").toString().toLatin1());
    QByteArray etag = OCC::parseEtag(reply.value("ETag").toString().toLatin1());
    QByteArray ret = ocEtag;
    if (ret.isEmpty()) {
        ret = etag;
    }
    if (ocEtag.length() > 0 && ocEtag != etag) {
        qCDebug(OCC::lcBulkPropagatorJob) << "Quite peculiar, we have an etag != OC-Etag [no problem!]" << etag << ocEtag;
    }
    return ret;
}

inline QByteArray getHeaderFromJsonReply(const QJsonObject &reply, const QByteArray &headerName)
{
    return reply.value(headerName).toString().toLatin1();
}

}

namespace OCC {

BulkPropagatorJob::BulkPropagatorJob(OwncloudPropagator *propagator,
                                     const std::deque<SyncFileItemPtr> &items)
    : PropagatorJob(propagator)
    , _items(items)
{
    _uploadFileParameters.reserve(_items.size());
}

bool BulkPropagatorJob::scheduleSelfOrChild()
{
    if (_items.empty()) {
        return false;
    }

    {
        auto bulkUploadUrl = Utility::concatUrlPath(propagator()->account()->url(), QStringLiteral("/remote.php/dav/bulk"));
        qCInfo(lcBulkPropagatorJob()) << "going to" << bulkUploadUrl;
    }

    _state = Running;
    for(int i = 0; i < 100 && !_items.empty(); ++i) {
        auto currentItem = _items.front();
        _items.pop_front();
        QMetaObject::invokeMethod(this, [this, currentItem] () {
            UploadFileInfo fileToUpload;
            fileToUpload._file = currentItem->_file;
            fileToUpload._size = currentItem->_size;
            fileToUpload._path = propagator()->fullLocalPath(fileToUpload._file);
            startUploadFile(currentItem, fileToUpload);
        }); // We could be in a different thread (neon jobs)
    }

    return _items.empty() && _uploadFileParameters.empty();
}

PropagatorJob::JobParallelism BulkPropagatorJob::parallelism()
{
    return PropagatorJob::JobParallelism::WaitForFinished;
}

void BulkPropagatorJob::startUploadFile(SyncFileItemPtr item, UploadFileInfo fileToUpload)
{
    if (propagator()->_abortRequested) {
        return;
    }

    // Check if the specific file can be accessed
    if (propagator()->hasCaseClashAccessibilityProblem(fileToUpload._file)) {
        done(item, SyncFileItem::NormalError, tr("File %1 cannot be uploaded because another file with the same name, differing only in case, exists").arg(QDir::toNativeSeparators(item->_file)));
        return;
    }

    // Check if we believe that the upload will fail due to remote quota limits
    const qint64 quotaGuess = propagator()->_folderQuota.value(
                QFileInfo(fileToUpload._file).path(), std::numeric_limits<qint64>::max());
    if (fileToUpload._size > quotaGuess) {
        // Necessary for blacklisting logic
        item->_httpErrorCode = 507;
        emit propagator()->insufficientRemoteStorage();
        done(item, SyncFileItem::DetailError, tr("Upload of %1 exceeds the quota for the folder").arg(Utility::octetsToString(fileToUpload._size)));
        return;
    }

    qCDebug(lcBulkPropagatorJob) << "Running the compute checksum";
    return slotComputeContentChecksum(item, fileToUpload);
}

void BulkPropagatorJob::doStartUpload(SyncFileItemPtr item,
                                      UploadFileInfo fileToUpload,
                                      QByteArray transmissionChecksumHeader)
{
    if (propagator()->_abortRequested) {
        return;
    }

    // If there is only one chunk, write the checksum in the database, so if the PUT is sent
    // to the server, but the connection drops before we get the etag, we can check the checksum
    // in reconcile (issue #5106)
    SyncJournalDb::UploadInfo pi;
    pi._valid = true;
    pi._chunk = 0;
    pi._transferid = 0; // We set a null transfer id because it is not chunked.
    pi._modtime = item->_modtime;
    pi._errorCount = 0;
    pi._contentChecksum = item->_checksumHeader;
    pi._size = item->_size;
    propagator()->_journal->setUploadInfo(item->_file, pi);
    propagator()->_journal->commit("Upload info");

    qint64 fileSize = fileToUpload._size;
    auto currentHeaders = headers(item);
    currentHeaders[QByteArrayLiteral("Content-Length")] = QByteArray::number(fileSize);

    if (!item->_renameTarget.isEmpty() && item->_file != item->_renameTarget) {
        // Try to rename the file
        const auto originalFilePathAbsolute = propagator()->fullLocalPath(item->_file);
        const auto newFilePathAbsolute = propagator()->fullLocalPath(item->_renameTarget);
        const auto renameSuccess = QFile::rename(originalFilePathAbsolute, newFilePathAbsolute);
        if (!renameSuccess) {
            done(item, SyncFileItem::NormalError, "File contains trailing spaces and couldn't be renamed");
            return;
        }
        qCWarning(lcBulkPropagatorJob()) << item->_file << item->_renameTarget;
        fileToUpload._file = item->_file = item->_renameTarget;
        fileToUpload._path = propagator()->fullLocalPath(fileToUpload._file);
        item->_modtime = FileSystem::getModTime(newFilePathAbsolute);
    }

    QString path = fileToUpload._file;

    qCInfo(lcBulkPropagatorJob) << propagator()->fullRemotePath(path) << "transmission checksum" << transmissionChecksumHeader;
    currentHeaders["X-File-MD5"] = transmissionChecksumHeader;

    const QString fileName = fileToUpload._path;

    UploadFileParameters newUploadFile{propagator()->account(), item, fileToUpload,
                propagator()->fullRemotePath(path), fileName,
                fileSize, currentHeaders};

    _uploadFileParameters.push_back(std::move(newUploadFile));

    if (_checksumsJobs.empty()) {
        triggerUpload();
    }
}

void BulkPropagatorJob::triggerUpload()
{
    auto uploadParametersData = std::vector<OneUploadFileData>{};
    uploadParametersData.reserve(_uploadFileParameters.size());

    for(auto &oneFile : _uploadFileParameters) {
        // job takes ownership of device via a QScopedPointer. Job deletes itself when finishing
        auto device = std::make_unique<UploadDevice>(
                oneFile._localPath, 0, oneFile._fileSize, &propagator()->_bandwidthManager);
        if (!device->open(QIODevice::ReadOnly)) {
            qCWarning(lcBulkPropagatorJob) << "Could not prepare upload device: " << device->errorString();

            // If the file is currently locked, we want to retry the sync
            // when it becomes available again.
            if (FileSystem::isFileLocked(oneFile._localPath)) {
                emit propagator()->seenLockedFile(oneFile._localPath);
            }
            // Soft error because this is likely caused by the user modifying his files while syncing
            abortWithError(oneFile._item, SyncFileItem::SoftError, device->errorString());
            return;
        }
        oneFile._headers["X-File-Path"] = oneFile._remotePath.toUtf8();
        uploadParametersData.push_back({std::move(device), oneFile._headers});
    }

    auto bulkUploadUrl = Utility::concatUrlPath(propagator()->account()->url(), QStringLiteral("/remote.php/dav/bulk"));
    qCInfo(lcBulkPropagatorJob()) << "going to" << bulkUploadUrl;
    auto job = std::make_unique<PutMultiFileJob>(propagator()->account(), bulkUploadUrl, std::move(uploadParametersData), this);
    connect(job.get(), &PutMultiFileJob::finishedSignal, this, &BulkPropagatorJob::slotPutFinished);
    connect(job.get(), &PutMultiFileJob::uploadProgress, this, &BulkPropagatorJob::slotUploadProgress);
    //connect(job.get(), &PutMultiFileJob::uploadProgress, device.get(), &UploadDevice::slotJobUploadProgress);
    connect(job.get(), &QObject::destroyed, this, &BulkPropagatorJob::slotJobDestroyed);
    //adjustLastJobTimeout(job.get(), oneFile._fileSize);
    auto jobCopy = job.get();
    _jobs.append(job.release());
    jobCopy->start();
}

void BulkPropagatorJob::slotComputeContentChecksum(SyncFileItemPtr item,
                                                   UploadFileInfo fileToUpload)
{
    qDebug() << "Trying to compute the checksum of the file";
    qDebug() << "Still trying to understand if this is the local file or the uploaded one";
    if (propagator()->_abortRequested) {
        return;
    }

    const QString filePath = propagator()->fullLocalPath(item->_file);

    // remember the modtime before checksumming to be able to detect a file
    // change during the checksum calculation - This goes inside of the item->_file
    // and not the fileToUpload because we are checking the original file, not there
    // probably temporary one.
    item->_modtime = FileSystem::getModTime(filePath);

    const QByteArray checksumType = "MD5" /*propagator()->account()->capabilities().preferredUploadChecksumType()*/;

    // Maybe the discovery already computed the checksum?
    // Should I compute the checksum of the original (item->_file)
    // or the maybe-modified? (fileToUpload._file) ?

    QByteArray existingChecksumType;
    QByteArray existingChecksum;
    parseChecksumHeader(item->_checksumHeader, &existingChecksumType, &existingChecksum);
    if (existingChecksumType == checksumType) {
        slotComputeTransmissionChecksum(item, fileToUpload, checksumType, existingChecksum);
        return;
    }

    // Compute the content checksum.
    auto computeChecksum = std::make_unique<ComputeChecksum>(this);
    computeChecksum->setChecksumType(checksumType);

    connect(computeChecksum.get(), &ComputeChecksum::done,
            this, [this, item, fileToUpload] (const QByteArray &contentChecksumType, const QByteArray &contentChecksum) {
        slotComputeTransmissionChecksum(item, fileToUpload, contentChecksumType, contentChecksum);
    });
    connect(computeChecksum.get(), &ComputeChecksum::done,
            computeChecksum.get(), &QObject::deleteLater);
    connect(computeChecksum.get(), &QObject::destroyed, this, [this] (QObject *deletedJob) {
        _checksumsJobs.erase(std::remove(_checksumsJobs.begin(), _checksumsJobs.end(), deletedJob), _checksumsJobs.end());
    });

    auto jobCopy = computeChecksum.get();
    _checksumsJobs.append(computeChecksum.release());
    jobCopy->start(fileToUpload._path);
}

void BulkPropagatorJob::slotComputeTransmissionChecksum(SyncFileItemPtr item,
                                                        UploadFileInfo fileToUpload,
                                                        const QByteArray &contentChecksumType,
                                                        const QByteArray &contentChecksum)
{
    item->_checksumHeader = makeChecksumHeader(contentChecksumType, contentChecksum);

    // Reuse the content checksum as the transmission checksum if possible
    const auto supportedTransmissionChecksums =
        propagator()->account()->capabilities().supportedChecksumTypes();
    if (supportedTransmissionChecksums.contains(contentChecksumType)) {
        slotStartUpload(item, fileToUpload, contentChecksumType, contentChecksum);
        return;
    }

    // Compute the transmission checksum.
    auto computeChecksum = std::make_unique<ComputeChecksum>(this);
    if (uploadChecksumEnabled()) {
        computeChecksum->setChecksumType("MD5" /*propagator()->account()->capabilities().uploadChecksumType()*/);
    } else {
        computeChecksum->setChecksumType(QByteArray());
    }

    connect(computeChecksum.get(), &ComputeChecksum::done,
            this, [this, item, fileToUpload] (const QByteArray &contentChecksumType, const QByteArray &contentChecksum) {
        slotStartUpload(item, fileToUpload, contentChecksumType, contentChecksum);
    });
    connect(computeChecksum.get(), &ComputeChecksum::done,
            computeChecksum.get(), &QObject::deleteLater);
    computeChecksum.release()->start(fileToUpload._path);
}

void BulkPropagatorJob::slotStartUpload(SyncFileItemPtr item,
                                        UploadFileInfo fileToUpload,
                                        const QByteArray &transmissionChecksumType,
                                        const QByteArray &transmissionChecksum)
{
    QByteArray transmissionChecksumHeader;

    transmissionChecksumHeader = makeChecksumHeader(transmissionChecksumType, transmissionChecksum);

    // If no checksum header was not set, reuse the transmission checksum as the content checksum.
    if (item->_checksumHeader.isEmpty()) {
        item->_checksumHeader = transmissionChecksumHeader;
    }

    const QString fullFilePath = fileToUpload._path;
    const QString originalFilePath = propagator()->fullLocalPath(item->_file);

    if (!FileSystem::fileExists(fullFilePath)) {
        return slotOnErrorStartFolderUnlock(item, SyncFileItem::SoftError, tr("File Removed (start upload) %1").arg(fullFilePath));
    }
    time_t prevModtime = item->_modtime; // the _item value was set in PropagateUploadFile::start()
    // but a potential checksum calculation could have taken some time during which the file could
    // have been changed again, so better check again here.

    item->_modtime = FileSystem::getModTime(originalFilePath);
    if (prevModtime != item->_modtime) {
        propagator()->_anotherSyncNeeded = true;
        qDebug() << "prevModtime" << prevModtime << "Curr" << item->_modtime;
        return slotOnErrorStartFolderUnlock(item, SyncFileItem::SoftError, tr("Local file changed during syncing. It will be resumed."));
    }

    fileToUpload._size = FileSystem::getSize(fullFilePath);
    item->_size = FileSystem::getSize(originalFilePath);

    // But skip the file if the mtime is too close to 'now'!
    // That usually indicates a file that is still being changed
    // or not yet fully copied to the destination.
    if (fileIsStillChanging(*item)) {
        propagator()->_anotherSyncNeeded = true;
        return slotOnErrorStartFolderUnlock(item, SyncFileItem::SoftError, tr("Local file changed during sync."));
    }

    doStartUpload(item, fileToUpload, transmissionChecksum);
}

void BulkPropagatorJob::slotOnErrorStartFolderUnlock(SyncFileItemPtr item,
                                                     SyncFileItem::Status status,
                                                     const QString &errorString)
{
    done(item, status, errorString);
    qCInfo(lcBulkPropagatorJob()) << status << errorString;
}

void BulkPropagatorJob::slotPutFinished()
{
    for(auto &oneFile : _uploadFileParameters) {
        qCInfo(lcBulkPropagatorJob()) << oneFile._item->_file;
        auto *job = qobject_cast<PutMultiFileJob *>(sender());
        ASSERT(job)

        bool finished = false;

        slotJobDestroyed(job); // remove it from the _jobs list

        oneFile._item->_httpErrorCode = static_cast<quint16>(job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toUInt());
        oneFile._item->_responseTimeStamp = job->responseTimestamp();
        oneFile._item->_requestId = job->requestId();
        QNetworkReply::NetworkError err = job->reply()->error();
        if (err != QNetworkReply::NoError) {
            commonErrorHandling(oneFile._item, oneFile._fileToUpload, job);
            return;
        }

        auto replyData = QByteArray{};
        while(job->reply()->bytesAvailable()) {
            replyData += job->reply()->readAll();
        }

        auto replyJson = QJsonDocument::fromJson(replyData);
        auto replyArray = replyJson.array();
        auto fileReply = QJsonObject{};
        for (const auto &oneReply : qAsConst(replyArray)) {
            auto replyObject = oneReply.toObject();
            qCDebug(lcBulkPropagatorJob()) << "searching for reply" << replyObject << oneFile._item->_file;
            if (replyObject.value("X-File-Path").toString() == oneFile._item->_file) {
                fileReply = replyObject;
                break;
            }
        }

        qCInfo(lcBulkPropagatorJob()) << "file headers" << fileReply;

        // The server needs some time to process the request and provide us with a poll URL
        if (oneFile._item->_httpErrorCode == 202) {
            QString path = QString::fromUtf8(getHeaderFromJsonReply(fileReply, "OC-JobStatus-Location"));
            if (path.isEmpty()) {
                done(oneFile._item, SyncFileItem::NormalError, tr("Poll URL missing"));
                return;
            }
            startPollJob(oneFile._item, oneFile._fileToUpload, path);
            return;
        }

        // Check the file again post upload.
        // Two cases must be considered separately: If the upload is finished,
        // the file is on the server and has a changed ETag. In that case,
        // the etag has to be properly updated in the client journal, and because
        // of that we can bail out here with an error. But we can reschedule a
        // sync ASAP.
        // But if the upload is ongoing, because not all chunks were uploaded
        // yet, the upload can be stopped and an error can be displayed, because
        // the server hasn't registered the new file yet.
        QByteArray etag = getEtagFromJsonReply(fileReply);
        finished = etag.length() > 0;

        const QString fullFilePath(propagator()->fullLocalPath(oneFile._item->_file));

        // Check if the file still exists
        if (!checkFileStillExists(oneFile._item, finished, fullFilePath)) {
            return;
        }

        // Check whether the file changed since discovery. the file check here is the original and not the temprary.
        if (!checkFileChanged(oneFile._item, finished, fullFilePath)) {
            return;
        }

        // the file id should only be empty for new files up- or downloaded
        computeFileId(oneFile._item, fileReply);

        oneFile._item->_etag = etag;

        if (getHeaderFromJsonReply(fileReply, "X-OC-MTime") != "accepted") {
            // X-OC-MTime is supported since owncloud 5.0.   But not when chunking.
            // Normally Owncloud 6 always puts X-OC-MTime
            qCWarning(lcBulkPropagatorJob) << "Server does not support X-OC-MTime" << getHeaderFromJsonReply(fileReply, "X-OC-MTime");
            // Well, the mtime was not set
        }
    }

    finalize();
}

void BulkPropagatorJob::slotUploadProgress(qint64, qint64) const
{
    qCInfo(lcBulkPropagatorJob()) << "slotUploadProgress";
}

void BulkPropagatorJob::slotJobDestroyed(QObject *job)
{
    qCInfo(lcBulkPropagatorJob()) << "slotJobDestroyed";
    _jobs.erase(std::remove(_jobs.begin(), _jobs.end(), job), _jobs.end());
}

void BulkPropagatorJob::adjustLastJobTimeout(AbstractNetworkJob *job, qint64 fileSize) const
{
    constexpr double threeMinutes = 3.0 * 60 * 1000;

    job->setTimeout(qBound(
        job->timeoutMsec(),
        // Calculate 3 minutes for each gigabyte of data
        qRound64(threeMinutes * static_cast<double>(fileSize) / 1e9),
        // Maximum of 30 minutes
                        static_cast<qint64>(30 * 60 * 1000)));
}

void BulkPropagatorJob::finalize()
{
    for(auto &oneFile : _uploadFileParameters) {
        // Update the quota, if known
        auto quotaIt = propagator()->_folderQuota.find(QFileInfo(oneFile._item->_file).path());
        if (quotaIt != propagator()->_folderQuota.end()) {
            quotaIt.value() -= oneFile._fileToUpload._size;
        }

        // Update the database entry
        const auto result = propagator()->updateMetadata(*oneFile._item);
        if (!result) {
            done(oneFile._item, SyncFileItem::FatalError, tr("Error updating metadata: %1").arg(result.error()));
            return;
        } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
            done(oneFile._item, SyncFileItem::SoftError, tr("The file %1 is currently in use").arg(oneFile._item->_file));
            return;
        }

        // Files that were new on the remote shouldn't have online-only pin state
        // even if their parent folder is online-only.
        if (oneFile._item->_instruction == CSYNC_INSTRUCTION_NEW
            || oneFile._item->_instruction == CSYNC_INSTRUCTION_TYPE_CHANGE) {
            auto &vfs = propagator()->syncOptions()._vfs;
            const auto pin = vfs->pinState(oneFile._item->_file);
            if (pin && *pin == PinState::OnlineOnly && !vfs->setPinState(oneFile._item->_file, PinState::Unspecified)) {
                qCWarning(lcBulkPropagatorJob) << "Could not set pin state of" << oneFile._item->_file << "to unspecified";
            }
        }

        // Remove from the progress database:
        propagator()->_journal->setUploadInfo(oneFile._item->_file, SyncJournalDb::UploadInfo());
        propagator()->_journal->commit("upload file start");

        done(oneFile._item, SyncFileItem::Success, {});
    }

    if (_items.empty()) {
        if (!_jobs.empty()) {
            // just wait for the other job to finish.
            return;
        }
        if (!_checksumsJobs.empty()) {
            // just wait for the other job to finish.
            return;
        }

        qCInfo(lcBulkPropagatorJob) << "final status" << _finalStatus;
        emit finished(_finalStatus);
        propagator()->scheduleNextJob();
    } else {
        qCInfo(lcBulkPropagatorJob) << "remaining upload tasks" << _items.size();
        scheduleSelfOrChild();
    }
}

void BulkPropagatorJob::done(SyncFileItemPtr item,
                             SyncFileItem::Status status,
                             const QString &errorString)
{
    item->_status = status;
    item->_errorString = errorString;

    qCInfo(lcBulkPropagatorJob) << "Item completed" << item->destination() << item->_status << item->_instruction << item->_errorString;

    handleFileRestoration(item, errorString);

    if (propagator()->_abortRequested && (item->_status == SyncFileItem::NormalError
                                          || item->_status == SyncFileItem::FatalError)) {
        // an abort request is ongoing. Change the status to Soft-Error
        item->_status = SyncFileItem::SoftError;
    }

    // Blacklist handling
    handleBlackList(item);

    handleJobDoneErrors(item, status);

    emit propagator()->itemCompleted(item);
}

void BulkPropagatorJob::startPollJob(SyncFileItemPtr item,
                                     UploadFileInfo fileToUpload,
                                     const QString &path)
{
    auto job = std::make_unique<PollJob>(propagator()->account(),
                                         path, item,
                                         propagator()->_journal,
                                         propagator()->localPath(), this);
    connect(job.get(), &PollJob::finishedSignal, this, [this, fileToUpload] () {
        slotPollFinished(fileToUpload);
    });
    SyncJournalDb::PollInfo info;
    info._file = item->_file;
    info._url = path;
    info._modtime = item->_modtime;
    info._fileSize = item->_size;
    propagator()->_journal->setPollInfo(info);
    propagator()->_journal->commit("add poll info");
    auto jobCopy = job.get();
    _jobs.append(job.release());
    jobCopy->start();
    if (!_items.empty()) {
        scheduleSelfOrChild();
    }
}

void BulkPropagatorJob::slotPollFinished(UploadFileInfo fileToUpload)
{
    auto *job = qobject_cast<PollJob *>(sender());
    ASSERT(job)

    slotJobDestroyed(job);

    if (job->_item->_status != SyncFileItem::Success) {
        done(job->_item, job->_item->_status, job->_item->_errorString);
        return;
    }

    finalize();
}

QMap<QByteArray, QByteArray> BulkPropagatorJob::headers(SyncFileItemPtr item) const
{
    QMap<QByteArray, QByteArray> headers;
    headers[QByteArrayLiteral("Content-Type")] = QByteArrayLiteral("application/octet-stream");
    headers[QByteArrayLiteral("X-File-Mtime")] = QByteArray::number(qint64(item->_modtime));
    if (qEnvironmentVariableIntValue("OWNCLOUD_LAZYOPS")) {
        headers[QByteArrayLiteral("OC-LazyOps")] = QByteArrayLiteral("true");
    }

    if (item->_file.contains(QLatin1String(".sys.admin#recall#"))) {
        // This is a file recall triggered by the admin.  Note: the
        // recall list file created by the admin and downloaded by the
        // client (.sys.admin#recall#) also falls into this category
        // (albeit users are not supposed to mess up with it)

        // We use a special tag header so that the server may decide to store this file away in some admin stage area
        // And not directly in the user's area (which would trigger redownloads etc).
        headers["OC-Tag"] = ".sys.admin#recall#";
    }

    if (!item->_etag.isEmpty() && item->_etag != "empty_etag"
        && item->_instruction != CSYNC_INSTRUCTION_NEW // On new files never send a If-Match
        && item->_instruction != CSYNC_INSTRUCTION_TYPE_CHANGE) {
        // We add quotes because the owncloud server always adds quotes around the etag, and
        //  csync_owncloud.c's owncloud_file_id always strips the quotes.
        headers[QByteArrayLiteral("If-Match")] = '"' + item->_etag + '"';
    }

    // Set up a conflict file header pointing to the original file
    auto conflictRecord = propagator()->_journal->conflictRecord(item->_file.toUtf8());
    if (conflictRecord.isValid()) {
        headers[QByteArrayLiteral("OC-Conflict")] = "1";
        if (!conflictRecord.initialBasePath.isEmpty()) {
            headers[QByteArrayLiteral("OC-ConflictInitialBasePath")] = conflictRecord.initialBasePath;
        }
        if (!conflictRecord.baseFileId.isEmpty()) {
            headers[QByteArrayLiteral("OC-ConflictBaseFileId")] = conflictRecord.baseFileId;
        }
        if (conflictRecord.baseModtime != -1) {
            headers[QByteArrayLiteral("OC-ConflictBaseMtime")] = QByteArray::number(conflictRecord.baseModtime);
        }
        if (!conflictRecord.baseEtag.isEmpty()) {
            headers[QByteArrayLiteral("OC-ConflictBaseEtag")] = conflictRecord.baseEtag;
        }
    }

    return headers;
}

void BulkPropagatorJob::abortWithError(SyncFileItemPtr item,
                                       SyncFileItem::Status status,
                                       const QString &error)
{
    abort(AbortType::Synchronous);
    done(item, status, error);
}

void BulkPropagatorJob::checkResettingErrors(SyncFileItemPtr item) const
{
    if (item->_httpErrorCode == 412
        || propagator()->account()->capabilities().httpErrorCodesThatResetFailingChunkedUploads().contains(item->_httpErrorCode)) {
        auto uploadInfo = propagator()->_journal->getUploadInfo(item->_file);
        uploadInfo._errorCount += 1;
        if (uploadInfo._errorCount > 3) {
            qCInfo(lcBulkPropagatorJob) << "Reset transfer of" << item->_file
                                      << "due to repeated error" << item->_httpErrorCode;
            uploadInfo = SyncJournalDb::UploadInfo();
        } else {
            qCInfo(lcBulkPropagatorJob) << "Error count for maybe-reset error" << item->_httpErrorCode
                                      << "on file" << item->_file
                                      << "is" << uploadInfo._errorCount;
        }
        propagator()->_journal->setUploadInfo(item->_file, uploadInfo);
        propagator()->_journal->commit("Upload info");
    }
}

void BulkPropagatorJob::commonErrorHandling(SyncFileItemPtr item,
                                            UploadFileInfo fileToUpload,
                                            AbstractNetworkJob *job)
{
    QByteArray replyContent;
    QString errorString = job->errorStringParsingBody(&replyContent);
    qCDebug(lcBulkPropagatorJob) << replyContent; // display the XML error in the debug

    if (item->_httpErrorCode == 412) {
        // Precondition Failed: Either an etag or a checksum mismatch.

        // Maybe the bad etag is in the database, we need to clear the
        // parent folder etag so we won't read from DB next sync.
        propagator()->_journal->schedulePathForRemoteDiscovery(item->_file);
        propagator()->_anotherSyncNeeded = true;
    }

    // Ensure errors that should eventually reset the chunked upload are tracked.
    checkResettingErrors(item);

    SyncFileItem::Status status = classifyError(job->reply()->error(), item->_httpErrorCode,
        &propagator()->_anotherSyncNeeded, replyContent);

    // Insufficient remote storage.
    if (item->_httpErrorCode == 507) {
        // Update the quota expectation
        /* store the quota for the real local file using the information
         * on the file to upload, that could have been modified by
         * filters or something. */
        const auto path = QFileInfo(item->_file).path();
        auto quotaIt = propagator()->_folderQuota.find(path);
        if (quotaIt != propagator()->_folderQuota.end()) {
            quotaIt.value() = qMin(quotaIt.value(), fileToUpload._size - 1);
        } else {
            propagator()->_folderQuota[path] = fileToUpload._size - 1;
        }

        // Set up the error
        status = SyncFileItem::DetailError;
        errorString = tr("Upload of %1 exceeds the quota for the folder").arg(Utility::octetsToString(fileToUpload._size));
        emit propagator()->insufficientRemoteStorage();
    }

    abortWithError(item, status, errorString);
}

bool BulkPropagatorJob::checkFileStillExists(SyncFileItemPtr item,
                                             const bool finished,
                                             const QString &fullFilePath)
{
    if (!FileSystem::fileExists(fullFilePath)) {
        if (!finished) {
            abortWithError(item, SyncFileItem::SoftError, tr("The local file was removed during sync."));
            return false;
        } else {
            propagator()->_anotherSyncNeeded = true;
        }
    }

    return true;
}

bool BulkPropagatorJob::checkFileChanged(SyncFileItemPtr item,
                                         const bool finished,
                                         const QString &fullFilePath)
{
    if (!FileSystem::verifyFileUnchanged(fullFilePath, item->_size, item->_modtime)) {
        propagator()->_anotherSyncNeeded = true;
        if (!finished) {
            abortWithError(item, SyncFileItem::SoftError, tr("Local file changed during sync."));
            // FIXME:  the legacy code was retrying for a few seconds.
            //         and also checking that after the last chunk, and removed the file in case of INSTRUCTION_NEW
            return false;
        }
    }

    return true;
}

void BulkPropagatorJob::computeFileId(SyncFileItemPtr item,
                                      const QJsonObject &fileReply) const
{
    QByteArray fid = getHeaderFromJsonReply(fileReply, "OC-FileID");
    if (!fid.isEmpty()) {
        if (!item->_fileId.isEmpty() && item->_fileId != fid) {
            qCWarning(lcBulkPropagatorJob) << "File ID changed!" << item->_fileId << fid;
        }
        item->_fileId = fid;
    }
}

void BulkPropagatorJob::handleFileRestoration(SyncFileItemPtr item,
                                              const QString &errorString) const
{
    if (item->_isRestoration) {
        if (item->_status == SyncFileItem::Success
            || item->_status == SyncFileItem::Conflict) {
            item->_status = SyncFileItem::Restoration;
        } else {
            item->_errorString += tr("; Restoration Failed: %1").arg(errorString);
        }
    } else {
        if (item->_errorString.isEmpty()) {
            item->_errorString = errorString;
        }
    }
}

void BulkPropagatorJob::handleBlackList(SyncFileItemPtr item) const
{
    switch (item->_status) {
    case SyncFileItem::SoftError:
    case SyncFileItem::FatalError:
    case SyncFileItem::NormalError:
    case SyncFileItem::DetailError:
        // Check the blacklist, possibly adjusting the item (including its status)
        blacklistUpdate(propagator()->_journal, *item);
        break;
    case SyncFileItem::Success:
    case SyncFileItem::Restoration:
        if (item->_hasBlacklistEntry) {
            // wipe blacklist entry.
            propagator()->_journal->wipeErrorBlacklistEntry(item->_file);
            // remove a blacklist entry in case the file was moved.
            if (item->_originalFile != item->_file) {
                propagator()->_journal->wipeErrorBlacklistEntry(item->_originalFile);
            }
        }
        break;
    case SyncFileItem::Conflict:
    case SyncFileItem::FileIgnored:
    case SyncFileItem::NoStatus:
    case SyncFileItem::BlacklistedError:
    case SyncFileItem::FileLocked:
    case SyncFileItem::FileNameInvalid:
        // nothing
        break;
    }
}

void BulkPropagatorJob::handleJobDoneErrors(SyncFileItemPtr item,
                                            SyncFileItem::Status status)
{
    if (item->hasErrorStatus()) {
        qCWarning(lcPropagator) << "Could not complete propagation of" << item->destination() << "by" << this << "with status" << item->_status << "and error:" << item->_errorString;
    } else {
        qCInfo(lcPropagator) << "Completed propagation of" << item->destination() << "by" << this << "with status" << item->_status;
    }

    if (item->_status == SyncFileItem::FatalError) {
        // Abort all remaining jobs.
        propagator()->abort();
    }

    switch (item->_status)
    {
    case SyncFileItem::BlacklistedError:
    case SyncFileItem::Conflict:
    case SyncFileItem::FatalError:
    case SyncFileItem::FileIgnored:
    case SyncFileItem::FileLocked:
    case SyncFileItem::FileNameInvalid:
    case SyncFileItem::NoStatus:
    case SyncFileItem::NormalError:
    case SyncFileItem::Restoration:
    case SyncFileItem::SoftError:
        _finalStatus = SyncFileItem::NormalError;
        qCInfo(lcBulkPropagatorJob) << "modify final status NormalError" << _finalStatus << status;
        break;
    case SyncFileItem::DetailError:
        _finalStatus = SyncFileItem::DetailError;
        qCInfo(lcBulkPropagatorJob) << "modify final status DetailError" << _finalStatus << status;
        break;
    case SyncFileItem::Success:
        break;
    }
}

}
