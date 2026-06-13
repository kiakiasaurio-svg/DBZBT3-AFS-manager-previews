#include <AdxPreview.h>

#include <QDir>
#include <QFile>
#include <QUrl>
#include <QUuid>
#include <QStandardPaths>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif

#include <ADXCore.h>
#include <Shared.h>

using namespace Shared;

AdxPreview::AdxPreview(QObject *parent) : QObject(parent), player(new QMediaPlayer(this))
{
	connect(player, &QMediaPlayer::mediaStatusChanged, this, &AdxPreview::onMediaStatusChanged);
	connect(player, static_cast<void (QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error), this, &AdxPreview::onPlayerError);
}

AdxPreview::~AdxPreview()
{
	stop();
}

bool AdxPreview::play(const AFS_File *afs, uint32_t index, const std::string &filename)
{
	if (afs == nullptr) {
		return false;
	}

	/* Stop and clean up any previous preview before starting a new one */
	stop();

	QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (tempDir.isEmpty()) {
		tempDir = QDir::tempPath();
	}

	QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QString baseName = QString::fromLocal8Bit(getFileBasename(filename).c_str());
	if (baseName.isEmpty()) {
		baseName = "preview";
	}

	QString adxPath = QDir(tempDir).filePath("AFSManager_" + uniqueId + "_" + baseName);
	QString wavPath = adxPath + ".wav";

	tempAdxPath = adxPath.toLocal8Bit().toStdString();
	tempWavPath = wavPath.toLocal8Bit().toStdString();

	/* Step 1: extract only the selected ADX entry to a temporary file */
	char *content = nullptr;
	bool exported = afs->exportFile(index, tempAdxPath, content);

	if (!exported) {
		emit errorMessage("Unable to extract '" + QString::fromLocal8Bit(filename.c_str()) + "' for preview");
		cleanupFiles();
		return false;
	}

	/* Step 2: decode the extracted ADX to a temporary WAV file */
	ADX_File::Error decodeError;
	bool decoded = ADX_File::convertToWAV(tempAdxPath, tempWavPath, &decodeError);

	if (!decoded) {
		QString reason;
		if (decodeError.unableToOpen) {
			reason = "unable to open extracted file";
		}
		else if (decodeError.notADX) {
			reason = "file is not a valid ADX";
		}
		else if (decodeError.unsupportedFormat) {
			reason = "unsupported ADX format (only standard type-03 ADPCM is supported)";
		}
		else if (decodeError.corruptedContent) {
			reason = "corrupted ADX content";
		}
		else if (decodeError.badStream) {
			reason = "read/write error while decoding";
		}
		else {
			reason = "unknown decoding error";
		}

		emit errorMessage("Unable to preview '" + QString::fromLocal8Bit(filename.c_str()) + "':\n" + reason);
		cleanupFiles();
		return false;
	}

	/* Step 3: play the decoded WAV */
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	player->setMedia(QUrl::fromLocalFile(wavPath));
#else
	player->setSource(QUrl::fromLocalFile(wavPath));
#endif

	player->play();

	return true;
}

void AdxPreview::stop()
{
	if (player->state() != QMediaPlayer::StoppedState) {
		player->stop();
	}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	player->setMedia(QMediaContent());
#else
	player->setSource(QUrl());
#endif

	cleanupFiles();
}

bool AdxPreview::isPlaying() const
{
	return player->state() == QMediaPlayer::PlayingState;
}

void AdxPreview::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
	if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) {
		stop();
		emit playbackFinished();
	}
}

void AdxPreview::onPlayerError(QMediaPlayer::Error error)
{
	if (error != QMediaPlayer::NoError) {
		emit errorMessage("Playback error: " + player->errorString());
		stop();
		emit playbackFinished();
	}
}

void AdxPreview::cleanupFiles()
{
	if (!tempWavPath.empty()) {
		QFile::remove(QString::fromLocal8Bit(tempWavPath.c_str()));
		tempWavPath.clear();
	}

	if (!tempAdxPath.empty()) {
		QFile::remove(QString::fromLocal8Bit(tempAdxPath.c_str()));
		tempAdxPath.clear();
	}
}
