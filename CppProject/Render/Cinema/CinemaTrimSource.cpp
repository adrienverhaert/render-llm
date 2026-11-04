#include "CinemaTrimSource.hpp"

#include "CinemaKernelM0.hpp"
#include "CinemaTrimGen.hpp"

#include <QString>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace CppProject
{
	namespace
	{
		const char* const TRIM_MARKER = "// @@TRIM@@";

		// Cache por conjunto de efectos. Los fuentes compuestos son de unos
		// pocos KB y el numero de conjuntos distintos que ve una sesion es
		// diminuto (cambiar el efecto de una pieza = un conjunto nuevo), asi
		// que se guardan y no se liberan hasta ClearCache: los backends se
		// quedan con el puntero mientras compilan.
		std::map<uint64_t, std::string>& Cache()
		{
			static std::map<uint64_t, std::string> cache;
			return cache;
		}

		IntType g_composeCount = 0;

		const CinemaTrimGen::Effect* FindEffect(uint32_t slot)
		{
			const CinemaTrimGen::Effect* table = CinemaTrimGen::Table();
			for (unsigned int i = 0; i < CinemaTrimGen::COUNT; i++)
				if (table[i].slot == slot)
					return &table[i];
			return nullptr;
		}

		std::string FloatLit(float v)
		{
			// Literal con precision suficiente para round-trip de float32.
			// El valor sale del catalogo y NO puede perder digitos por el
			// camino: si lo hiciera, el A/B contra el raster mediria otra cosa.
			char buf[64];
			snprintf(buf, sizeof(buf), "%.9g", (double)v);
			std::string out(buf);
			if (out.find('.') == std::string::npos && out.find('e') == std::string::npos)
				out += ".0";
			return out;
		}

		std::string BuildTrimBlock(const CinemaTrimSet& fxSet)
		{
			std::string out;
			out += "#define CINEMA_TRIM 1\n";
			out += CinemaTrimGen::RUNTIME;
			out += "\n\n";

			// Ganancia de calibracion por efecto. Se emite como cadena de ifs
			// sobre los efectos PRESENTES: con uno solo, el compilador la
			// pliega a una constante.
			out += "float TrimCinemaGain(uint uTrimFx)\n{\n";
			for (size_t i = 0; i < fxSet.size(); i++)
			{
				const CinemaTrimGen::Effect* fx = FindEffect(fxSet[i]);
				if (!fx)
					continue;
				out += "\tif (uTrimFx == ";
				out += std::to_string(fx->slot);
				out += "u) return ";
				out += FloatLit(fx->cinemaGain);
				out += "; // ";
				out += fx->name;
				out += "\n";
			}
			out += "\treturn 1.0;\n}\n\n";

			out += "float3 TrimAddDispatch(uint uTrimFx, float3 base, float2 p, float2 tx,\n";
			out += "                       float2 uv, float2 uTextureSize, float uTrimSceneLight)\n";
			out += "{\n\tfloat3 add = float3(0.0, 0.0, 0.0);\n";
			bool first = true;
			for (size_t i = 0; i < fxSet.size(); i++)
			{
				const CinemaTrimGen::Effect* fx = FindEffect(fxSet[i]);
				if (!fx)
					continue;
				out += "\t";
				if (!first)
					out += "else ";
				out += "if (uTrimFx == ";
				out += std::to_string(fx->slot);
				out += "u) // ";
				out += fx->name;
				out += "\n\t{\n";
				out += fx->body;
				out += "\n\t}\n";
				first = false;
			}
			out += "\treturn add;\n}\n";
			return out;
		}
	}

	uint64_t CinemaTrimSetKey(const CinemaTrimSet& fxSet)
	{
		if (fxSet.empty())
			return 0;
		uint64_t h = 1469598103934665603ull;
		for (size_t i = 0; i < fxSet.size(); i++)
		{
			uint32_t v = fxSet[i];
			for (int b = 0; b < 4; b++)
			{
				h ^= (uint64_t)((v >> (b * 8)) & 0xFFu);
				h *= 1099511628211ull;
			}
		}
		// Nunca colisionar con "sin efectos", que es la clave 0 y significa
		// literalmente "el fuente de siempre".
		return h ? h : 1ull;
	}

	const char* CinemaTrimComposeSource(const CinemaTrimSet& fxSet)
	{
		uint64_t key = CinemaTrimSetKey(fxSet);
		if (key == 0)
			return CinemaKernelM0::SOURCE; // conjunto vacio: el fuente TAL CUAL

		std::map<uint64_t, std::string>& cache = Cache();
		std::map<uint64_t, std::string>::iterator it = cache.find(key);
		if (it != cache.end())
			return it->second.c_str();

		std::string src = CinemaKernelM0::SOURCE;
		size_t at = src.find(TRIM_MARKER);
		if (at == std::string::npos)
		{
			// El marcador es contrato: sin el, el trim no puede entrar y lo
			// honesto es renderizar SIN trim, no adivinar donde inyectar.
			WARNING(QString("Cinema trim: no encuentro el marcador ") + TRIM_MARKER
				+ " en el kernel; el trim NO se evalua en este render");
			return CinemaKernelM0::SOURCE;
		}

		std::string block = BuildTrimBlock(fxSet);
		src.replace(at, strlen(TRIM_MARKER), block);
		g_composeCount++;
		cache[key] = src;
		return cache[key].c_str();
	}

	IntType CinemaTrimComposeCount()
	{
		return g_composeCount;
	}

	void CinemaTrimClearCache()
	{
		Cache().clear();
	}
}
