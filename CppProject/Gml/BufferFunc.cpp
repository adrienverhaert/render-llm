#include "Generated/GmlFunc.hpp"
#include "Generated/Scripts.hpp"

#include "Asset/Buffer.hpp"
#include "Asset/Surface.hpp"

namespace CppProject
{
	void buffer_copy(IntType src, IntType srcOffset, IntType size, IntType dst, IntType dstOffset)
	{
		Buffer* srcBuf = FindBuffer(src);
		Buffer* dstBuf = FindBuffer(dst);
		if (!srcBuf || !dstBuf)
			return;

		dstBuf->data.Copy(srcBuf->data, size, srcOffset, dstOffset);
	}

	IntType buffer_create(IntType size, IntType type, IntType align)
	{
		return (new Buffer(size, type == buffer_grow, align))->id;
	}

	IntType buffer_create_from_vertex_buffer(IntType, IntType, IntType)
	{
		// Unused
		return 0;
	}

	void buffer_delete(IntType id)
	{
		if (Buffer* buf = FindBuffer(id))
			delete buf;
	}

	void buffer_fill(IntType id, IntType offset, IntType type, VarType value, IntType size)
	{
		if (Buffer* buf = FindBuffer(id))
		{
			IntType pos = offset;
			while (pos < offset + size && buf->WriteVar(type, value, pos))
				continue;
		}
	}

	IntType buffer_get_size(IntType id)
	{
		if (Buffer* buf = FindBuffer(id))
			return buf->data.Size();
		return 0;
	}

	void buffer_get_surface(IntType buffer, IntType surface, IntType)
	{
		if (Buffer* buf = FindBuffer(buffer))
		{
			if (Surface* surf = FindSurface(surface))
			{
				buf->data.Alloc(surf->size.width() * surf->size.height() * 4, false);
				surf->frameBuffer->CopyColorData(buf->data.data);
			}
		}
	}

	IntType buffer_load(StringType file)
	{
		if (QFile::exists(file))
		{
			Buffer* buf = new Buffer(file);
			if (buf->data.Size())
				return buf->id;
			else
				delete buf;
		}
		return -1;
	}

	VarType buffer_peek(IntType id, IntType offset, IntType type)
	{
		if (Buffer* buf = FindBuffer(id))
			return buf->ReadVar(type, offset);
		
		return VarType();
	}

	void buffer_poke(IntType id, IntType offset, VarType type, IntType value)
	{
		if (Buffer* buf = FindBuffer(id))
			buf->WriteVar(type, value, offset);
	}

	VarType buffer_read(IntType id, IntType type)
	{
		if (Buffer* buf = FindBuffer(id))
			return buf->ReadVar(type, buf->pos);

		return VarType();
	}

	void buffer_resize(IntType id, IntType newSize)
	{
		if (Buffer* buf = FindBuffer(id))
			buf->data.Alloc(newSize);
	}

	void buffer_save(IntType id, StringType file)
	{
		if (Buffer* buf = FindBuffer(id))
			buf->Save(file);
	}

	void buffer_seek(IntType id, IntType base, IntType offset)
	{
		if (Buffer* buf = FindBuffer(id))
		{
			if (base == buffer_seek_start)
				buf->pos = offset;
			else if (base == buffer_seek_relative)
				buf->pos += offset;
			else if (base == buffer_seek_end)
				buf->pos = buf->data.Size() + offset;
		}
	}

	IntType buffer_tell(IntType id)
	{
		if (Buffer* buf = FindBuffer(id))
			return buf->pos;
		return -1;
	}

	IntType buffer_write(IntType id, IntType type, VarType value)
	{
		if (Buffer* buf = FindBuffer(id))
			if (buf->WriteVar(type, value, buf->pos))
				return 0;
		return -1;
	}

	IntType buffer_fast_peek_u8(IntType id, IntType offset)
	{
		if (Buffer* buf = FindBuffer(id))
			return CAST_BITS(uchar, buf->data[offset]);
		return 0;
	}

	IntType buffer_fast_peek_s32(IntType id, IntType offset)
	{
		if (Buffer* buf = FindBuffer(id))
			return CAST_BITS(int32_t, buf->data[offset]);
		return 0;
	}

	void buffer_fast_poke_u8(IntType id, IntType offset, IntType value)
	{
		if (Buffer* buf = FindBuffer(id))
			CAST_BITS(uchar, buf->data[offset]) = (uchar)value;
	}

	void buffer_fast_poke_s32(IntType id, IntType offset, IntType value)
	{
		if (Buffer* buf = FindBuffer(id))
			CAST_BITS(int32_t, buf->data[offset]) = (int32_t)value;
	}

	// MI3 (fix/utf8-buffer): GML strings are UTF-16 (QString); buffers/files must
	// receive them as UTF-8. The old path wrote ONE byte per UTF-16 code unit
	// (truncation: U+2714 -> 0x14, U+00F1 -> a lone 0xF1 byte), which produced
	// files that were not valid UTF-8 (headless_result.json, checkpoints.json).
	// Writing now encodes UTF-8; reading decodes strict UTF-8 and, when the bytes
	// are NOT valid UTF-8 (old files written by the truncating path, or legacy
	// MI 1.x data), falls back to Latin-1 so old projects keep opening (their
	// non-ASCII characters > U+00FF were already lost at write time).

	// Returns true if the byte array is well-formed UTF-8 (strict: no overlongs,
	// no surrogates, no codepoints above U+10FFFF).
	static bool IsValidUtf8(const QByteArray& bytes)
	{
		const uchar* p = (const uchar*)bytes.constData();
		IntType len = bytes.size(), i = 0;
		while (i < len)
		{
			uchar b = p[i];
			if (b < 0x80) { i++; continue; }

			int n; uint cp;
			if ((b & 0xE0) == 0xC0) { n = 1; cp = b & 0x1F; }
			else if ((b & 0xF0) == 0xE0) { n = 2; cp = b & 0x0F; }
			else if ((b & 0xF8) == 0xF0) { n = 3; cp = b & 0x07; }
			else return false;

			if (i + n >= len)
				return false;
			for (int k = 1; k <= n; k++)
			{
				uchar c = p[i + k];
				if ((c & 0xC0) != 0x80)
					return false;
				cp = (cp << 6) | (c & 0x3F);
			}

			// Overlong encodings, UTF-16 surrogates and out-of-range codepoints
			if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000))
				return false;
			if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
				return false;
			i += n + 1;
		}
		return true;
	}

	// Decodes bytes read from a buffer into a GML string (UTF-8, with Latin-1
	// fallback for files predating the UTF-8 fix).
	static StringType DecodeBufferString(const QByteArray& bytes)
	{
		if (IsValidUtf8(bytes))
			return QString::fromUtf8(bytes);
		return QString::fromLatin1(bytes);
	}

	// Appends raw bytes at the buffer position, growing the data like the old
	// buffer_write_string did.
	static void WriteBufferBytes(Buffer* buf, const QByteArray& bytes)
	{
		IntType len = bytes.size();
		if (buf->pos + len > buf->data.Size()) // Allocate data
			buf->data.Alloc(buf->pos + len);

		for (IntType i = 0; i < len; i++)
			buf->data[buf->pos++] = (uchar)bytes.at(i);
	}

	void buffer_write_string(StringType arg)
	{
		if (Buffer* buf = FindBuffer(global::buffer_current))
			WriteBufferBytes(buf, arg.ToUtf8());
	}

	void buffer_write_string_int(StringType arg)
	{
		if (Buffer* buf = FindBuffer(global::buffer_current))
		{
			QByteArray utf8 = arg.ToUtf8();
			buf->WriteVar(buffer_s32, (IntType)utf8.size(), buf->pos); // Length in BYTES
			WriteBufferBytes(buf, utf8);
		}
	}

	// Reads 'len' raw bytes from the buffer position (clamped to the remaining
	// data, like the old byte-per-byte loop which returned 0 past the end).
	static QByteArray ReadBufferBytes(Buffer* buf, IntType len)
	{
		if (len < 0)
			len = 0;
		IntType remaining = buf->data.Size() - buf->pos;
		if (len > remaining)
			len = std::max<IntType>(remaining, 0);

		QByteArray bytes((const char*)(buf->data.data + buf->pos), len);
		buf->pos += len;
		return bytes;
	}

	StringType buffer_read_string_int()
	{
		if (Buffer* buf = FindBuffer(global::buffer_current))
		{
			IntType len = buf->ReadVar(buffer_s32, buf->pos).ToInt();
			return DecodeBufferString(ReadBufferBytes(buf, len));
		}
		return "";
	}

	StringType buffer_read_string_short_be()
	{
		if (Buffer* buf = FindBuffer(global::buffer_current))
		{
			IntType hi = buf->ReadVar(buffer_u8, buf->pos).ToInt();
			IntType lo = buf->ReadVar(buffer_u8, buf->pos).ToInt();
			return DecodeBufferString(ReadBufferBytes(buf, hi * 256 + lo));
		}
		return "";
	}
}