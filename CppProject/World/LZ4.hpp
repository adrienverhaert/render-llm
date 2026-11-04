#pragma once

#include "Common.hpp"

namespace CppProject
{
	struct LZ4
	{
		// Decompress a lz4-java LZ4BlockOutputStream frame (region compression type 4) into the given byte array, returning whether successful.
		static BoolType Decompress(const char* src, IntType size, QByteArray& dst);
	};
}
