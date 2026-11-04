#pragma once

// MI3 modo SERVE (capa 2, transporte stdin/stdout).
//
// El motor arranca UNA vez (~8-10 s de los cuales "Load blocks" son ~5,6 s) y
// se queda vivo atendiendo op-scripts sucesivos, en vez de pagar ese arranque
// por cada render. El transporte por FICHEROS (capa 1) vive entero en GML
// (mi3_serve.gml) y no necesita nada de aqui; esta capa anade el transporte
// que ya usa en produccion el pool de v1 sobre MI2: una linea JSON por
// trabajo en stdin, una linea JSON por respuesta en stdout, y EOF de stdin
// como senal de "el padre ha muerto" (la unica defensa barata contra
// huerfanos, que es justo donde se quemo v2).
//
// Mi3ServeInit() se llama desde main() ANTES de construir el AppHandler: el
// hilo lector tiene que estar encolando desde el primer instante, porque el
// arranque completo del motor tarda segundos y el pool puede mandar el primer
// trabajo mucho antes.

namespace CppProject
{
	// Parsea argv buscando --serve y, si esta, lanza el hilo lector de stdin.
	void Mi3ServeInit(int argc, char* argv[]);

	// true si el proceso arranco con --serve. Lo consulta Mi3HeadlessMode()
	// (AppWindow.cpp) para heredar el camuflaje de ventana y el auto-rechazo
	// de dialogos modales sin necesidad de Data/headless.json.
	bool Mi3ServeEnabled();
}
