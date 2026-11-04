#include "TextFile.hpp"
#include "Type/StringType.hpp"

namespace CppProject
{
	TextFile::TextFile(QFile* file, bool read, bool utf8) : Asset(ID_TextFile)
	{
		this->file = file;
		stream = new QTextStream(file);

		// MI3 (fix UTF-8 del canal del asistente): por defecto QTextStream decodifica
		// con el codec del SISTEMA (CP1252 en un Windows en espanol), asi que un
		// fichero UTF-8 escrito por el cerebro (JSON.stringify + appendFileSync utf8)
		// llegaba a GML con un caracter por BYTE (U+00E1 -> U+00C3 U+00A1), que es
		// el mojibake clasico. Con el codec fijado a UTF-8 la decodificacion ocurre
		// UNA vez, aqui dentro y en C++, asi que leer una linea sigue costando lo
		// mismo que antes (un indice de array).
		// setAutoDetectUnicode sigue activo: un BOM UTF-8/UTF-16 se respeta igual.
		if (utf8)
			stream->setCodec("UTF-8");

		if (read)
		{
			StringType text = stream->readAll();
			lines = text.Split('\n');
			lineWords = lines[0].split(" ");
			currentLine = currentWord = 0;
		}
	}

	TextFile::~TextFile()
	{
		deleteAndReset(stream);
		deleteAndReset(file);
	}

	StringType TextFile::ReadLine()
	{
		if (IsEof())
			return "";

		StringType line = lines[currentLine++];
		lineWords = line.Split(' ');
		currentWord = 0;
		return line;
	}

	StringType TextFile::ReadWord()
	{
		if (currentWord >= lineWords.size())
			return "";

		return lineWords[currentWord++];
	}

	bool TextFile::IsEof() const
	{
		return (currentLine >= lines.size());
	}
}