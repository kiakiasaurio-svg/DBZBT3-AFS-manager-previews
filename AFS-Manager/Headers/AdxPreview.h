#ifndef ADXPREVIEW_H
#define ADXPREVIEW_H

#include <QObject>
#include <QMediaPlayer>

#include <string>

#include <AFSCore.h>

/*
 * AdxPreview
 *
 * Handles the "Preview" workflow for a single ADX entry inside an AFS:
 *  1) Extract only the selected entry to a temporary file (reusing AFS_File::exportFile)
 *  2) Decode it to a temporary WAV file (reusing ADX_File / ADXCore)
 *  3) Play the WAV with QMediaPlayer (Qt5 Multimedia)
 *  4) Remove both temporary files once playback finishes or is stopped
 *
 * Only one preview can be active at a time; starting a new preview while
 * one is playing stops and cleans up the previous one first.
 */
class AdxPreview : public QObject
{
Q_OBJECT

public:
	explicit AdxPreview(QObject *parent = nullptr);

	~AdxPreview() override;

	/* Extract+decode+play the given AFS entry. Returns true if playback started.
	 * On failure, errorMessage() will be emitted with details. */
	bool play(const AFS_File *afs, uint32_t index, const std::string &filename);

	/* Stop current playback (if any) and remove temporary files */
	void stop();

	bool isPlaying() const;

signals:
	void errorMessage(const QString &message);

	void playbackFinished();

private slots:
	void onMediaStatusChanged(QMediaPlayer::MediaStatus status);

	void onPlayerError(QMediaPlayer::Error error);

private:
	void cleanupFiles();

private:
	QMediaPlayer *player;

	std::string tempAdxPath;
	std::string tempWavPath;
};

#endif // ADXPREVIEW_H
