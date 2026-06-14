#include <ISOCore.h>

#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace
{
	constexpr uint64_t SECTOR_SIZE = 2048;
	constexpr uint64_t AVDP_SECTOR = 256; // Anchor Volume Descriptor Pointer

	/* Read little-endian integers from a byte buffer */
	inline uint16_t readLE16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
	inline uint32_t readLE32(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
	inline uint64_t readLE64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

	/* UDF descriptor tag (16 bytes, at start of every UDF descriptor) */
	struct Tag
	{
		uint16_t tagId;
		uint16_t descVersion;
		uint8_t  checksum;
		uint8_t  reserved;
		uint16_t serialNum;
		uint16_t descCRC;
		uint16_t descCRCLen;
		uint32_t tagLoc;
	};

	Tag readTag(const uint8_t *p)
	{
		Tag t;
		t.tagId      = readLE16(p + 0);
		t.descVersion= readLE16(p + 2);
		t.checksum   = p[4];
		t.reserved   = p[5];
		t.serialNum  = readLE16(p + 6);
		t.descCRC    = readLE16(p + 8);
		t.descCRCLen = readLE16(p + 10);
		t.tagLoc     = readLE32(p + 12);
		return t;
	}

	enum TagId : uint16_t
	{
		TAG_PRIMARY_VOLUME_DESC  = 1,
		TAG_ANCHOR_VOLUME_DESC   = 2,
		TAG_VOLUME_DESC_PTR      = 3,
		TAG_IMP_USE_VOLUME_DESC  = 4,
		TAG_PARTITION_DESC       = 5,
		TAG_LOGICAL_VOLUME_DESC  = 6,
		TAG_UNALLOCATED_SPACE    = 7,
		TAG_TERMINATING_DESC     = 8,
		TAG_LOGICAL_VOLUME_INT   = 9,
		TAG_FILE_SET_DESC        = 256,
		TAG_FILE_IDENT_DESC      = 257,
		TAG_FILE_ENTRY           = 260,
		TAG_EXTENDED_FILE_ENTRY  = 266,
	};

	/* UDF "long_ad" (16 bytes): length + LBA location */
	struct LongAD
	{
		uint32_t length;
		uint32_t lba;
		uint16_t partRef;
		uint8_t  implUse[6];
	};

	LongAD readLongAD(const uint8_t *p)
	{
		LongAD a;
		a.length  = readLE32(p + 0);
		a.lba     = readLE32(p + 4);
		a.partRef = readLE16(p + 8);
		memcpy(a.implUse, p + 10, 6);
		return a;
	}

	/* UDF "short_ad" (8 bytes) */
	struct ShortAD
	{
		uint32_t length;
		uint32_t lba;
	};

	ShortAD readShortAD(const uint8_t *p)
	{
		ShortAD a;
		a.length = readLE32(p + 0);
		a.lba    = readLE32(p + 4);
		return a;
	}

	/* Convert a UDF OSTA CS0 dstring to a plain std::string.
	 * Byte 0 is the compression ID (8 = 8-bit chars, 16 = 16-bit BE).
	 * The last byte of the field is the length of the string (0 = empty). */
	std::string dstringToString(const uint8_t *p, size_t fieldLen)
	{
		if (fieldLen < 2) return {};
		uint8_t len = p[fieldLen - 1]; // length byte at the end
		if (len == 0 || len > fieldLen - 1) return {};

		uint8_t compId = p[0];
		std::string result;

		if (compId == 8) {
			for (uint8_t i = 1; i <= len - 1 && i < fieldLen; ++i) {
				result += static_cast<char>(p[i]);
			}
		}
		else if (compId == 16) {
			// 16-bit big-endian Unicode — keep only the low byte (ASCII range)
			for (uint8_t i = 1; i + 1 < static_cast<uint8_t>(len) && static_cast<size_t>(i + 1) < fieldLen; i += 2) {
				uint16_t wchar = (static_cast<uint16_t>(p[i]) << 8) | p[i + 1];
				result += static_cast<char>(wchar & 0xFF);
			}
		}

		return result;
	}

	/* Read a UDF File Identifier Descriptor filename field.
	 * The OSTA CS0 identifier starts at p[0], fieldLen bytes total. */
	std::string readFIDName(const uint8_t *p, uint8_t len)
	{
		if (len == 0) return {};
		uint8_t compId = p[0];
		std::string result;

		if (compId == 8) {
			for (uint8_t i = 1; i < len; ++i) {
				result += static_cast<char>(p[i]);
			}
		}
		else if (compId == 16) {
			for (uint8_t i = 1; i + 1 < len; i += 2) {
				uint16_t wchar = (static_cast<uint16_t>(p[i]) << 8) | p[i + 1];
				result += static_cast<char>(wchar & 0xFF);
			}
		}

		return result;
	}

	std::string toLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
		return s;
	}
} // namespace

ISO_File::Error::Error() : unableToOpen(false), notISO(false), unsupportedFormat(false), corruptedContent(false), fileNotFound(false), fileTooLarge(false), writeFailed(false)
{
}

ISO_File::ISO_File(const std::string &isoPath) : isoPath(isoPath)
{
	parse();
}

ISO_File::~ISO_File() = default;

ISO_File::Error ISO_File::getError() const { return error; }

bool ISO_File::isOpen() const
{
	return !error.unableToOpen && !error.notISO && !error.unsupportedFormat && !error.corruptedContent;
}

const std::vector<ISO_File::FileEntry> &ISO_File::getFiles() const { return files; }

const std::string &ISO_File::getPath() const { return isoPath; }

bool ISO_File::readSector(uint64_t lba, std::vector<uint8_t> &buf) const
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) return false;

	f.seekg(static_cast<std::streamoff>(lba * SECTOR_SIZE));
	buf.resize(SECTOR_SIZE, 0);
	f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(SECTOR_SIZE));

	return !f.fail();
}

bool ISO_File::parse()
{
	std::ifstream f(isoPath, std::ios::binary);
	if (!f.is_open()) {
		error.unableToOpen = true;
		return false;
	}
	f.close();

	// Verify it's a PS2 UDF ISO by looking for "NSR02" or "NSR03" at sector 17
	std::vector<uint8_t> sec;
	if (!readSector(17, sec)) {
		error.notISO = true;
		return false;
	}

	bool hasNSR = (memcmp(sec.data() + 1, "NSR0", 4) == 0);
	if (!hasNSR) {
		// also check sector 16 and 18
		for (uint64_t s : {16ULL, 18ULL}) {
			if (readSector(s, sec) && memcmp(sec.data() + 1, "NSR0", 4) == 0) {
				hasNSR = true;
				break;
			}
		}
	}

	if (!hasNSR) {
		error.notISO = true;
		return false;
	}

	// Read AVDP at sector 256
	if (!readSector(AVDP_SECTOR, sec)) {
		error.corruptedContent = true;
		return false;
	}

	Tag avdpTag = readTag(sec.data());
	if (avdpTag.tagId != TAG_ANCHOR_VOLUME_DESC) {
		error.corruptedContent = true;
		return false;
	}

	// AVDP: bytes 16-23 = Main VDS extent (length + lba)
	uint32_t mainVDSLBA = readLE32(sec.data() + 20);
	uint32_t mainVDSLen = readLE32(sec.data() + 16);
	(void)mainVDSLen;

	// Walk the Main Volume Descriptor Sequence looking for:
	//   - Partition Descriptor (to get partitionStart)
	//   - Logical Volume Descriptor (to get Fileset LBA)
	uint32_t partitionStart = 0;
	uint32_t filesetLBA = 0;
	uint32_t filesetPartRef = 0;
	bool foundPartition = false;
	bool foundLVD = false;

	for (uint32_t i = 0; i < 32; ++i) {
		if (!readSector(mainVDSLBA + i, sec)) break;

		Tag tag = readTag(sec.data());

		if (tag.tagId == TAG_TERMINATING_DESC) break;

		if (tag.tagId == TAG_PARTITION_DESC) {
			// Partition Descriptor: bytes 188..191 = Access Type, 188 = part flags
			// Partition start LBA at offset 188
			partitionStart = readLE32(sec.data() + 188);
			foundPartition = true;
		}
		else if (tag.tagId == TAG_LOGICAL_VOLUME_DESC) {
			// LVD: Logical Volume Contents Use (domain identifier)
			// Map Table at variable offset; instead find the Integrity Sequence
			// and then the Fileset via the Partition Maps.
			// Simpler: integrity desc pointer is at bytes 432..447 (LongAD).
			// Actually, the LVD contains partition maps that list the Fileset.
			// For PS2 UDF the fileset is at partitionStart + (IC field).
			// LVD Map Table length at byte 248, Map Table data at byte 440.

			uint32_t mapTableLen = readLE32(sec.data() + 248);
			// The IntegritySeq is at 432 (LongAD 16 bytes), then Implementation fields
			// then partition maps start at 440.
			// For Type 1 partition map (8 bytes): 00 01 08 00 .. partRef(2) volSeq(2)
			// We just take the partition #0 map which tells us the partition number.
			// The Fileset LBA comes from the LVD LogicalVolumeContentsUse at byte 72.
			LongAD lvContents = readLongAD(sec.data() + 72);
			filesetLBA    = lvContents.lba;
			filesetPartRef = lvContents.partRef;
			foundLVD = true;
			(void)mapTableLen;
			(void)filesetPartRef;
		}
	}

	if (!foundPartition || !foundLVD) {
		error.corruptedContent = true;
		return false;
	}

	// Read Fileset Descriptor at (partitionStart + filesetLBA)
	uint64_t fsdAbsLBA = static_cast<uint64_t>(partitionStart) + filesetLBA;
	if (!readSector(fsdAbsLBA, sec)) {
		error.corruptedContent = true;
		return false;
	}

	Tag fsdTag = readTag(sec.data());
	if (fsdTag.tagId != TAG_FILE_SET_DESC) {
		error.corruptedContent = true;
		return false;
	}

	// Fileset Descriptor: Root Dir ICB at bytes 400..415 (LongAD)
	LongAD rootIcb = readLongAD(sec.data() + 400);
	uint64_t rootDirAbsLBA = static_cast<uint64_t>(partitionStart) + rootIcb.lba;

	// Recursively parse directory tree
	if (!readSector(rootDirAbsLBA, sec)) {
		error.corruptedContent = true;
		return false;
	}

	Tag rootTag = readTag(sec.data());
	uint64_t rootDirDataLBA = 0;
	uint64_t rootDirDataLen = 0;

	if (rootTag.tagId == TAG_FILE_ENTRY || rootTag.tagId == TAG_EXTENDED_FILE_ENTRY) {
		// File Entry: Information Length at 56, AD type at 18 (lower 2 bits), ADs at 176+EA_length
		uint8_t icbFlags = sec[18];
		uint8_t adType = icbFlags & 0x07;
		uint32_t eaLen = readLE32(sec.data() + 168);
		uint32_t adLen = readLE32(sec.data() + 172);

		const uint8_t *adStart = sec.data() + 176 + eaLen;

		if (adType == 0) {
			// Short ADs
			for (uint32_t off = 0; off + 8 <= adLen; off += 8) {
				ShortAD ad = readShortAD(adStart + off);
				if (ad.length > 0) {
					rootDirDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba;
					rootDirDataLen = ad.length;
					break;
				}
			}
		}
		else if (adType == 1) {
			// Long ADs
			for (uint32_t off = 0; off + 16 <= adLen; off += 16) {
				LongAD ad = readLongAD(adStart + off);
				if (ad.length > 0) {
					rootDirDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba;
					rootDirDataLen = ad.length;
					break;
				}
			}
		}
	}

	if (rootDirDataLBA == 0) {
		error.corruptedContent = true;
		return false;
	}

	parseDirectory(rootDirDataLBA, rootDirDataLen, partitionStart, "");

	return true;
}

bool ISO_File::parseDirectory(uint64_t lba, uint64_t length, uint32_t partitionStart, const std::string &parentPath)
{
	uint64_t bytesRemaining = length;
	uint64_t currentLBA = lba;

	while (bytesRemaining > 0) {
		std::vector<uint8_t> sec;
		if (!readSector(currentLBA, sec)) break;

		uint64_t sectorBytesLeft = std::min(bytesRemaining, SECTOR_SIZE);
		uint64_t pos = 0;

		while (pos + 38 <= sectorBytesLeft) {
			const uint8_t *fid = sec.data() + pos;

			Tag tag = readTag(fid);
			if (tag.tagId != TAG_FILE_IDENT_DESC) {
				pos += 4; // skip non-FID data
				continue;
			}

			uint8_t fileCharacteristics = fid[18];
			uint8_t fileIdentLen        = fid[19];
			LongAD  icb                 = readLongAD(fid + 20);
			uint16_t implUseLen         = readLE16(fid + 36);

			// Total FID length must be padded to 4-byte boundary
			uint64_t fidLen = 38 + implUseLen + fileIdentLen;
			fidLen = (fidLen + 3) & ~3ULL;

			bool isDirectory = (fileCharacteristics & 0x02) != 0;
			bool isParent    = (fileCharacteristics & 0x08) != 0;

			if (!isParent && fileIdentLen > 0) {
				const uint8_t *namePtr = fid + 38 + implUseLen;
				std::string name = readFIDName(namePtr, fileIdentLen);

				if (!name.empty() && icb.length > 0) {
					std::string fullPath = parentPath.empty() ? name : (parentPath + "/" + name);
					uint64_t icbAbsLBA = static_cast<uint64_t>(partitionStart) + icb.lba;

					if (isDirectory) {
						// Read the File Entry for this directory to get its data LBA
						std::vector<uint8_t> feData;
						if (readSector(icbAbsLBA, feData)) {
							Tag feTag = readTag(feData.data());
							if (feTag.tagId == TAG_FILE_ENTRY || feTag.tagId == TAG_EXTENDED_FILE_ENTRY) {
								uint8_t adType = feData[18] & 0x07;
								uint32_t eaLen = readLE32(feData.data() + 168);
								uint32_t adLen = readLE32(feData.data() + 172);
								const uint8_t *adStart = feData.data() + 176 + eaLen;

								uint64_t dirDataLBA = 0;
								uint64_t dirDataLen = 0;

								if (adType == 0) {
									for (uint32_t off = 0; off + 8 <= adLen; off += 8) {
										ShortAD ad = readShortAD(adStart + off);
										if (ad.length > 0) { dirDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; dirDataLen = ad.length; break; }
									}
								}
								else if (adType == 1) {
									for (uint32_t off = 0; off + 16 <= adLen; off += 16) {
										LongAD ad = readLongAD(adStart + off);
										if (ad.length > 0) { dirDataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; dirDataLen = ad.length; break; }
									}
								}

								if (dirDataLBA > 0) {
									parseDirectory(dirDataLBA, dirDataLen, partitionStart, fullPath);
								}
							}
						}
					}
					else {
						// It's a file - read its File Entry to get size and data LBA
						std::vector<uint8_t> feData;
						if (readSector(icbAbsLBA, feData)) {
							Tag feTag = readTag(feData.data());
							if (feTag.tagId == TAG_FILE_ENTRY || feTag.tagId == TAG_EXTENDED_FILE_ENTRY) {
								uint64_t fileSize = readLE64(feData.data() + 56);
								uint8_t adType = feData[18] & 0x07;
								uint32_t eaLen = readLE32(feData.data() + 168);
								uint32_t adLen = readLE32(feData.data() + 172);
								const uint8_t *adStart = feData.data() + 176 + eaLen;

								uint64_t dataLBA = 0;

								if (adType == 0) {
									for (uint32_t off = 0; off + 8 <= adLen; off += 8) {
										ShortAD ad = readShortAD(adStart + off);
										if (ad.length > 0) { dataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; break; }
									}
								}
								else if (adType == 1) {
									for (uint32_t off = 0; off + 16 <= adLen; off += 16) {
										LongAD ad = readLongAD(adStart + off);
										if (ad.length > 0) { dataLBA = static_cast<uint64_t>(partitionStart) + ad.lba; break; }
									}
								}

								if (dataLBA > 0) {
									FileEntry entry;
									entry.name     = name;
									entry.fullPath = fullPath;
									entry.size     = fileSize;
									entry.lba      = dataLBA;
									entry.offset   = dataLBA * SECTOR_SIZE;
									files.push_back(entry);
								}
							}
						}
					}
				}
			}

			if (fidLen == 0) break;
			pos += fidLen;
		}

		bytesRemaining -= std::min(bytesRemaining, SECTOR_SIZE);
		++currentLBA;
	}

	return true;
}

const ISO_File::FileEntry *ISO_File::findFile(const std::string &name) const
{
	std::string lowerName = toLower(name);

	for (const auto &entry : files) {
		if (toLower(entry.name) == lowerName || toLower(entry.fullPath) == lowerName) {
			return &entry;
		}
	}

	return nullptr;
}

bool ISO_File::extractFile(const FileEntry &entry, const std::string &destPath, Error *outError) const
{
	Error localError;

	std::ifstream src(isoPath, std::ios::binary);
	if (!src.is_open()) {
		localError.unableToOpen = true;
		if (outError) *outError = localError;
		return false;
	}

	std::ofstream dst(destPath, std::ios::binary | std::ios::trunc);
	if (!dst.is_open()) {
		localError.writeFailed = true;
		if (outError) *outError = localError;
		return false;
	}

	src.seekg(static_cast<std::streamoff>(entry.offset));

	uint64_t remaining = entry.size;
	std::vector<char> buf(SECTOR_SIZE);

	while (remaining > 0) {
		uint64_t toRead = std::min(remaining, SECTOR_SIZE);
		src.read(buf.data(), static_cast<std::streamsize>(toRead));
		dst.write(buf.data(), static_cast<std::streamsize>(toRead));
		remaining -= toRead;
	}

	src.close();
	dst.close();

	if (outError) *outError = localError;
	return !dst.fail();
}

bool ISO_File::replaceFile(const FileEntry &entry, const std::string &srcPath, Error *outError)
{
	Error localError;

	// Check source file size
	std::ifstream src(srcPath, std::ios::binary | std::ios::ate);
	if (!src.is_open()) {
		localError.unableToOpen = true;
		if (outError) *outError = localError;
		return false;
	}

	uint64_t srcSize = static_cast<uint64_t>(src.tellg());
	src.seekg(0);

	// The replacement file must fit within the space allocated in the ISO.
	// Space allocated = ceil(originalSize / 2048) * 2048 (sector-aligned slot).
	uint64_t allocatedBytes = ((entry.size + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;

	if (srcSize > allocatedBytes) {
		localError.fileTooLarge = true;
		if (outError) *outError = localError;
		src.close();
		return false;
	}

	// Open the ISO for writing at the file's sector offset
	std::fstream iso(isoPath, std::ios::in | std::ios::out | std::ios::binary);
	if (!iso.is_open()) {
		localError.writeFailed = true;
		if (outError) *outError = localError;
		src.close();
		return false;
	}

	iso.seekp(static_cast<std::streamoff>(entry.offset));

	// Write the new file content
	std::vector<char> buf(SECTOR_SIZE, 0);
	uint64_t remaining = srcSize;

	while (remaining > 0) {
		uint64_t toRead = std::min(remaining, SECTOR_SIZE);
		std::fill(buf.begin(), buf.end(), 0);
		src.read(buf.data(), static_cast<std::streamsize>(toRead));
		iso.write(buf.data(), static_cast<std::streamsize>(SECTOR_SIZE));
		remaining -= toRead;
	}

	// Pad remaining allocated sectors with zeros
	uint64_t writtenSectors = (srcSize + SECTOR_SIZE - 1) / SECTOR_SIZE;
	uint64_t totalSectors = allocatedBytes / SECTOR_SIZE;

	std::fill(buf.begin(), buf.end(), 0);
	for (uint64_t s = writtenSectors; s < totalSectors; ++s) {
		iso.write(buf.data(), static_cast<std::streamsize>(SECTOR_SIZE));
	}

	src.close();
	iso.close();

	if (outError) *outError = localError;
	return !iso.fail();
}

bool ISO_File::replaceFileByName(const std::string &name, const std::string &srcPath, Error *outError)
{
	const FileEntry *entry = findFile(name);

	if (entry == nullptr) {
		if (outError) {
			outError->fileNotFound = true;
		}
		return false;
	}

	return replaceFile(*entry, srcPath, outError);
}
