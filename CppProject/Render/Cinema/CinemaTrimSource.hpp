#pragma once

#include "Common.hpp"

#include <vector>

// ---------------------------------------------------------------------------
// CinemaTrimSource -- compositor del fuente HLSL del kernel con SOLO los
// efectos de trim que la escena usa de verdad (D10 del spec de la ronda).
//
// El kernel lleva un marcador `// @@TRIM@@`. Este modulo lo sustituye por:
//   - nada (cadena VACIA) si la escena no tiene ningun efecto -> el texto
//     HLSL es BYTE A BYTE el de CinemaKernelM0::SOURCE, CINEMA_TRIM no queda
//     definida y todos sus #ifdef desaparecen. Esa es la garantia ESTRUCTURAL
//     del gate de regresion: no dependemos de que el compilador no reordene
//     el resto del kernel al crecer.
//   - el runtime st* + el cuerpo de los efectos PRESENTES + la tabla de
//     ganancias, si los hay.
//
// Por que la especializacion es el diseno y no una mitigacion: una escena real
// lleva uno o dos efectos, no veinte. La medida estatica sobre el bloque
// generado da +30 slots (+0.9%) con 1 efecto y +740 (+22.4%, 70 -> 82
// temporales) con los 20 -- y el kernel YA dispara X4714 de presion de
// registros. Compilar las 20 ramas en una escena que usa una es pagar el peor
// caso siempre.
//
// COSTE NUEVO que esto introduce y que hay que rendir: al cambiar el CONJUNTO
// de efectos de la escena (no el marker, no la camara, no la intensidad) hay
// que RECOMPILAR el kernel. Se cachea por hash del conjunto.
// ---------------------------------------------------------------------------

namespace CppProject
{
	// Conjunto (ordenado y sin duplicados) de slots de efecto presentes en la
	// escena. 0 nunca entra: es "sin efecto".
	typedef std::vector<uint32_t> CinemaTrimSet;

	// Compone el fuente definitivo. Devuelve un puntero ESTABLE (cacheado por
	// conjunto) valido hasta el siguiente ClearCache: los backends guardan el
	// puntero mientras compilan.
	const char* CinemaTrimComposeSource(const CinemaTrimSet& fxSet);

	// NOMBRE DEL PARAMETRO: `fxSet`, NUNCA `slots`. Qt define `slots` como
	// macro vacia en qobjectdefs.h (las palabras clave de moc), asi que un
	// parametro con ese nombre DESAPARECE del codigo y el error que sale es
	// un sintaxis error a diez lineas de distancia.

	// Clave del conjunto, para que los backends sepan si su kernel cacheado
	// sigue siendo valido. Conjunto vacio -> 0.
	uint64_t CinemaTrimSetKey(const CinemaTrimSet& fxSet);

	// Numero de recompilaciones y ms acumulados de composicion (el acta rinde
	// el coste; medir la compilacion en si es cosa de cada backend).
	IntType CinemaTrimComposeCount();

	void CinemaTrimClearCache();
}
