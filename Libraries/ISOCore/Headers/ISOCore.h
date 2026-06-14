#ifndef ISOCORE_H
#define ISOCORE_H

#include <cstdint>
#include <string>
#include <vector>

/*
 * ISOCore
 *
 * Minimal PS2 UDF ISO reader/writer.
 *
 * Supports:
 *  - Listing files in the UDF filesystem of a PS2 ISO.
 *  - Extracting a file from the ISO by name.
 *  - Replacing a file IN-PLACE inside the ISO (same LBA, same size or smaller),
 *    which is the only safe way to patch PS2 ISOs without breaking LBA-based
 *    file references that the game engine has hardcoded.
 *
 * Limitations:
 *  - Does NOT support replacing files with a larger version (would require
 *    moving data and changing LBAs, which breaks PS2 games).
 *  - Does NOT rebuild or reorganize the ISO filesystem.
 *  - Targets PS2 UDF Bridge ISOs (DVD-5/DVD-9).
 */
class ISO_File
{
public:
	class Error
	{
	public:
		Error();

	public:
		bool unableToOpen;
		bool notISO;
		bool unsupportedFormat;
		bool corruptedContent;
		bool fileNotFound;
		bool fileTooLarge; // replacement file is larger than the original slot
		bool writeFailed;
	};

	struct FileEntry
	{
		std::string name;     // filename as stored in UDF
		std::string fullPath; // full path from root, e.g. "DATA/PZS3US1.AFS"
		uint64_t size;        // file size in bytes
		uint64_t lba;         // Logical Block Address (sector number in the ISO)
		uint64_t offset;      // byte offset in the ISO file (lba * 2048)
	};

public:
	explicit ISO_File(const std::string &isoPath);

	~ISO_File();

	Error getError() const;

	bool isOpen() const;

	/* List all files in the UDF filesystem */
	const std::vector<FileEntry> &getFiles() const;

	/* Find a file by name (case-insensitive, matches any path component) */
	const FileEntry *findFile(const std::string &name) const;

	/* Extract a file from the ISO to disk */
	bool extractFile(const FileEntry &entry, const std::string &destPath, Error *outError = nullptr) const;

	/* Replace a file inside the ISO in-place.
	 * The replacement file must be <= the original file's size.
	 * The ISO is modified directly at entry.offset without changing any
	 * LBA references or moving any other data. */
	bool replaceFile(const FileEntry &entry, const std::string &srcPath, Error *outError = nullptr);

	/* Convenience: find a file by name and replace it */
	bool replaceFileByName(const std::string &name, const std::string &srcPath, Error *outError = nullptr);

	const std::string &getPath() const;

private:
	bool parse();

	bool readSector(uint64_t lba, std::vector<uint8_t> &buf) const;

	bool parseDirectory(uint64_t lba, uint64_t length, uint32_t partitionStart, const std::string &parentPath);

private:
	std::string isoPath;
	Error error;
	std::vector<FileEntry> files;
};

#endif // ISOCORE_H
