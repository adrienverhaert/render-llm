#pragma once
#include "Common.hpp"

namespace CppProject
{
	// SPEC_WHITE_DETAILS_GOLD #C1: motor GOLD del post-proceso "white details"
	// (rework completo de SPEC_WHITE_DETAILS.md; ver ese documento SS1 para el
	// mapa de que se conserva/reescribe/jubila). Genera una capa separada
	// "_whitedetails.png" con laminas blancas planas (taper de un solo pico,
	// no cunas-por-segmento) en los tramos de mayor CONTRASTE de borde de la
	// silueta, mas acentos en V y (opcional) laminas en bordes internos; y
	// opcionalmente una segunda capa "_wdcurvas.png" (lift tipo ajuste de
	// curvas del borde, no blanco plano). Pilotado por una cadena "opts"
	// clave=valor;... (ver SS4 del spec para la tabla completa).
	// Este pase NO toca el kernel HLSL ni el oraculo de paridad (CinemaOracle):
	// es post-proceso puro CPU sobre PNG straight-alpha, fuera del camino de
	// render. Determinista: mono-hilo, mismo binario + mismos args -> mismo
	// PNG byte a byte (requisito duro de la ronda "scrub" de secuencias).
	//
	// Devuelve "ok cov=<frac> runs=<n> lam=<n> vacc=<n> int=<n> wedgepx=<n>
	// K=<n> con=<bg|in> curves=<0|1> ang=<..> dens=<..> w=<..> lb=<..> mg=<..>
	// mr=<..>" (mas " bg=off" si opts.bg no cargo/no caso el tamano) o
	// "error:<motivo>".
	QString Mi3WhiteDetailsRunOpt(const QString& srcPng, const QString& outPng, const QString& opts);

	// DEPRECADO: ver SPEC_WHITE_DETAILS_GOLD.md C2. Wrapper de compatibilidad
	// sobre la API vieja de 10 argumentos posicionales (SPEC_WHITE_DETAILS.md
	// v1): construye la cadena "opts" equivalente y llama a
	// Mi3WhiteDetailsRunOpt. Mantenido para no romper mi3_whitedetails_run
	// (native GML existente) hasta que toda la superficie migre a
	// mi3_whitedetails_run_opt.
	QString Mi3WhiteDetailsRun(const QString& srcPng, const QString& outPng,
		double angleDeg, double density, double wedgeWidth, double outlineWidth,
		double outlineAlpha, double minRunLen, IntType seed, IntType bake);

	// Angulo (grados, convencion SS3 del spec: 0=luz desde la derecha,
	// 90=desde arriba) de la direccion de la luz clave de la escena Cinema
	// actualmente cargada, proyectada a pantalla via la viewProj de la
	// ultima camara renderizada. Solo valido TRAS un render Cinema (camara
	// y entorno quedan cargados en CinemaSceneCamera()/CinemaSceneEnv());
	// devuelve -999 si no hay datos validos o si el sol queda casi paralelo
	// al eje de vista (proyeccion de la direccion degenerada, |v|~0).
	// SPEC_WHITE_DETAILS_GOLD SS1: SE CONSERVA TAL CUAL (sin cambios).
	double Mi3WhiteDetailsAutoAngle();
}
