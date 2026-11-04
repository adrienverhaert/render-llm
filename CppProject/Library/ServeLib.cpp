#include "ServeLib.hpp"
#include "Generated/Scripts.hpp"

#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef OS_WINDOWS
#include <windows.h>
#endif

namespace CppProject
{
	namespace
	{
		std::mutex g_svMx;
		std::deque<std::string> g_svQueue;
		bool g_svMode = false;
		bool g_svEof = false;
	}

	// Hilo lector: bloquea en std::getline y encola cada linea completa. NO
	// toca Qt ni el estado del motor -- solo std::string bajo mutex, que es lo
	// unico seguro fuera del hilo del bucle de la app (la tabla de cadenas de
	// StringType es por hilo). Cuando stdin se cierra (el padre murio o cerro
	// la tuberia) marca EOF y termina: mi3_serve_tick lo lee como "quit".
	static void Mi3ServeReaderLoop()
	{
		std::string line;
		while (std::getline(std::cin, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
				line.pop_back();
			if (line.empty())
				continue;
			std::lock_guard<std::mutex> lk(g_svMx);
			g_svQueue.push_back(line);
		}
		std::lock_guard<std::mutex> lk(g_svMx);
		g_svEof = true;
	}

	void Mi3ServeInit(int argc, char* argv[])
	{
		for (int i = 1; i < argc; i++)
		{
			std::string a = (argv[i] != nullptr) ? argv[i] : "";
			if (a == "--serve")
				g_svMode = true;
		}

		if (!g_svMode)
			return;

		std::thread(Mi3ServeReaderLoop).detach();
	}

	bool Mi3ServeEnabled()
	{
		return g_svMode;
	}

	// ================= natives visibles desde GML =================
	// Se declaran con external_define en lib_startup.gml (bloque muerto en el
	// binario C++: CppGen solo lo usa para emitir la firma en Scripts.hpp) y
	// se invocan con external_call, que CppGen transpila a una llamada
	// directa. Mismo patron exacto que lib_file_exists y compania.
	//
	// Todas devuelven ty_real y toman ty_string/ty_real: el job JSON viaja por
	// FICHERO (lib_serve_take lo escribe, GML lo lee con json_load) y la
	// respuesta tambien (lib_serve_emit_file la lee y la escupe por stdout).
	// Asi no hace falta que ningun native devuelva cadenas, que es la unica
	// forma de external_define que este arbol no ha ejercitado nunca.

	RealType lib_serve_mode()
	{
		return g_svMode ? 1.0 : 0.0;
	}

	RealType lib_serve_pid()
	{
#ifdef OS_WINDOWS
		return (RealType)GetCurrentProcessId();
#else
		return 0.0;
#endif
	}

	// 0 = no hay trabajo; 1 = escrito un trabajo en dst; 2 = EOF de stdin
	// (el padre se fue: el worker debe salir limpio).
	RealType lib_serve_take(StringType dst)
	{
		std::string line;

		{
			std::lock_guard<std::mutex> lk(g_svMx);
			if (g_svQueue.empty())
				return g_svEof ? 2.0 : 0.0;
			line = g_svQueue.front();
			g_svQueue.pop_front();
		}

		QFile f((QString)dst);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return 0.0;
		f.write(line.c_str(), (qint64)line.size());
		f.close();
		return 1.0;
	}

	static void Mi3ServeWriteLine(QByteArray b)
	{
		b.replace('\r', ' ');
		b.replace('\n', ' ');
		if (b.size() > 0)
			fwrite(b.constData(), 1, (size_t)b.size(), stdout);
		fputc('\n', stdout);
		fflush(stdout);
	}

	RealType lib_serve_emit(StringType line)
	{
		Mi3ServeWriteLine(((QString)line).toUtf8());
		return 1.0;
	}

	RealType lib_serve_emit_file(StringType src)
	{
		QFile f((QString)src);
		if (!f.open(QIODevice::ReadOnly))
			return 0.0;
		QByteArray b = f.readAll();
		f.close();
		Mi3ServeWriteLine(b);
		return 1.0;
	}

	// Anti-huerfanos del transporte por FICHEROS (que no tiene EOF): el
	// cliente declara su pid en el arranque y el worker se suicida en cuanto
	// ese pid deja de existir. Nunca se mata por NOMBRE de proceso -- ese
	// error ya costo en v2 matar el trabajo siguiente.
	RealType lib_serve_pid_alive(RealType pid)
	{
		int p = (int)pid;
		if (p <= 0)
			return 1.0; // sin padre declarado: no hay nada que vigilar

#ifdef OS_WINDOWS
		HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)p);
		if (h == nullptr)
			return 0.0;
		DWORD w = WaitForSingleObject(h, 0);
		CloseHandle(h);
		return (w == WAIT_TIMEOUT) ? 1.0 : 0.0;
#else
		return 1.0;
#endif
	}
}
