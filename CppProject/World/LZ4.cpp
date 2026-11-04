#include "LZ4.hpp"

#include <cstring>

namespace CppProject
{
	// Decompress a single raw LZ4 block (sequences of literals + back-references) into dst, returning whether successful.
	static BoolType DecompressRawBlock(const uint8_t* src, IntType srcSize, uint8_t* dst, IntType dstSize)
	{
		IntType srcPos = 0, dstPos = 0;
		while (srcPos < srcSize)
		{
			uint8_t token = src[srcPos++];

			// Literal length (extended by 0xFF bytes)
			IntType literalLength = token >> 4;
			if (literalLength == 15)
			{
				uint8_t add;
				do
				{
					if (srcPos >= srcSize)
						return false;
					add = src[srcPos++];
					literalLength += add;
				} while (add == 255);
			}

			// Copy literals
			if (literalLength < 0 || srcPos + literalLength > srcSize || dstPos + literalLength > dstSize)
				return false;
			memcpy(dst + dstPos, src + srcPos, literalLength);
			srcPos += literalLength;
			dstPos += literalLength;

			// The last sequence has no match, only literals
			if (srcPos >= srcSize)
				break;

			// Match offset (2 bytes, little endian, distance back into the output)
			if (srcPos + 2 > srcSize)
				return false;
			IntType offset = src[srcPos] | (src[srcPos + 1] << 8);
			srcPos += 2;
			if (offset == 0 || offset > dstPos)
				return false;

			// Match length (extended by 0xFF bytes, minimum match is 4)
			IntType matchLength = token & 15;
			if (matchLength == 15)
			{
				uint8_t add;
				do
				{
					if (srcPos >= srcSize)
						return false;
					add = src[srcPos++];
					matchLength += add;
				} while (add == 255);
			}
			matchLength += 4;

			// Copy match byte by byte since it may overlap the destination cursor
			if (dstPos + matchLength > dstSize)
				return false;
			IntType matchPos = dstPos - offset;
			for (IntType i = 0; i < matchLength; i++)
				dst[dstPos++] = dst[matchPos++];
		}
		return true;
	}

	BoolType LZ4::Decompress(const char* src, IntType size, QByteArray& dst)
	{
		static const char MAGIC[8] = { 'L', 'Z', '4', 'B', 'l', 'o', 'c', 'k' };
		const uint8_t COMPRESSION_METHOD_RAW = 0x10;
		const uint8_t COMPRESSION_METHOD_LZ4 = 0x20;
		const IntType HEADER_LENGTH = 8 + 1 + 4 + 4 + 4; // magic + token + compressed length + original length + checksum

		const uint8_t* in = (const uint8_t*)src;
		IntType pos = 0;
		dst.clear();

		while (pos + HEADER_LENGTH <= size)
		{
			if (memcmp(in + pos, MAGIC, 8) != 0)
			{
				WARNING("Invalid LZ4 block magic");
				return false;
			}
			pos += 8;

			uint8_t compressionMethod = in[pos++] & 0xF0;
			IntType compressedLength = in[pos] | (in[pos + 1] << 8) | (in[pos + 2] << 16) | ((IntType)in[pos + 3] << 24);
			pos += 4;
			IntType originalLength = in[pos] | (in[pos + 1] << 8) | (in[pos + 2] << 16) | ((IntType)in[pos + 3] << 24);
			pos += 4;
			pos += 4; // Skip checksum

			// End of stream mark
			if (originalLength == 0)
				break;

			if (compressedLength < 0 || originalLength < 0 || pos + compressedLength > size)
			{
				WARNING("Truncated LZ4 block");
				return false;
			}

			IntType dstOffset = dst.size();
			dst.resize(dstOffset + originalLength);

			if (compressionMethod == COMPRESSION_METHOD_RAW)
			{
				if (compressedLength != originalLength)
				{
					WARNING("Invalid raw LZ4 block");
					return false;
				}
				memcpy(dst.data() + dstOffset, in + pos, originalLength);
			}
			else if (compressionMethod == COMPRESSION_METHOD_LZ4)
			{
				if (!DecompressRawBlock(in + pos, compressedLength, (uint8_t*)dst.data() + dstOffset, originalLength))
				{
					WARNING("Error decompressing LZ4 block");
					return false;
				}
			}
			else
			{
				WARNING("Unknown LZ4 compression method");
				return false;
			}

			pos += compressedLength;
		}

		return true;
	}
}
