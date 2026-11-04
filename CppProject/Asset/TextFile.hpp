#pragma once
#include "Asset.hpp"

namespace CppProject
{
	// Opened text file asset
	struct TextFile : Asset
	{
		// MI3 (fix UTF-8 del canal del asistente): `utf8` fuerza la decodificacion
		// UTF-8 del contenido en vez del codec del sistema (CP1252 en Windows),
		// que convertia cada byte de una tilde en un caracter suelto.
		TextFile(QFile* file, bool read, bool utf8 = false);
		~TextFile();

		StringType ReadLine();
		StringType ReadWord();
		bool IsEof() const;

		QFile* file = nullptr;
		QTextStream* stream = nullptr;
		QStringList lines;
		QStringList lineWords;
		IntType currentLine = 0;
		IntType currentWord = 0;
	};
}