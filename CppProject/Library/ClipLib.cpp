// ClipLib.cpp
//
// RONDA UI (F1): pegar IMAGENES en el chat del asistente con Ctrl+V.
//
// El encargo daba por hecho que haria falta Win32 (OpenClipboard, CF_DIB...).
// No hace falta: el runtime de este motor es Qt5 (CMakeLists.txt, Qt en
// C:/Dev/Qt/5.15.9) y Gml/UtilFunc.cpp ya usa QApplication::clipboard() para
// texto. QClipboard trae image() y mimeData() de serie, asi que esto son dos
// funciones y CERO codigo especifico de plataforma.
//
// Estas funciones NO existen en file.dll: solo en el binario C++, donde CppGen
// convierte cada external_call en una llamada directa (ver el bloque if(false)
// de lib_startup.gml). El simbolo C++ toma el nombre de la VARIABLE GML
// (lib_clip_has_image), no el de la cadena del external_define -- mismo
// mecanismo que lib_serve_take en ServeLib.cpp.

#include "Generated/Scripts.hpp"

#include <QtGui/QClipboard>
#include <QtGui/QImage>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>

namespace CppProject
{
	// 1 si el portapapeles trae una imagen (CF_DIB/CF_DIBV5/CF_BITMAP en
	// Windows) o un fichero de imagen copiado desde el Explorador; 0 si no.
	RealType lib_clip_has_image()
	{
		const QMimeData* md = QApplication::clipboard()->mimeData();
		if (md == nullptr)
			return 0.0;
		if (md->hasImage())
			return 1.0;
		if (md->hasUrls())
		{
			const QList<QUrl> urls = md->urls();
			for (const QUrl& u : urls)
			{
				if (!u.isLocalFile())
					continue;
				const QString s = u.toLocalFile().toLower();
				if (s.endsWith(".png") || s.endsWith(".jpg") || s.endsWith(".jpeg"))
					return 1.0;
			}
		}
		return 0.0;
	}

	// Guarda la imagen del portapapeles como PNG en `path`. 1 = escrito, 0 = nada.
	RealType lib_clip_save_image(StringType path)
	{
		const QMimeData* md = QApplication::clipboard()->mimeData();
		if (md == nullptr)
			return 0.0;

		QImage img = QApplication::clipboard()->image();
		if (img.isNull() && md->hasUrls())
		{
			const QList<QUrl> urls = md->urls();
			for (const QUrl& u : urls)
			{
				if (u.isLocalFile() && img.load(u.toLocalFile()))
					break;
			}
		}
		if (img.isNull())
			return 0.0;

		// Mismo puente StringType -> QString que usa clipboard_set_text
		// (Gml/UtilFunc.cpp): StringType convierte a QString directamente.
		return img.save((QString)path, "PNG") ? 1.0 : 0.0;
	}
}
