#include "Mi3WhiteDetails.hpp"
#include "Render/Cinema/CinemaScene.hpp"

#include <QFile>
#include <QImage>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// SPEC_WHITE_DETAILS_GOLD.md: motor GOLD del post-proceso "white details".
// Rework completo del motor v1 (SPEC_WHITE_DETAILS.md) para clonar el trazo
// manual de Antonio (ver METRICAS_MANUAL.md, el faro medido). SS1 del spec
// GOLD documenta que se conserva / reescribe / jubila de v1; los bloques de
// abajo estan marcados igual. Invariantes NO negociables (R6, sin cambios):
// mono-hilo, sin contenedores de orden no determinista, sin dependencia de
// punteros/direcciones, redondeos fijos (llround), mismo binario + mismas
// entradas -> PNG byte a byte identico, funcion pura del frame (sin estado
// temporal) para el scrub de secuencias.
namespace CppProject
{
	namespace
	{
		// ---- SS1: SE CONSERVA (v1, sin cambios de comportamiento) ----
		// Vec2 gana operadores (+,-,*escalar) respecto a v1: es una extension
		// aditiva (v1 hacia la aritmetica en linea) que no cambia ningun
		// resultado, solo abrevia el algebra 2D que el motor GOLD necesita
		// mucho mas que v1 (E2-E9).
		struct Vec2
		{
			double x, y;
			Vec2 operator+(const Vec2& o) const { return Vec2{ x + o.x, y + o.y }; }
			Vec2 operator-(const Vec2& o) const { return Vec2{ x - o.x, y - o.y }; }
			Vec2 operator*(double s) const { return Vec2{ x * s, y * s }; }
		};
		struct IPt { IntType y, x; };

		inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
		inline double ClampRange(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

		inline double Smoothstep(double lo, double hi, double x)
		{
			if (hi <= lo)
				return x >= hi ? 1.0 : 0.0;
			double t = Clamp01((x - lo) / (hi - lo));
			return t * t * (3.0 - 2.0 * t);
		}

		inline bool InMask(const std::vector<uint8_t>& M, IntType W, IntType H, IntType y, IntType x)
		{
			if (y < 0 || y >= H || x < 0 || x >= W)
				return false;
			return M[(size_t)(y * W + x)] != 0;
		}

		// Hash determinista (sin dependencia de estado global / punteros) para
		// el jitter opcional de wedge_width cuando seed != 0 (legado v1, ver
		// wrapper Mi3WhiteDetailsRun / SPEC_WHITE_DETAILS.md SS4).
		inline double Hash01(IntType seed, IntType a, IntType b)
		{
			uint64_t h = (uint64_t)seed * 0x9E3779B97F4A7C15ULL;
			h ^= (uint64_t)a * 0xBF58476D1CE4E5B9ULL + 0x9E3779B9u;
			h ^= (uint64_t)b * 0x94D049BB133111EBULL + 0x85EBCA6Bu;
			h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL; h ^= h >> 33;
			h *= 0xC4CEB9FE1A85EC53ULL; h ^= h >> 33;
			return (double)(h & 0xFFFFFFFFULL) / (double)0xFFFFFFFFULL;
		}

		// Transformada de distancia chamfer 5x5, dos pasadas, pesos 3/4/7,
		// resultado /3.0 -> unidades de pixel. GOLD la reusa mas todavia que
		// v1: silueta (wedge clip), banda de curvas (E10) y bordes internos
		// (E9, umbral de profundidad).
		std::vector<float> ChamferDistance(const std::vector<uint8_t>& seed, IntType W, IntType H)
		{
			const float INF = 1.0e9f;
			std::vector<float> D((size_t)(W * H), INF);
			for (IntType i = 0; i < W * H; i++)
				if (seed[(size_t)i]) D[(size_t)i] = 0.f;

			struct Off { IntType dy, dx; float w; };
			static const Off fwd[] = {
				{-2,-1,7.f}, {-2,1,7.f},
				{-1,-2,7.f}, {-1,-1,4.f}, {-1,0,3.f}, {-1,1,4.f}, {-1,2,7.f},
				{0,-1,3.f}
			};
			static const Off bwd[] = {
				{0,1,3.f},
				{1,-2,7.f}, {1,-1,4.f}, {1,0,3.f}, {1,1,4.f}, {1,2,7.f},
				{2,-1,7.f}, {2,1,7.f}
			};

			for (IntType y = 0; y < H; y++)
			{
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					float best = D[(size_t)idx];
					for (auto& o : fwd)
					{
						IntType ny = y + o.dy, nx = x + o.dx;
						if (ny < 0 || ny >= H || nx < 0 || nx >= W) continue;
						float cand = D[(size_t)(ny * W + nx)] + o.w;
						if (cand < best) best = cand;
					}
					D[(size_t)idx] = best;
				}
			}
			for (IntType y = H - 1; y >= 0; y--)
			{
				for (IntType x = W - 1; x >= 0; x--)
				{
					IntType idx = y * W + x;
					float best = D[(size_t)idx];
					for (auto& o : bwd)
					{
						IntType ny = y + o.dy, nx = x + o.dx;
						if (ny < 0 || ny >= H || nx < 0 || nx >= W) continue;
						float cand = D[(size_t)(ny * W + nx)] + o.w;
						if (cand < best) best = cand;
					}
					D[(size_t)idx] = best;
				}
			}
			for (IntType i = 0; i < W * H; i++)
				D[(size_t)i] = D[(size_t)i] / 3.f;
			return D;
		}

		struct Component { IPt seed; IntType area; };

		// Etiquetado de componentes 8-conectadas por flood fill con pila
		// explicita (sin recursion, sin contenedores de orden no determinista).
		// El orden de salida en `outComps` sigue el orden de DESCUBRIMIENTO por
		// scanline -- requisito de determinismo. GOLD reusa esta misma funcion
		// para las componentes de la silueta (E1) Y para las componentes de
		// cresta interior (E9, sobre la mascara `ridge`).
		void FindComponents(const std::vector<uint8_t>& M, IntType W, IntType H, double minArea,
			std::vector<Component>& outComps)
		{
			std::vector<uint8_t> visited((size_t)(W * H), 0);
			std::vector<IPt> stack;
			static const IntType ny8[8] = { -1,-1,-1, 0, 0, 1, 1, 1 };
			static const IntType nx8[8] = { -1, 0, 1,-1, 1,-1, 0, 1 };

			for (IntType y = 0; y < H; y++)
			{
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					if (!M[(size_t)idx] || visited[(size_t)idx]) continue;

					IPt seedPt = { y, x };
					visited[(size_t)idx] = 1;
					stack.clear();
					stack.push_back(seedPt);
					IntType head = 0, area = 0;
					while (head < (IntType)stack.size())
					{
						IPt p = stack[(size_t)head]; head++;
						area++;
						for (IntType dd = 0; dd < 8; dd++)
						{
							IntType ny = p.y + ny8[dd], nx = p.x + nx8[dd];
							if (ny < 0 || ny >= H || nx < 0 || nx >= W) continue;
							IntType nidx = ny * W + nx;
							if (M[(size_t)nidx] && !visited[(size_t)nidx])
							{
								visited[(size_t)nidx] = 1;
								stack.push_back({ ny, nx });
							}
						}
					}
					if ((double)area >= minArea)
						outComps.push_back({ seedPt, area });
				}
			}
		}

		// Moore-neighbor tracing (8-conectividad, sentido horario). `start`
		// debe ser el pixel mas arriba-luego-izquierda de la componente (lo
		// garantiza FindComponents), asi el vecino oeste es fondo por
		// construccion y sirve de "backtrack" inicial. Criterio de parada:
		// el ESTADO (punto de borde, punto de retroceso) repite el estado
		// inicial -- finito y determinista.
		std::vector<IPt> TraceContour(const std::vector<uint8_t>& M, IntType W, IntType H, IPt start)
		{
			static const IntType dy[8] = { -1,-1, 0, 1, 1, 1, 0,-1 };
			static const IntType dx[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };

			std::vector<IPt> contour;
			contour.push_back(start);

			IPt b = start;
			IPt c = { start.y, start.x - 1 };
			IPt b0 = start, c0 = c;
			bool firstIter = true;
			IntType maxSteps = 8 * W * H + 64;

			for (IntType steps = 0; steps < maxSteps; steps++)
			{
				IntType cdy = c.y - b.y, cdx = c.x - b.x;
				IntType dirIdx = 6; // default defensivo (W)
				for (IntType d = 0; d < 8; d++)
					if (dy[d] == cdy && dx[d] == cdx) { dirIdx = d; break; }

				bool ok = false;
				IPt found = { -1,-1 }, foundC = c;
				for (IntType i = 1; i <= 8; i++)
				{
					IntType nd = (dirIdx + i) % 8;
					IPt cand = { b.y + dy[nd], b.x + dx[nd] };
					if (InMask(M, W, H, cand.y, cand.x))
					{
						found = cand;
						IntType pd = (nd + 7) % 8;
						foundC = { b.y + dy[pd], b.x + dx[pd] };
						ok = true;
						break;
					}
				}
				if (!ok)
					break; // pixel aislado: componente de un solo punto

				if (firstIter)
				{
					contour.push_back(found);
					b = found; c = foundC;
					firstIter = false;
					continue;
				}
				if (found.y == b0.y && found.x == b0.x && foundC.y == c0.y && foundC.x == c0.x)
					break; // vuelta completa al estado inicial
				contour.push_back(found);
				b = found; c = foundC;
			}
			return contour;
		}

		// Media movil circular (radio `radius`) sobre un array de tamano n,
		// via sumas de prefijo (O(n), sin depender de orden de iteracion no
		// determinista).
		std::vector<float> BoxCircular(const std::vector<float>& s, IntType radius)
		{
			IntType n = (IntType)s.size();
			std::vector<float> out((size_t)n, 0.f);
			if (n == 0) return out;
			std::vector<double> prefix((size_t)n + 1, 0.0);
			for (IntType i = 0; i < n; i++) prefix[(size_t)i + 1] = prefix[(size_t)i] + s[(size_t)i];
			double total = prefix[(size_t)n];
			for (IntType i = 0; i < n; i++)
			{
				IntType lo = i - radius, hi = i + radius;
				IntType count = hi - lo + 1;
				double sum;
				if (count >= n)
				{
					sum = total;
					count = n;
				}
				else
				{
					IntType loM = ((lo % n) + n) % n;
					IntType hiM = ((hi % n) + n) % n;
					if (loM <= hiM)
						sum = prefix[(size_t)hiM + 1] - prefix[(size_t)loM];
					else
						sum = (prefix[(size_t)n] - prefix[(size_t)loM]) + prefix[(size_t)hiM + 1];
				}
				out[(size_t)i] = (float)(sum / (double)count);
			}
			return out;
		}

		inline double EdgeFn(const Vec2& a, const Vec2& b, const Vec2& p)
		{
			return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
		}

		// Rasteriza un triangulo con cobertura antialiaseada (supersampling
		// 4x4, deterministico) y hace buf(p) = max(buf(p), alphaR*cobertura(p)).
		// GOLD la usa como PRIMITIVA (SS1 del spec): la tira de la lamina (E7)
		// llama esto dos veces por tramo.
		// GOLD r1 (afinado Opus): variante ADITIVA sobre un acumulador de
		// COBERTURA. El `max()` de RasterizeWedge subcubre los pixeles que
		// caen entre dos triangulos de la MISMA tira (cada uno aporta media
		// cobertura y el max se queda con una mitad): eso producia el aspecto
		// "abalorado"/punteado de la lamina en la ronda 0. Sumando la
		// cobertura dentro de la tira y fundiendo al final con max() contra el
		// buffer global, la tira queda solida y las laminas siguen sin sumarse
		// entre si. Devuelve la caja tocada para poder limpiar el acumulador.
		void RasterizeTriAdd(std::vector<float>& acc, IntType W, IntType H, Vec2 v1, Vec2 v2, Vec2 v3,
			IntType& bx0, IntType& by0, IntType& bx1, IntType& by1)
		{
			double minx = std::min(v1.x, std::min(v2.x, v3.x));
			double maxx = std::max(v1.x, std::max(v2.x, v3.x));
			double miny = std::min(v1.y, std::min(v2.y, v3.y));
			double maxy = std::max(v1.y, std::max(v2.y, v3.y));
			IntType x0 = (IntType)std::floor(minx) - 1, x1 = (IntType)std::ceil(maxx) + 1;
			IntType y0 = (IntType)std::floor(miny) - 1, y1 = (IntType)std::ceil(maxy) + 1;
			x0 = std::max<IntType>(x0, 0); y0 = std::max<IntType>(y0, 0);
			x1 = std::min<IntType>(x1, W - 1); y1 = std::min<IntType>(y1, H - 1);
			if (x0 > x1 || y0 > y1) return;

			const IntType SS = 4;
			double area = EdgeFn(v1, v2, v3);
			if (std::abs(area) < 1e-9) return;
			bool ccw = area > 0;

			for (IntType y = y0; y <= y1; y++)
			{
				for (IntType x = x0; x <= x1; x++)
				{
					IntType hits = 0;
					for (IntType sy = 0; sy < SS; sy++)
					{
						for (IntType sx = 0; sx < SS; sx++)
						{
							Vec2 p{ (double)x + ((double)sx + 0.5) / SS, (double)y + ((double)sy + 0.5) / SS };
							double w0 = EdgeFn(v2, v3, p);
							double w1 = EdgeFn(v3, v1, p);
							double w2 = EdgeFn(v1, v2, p);
							// El test es semiabierto en dos de las tres aristas
							// para que un punto sobre la arista COMPARTIDA de
							// dos triangulos de la tira cuente UNA sola vez.
							bool inside = ccw ? (w0 >= 0 && w1 > 0 && w2 > 0) : (w0 <= 0 && w1 < 0 && w2 < 0);
							if (inside) hits++;
						}
					}
					if (hits > 0)
					{
						acc[(size_t)(y * W + x)] += (float)((double)hits / (double)(SS * SS));
						if (x < bx0) bx0 = x;
						if (x > bx1) bx1 = x;
						if (y < by0) by0 = y;
						if (y > by1) by1 = y;
					}
				}
			}
		}

		void RasterizeWedge(std::vector<float>& buf, IntType W, IntType H, Vec2 v1, Vec2 v2, Vec2 v3, double alphaR)
		{
			double minx = std::min(v1.x, std::min(v2.x, v3.x));
			double maxx = std::max(v1.x, std::max(v2.x, v3.x));
			double miny = std::min(v1.y, std::min(v2.y, v3.y));
			double maxy = std::max(v1.y, std::max(v2.y, v3.y));
			IntType x0 = (IntType)std::floor(minx) - 1, x1 = (IntType)std::ceil(maxx) + 1;
			IntType y0 = (IntType)std::floor(miny) - 1, y1 = (IntType)std::ceil(maxy) + 1;
			x0 = std::max<IntType>(x0, 0); y0 = std::max<IntType>(y0, 0);
			x1 = std::min<IntType>(x1, W - 1); y1 = std::min<IntType>(y1, H - 1);
			if (x0 > x1 || y0 > y1) return;

			const IntType SS = 4;
			double area = EdgeFn(v1, v2, v3);
			if (std::abs(area) < 1e-9) return;
			bool ccw = area > 0;

			for (IntType y = y0; y <= y1; y++)
			{
				for (IntType x = x0; x <= x1; x++)
				{
					IntType hits = 0;
					for (IntType sy = 0; sy < SS; sy++)
					{
						for (IntType sx = 0; sx < SS; sx++)
						{
							Vec2 p{ (double)x + ((double)sx + 0.5) / SS, (double)y + ((double)sy + 0.5) / SS };
							double w0 = EdgeFn(v2, v3, p);
							double w1 = EdgeFn(v3, v1, p);
							double w2 = EdgeFn(v1, v2, p);
							bool inside = ccw ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
							if (inside) hits++;
						}
					}
					if (hits > 0)
					{
						double coverage = (double)hits / (double)(SS * SS);
						size_t idx = (size_t)(y * W + x);
						float val = (float)(alphaR * coverage);
						if (val > buf[idx]) buf[idx] = val;
					}
				}
			}
		}

		// ---- GOLD: constantes de la medida (SPEC_WHITE_DETAILS_GOLD.md SS3) ----
		// Calibrables +-40% durante el desarrollo; CONGELADAS aqui tras la
		// ronda de calibracion (ver reporte final de la mision). No son opts
		// (no viven en WdParams / no son parte de la cadena "opts="): son
		// constantes del algoritmo, igual que v1 tenia sus "constantes
		// internas fijas en codigo".
		static const double CON_LO = 30.0, CON_HI = 110.0;   // E3, modo "bg" (con fondo real)
		static const double IN_LO = 120.0, IN_HI = 190.0;    // E3, modo "in" (fallback sin fondo)
		// GOLD r3: los tres umbrales del pre-filtro pasan a WdParams
		// (t_seed / t_lo / t_hi); estas constantes quedan solo como los
		// DEFAULTS documentados de r1/r2.
		static const double GAP_ANG = 35.0;                  // E5: angulo maximo fusionable (grados)
		static const double kPi = 3.14159265358979323846;

		// ---- GOLD: algebra 2D minima ----
		inline double Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
		inline double Length(const Vec2& a) { return std::sqrt(Dot(a, a)); }
		inline Vec2 Normalize(const Vec2& a) { double l = Length(a); return l > 1e-9 ? Vec2{ a.x / l, a.y / l } : Vec2{ 0.0, 0.0 }; }
		inline Vec2 Perp(const Vec2& a) { return Vec2{ -a.y, a.x }; } // misma convencion que v1 (perp(t) crudo, se orienta despues via InMask)

		// BoxCircular aplicado a posiciones 2D (E2 paso 2: "aplica BoxCircular
		// a x e y por separado", misma suma de prefijos que el campo escalar).
		std::vector<Vec2> BoxCircular2D(const std::vector<Vec2>& pts, IntType radius)
		{
			IntType n = (IntType)pts.size();
			std::vector<float> xs((size_t)n), ys((size_t)n);
			for (IntType i = 0; i < n; i++) { xs[(size_t)i] = (float)pts[(size_t)i].x; ys[(size_t)i] = (float)pts[(size_t)i].y; }
			std::vector<float> xo = BoxCircular(xs, radius);
			std::vector<float> yo = BoxCircular(ys, radius);
			std::vector<Vec2> out((size_t)n);
			for (IntType i = 0; i < n; i++) out[(size_t)i] = Vec2{ (double)xo[(size_t)i], (double)yo[(size_t)i] };
			return out;
		}

		// E2 paso 1: resample de un lazo CERRADO a paso de arco ~constante
		// (interpolacion lineal circular). El punto de salida 0 coincide con
		// pts[0] (determinismo: el inicio de TraceContour ya es determinista).
		std::vector<Vec2> ResampleClosed(const std::vector<Vec2>& pts, double step)
		{
			IntType n = (IntType)pts.size();
			if (n < 2) return pts;
			std::vector<double> prefix((size_t)n + 1, 0.0);
			for (IntType i = 0; i < n; i++)
			{
				IntType j = (i + 1) % n;
				prefix[(size_t)i + 1] = prefix[(size_t)i] + Length(pts[(size_t)j] - pts[(size_t)i]);
			}
			double total = prefix[(size_t)n];
			if (total < 1e-6) return pts;
			IntType m = std::max<IntType>(3, (IntType)std::llround(total / step));
			double actualStep = total / (double)m;
			std::vector<Vec2> out;
			out.reserve((size_t)m);
			IntType seg = 0;
			for (IntType kk = 0; kk < m; kk++)
			{
				double d = (double)kk * actualStep;
				while (seg < n - 1 && prefix[(size_t)seg + 1] < d) seg++;
				IntType i0 = seg, i1 = (seg + 1) % n;
				double segStart = prefix[(size_t)seg];
				double segL = prefix[(size_t)seg + 1] - segStart;
				double t = segL > 1e-9 ? Clamp01((d - segStart) / segL) : 0.0;
				out.push_back(pts[(size_t)i0] + (pts[(size_t)i1] - pts[(size_t)i0]) * t);
			}
			return out;
		}

		// E2 paso 3: tangente/normal EXTERIOR por indice sobre el contorno
		// suavizado S (cerrado). t_j = normalize(S[j+wn]-S[j-wn]) (diferencia
		// directa, simetrica -- distinto del promedio-de-vectores-unitarios
		// que usaba v1 para su tangente).
		void ComputeTangentNormal(const std::vector<Vec2>& S, IntType wn,
			const std::vector<uint8_t>& M, IntType W, IntType H,
			std::vector<Vec2>& nOut)
		{
			IntType m = (IntType)S.size();
			nOut.assign((size_t)m, Vec2{ 0.0, 1.0 });
			for (IntType i = 0; i < m; i++)
			{
				IntType hi = ((i + wn) % m + m) % m;
				IntType lo = ((i - wn) % m + m) % m;
				Vec2 t = Normalize(S[(size_t)hi] - S[(size_t)lo]);
				if (Length(t) < 1e-9) t = Vec2{ 1.0, 0.0 };
				Vec2 n = Perp(t);
				IntType px = (IntType)std::llround(S[(size_t)i].x + 2.5 * n.x);
				IntType py = (IntType)std::llround(S[(size_t)i].y + 2.5 * n.y);
				if (InMask(M, W, H, py, px))
					n = Vec2{ -n.x, -n.y };
				nOut[(size_t)i] = n;
			}
		}

		// E4: rectitud sobre una ventana wr (distinta de wn de E2), formula
		// v1 (promedio de vectores unitarios) con el remapeo GOLD 0.80/0.18.
		double StraightnessAt(const std::vector<Vec2>& S, IntType j, IntType wr)
		{
			IntType m = (IntType)S.size();
			double sumx = 0.0, sumy = 0.0; IntType cnt = 0;
			for (IntType u = -wr; u <= wr; u++)
			{
				IntType a = ((j + u) % m + m) % m;
				IntType b = ((j + u + 1) % m + m) % m;
				Vec2 uv = S[(size_t)b] - S[(size_t)a];
				double ul = Length(uv);
				if (ul > 1e-9) { sumx += uv.x / ul; sumy += uv.y / ul; cnt++; }
			}
			double mag = cnt > 0 ? std::sqrt(sumx * sumx + sumy * sumy) / (double)cnt : 0.0;
			return Clamp01((mag - 0.80) / 0.18);
		}

		inline double AngleBetweenDeg(const Vec2& a, const Vec2& b)
		{
			double la = Length(a), lb = Length(b);
			if (la < 1e-9 || lb < 1e-9) return 0.0;
			double c = Dot(a, b) / (la * lb);
			c = ClampRange(c, -1.0, 1.0);
			return std::acos(c) * RADTODEG;
		}

		// Longitud de arco caminando el indice hacia ADELANTE (mod m) desde a
		// hasta b sobre las posiciones S. Usado por E5 (huecos), E6 (distancia
		// entre runs) y E8 (distancia esquina-extremo). Acotado a <= m+2
		// pasos: siempre termina.
		double ArcLenFwd(const std::vector<Vec2>& S, IntType a, IntType b)
		{
			IntType m = (IntType)S.size();
			double len = 0.0;
			IntType i = a;
			IntType guard = 0;
			while (i != b && guard < m + 2)
			{
				IntType j = (i + 1) % m;
				len += Length(S[(size_t)j] - S[(size_t)i]);
				i = j;
				guard++;
			}
			return len;
		}

		double MeanSbarFwd(const std::vector<float>& sbar, IntType a, IntType b)
		{
			IntType m = (IntType)sbar.size();
			double sum = 0.0; IntType cnt = 0;
			IntType i = a;
			IntType guard = 0;
			while (i != b && guard < m + 2)
			{
				i = (i + 1) % m;
				sum += sbar[(size_t)i];
				cnt++;
				guard++;
			}
			return cnt > 0 ? sum / (double)cnt : 0.0;
		}

		// Anade a `cur` los indices ESTRICTAMENTE entre a y b (exclusive),
		// caminando hacia adelante -- el hueco que la fusion E5 absorbe.
		void AppendGapIndices(std::vector<IntType>& cur, IntType a, IntType b, IntType m)
		{
			IntType i = (a + 1) % m;
			IntType guard = 0;
			while (i != b && guard < m + 2)
			{
				cur.push_back(i);
				i = (i + 1) % m;
				guard++;
			}
		}

		// E6: distancia de arco entre dos TRAMOS (rangos de indices) de la
		// MISMA componente -- el minimo de los dos huecos que los separan
		// alrededor del lazo (se asume que no se solapan: los runs post-fusion
		// siguen siendo arcos disjuntos).
		double RunGapDist(const std::vector<Vec2>& S, IntType startA, IntType endA, IntType startB, IntType endB)
		{
			double d1 = ArcLenFwd(S, endA, startB);
			double d2 = ArcLenFwd(S, endB, startA);
			return std::min(d1, d2);
		}

		// E3: muestra de luminancia INTERIOR (vecino a nearest, solo cuenta si
		// cae dentro de M).
		bool SampleYInMask(const std::vector<float>& Y, const std::vector<uint8_t>& M,
			IntType W, IntType H, double x, double y, double& outY)
		{
			IntType px = (IntType)std::llround(x), py = (IntType)std::llround(y);
			if (px < 0 || px >= W || py < 0 || py >= H) return false;
			if (!M[(size_t)(py * W + px)]) return false;
			outY = Y[(size_t)(py * W + px)];
			return true;
		}

		// Muestra de una imagen aparte (fondo, o la propia Y para E9) con el
		// punto siempre CLAMPEADO al lienzo (no depende de ninguna mascara).
		double SampleYClamped(const std::vector<float>& Y, IntType W, IntType H, double x, double y)
		{
			IntType px = (IntType)std::llround(x), py = (IntType)std::llround(y);
			px = px < 0 ? 0 : (px >= W ? W - 1 : px);
			py = py < 0 ? 0 : (py >= H ? H - 1 : py);
			return (double)Y[(size_t)(py * W + px)];
		}

		// E7/E8/E9: resamplea un tramo ABIERTO (lista de indices en S, ya
		// contiguos) a N muestras equiespaciadas por arco, junto con su normal
		// (interpolada y renormalizada) y un valor escalar acompanante (sbar,
		// para localizar el pico 'up'). No es circular.
		void ResampleRunPNS(const std::vector<Vec2>& S, const std::vector<Vec2>& normal,
			const std::vector<float>& scalar, const std::vector<IntType>& runIdx, IntType N,
			std::vector<Vec2>& outPts, std::vector<Vec2>& outNormal, std::vector<double>& outScalar)
		{
			IntType rn = (IntType)runIdx.size();
			outPts.assign((size_t)N, Vec2{ 0.0, 0.0 });
			outNormal.assign((size_t)N, Vec2{ 0.0, 1.0 });
			outScalar.assign((size_t)N, 0.0);
			if (rn < 2)
			{
				Vec2 p = rn == 1 ? S[(size_t)runIdx[0]] : Vec2{ 0.0, 0.0 };
				Vec2 nn = rn == 1 ? normal[(size_t)runIdx[0]] : Vec2{ 0.0, 1.0 };
				double sc = rn == 1 ? (double)scalar[(size_t)runIdx[0]] : 0.0;
				for (IntType i = 0; i < N; i++) { outPts[(size_t)i] = p; outNormal[(size_t)i] = nn; outScalar[(size_t)i] = sc; }
				return;
			}
			std::vector<double> prefix((size_t)rn, 0.0);
			for (IntType i = 1; i < rn; i++)
				prefix[(size_t)i] = prefix[(size_t)i - 1] + Length(S[(size_t)runIdx[(size_t)i]] - S[(size_t)runIdx[(size_t)i - 1]]);
			double total = prefix[(size_t)rn - 1];
			IntType seg = 0;
			for (IntType k2 = 0; k2 < N; k2++)
			{
				double d = N > 1 ? total * (double)k2 / (double)(N - 1) : 0.0;
				while (seg < rn - 2 && prefix[(size_t)seg + 1] < d) seg++;
				IntType i0 = runIdx[(size_t)seg], i1 = runIdx[(size_t)seg + 1];
				double segStart = prefix[(size_t)seg], segL = prefix[(size_t)seg + 1] - segStart;
				double t = segL > 1e-9 ? Clamp01((d - segStart) / segL) : 0.0;
				outPts[(size_t)k2] = S[(size_t)i0] + (S[(size_t)i1] - S[(size_t)i0]) * t;
				Vec2 nrm = normal[(size_t)i0] + (normal[(size_t)i1] - normal[(size_t)i0]) * t;
				outNormal[(size_t)k2] = Normalize(nrm);
				outScalar[(size_t)k2] = (double)scalar[(size_t)i0] + ((double)scalar[(size_t)i1] - (double)scalar[(size_t)i0]) * t;
			}
		}

		// E7 (GOLD r1, afinado Opus): LAMINA RECTA. El faro manual dibuja
		// CUERDAS rectas (rectitud media 0.921) que ignoran la escalera de la
		// silueta blocky; el motor r0 seguia el contorno punto a punto y por
		// eso envolvia picos y esquinas de 90 grados y salia "abalorado" por
		// el aliasing del escalon. Aqui el spine se proyecta sobre su recta de
		// ajuste (total-least-squares por el momento de segundo orden) y se
		// mezcla con el original segun `amount`; la normal se lleva a la
		// normal MEDIA en la misma proporcion para que los dos bordes de la
		// lamina queden paralelos. amount=0 => comportamiento r0 exacto.
		void StraightenSpine(std::vector<Vec2>& spine, std::vector<Vec2>& normal, double amount)
		{
			IntType N = (IntType)spine.size();
			if (N < 3 || amount <= 0.0) return;
			double a = ClampRange(amount, 0.0, 1.0);
			double cx = 0.0, cy = 0.0;
			for (IntType i = 0; i < N; i++) { cx += spine[(size_t)i].x; cy += spine[(size_t)i].y; }
			cx /= (double)N; cy /= (double)N;
			double sxx = 0.0, sxy = 0.0, syy = 0.0;
			for (IntType i = 0; i < N; i++)
			{
				double dx = spine[(size_t)i].x - cx, dy = spine[(size_t)i].y - cy;
				sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
			}
			if (sxx + syy < 1e-9) return;
			double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
			Vec2 ax{ std::cos(theta), std::sin(theta) };
			Vec2 chord = spine[(size_t)N - 1] - spine[0];
			if (Dot(ax, chord) < 0.0) ax = Vec2{ -ax.x, -ax.y };
			Vec2 nAvg{ 0.0, 0.0 };
			for (IntType i = 0; i < N; i++) { nAvg.x += normal[(size_t)i].x; nAvg.y += normal[(size_t)i].y; }
			nAvg = Normalize(nAvg);
			bool nAvgOk = Length(nAvg) > 0.5;
			for (IntType i = 0; i < N; i++)
			{
				double t = (spine[(size_t)i].x - cx) * ax.x + (spine[(size_t)i].y - cy) * ax.y;
				Vec2 proj{ cx + ax.x * t, cy + ax.y * t };
				spine[(size_t)i] = spine[(size_t)i] + (proj - spine[(size_t)i]) * a;
				if (!nAvgOk) continue;
				Vec2 nb = Normalize(normal[(size_t)i] + (nAvg - normal[(size_t)i]) * a);
				if (Length(nb) > 0.5) normal[(size_t)i] = nb;
			}
		}

		// GOLD tanda 2 (afinado t2r2, D-E/G8): PEGAR la cuerda al borde.
		// MEDIDO sobre el ganador de la t2r1 (probe_g8.py + volcado de
		// distancias contorno->tinta): a lo largo de la horizontal de la BOTA
		// el trazo AUTO vive a 4.0-6.3 px del contorno crudo y el del faro a
		// 0.0-1.0 px. La causa es `straighten`: la cuerda de mejor ajuste pasa
		// por el MEDIO de la escalera de la silueta blocky y en las muescas se
		// hunde ~6 px hacia dentro. Como la tira crece SOLO hacia el interior
		// (A = spine + m*1, B = spine + m*(1+w)), engordar la lamina no acerca
		// su borde exterior ni un pixel: el hueco es de la ESPINA, no del
		// ancho. Y como G8 mide "contorno a <=6 px de tinta", ese medio pixel
		// de mas partia la cobertura en 6 trozos punteados de 2..10 px (14 runs
		// contra los 7 del faro).
		// `hug` traslada la cuerda YA RECTA hacia FUERA (normal exterior media)
		// hasta apoyarla en el punto mas saliente de la escalera: sigue siendo
		// una recta (no se recupera el temblor del contorno) y lo que se salga
		// de la silueta lo recorta E11 (aW=0 fuera de M), que es justo como se
		// ve un trazo a mano apoyado en un borde escalonado.
		// hug=0 => comportamiento anterior EXACTO (gate G22).
		void HugSpineOutward(std::vector<Vec2>& spine, const std::vector<Vec2>& spineOrig,
			const std::vector<Vec2>& normal, double hug)
		{
			IntType N = (IntType)spine.size();
			if (N < 2 || hug <= 0.0 || (IntType)spineOrig.size() != N) return;
			Vec2 nAvg{ 0.0, 0.0 };
			for (IntType i = 0; i < N; i++) { nAvg.x += normal[(size_t)i].x; nAvg.y += normal[(size_t)i].y; }
			nAvg = Normalize(nAvg);
			if (Length(nAvg) <= 0.5) return;
			double dMax = 0.0;
			for (IntType i = 0; i < N; i++)
			{
				double d = Dot(spineOrig[(size_t)i] - spine[(size_t)i], nAvg);
				if (d > dMax) dMax = d;
			}
			if (dMax <= 0.0) return;
			double off = ClampRange(hug, 0.0, 1.0) * dMax;
			for (IntType i = 0; i < N; i++) spine[(size_t)i] = spine[(size_t)i] + nAvg * off;
		}

		// E7: perfil de ancho de un solo pico -- crece de 0 en u=0 a 1 en u=up
		// y vuelve a 0 en u=1 (o es monotono si up esta en un extremo).
		inline double TaperProfile(double u, double up, double taperExp)
		{
			if (u <= up)
				return up > 0.0 ? std::pow(u / up, taperExp) : 1.0;
			return up < 1.0 ? std::pow((1.0 - u) / (1.0 - up), taperExp) : 1.0;
		}

		// E7: "si up<0.15 -> up=0; si up>0.85 -> up=1; si no clamp(0.20,0.80)"
		// (pico en un extremo si esta cerca de uno, si no interior acotado).
		inline double SnapPeakPos(double up)
		{
			if (up < 0.15) return 0.0;
			if (up > 0.85) return 1.0;
			return ClampRange(up, 0.20, 0.80);
		}

		// GOLD tanda 2 (D-C SS14.2b): terminal de lamina a AGUJA. Multiplica
		// w[] y define un alfa a[] POR MUESTRA que se apaga hacia el/los
		// extremo(s) LIBRE(s) (aquel(los) donde TaperProfile ya vale 0, es
		// decir el extremo opuesto a `up`; los DOS si up es interior).
		// tailFrac<=0 => a[i]=alphaBase constante y w[] intacto (comportamiento
		// tanda 1 EXACTO, gate G22): por eso el llamador elige entre esta
		// funcion + RasterizeStripTail (alfa por muestra) y el camino viejo
		// RasterizeStrip (alfa unico) segun tailFrac, en vez de dejar que la
		// aritmetica de dos acumuladores "de por casualidad" el mismo bit.
		void ApplyNeedleTail(std::vector<double>& w, std::vector<double>& a, double up,
			double tailFrac, double tailExp, double tailAlphaLo, double alphaBase)
		{
			IntType N = (IntType)w.size();
			a.assign((size_t)N, alphaBase);
			if (tailFrac <= 0.0) return;
			bool freeStart = up > 0.0;
			bool freeEnd = up < 1.0;
			for (IntType i = 0; i < N; i++)
			{
				double u = N > 1 ? (double)i / (double)(N - 1) : 0.0;
				double ttStart = freeStart ? Clamp01(u / tailFrac) : 1.0;
				double ttEnd = freeEnd ? Clamp01((1.0 - u) / tailFrac) : 1.0;
				double tt = std::min(ttStart, ttEnd);
				double needle = std::pow(tt, std::max(1e-6, tailExp));
				w[(size_t)i] *= needle;
				a[(size_t)i] = alphaBase * (tailAlphaLo + (1.0 - tailAlphaLo) * needle);
			}
		}

		// E7 (comun a laminas de silueta, acentos en V y laminas internas):
		// rasteriza una tira de N muestras con perfil de ancho w[i] hacia la
		// direccion m[i] (2 triangulos por tramo -- comparten arista, sin
		// costura visible via el max() de RasterizeWedge).
		// `acc` es un acumulador W*H que ENTRA a cero y SALE a cero (se limpia
		// solo la caja tocada): la cobertura se suma DENTRO de la tira y se
		// funde con el buffer global por max() una sola vez.
		void RasterizeStrip(std::vector<float>& buf, IntType W, IntType H,
			const std::vector<Vec2>& spine, const std::vector<Vec2>& mDir,
			const std::vector<double>& w, double alphaR, std::vector<float>& acc)
		{
			IntType N = (IntType)spine.size();
			if (N < 2) return;
			std::vector<Vec2> A((size_t)N), B((size_t)N);
			for (IntType i = 0; i < N; i++)
			{
				A[(size_t)i] = spine[(size_t)i] + mDir[(size_t)i] * 1.0;
				B[(size_t)i] = spine[(size_t)i] + mDir[(size_t)i] * (1.0 + w[(size_t)i]);
			}
			IntType bx0 = W, by0 = H, bx1 = -1, by1 = -1;
			for (IntType i = 0; i + 1 < N; i++)
			{
				RasterizeTriAdd(acc, W, H, A[(size_t)i], A[(size_t)i + 1], B[(size_t)i + 1], bx0, by0, bx1, by1);
				RasterizeTriAdd(acc, W, H, A[(size_t)i], B[(size_t)i + 1], B[(size_t)i], bx0, by0, bx1, by1);
			}
			if (bx1 < bx0 || by1 < by0) return;
			for (IntType y = by0; y <= by1; y++)
			{
				for (IntType x = bx0; x <= bx1; x++)
				{
					size_t idx = (size_t)(y * W + x);
					float c = acc[idx];
					if (c <= 0.f) continue;
					acc[idx] = 0.f;
					float val = (float)(alphaR * Clamp01((double)c));
					if (val > buf[idx]) buf[idx] = val;
				}
			}
		}

		// GOLD tanda 2 (D-C SS14.2b): variante de RasterizeTriAdd con DOS
		// acumuladores para el terminal a AGUJA (alfa por muestra). 'acc'
		// sigue siendo cobertura pura (identico a RasterizeTriAdd); 'accW'
		// acumula cobertura*alfa. `aTri` es constante en toda la llamada (la
		// media de alfa en los dos extremos del TRAMO, SS14.2b). Funcion
		// NUEVA y separada de RasterizeTriAdd (no una rama con alfa uniforme
		// dentro de ella) a proposito: así el camino tail_frac<=0 seguro
		// sigue usando la aritmetica EXACTA de la tanda 1 (un solo
		// acumulador), byte a byte, sin depender de que a*c/c redondee igual
		// que a en punto flotante (gate G22).
		void RasterizeTriAddW(std::vector<float>& acc, std::vector<float>& accW, IntType W, IntType H,
			Vec2 v1, Vec2 v2, Vec2 v3, double aTri, IntType& bx0, IntType& by0, IntType& bx1, IntType& by1)
		{
			double minx = std::min(v1.x, std::min(v2.x, v3.x));
			double maxx = std::max(v1.x, std::max(v2.x, v3.x));
			double miny = std::min(v1.y, std::min(v2.y, v3.y));
			double maxy = std::max(v1.y, std::max(v2.y, v3.y));
			IntType x0 = (IntType)std::floor(minx) - 1, x1 = (IntType)std::ceil(maxx) + 1;
			IntType y0 = (IntType)std::floor(miny) - 1, y1 = (IntType)std::ceil(maxy) + 1;
			x0 = std::max<IntType>(x0, 0); y0 = std::max<IntType>(y0, 0);
			x1 = std::min<IntType>(x1, W - 1); y1 = std::min<IntType>(y1, H - 1);
			if (x0 > x1 || y0 > y1) return;

			const IntType SS = 4;
			double area = EdgeFn(v1, v2, v3);
			if (std::abs(area) < 1e-9) return;
			bool ccw = area > 0;

			for (IntType y = y0; y <= y1; y++)
			{
				for (IntType x = x0; x <= x1; x++)
				{
					IntType hits = 0;
					for (IntType sy = 0; sy < SS; sy++)
					{
						for (IntType sx = 0; sx < SS; sx++)
						{
							Vec2 p{ (double)x + ((double)sx + 0.5) / SS, (double)y + ((double)sy + 0.5) / SS };
							double w0 = EdgeFn(v2, v3, p);
							double w1 = EdgeFn(v3, v1, p);
							double w2 = EdgeFn(v1, v2, p);
							bool inside = ccw ? (w0 >= 0 && w1 > 0 && w2 > 0) : (w0 <= 0 && w1 < 0 && w2 < 0);
							if (inside) hits++;
						}
					}
					if (hits > 0)
					{
						size_t idx = (size_t)(y * W + x);
						float hitsF = (float)((double)hits / (double)(SS * SS));
						acc[idx] += hitsF;
						accW[idx] += (float)(hitsF * aTri);
						if (x < bx0) bx0 = x;
						if (x > bx1) bx1 = x;
						if (y < by0) by0 = y;
						if (y > by1) by1 = y;
					}
				}
			}
		}

		// GOLD tanda 2 (D-C SS14.2b): igual que RasterizeStrip pero con alfa
		// POR MUESTRA `a[i]` (el terminal a aguja necesita apagar el alfa del
		// extremo LIBRE, no solo el ancho). `aTri` de cada tramo es la media
		// de a[i]/a[i+1] (SS14.2b); la resolucion final pondera por cobertura
		// (aMean = accW/acc), que es lo que mantiene la tira sin costura en
		// la arista compartida de los dos triangulos.
		void RasterizeStripTail(std::vector<float>& buf, IntType W, IntType H,
			const std::vector<Vec2>& spine, const std::vector<Vec2>& mDir,
			const std::vector<double>& w, const std::vector<double>& a,
			std::vector<float>& acc, std::vector<float>& accW)
		{
			IntType N = (IntType)spine.size();
			if (N < 2) return;
			std::vector<Vec2> A((size_t)N), B((size_t)N);
			for (IntType i = 0; i < N; i++)
			{
				A[(size_t)i] = spine[(size_t)i] + mDir[(size_t)i] * 1.0;
				B[(size_t)i] = spine[(size_t)i] + mDir[(size_t)i] * (1.0 + w[(size_t)i]);
			}
			IntType bx0 = W, by0 = H, bx1 = -1, by1 = -1;
			for (IntType i = 0; i + 1 < N; i++)
			{
				double aTri = 0.5 * (a[(size_t)i] + a[(size_t)i + 1]);
				RasterizeTriAddW(acc, accW, W, H, A[(size_t)i], A[(size_t)i + 1], B[(size_t)i + 1], aTri, bx0, by0, bx1, by1);
				RasterizeTriAddW(acc, accW, W, H, A[(size_t)i], B[(size_t)i + 1], B[(size_t)i], aTri, bx0, by0, bx1, by1);
			}
			if (bx1 < bx0 || by1 < by0) return;
			for (IntType y = by0; y <= by1; y++)
			{
				for (IntType x = bx0; x <= bx1; x++)
				{
					size_t idx = (size_t)(y * W + x);
					float c = acc[idx];
					if (c <= 0.f) continue;
					float aMean = accW[idx] / c;
					acc[idx] = 0.f; accW[idx] = 0.f;
					float val = (float)((double)aMean * Clamp01((double)c));
					if (val > buf[idx]) buf[idx] = val;
				}
			}
		}

		// E9: recolecta los pixeles de una componente de `mask` desde `seed`
		// (flood fill 8-conexo). `visitedScratch` es un buffer W*H reutilizable
		// entre llamadas: entra a 0 y SALE a 0 (limpia solo lo tocado) para no
		// pagar una asignacion+memset de W*H por cada cresta interior.
		std::vector<IPt> CollectComponentPixels(const std::vector<uint8_t>& mask, IntType W, IntType H,
			IPt seed, std::vector<uint8_t>& visitedScratch)
		{
			std::vector<IPt> pts;
			std::vector<IPt> stack;
			static const IntType ny8[8] = { -1,-1,-1, 0, 0, 1, 1, 1 };
			static const IntType nx8[8] = { -1, 0, 1,-1, 1,-1, 0, 1 };
			IntType sidx = seed.y * W + seed.x;
			visitedScratch[(size_t)sidx] = 1;
			stack.push_back(seed);
			IntType head = 0;
			while (head < (IntType)stack.size())
			{
				IPt p = stack[(size_t)head]; head++;
				pts.push_back(p);
				for (IntType dd = 0; dd < 8; dd++)
				{
					IntType ny = p.y + ny8[dd], nx = p.x + nx8[dd];
					if (ny < 0 || ny >= H || nx < 0 || nx >= W) continue;
					IntType nidx = ny * W + nx;
					if (mask[(size_t)nidx] && !visitedScratch[(size_t)nidx])
					{
						visitedScratch[(size_t)nidx] = 1;
						stack.push_back({ ny, nx });
					}
				}
			}
			for (auto& p : pts)
				visitedScratch[(size_t)(p.y * W + p.x)] = 0;
			return pts;
		}

		// E10: LUT monotona (Fritsch-Carlson) de la curva medida en el PSD,
		// puntos de control (entrada->salida) (0,0) (93,194) (199,204)
		// (255,255) -- ver METRICAS_MANUAL.md SS8. Algoritmo estandar (single
		// pass izquierda->derecha, tangentes iniciales por promedio de
		// pendientes vecinas, recorte por el criterio de Fritsch-Carlson
		// alpha^2+beta^2<=9).
		std::array<uint8_t, 256> BuildCurvesLUT()
		{
			static const double xs[4] = { 0.0, 93.0, 199.0, 255.0 };
			static const double ys[4] = { 0.0, 194.0, 204.0, 255.0 };
			const IntType n = 4;
			double delta[3] = { 0,0,0 };
			double m[4] = { 0,0,0,0 };
			for (IntType i = 0; i < n - 1; i++)
				delta[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
			m[0] = delta[0];
			m[n - 1] = delta[n - 2];
			for (IntType i = 1; i < n - 1; i++)
				m[i] = (delta[i - 1] + delta[i]) / 2.0;
			for (IntType i = 0; i < n - 1; i++)
			{
				if (std::abs(delta[i]) < 1e-9) { m[i] = 0.0; m[i + 1] = 0.0; continue; }
				double alpha = m[i] / delta[i];
				double beta = m[i + 1] / delta[i];
				if (alpha < 0.0) m[i] = 0.0;
				if (beta < 0.0) m[i + 1] = 0.0;
				double a2 = alpha * alpha, b2 = beta * beta;
				if (a2 + b2 > 9.0)
				{
					double tau = 3.0 / std::sqrt(a2 + b2);
					m[i] = tau * alpha * delta[i];
					m[i + 1] = tau * beta * delta[i];
				}
			}

			std::array<uint8_t, 256> lut{};
			for (IntType x = 0; x < 256; x++)
			{
				IntType seg = 0;
				while (seg < n - 2 && (double)x > xs[seg + 1]) seg++;
				double x0 = xs[seg], x1 = xs[seg + 1], y0 = ys[seg], y1 = ys[seg + 1];
				double h = x1 - x0;
				double t = h > 1e-9 ? ((double)x - x0) / h : 0.0;
				double t2 = t * t, t3 = t2 * t;
				double h00 = 2 * t3 - 3 * t2 + 1;
				double h10 = t3 - 2 * t2 + t;
				double h01 = -2 * t3 + 3 * t2;
				double h11 = t3 - t2;
				double yv = h00 * y0 + h10 * h * m[seg] + h01 * y1 + h11 * h * m[seg + 1];
				yv = ClampRange(yv, 0.0, 255.0);
				lut[(size_t)x] = (uint8_t)std::llround(yv);
			}
			return lut;
		}

		// GOLD tanda 2 (D-A SS13.3): construye una espina 1D (eje PCA +
		// resample a paso 2k) desde los pixeles de una componente de cresta.
		// LITERALMENTE el bloque que ya usaba E9 (bordes internos por
		// luminancia): E9 y E12 (bordes por profundidad) comparten esta
		// funcion -- "prohibido duplicar el codigo" (spec, refactor obligatorio
		// SS13.3). Devuelve false si la componente es demasiado corta
		// (< minRunK) para ser un candidato.
		bool RidgeToSpine(const std::vector<IPt>& pts, double k, double minRunK,
			std::vector<Vec2>& outSpine, double& outLen)
		{
			if (pts.size() < 2) return false;
			double mx = 0.0, my = 0.0;
			for (auto& p : pts) { mx += (double)p.x; my += (double)p.y; }
			mx /= (double)pts.size(); my /= (double)pts.size();
			double sxx = 0.0, syy = 0.0, sxy = 0.0;
			for (auto& p : pts)
			{
				double dx = (double)p.x - mx, dy = (double)p.y - my;
				sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
			}
			double npf = (double)pts.size();
			sxx /= npf; syy /= npf; sxy /= npf;
			double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
			Vec2 axis{ std::cos(theta), std::sin(theta) };
			Vec2 axisPerp = Perp(axis);

			std::vector<std::pair<double, double>> proj; // (s, perp)
			proj.reserve(pts.size());
			for (auto& p : pts)
			{
				double dx = (double)p.x - mx, dy = (double)p.y - my;
				proj.push_back(std::make_pair(dx * axis.x + dy * axis.y, dx * axisPerp.x + dy * axisPerp.y));
			}
			std::stable_sort(proj.begin(), proj.end(),
				[](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });

			IntType npp = (IntType)proj.size();
			std::vector<double> perpSmooth((size_t)npp, 0.0);
			double winS = 6.0 * k;
			IntType lo = 0, hiP = 0; double sumP = 0.0;
			for (IntType i = 0; i < npp; i++)
			{
				double si = proj[(size_t)i].first;
				while (hiP < npp && proj[(size_t)hiP].first <= si + winS) { sumP += proj[(size_t)hiP].second; hiP++; }
				while (lo < npp && proj[(size_t)lo].first < si - winS) { sumP -= proj[(size_t)lo].second; lo++; }
				perpSmooth[(size_t)i] = (hiP > lo) ? sumP / (double)(hiP - lo) : proj[(size_t)i].second;
			}

			double sMin = proj.front().first, sMax = proj.back().first;
			double sLen = sMax - sMin;
			if (sLen < minRunK) return false;
			IntType nSpine = std::max<IntType>(8, (IntType)std::llround(sLen / (2.0 * k)) + 1);

			std::vector<Vec2> spineOut((size_t)nSpine);
			IntType ptr = 0;
			for (IntType i = 0; i < nSpine; i++)
			{
				double starget = sMin + sLen * (double)i / (double)(nSpine - 1);
				while (ptr < npp - 2 && proj[(size_t)(ptr + 1)].first < starget) ptr++;
				double s0 = proj[(size_t)ptr].first, s1 = proj[(size_t)(ptr + 1)].first;
				double tt = (s1 - s0) > 1e-9 ? Clamp01((starget - s0) / (s1 - s0)) : 0.0;
				double perpI = perpSmooth[(size_t)ptr] + (perpSmooth[(size_t)(ptr + 1)] - perpSmooth[(size_t)ptr]) * tt;
				spineOut[(size_t)i] = Vec2{ mx + starget * axis.x + perpI * axisPerp.x, my + starget * axis.y + perpI * axisPerp.y };
			}
			outSpine = spineOut;
			outLen = sLen;
			return true;
		}

		// GOLD tanda 2 (D-B SS14.1a): formula UNIFICADA de Q, usada tanto por
		// candidatos de silueta (kind 0) como de profundidad (kind 1, D-A) --
		// "un candidato de profundidad tiene EXACTAMENTE los mismos cinco
		// escalares que uno de silueta" (SS13.5): lenR, sNLMean, c01Mean,
		// lobeMean entran en la MISMA formula.
		inline double ComputeQCand(double lenR, double c01Mean, double lobeMean, double sNLMean,
			double q_len, double q_con_bias, double q_light_bias, double q_light_pow, double q_sbar_bias)
		{
			double qLight = q_light_bias + (1.0 - q_light_bias) * std::pow(Clamp01(lobeMean), std::max(1e-6, q_light_pow));
			double qSbar = q_sbar_bias + (1.0 - q_sbar_bias) * Clamp01(sNLMean);
			double qCon = q_con_bias + (1.0 - q_con_bias) * Clamp01(c01Mean);
			return std::pow(lenR / 100.0, q_len) * std::max(0.0, qSbar) * std::max(0.0, qLight) * std::max(0.0, qCon);
		}

		// ---- GOLD: tipos de trabajo del pipeline (SS3 E1-E9) ----
		struct CompData
		{
			std::vector<Vec2> S;               // contorno resampleado+suavizado (cerrado)
			std::vector<Vec2> normal;          // normal exterior por indice
			std::vector<float> sbar;           // campo de score suavizado
			// GOLD tanda 2 (D-B, SS14.1a): score SIN el factor de luz, suavizado
			// con la MISMA ventana que sbar -- evita que Q doble-cuente la luz
			// cuando q_sbar_bias < 1.
			std::vector<float> sbarNL;
			std::vector<double> lobe;          // GOLD r2: lobulo de luz crudo por indice (E4), para q_light_bias
			std::vector<double> c01;           // contraste normalizado (E3), para el gate de v_contrast_min de E8
			std::vector<IntType> cornerIdx;     // esquinas convexas candidatas a acento en V (E8)
			std::vector<uint8_t> usedCorner;
			std::vector<double> turnDeg;        // giro local por indice (E8 parte 1); lo reusa E5b
		};
		// GOLD tanda 2 (D-A, SS13.5.1): RunCand se unifica en dos "sabores"
		// (kind 0 = silueta, existente; kind 1 = profundidad/E12, nuevo) que
		// compiten en el MISMO top-K (SS14.1). Refactor obligatorio del spec:
		// prohibido duplicar el codigo de seleccion/aceptacion.
		struct RunCand
		{
			IntType kind = 0;                 // 0 = silueta, 1 = profundidad (E12)
			IntType compIdx = -1;             // kind 0: indice en compData; kind 1: -1 (sin componente)
			std::vector<IntType> idx;         // kind 0: indices en compData[compIdx].S
			std::vector<Vec2> spine;          // kind 1: spine explicito (ya resampleado a 2k)
			Vec2 dirDraw{ 0.0, 0.0 };          // kind 1: lado del trazo (constante a lo largo)
			std::vector<double> prof;         // kind 1: perfil de rstep (para localizar 'up')
			double lenR = 0.0, sbarMean = 0.0, sNLMean = 0.0, c01Mean = 0.0, lobeMean = 0.0, Q = 0.0, prom = 0.0;
		};
		struct AcceptedRun
		{
			IntType kind = 0;
			IntType compIdx = -1;
			std::vector<IntType> idx;          // kind 0
			std::vector<Vec2> spine;           // kind 1
			Vec2 dirDraw{ 0.0, 0.0 };           // kind 1
			std::vector<double> prof;          // kind 1
			double lenR = 0.0, sbarMean = 0.0, Q = 0.0, alphaR = 0.0, wRun = 0.0, up = 0.0;
			double prom = 0.0;                 // D-C: prominencia (contraste x largo)
		};
		struct InternalCand
		{
			std::vector<Vec2> spine;
			Vec2 dir{ 0.0, 0.0 };
			double lenInt = 0.0, scoreInt = 0.0;
			std::vector<double> deltaProfile;
		};

		// ---- GOLD C2: parametros del motor, pilotados por una cadena
		// "clave=valor;..." (SPEC_WHITE_DETAILS_GOLD.md SS4). Los campos que
		// no aparecen en la tabla SS4 (v_dist, v_contrast_min, int_depth_min,
		// int_grad_min, int_min_px, w_ref, k_base, k_span) son constantes del
		// algoritmo referenciadas con prefijo "P." en el pseudocodigo SS3 pero
		// fuera de la superficie expuesta por C4/C5/UI -- interpretacion
		// minima anotada: viven aqui con el default inline del spec y SI son
		// parseables por `opts=` (por si un barrido futuro las quiere tocar),
		// pero ningun llamador actual de la superficie las fija explicitamente.
		struct WdParams
		{
			double angle = 135.0;
			double density = 0.5;
			double width = 11.5;   // GOLD r4: calibrado al pico de la CAPA del faro (id14 = 11.66 px)
			double light_bias = 0.35;   // GOLD r3: reequilibrado al aplanar el contraste (ver contrast_bias)
			double light_pow = 0.7;
			double contrast_pow = 2.50;
			double straight_floor = 0.50;
			double smooth_win = 22.0;
			double min_run = 100.0;     // GOLD r3
			double min_gap = 90.0;
			double gap_merge = 40.0;   // GOLD tanda 2 (D-E): era 25.0 en la tanda 1
			double q_len = 0.70;
			double hyst = 0.25;
			double taper_exp = 0.90;
			double w_lo = 0.60;
			double w_hi = 1.15;
			double v_max = 4.0;         // GOLD r4: solo 4 acentos llegan a emitirse; 8 era inerte
			double v_angle_min = 55.0;
			// GOLD r1 (afinado Opus): geometria de LAMINA RECTA. El manual
			// dibuja cuerdas RECTAS que mueren en las esquinas; el motor r0
			// seguia el contorno y envolvia picos y esquinas de 90 grados
			// (ver veredicto visual de la ronda 0). Estos cinco mandos
			// implementan R1 (partir) y R3 (recta) del mandato.
			double turn_split = 30.0;   // grados: giro LOCAL que parte un run
			double turn_max = 70.0;     // grados: giro ACUMULADO maximo en un run
			// GOLD r2 (R1): no partir en las MUESCAS. turn_keep es el giro
			// GRUESO minimo (medido en una ventana de turn_win px) que debe
			// haber en el corte para que el corte valga; 0 = como r1.
			double turn_keep = 50.0;
			double turn_win = 45.0;
			double straighten = 0.60;    // [0,1]: mezcla contorno -> recta de ajuste
			double pair_dist = 20.0;    // px: separacion bajo la cual dos runs son par en V
			double pair_angle = 15.0;   // grados: giro minimo entre cuerdas del par en V
			double internal = 0.0;      // GOLD r4: OFF por defecto (SS11.2 del spec, ver veredicto r4)
			double internal_max = 1.0;  // GOLD r3
			double obj_min_frac = 0.35;
			QString bg;
			QString outCurves;
			double curves_width = 8.0;
			double curves_ramp = 3.0;
			double curves_strength = 1.0;
			double curves_alpha = 1.0;
			double outline_width = 8.0;
			double outline_alpha = 0.0;
			double seed = 0.0;
			double bake = 0.0;
			double v_dist = 12.0;
			double v_contrast_min = 0.35;
			// GOLD r2: lobulo de luz MEDIO minimo del acento en V (0 = como r1).
			double v_light_min = 0.05;  // GOLD tanda 2 (afinado r2): oro r2 (0 deja entrar el rizo del bajo de la capa)
			double int_depth_min = 20.0;
			double int_grad_min = 28.0;
			double int_min_px = 80.0;
			double clip_depth = 40.0;   // GOLD r1: profundidad maxima permitida a una lamina de silueta
			// GOLD r1: la rampa alphaR (producto de tres smoothsteps) valia
			// tipicamente 0.2-0.35 para TODAS las laminas aceptadas, y ademas
			// multiplica el ancho: la lamina salia gris y de 2 px cuando el
			// faro la pinta BLANCA y de 6-11 px. `alpha_sat` es el valor de
			// alphaR a partir del cual la lamina esta ya del todo dentro; por
			// debajo sigue habiendo rampa CONTINUA hasta 0 (R6: sin popping).
			double alpha_sat = 0.55;
			// GOLD r1: dispersion de ancho ENTRE laminas (G5, manual CV 0.412).
			// Con w_rel=1 el ancho se reparte por el RANGO OBSERVADO de sbarMean
			// entre las laminas aceptadas (dispersion garantizada, independiente
			// de contrast_pow); con w_rel=0 se usa el w_ref absoluto del spec,
			// que se aplana en cuanto contrast_pow sube.
			double w_rel = 1.0;
			// GOLD r2 (afinado Opus): el faro NO pinta los flancos en sombra
			// (el gorro solo lleva lamina en el flanco izquierdo). Con
			// light_bias el lobulo solo modula el campo `s`, y un run LARGO en
			// sombra sigue ganando a uno corto iluminado en el top-K, porque Q
			// va como len^q_len. `q_light_bias` mete el lobulo MEDIO del run
			// directamente en Q (misma convencion que light_bias: 1.0 = sin
			// efecto, 0.0 = el lobulo manda del todo).
			double q_light_bias = 0.60;  // GOLD r4: con q_light_bias=0 la bota y la pierna daban Q=0.0000
			double q_light_pow = 1.0;
			// GOLD r2: posicion del pico de ancho. El faro engorda la lamina
			// hacia el extremo que TOCA la esquina convexa (pico del gorro,
			// arranque de la capa) y la afila a aguja en el extremo libre; el
			// motor r1 saca `up` del maximo de sbar y sale invertido en la capa.
			//   0 = pico de sbar (r1)
			//   1 = extremo mas cercano a una esquina convexa, si no fallback a 0
			//   2 = extremo mas cercano a la LUZ (proyeccion sobre L2)
			//   3 = esquina; si ningun extremo tiene esquina cerca, luz (2)
			//   4 = extremo mas alto en la VERTICAL de la luz (sol arriba -> y menor)
			//   5 = esquina; si ningun extremo tiene esquina cerca, vertical (4)
			double up_mode = 4.0;
			double up_corner_dist = 24.0; // px (escalados por k) para "el extremo toca esquina"
			// GOLD r2: el faro reparte el ancho tambien por LARGO (los dos
			// trazos mas largos son los mas gruesos). Mezcla en [0,1] entre el
			// reparto por score (r1) y el reparto por largo.
			double w_len = 0.0;
			double w_ref = 0.55;
			double k_base = 6.0;
			double k_span = 14.0;
			// ---- GOLD r3 (afinado Opus) ----
			// R4: el acento en V del faro es un TICK CORTO (manual id4 = 20 px
			// colgando del final de la lamina del gorro; id10/id11 = 59/69 px en
			// el guante), no una segunda lamina larga. El motor r2 lo dimensionaba
			// a 0.45*largo de la madre con suelo 40 px y techo 260 px, y por eso el
			// acento del bajo de la capa salia de 154 px donde el faro no pinta
			// NADA. Estos mandos lo desacoplan de la madre; v_len_frac=0.45,
			// v_len_min=40, v_len_max=260, v_width=0.85 reproducen la r2.
			double v_len_frac = 0.16;
			double v_len_min = 22.0;
			double v_len_max = 35.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double v_width = 1.35;   // GOLD r4: los ticks eran los que hundian G7 (pico medio)
			// R1/R2: EXTENSION DE EXTREMOS con histeresis. El seeding de runs usa
			// un umbral duro T_SEED y por eso la capa arranca ~130 px por debajo
			// del hombro y el ala del gorro muere ~40 px antes de la muesca (los
			// dos delatores de la r2). Con ext_thr>0 cada extremo del run se
			// alarga mientras sbar >= ext_thr*T_SEED, hasta ext_max px. ext_max=0
			// = comportamiento r2.
			double ext_thr = 0.55;
			double ext_max = 80.0;
			// ---- GOLD r4 (afinado Opus) ----
			// R1/R3: la extension de extremos NO mira el giro del contorno, y por
			// eso vuelve a cruzar justo las esquinas donde E5b acababa de PARTIR
			// a proposito. Medido en el ganador de la r3: 609 px de masa sobrante
			// en el "rizo" del extremo bajo de la capa y otro rizo en el pie de la
			// vertical de la bota, donde el faro MUERE en el vertice. `ext_turn`
			// es el giro LOCAL maximo (grados, la misma medida `turnDeg` que usa
			// E5b) que puede tener un indice para que la extension lo absorba.
			// 360 = comportamiento r3.
			double ext_turn = 360.0;
			// R2: la CALIDAD Q, que ordena el top-K y decide QUIEN reclama antes
			// el espacio de `sep_px`. Medido con dbg_runs sobre el ganador de la
			// r3: Q = (len/100)^q_len * sbarMean * lobeMean dejaba la horizontal
			// de la bota (que el faro pinta GRUESA: len 544, contraste 0.99) en
			// Q=0.0219 y la horizontal de la CADERA (que el faro no pinta: len
			// 254) en Q=0.893, porque sbarMean lleva dentro el termino de luz y
			// la bota mira hacia abajo (lobulo 0.019). Resultado: la cadera
			// reclamaba el sitio primero y la bota se salvaba de milagro. Con
			// estos dos mandos el contraste entra en Q por su cuenta (c01 CRUDO,
			// sin el contrast_pow que lo aplana) y el peso de sbarMean se puede
			// bajar, que es lo que ordena el faro por delante.
			//   q_sbar_bias = 0 y q_con_bias = 1  =>  Q identica a la r3.
			double q_sbar_bias = 1.0;
			double q_con_bias = 0.20;
			// R4: la EXENCION de par en V (dos laminas que se tocan en un vertice
			// no son un duplicado) la estaba aprovechando el bajo de la capa: una
			// lamina de 182 px colgada del extremo de la capa a 98 grados, que el
			// faro no pinta y que valia 624 px de masa sobrante (el "rizo" del
			// veredicto visual de la r3). Los pares en V del faro son TICKS
			// CORTOS (20-70 px, ver v_len_max), asi que la exencion solo vale si
			// el candidato es de ese tamano. 0 o negativo = sin limite (r3).
			// GOLD tanda 2 (D-C(c) SS14.2c): pasa de 0 (apagado) a 90 --
			// acoplado a D-D (remates en L): id20 (diagonal de la pierna) deja
			// de depender de la exencion porque ahora sale como remate de la
			// horizontal de la bota (SS14.3). Si D-D no la recupera, se
			// revierte a 0 y se declara el trade-off (SS16 riesgo 7).
			double pair_len_max = 90.0;
			// Volcado de diagnostico: si no esta vacio, escribe una linea por
			// lamina aceptada con sus magnitudes medidas (largo, sbar, lobulo de
			// luz, contraste, Q, alfa, ancho). Solo se escribe cuando el llamador
			// lo pide, asi que no toca la pureza del PNG (R6).
			QString dbgRuns;
			// R2: espaciado EUCLIDEO minimo entre laminas aceptadas (px). min_gap
			// mide arco DENTRO de una componente y no ve dos laminas que corren
			// paralelas y pegadas por caminos distintos del contorno, ni las
			// laminas internas: de ahi la "escuadra de la cadera" de la r2 (una
			// horizontal de silueta + una vertical interna encima). Se aplica a
			// las laminas de silueta entre si Y a las internas contra todo lo ya
			// aceptado, con la MISMA exencion de par en V que min_gap.
			// sep_px=0 = comportamiento r2.
			double sep_px = 250.0;
			// GOLD r3: separacion propia de las laminas INTERNAS contra las de
			// SILUETA ya aceptadas. Medido: los trazos internos del faro
			// (manual 9 en el pecho, 10/11 en el guante) viven a 135-158 px de
			// la silueta, asi que un sep_px de 200 px los prohibe por
            // construccion -- y son el 12% de la masa del oro y todo el
			// contenido del gate G14. Entre internas se sigue usando sep_px.
			// int_sep < 0 = usar sep_px (comportamiento previo).
			double int_sep = 140.0;
			// R2: los umbrales del PRE-FILTRO, que en r1/r2 eran constantes
			// cableadas del motor (0.22 / 0.20 / 0.34). Medido en la r3: con
			// ellos fijos, ni `density` ni `min_run` ni `min_gap` mueven nada
			// -- el borde del brazo derecho (donde el faro SI pinta, laminas
			// manuales 6 y 8) no llega ni a ser candidato. t_seed siembra el
			// run, t_lo lo admite y [t_lo,t_hi] es la rampa suave de alfa.
			double t_seed = 0.20;
			double t_lo = 0.20;
			double t_hi = 0.34;
			// R2 (el hallazgo medido de la r3): el CONTRASTE de borde no explica
			// donde pinta el faro. Medido bajo cada lamina manual (interior vs
			// fondo real, misma formula que E3): las laminas 6 (hombro derecho,
			// 91 px), 8 (brazo derecho, 213 px), 9 (diagonal del pecho, 404 px) y
			// 16 (216 px) caen en c01 = 0.10 / 0.06 / 0.02 / 0.09 -- contraste
			// practicamente NULO, y hasta negativo (el fondo es MAS claro que el
			// sujeto) -- y Antonio las pinta igual. Con c^contrast_pow eso da
			// ~0.001 y esos bordes no llegan ni a candidatos: 17% de la masa del
			// oro es inalcanzable a cualquier t_seed (medido en el barrido r29).
			// `contrast_bias` es el SUELO del termino de contraste, misma
			// convencion que light_bias: 0 = como r1/r2 (el contraste manda del
			// todo), 1 = el contraste no influye y decide largo x luz.
			double contrast_bias = 0.55;

			// ==== GOLD tanda 2 (SPEC_WHITE_DETAILS_GOLD.md SS12-SS17) ====
			// D-A (SS13): mapa de PROFUNDIDAD para bordes de OCLUSION (E12).
			// depth vacio/no-carga/no-casa de tamano => E12 apagada, byte-
			// identica a la tanda 1 (G22).
			QString depth;
			QString depthJson;
			// afinado t2r1 (MEDIDO sobre escena_depth.png, barrido ronda 203 de
			// 9 combinaciones): con 0.06/0.25 el candidato del PECHO nace con
			// c01=0.39 y sbar=0.207 < t_lo=0.20+ => la rampa de alfa lo mata y
			// dlam=0. Con 0.025/0.07 el mismo trazo sale con sbar suficiente y
			// el recall de la zona ORO del pecho pasa de 0.00 a 0.93 (G17).
			double int_depth_rel = 0.025;
			double int_depth_rel_hi = 0.07;
			double int_depth_gmin = 4.0;
			double depth_max = 2.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double depth_min_px = 120.0;
			// afinado t2r1: radio de ENLACE de la cresta de profundidad (px a
			// k=1). La cresta de oclusion llega partida en trozos separados por
			// 2..12 px y ninguno alcanza min_run; con 4 px de enlace los trozos
			// del pecho se funden en una sola cresta de 319 px de diagonal.
			// depth_gap=0 => comportamiento anterior exacto.
			double depth_gap = 4.0;
			// D-B (SS14.1): Qcut que muerde de verdad + densGain (density ya
			// no es inerte cuando sobran huecos de K). dens_gain=0 => densGain
			// siempre 1 (comportamiento tanda 1).
			double dens_gain = 0.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			// D-C (SS14.2): peso por RANGO de prominencia (contraste x largo^0.15)
			// en vez del reparto por sbarMean; terminal de lamina a AGUJA (alfa
			// por muestra). w_mode=0 => formula w_rel/w_len de la tanda 1 intacta.
			double w_mode = 1.0;
			double w_rank_con = 1.0;
			double w_rank_len = 0.15;
			double w_rank_lo = 0.60;
			double w_rank_hi = 1.35;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double w_rank_exp = 1.6;  // GOLD tanda 2 (afinado r3): default = el oro de la ronda 3 (G5: CV 0.317 -> 0.332)
			double a_rank_lo = 0.85;
			double tail_frac = 0.15;
			double tail_exp = 1.6;
			double tail_alpha_lo = 0.0;
			// D-D (SS14.3): remates en L (rama larga que continua la vertical
			// en la esquina, distinto del tick corto de E8). remate_max=0 =>
			// comportamiento tanda 1.
			double remate_max = 1.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double remate_min_run = 150.0;
			double remate_frac = 0.85;
			double remate_len_max = 420.0;
			double remate_dist = 16.0;
			double remate_width = 1.00;
			// afinado t2r1: radio euclideo propio del remate (<0 = sep_px).
			double remate_sep = 60.0;
			// afinado t2r1: contraste minimo bajo el remate (0 = sin gate).
			double remate_c01 = 0.0;
			// afinado t2r1: 0 = recorrer las madres por Q (orden de la tanda 2
			// original), 1 = por PROMINENCIA (contraste x largo, la magnitud de
			// D-C). El faro remata en L las DOS esquinas de la BOTA, que es su
			// trazo mas GRUESO, no el de mayor Q (la capa).
			double remate_order = 1.0;
			// D-C(c) (SS14.2c): pair_len_max ya vive en el bloque de campos
			// r4 de arriba (se cambio su default in-place a 90, ver ahi) --
			// no se redeclara aqui para no duplicar el miembro del struct.
			// D-E (SS14.4): fusion de runs colineales tras la seleccion (el
			// gate G8). gap_merge sube de 25 a 40 (default cambiado);
			// gap_sbar_frac reemplaza el suelo fijo 0.5*t_seed del hueco.
			double gap_sbar_frac = 0.35;
			double bridge_gap = 40.0;
			double bridge_ang = 25.0;
			double bridge_max = 6.0;
			// afinado t2r2 (D-E/G8): apoyo de la cuerda recta en el borde, ver
			// HugSpineOutward. 0 = tanda 1/t2r1 exacto.
			double hug = 0.4;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			// afinado t2r2 (D-A, punto 2 del veredicto t2r1): `straighten` NO se
			// aplicaba a kind=1 y por eso la lamina de PROFUNDIDAD del cuello
			// seguia el escalon de la disparidad pixel a pixel ("trazo
			// tembloroso" del veredicto visual). <0 = usar `straighten`;
			// 0 = comportamiento t2r1 exacto.
			double depth_straight = 0.0;
			// afinado t2r2 (D-G, CADENAS): MEDIDO sobre manual_laminas.json --
			// la distancia de CADA lamina del faro a su vecina mas cercana es
			// 2.8 / 4.0 / 5.1 / 5.1 / 5.1 / 5.8 / 5.8 / 6.3 / 8.2 px en 10 de
			// las 16, y >=114 px en las otras 6. El faro NO reparte trazos
			// sueltos cada 250 px: dibuja GRUPOS que se tocan (el gorro
			// id1/2/3/4, el guante id10/id11, la bota id17/18/19/22 y el
			// HOMBRO id6 + BRAZO id8 a 2.8 px) separados por huecos grandes.
			// `sep_px` mata esos grupos por construccion salvo la exencion de
			// par en V, que solo vale entre kind=0 de la misma componente y
			// con lenR <= pair_len_max. chain_px extiende la misma idea a
			// cualquier par (incluida silueta-vs-profundidad, que es lo que
			// tapaba el hombro: el candidato de 386 px cae a ~40 px de la
			// lamina de profundidad del brazo). Solo exime si hay GIRO
			// (chain_ang), para no admitir un duplicado paralelo encima, y con
			// un tope duro chain_max. chain_px=0 => comportamiento t2r1 exacto.
			double chain_px = 260.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double chain_ang = 50.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			double chain_max = 3.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			// afinado t2r2 (D-G): largo minimo de un candidato para poder
			// eximirse por cadena. MEDIDO: con chain_max=4 el presupuesto se lo
			// lleva por Q el RIZO del bajo de la capa (185 px, Q=0.818), que el
			// faro NO pinta -- alli su lamina se apaga en aguja -- y deja fuera
			// al HOMBRO (386 px, Q=0.772). Los eslabones cortos del faro
			// (id3=32, id4=20, id10=59, id11=69 px) no son cadenas: son ticks y
			// pares en V, que ya tienen su propio camino (v_max/pair_dist).
			// 0 = sin filtro.
			double chain_min = 260.0;  // GOLD tanda 2 (afinado r2): default = el oro de la ronda 2
			// afinado t2r3 (D-H, RECORTE AL LOBULO DOMINANTE): el unico delator
			// visual que quedaba en el veredicto t2r2 es UNA lamina: el
			// candidato de 386 px (1847,452)->(2159,664) cubre la ladera
			// DERECHA del gorro Y el hombro de un solo trazo, mientras el faro
			// pinta SOLO el tramo final (id6, (2046,593)->(2104,654), 91.5 px)
			// sobre la MISMA recta. METRICAS_MANUAL lo dice sin ambiguedad:
			// "1 solo pico por perfil" (el gate G6 ya lo mide sobre la salida).
			// Un run de contorno puede atravesar dos lobulos de iluminacion /
			// contraste; el faro solo dibuja el dominante. Aqui, DESPUES de la
			// seleccion (para no mover la dinamica del top-K ni de las cadenas,
			// que estan medidas y verdes), la lamina se recorta al tramo
			// contiguo alrededor del pico del perfil donde la senal se mantiene
			// por encima de trim_rel x pico.
			//   trim_rel = 0  => APAGADO, comportamiento t2r2 byte a byte.
			//   trim_src: 0=sbar 1=lobe 2=c01 3=lobe*c01.
			//   trim_abs: el pico debe superar esto o NO se recorta (protege a
			//     los runs de senal plana ~0 -- bota, pierna -- de que un pico
			//     de ruido decida el corte).
			//   trim_flat: MEDIDO sobre el volcado punto a punto del oro t2r2
			//     (evid/t2r3/base_dbg.txt, lineas `prof`). La razon
			//     media/pico del perfil de sbar de cada lamina aceptada es
			//     0.83 (capa) 0.93 (gorro) 0.92 (bota_h) 0.97 (pierna) 0.91
			//     (bota_v) ... y 0.49 en la lamina del DELATOR. Un run del
			//     faro tiene el perfil casi plano o un pico ancho; el delator
			//     es un pico corto seguido de 300 px de meseta muerta
			//     (sbar 0.30, c01 0.00). Solo se recorta si media/pico <=
			//     trim_flat: asi el bisturi toca UNA lamina y no la capa.
			//   trim_min: px a k=1; si el lobulo dominante queda por debajo,
			//     trim_drop decide (0 = dejar la lamina entera como t2r2,
			//     1 = TIRAR la lamina: un pico mas corto que min_run no es una
			//     lamina del faro, es un artefacto del run largo).
			//   trim_keep: fraccion MAXIMA del largo que se puede quitar.
			double trim_rel = 0.7;  // GOLD tanda 2 (afinado r3): default = el oro de la ronda 3
			double trim_src = 0.0;
			double trim_abs = 0.40;
			double trim_flat = 0.65;
			double trim_min = 100.0;
			double trim_drop = 1.0;
			double trim_keep = 1.0;
			// afinado t2r3 (D-H bis, RESCATE POR ESQUINA): medido punto a punto
			// sobre el run del delator, el tramo que el faro SI pinta (id6,
			// (2046,593)->(2104,654)) no se distingue por NINGUNA senal local
			// -- sbar 0.298, lobe 0.29, c01 0.00, identicos a los 240 px de
			// ladera que el faro deja en blanco. Lo que SI es medible es su
			// GEOMETRIA: la lamina del faro MUERE en la esquina gruesa del
			// hombro (2108,645) y mide ~91 px hacia atras. Es la misma regla
			// de D-D (el remate nace en la esquina), aplicada al run muerto:
			// en vez de tirarlo entero, se queda su tramo terminal de
			// trim_tail px que ENTRA en la esquina. 0 = tirar (D-H a secas).
			double trim_tail = 60.0;  // GOLD tanda 2 (afinado r3): default = el oro de la ronda 3
			double trim_corner = 20.0;
			// afinado t2r3: contraste MEDIO minimo bajo un candidato para poder
			// eximirse por CADENA. MEDIDO (dbg del oro t2r2): c01Mean de las
			// laminas aceptadas = 0.755 / 0.957 / 0.992 / 0.669 / 1.000 /
			// 1.000 / 0.261 / 0.028. La cadena de la PIERNA (0.9996) y la del
			// DELATOR (0.2614) se separan por un orden de magnitud: el faro
			// encadena trazos que tienen un borde real debajo. 0 = sin gate
			// (comportamiento t2r2 exacto).
			double chain_c01 = 0.0;
		};

		// afinado t2r3: los mandos NUEVOS de esta ronda se parsean aparte. La
		// cadena de `else if` de ParseOpts llego al limite de anidamiento de
		// bloques de MSVC (C1061) al anadir los diez de D-H; sacarlos aqui la
		// baja en nueve niveles y deja sitio para las rondas siguientes.
		bool ParseOptT2R3(const QString& key, double num, bool numOk, WdParams& p)
		{
			if (key == "trim_rel") { if (numOk) p.trim_rel = num; return true; }
			if (key == "trim_src") { if (numOk) p.trim_src = num; return true; }
			if (key == "trim_abs") { if (numOk) p.trim_abs = num; return true; }
			if (key == "trim_min") { if (numOk) p.trim_min = num; return true; }
			if (key == "trim_keep") { if (numOk) p.trim_keep = num; return true; }
			if (key == "trim_flat") { if (numOk) p.trim_flat = num; return true; }
			if (key == "trim_drop") { if (numOk) p.trim_drop = num; return true; }
			if (key == "trim_tail") { if (numOk) p.trim_tail = num; return true; }
			if (key == "trim_corner") { if (numOk) p.trim_corner = num; return true; }
			if (key == "chain_c01") { if (numOk) p.chain_c01 = num; return true; }
			return false;
		}

		bool ParseOpts(const QString& opts, WdParams& p, QString& err)
		{
			if (opts.isEmpty())
				return true;
			QStringList parts = opts.split(';');
			for (const QString& partRaw : parts)
			{
				QString part = partRaw.trimmed();
				if (part.isEmpty()) continue;
				int eq = part.indexOf('=');
				if (eq < 0) continue; // token sin '=' -- defensivo, el formato normativo siempre lo trae
				QString key = part.left(eq);
				QString val = part.mid(eq + 1);
				bool numOk = false;
				double num = val.toDouble(&numOk); // locale-agnostico (siempre C locale), SS0.8/C2

				if (key == "angle") { if (numOk) p.angle = num; }
				else if (key == "density") { if (numOk) p.density = num; }
				else if (key == "width") { if (numOk) p.width = num; }
				else if (key == "light_bias") { if (numOk) p.light_bias = num; }
				else if (key == "light_pow") { if (numOk) p.light_pow = num; }
				else if (key == "contrast_pow") { if (numOk) p.contrast_pow = num; }
				else if (key == "straight_floor") { if (numOk) p.straight_floor = num; }
				else if (key == "smooth_win") { if (numOk) p.smooth_win = num; }
				else if (key == "min_run") { if (numOk) p.min_run = num; }
				else if (key == "min_gap") { if (numOk) p.min_gap = num; }
				else if (key == "gap_merge") { if (numOk) p.gap_merge = num; }
				else if (key == "q_len") { if (numOk) p.q_len = num; }
				else if (key == "hyst") { if (numOk) p.hyst = num; }
				else if (key == "taper_exp") { if (numOk) p.taper_exp = num; }
				else if (key == "w_lo") { if (numOk) p.w_lo = num; }
				else if (key == "w_hi") { if (numOk) p.w_hi = num; }
				else if (key == "v_max") { if (numOk) p.v_max = num; }
				else if (key == "v_angle_min") { if (numOk) p.v_angle_min = num; }
				else if (key == "turn_split") { if (numOk) p.turn_split = num; }
				else if (key == "turn_max") { if (numOk) p.turn_max = num; }
				else if (key == "turn_keep") { if (numOk) p.turn_keep = num; }
				else if (key == "turn_win") { if (numOk) p.turn_win = num; }
				else if (key == "straighten") { if (numOk) p.straighten = num; }
				else if (key == "pair_dist") { if (numOk) p.pair_dist = num; }
				else if (key == "pair_angle") { if (numOk) p.pair_angle = num; }
				else if (key == "internal") { if (numOk) p.internal = num; }
				else if (key == "internal_max") { if (numOk) p.internal_max = num; }
				else if (key == "obj_min_frac") { if (numOk) p.obj_min_frac = num; }
				else if (key == "bg") p.bg = val;
				else if (key == "out_curves") p.outCurves = val;
				else if (key == "curves_width") { if (numOk) p.curves_width = num; }
				else if (key == "curves_ramp") { if (numOk) p.curves_ramp = num; }
				else if (key == "curves_strength") { if (numOk) p.curves_strength = num; }
				else if (key == "curves_alpha") { if (numOk) p.curves_alpha = num; }
				else if (key == "outline_width") { if (numOk) p.outline_width = num; }
				else if (key == "outline_alpha") { if (numOk) p.outline_alpha = num; }
				else if (key == "seed") { if (numOk) p.seed = num; }
				else if (key == "bake") { if (numOk) p.bake = num; }
				else if (key == "v_dist") { if (numOk) p.v_dist = num; }
				else if (key == "v_contrast_min") { if (numOk) p.v_contrast_min = num; }
				else if (key == "v_light_min") { if (numOk) p.v_light_min = num; }
				else if (key == "int_depth_min") { if (numOk) p.int_depth_min = num; }
				else if (key == "int_grad_min") { if (numOk) p.int_grad_min = num; }
				else if (key == "int_min_px") { if (numOk) p.int_min_px = num; }
				else if (key == "clip_depth") { if (numOk) p.clip_depth = num; }
				else if (key == "alpha_sat") { if (numOk) p.alpha_sat = num; }
				else if (key == "w_rel") { if (numOk) p.w_rel = num; }
				else if (key == "q_light_bias") { if (numOk) p.q_light_bias = num; }
				else if (key == "q_light_pow") { if (numOk) p.q_light_pow = num; }
				else if (key == "up_mode") { if (numOk) p.up_mode = num; }
				else if (key == "up_corner_dist") { if (numOk) p.up_corner_dist = num; }
				else if (key == "w_len") { if (numOk) p.w_len = num; }
				else if (key == "w_ref") { if (numOk) p.w_ref = num; }
				else if (key == "k_base") { if (numOk) p.k_base = num; }
				else if (key == "k_span") { if (numOk) p.k_span = num; }
				else if (key == "v_len_frac") { if (numOk) p.v_len_frac = num; }
				else if (key == "v_len_min") { if (numOk) p.v_len_min = num; }
				else if (key == "v_len_max") { if (numOk) p.v_len_max = num; }
				else if (key == "v_width") { if (numOk) p.v_width = num; }
				else if (key == "ext_thr") { if (numOk) p.ext_thr = num; }
				else if (key == "ext_max") { if (numOk) p.ext_max = num; }
				else if (key == "ext_turn") { if (numOk) p.ext_turn = num; }
				else if (key == "q_sbar_bias") { if (numOk) p.q_sbar_bias = num; }
				else if (key == "q_con_bias") { if (numOk) p.q_con_bias = num; }
				else if (key == "pair_len_max") { if (numOk) p.pair_len_max = num; }
				else if (key == "dbg_runs") p.dbgRuns = val;
				else if (key == "sep_px") { if (numOk) p.sep_px = num; }
				else if (key == "int_sep") { if (numOk) p.int_sep = num; }
				else if (key == "t_seed") { if (numOk) p.t_seed = num; }
				else if (key == "t_lo") { if (numOk) p.t_lo = num; }
				else if (key == "t_hi") { if (numOk) p.t_hi = num; }
				else if (key == "contrast_bias") { if (numOk) p.contrast_bias = num; }
				// ---- GOLD tanda 2 ----
				else if (key == "depth") p.depth = val;
				else if (key == "depth_json") p.depthJson = val;
				else if (key == "int_depth_rel") { if (numOk) p.int_depth_rel = num; }
				else if (key == "int_depth_rel_hi") { if (numOk) p.int_depth_rel_hi = num; }
				else if (key == "int_depth_gmin") { if (numOk) p.int_depth_gmin = num; }
				else if (key == "depth_max") { if (numOk) p.depth_max = num; }
				else if (key == "depth_min_px") { if (numOk) p.depth_min_px = num; }
				else if (key == "depth_gap") { if (numOk) p.depth_gap = num; }
				else if (key == "dens_gain") { if (numOk) p.dens_gain = num; }
				else if (key == "w_mode") { if (numOk) p.w_mode = num; }
				else if (key == "w_rank_con") { if (numOk) p.w_rank_con = num; }
				else if (key == "w_rank_len") { if (numOk) p.w_rank_len = num; }
				else if (key == "w_rank_lo") { if (numOk) p.w_rank_lo = num; }
				else if (key == "w_rank_hi") { if (numOk) p.w_rank_hi = num; }
				else if (key == "w_rank_exp") { if (numOk) p.w_rank_exp = num; }
				else if (key == "a_rank_lo") { if (numOk) p.a_rank_lo = num; }
				else if (key == "tail_frac") { if (numOk) p.tail_frac = num; }
				else if (key == "tail_exp") { if (numOk) p.tail_exp = num; }
				else if (key == "tail_alpha_lo") { if (numOk) p.tail_alpha_lo = num; }
				else if (key == "remate_max") { if (numOk) p.remate_max = num; }
				else if (key == "remate_min_run") { if (numOk) p.remate_min_run = num; }
				else if (key == "remate_frac") { if (numOk) p.remate_frac = num; }
				else if (key == "remate_len_max") { if (numOk) p.remate_len_max = num; }
				else if (key == "remate_dist") { if (numOk) p.remate_dist = num; }
				else if (key == "remate_width") { if (numOk) p.remate_width = num; }
				else if (key == "remate_sep") { if (numOk) p.remate_sep = num; }
				else if (key == "remate_c01") { if (numOk) p.remate_c01 = num; }
				else if (key == "remate_order") { if (numOk) p.remate_order = num; }
				else if (key == "gap_sbar_frac") { if (numOk) p.gap_sbar_frac = num; }
				else if (key == "bridge_gap") { if (numOk) p.bridge_gap = num; }
				else if (key == "bridge_ang") { if (numOk) p.bridge_ang = num; }
				else if (key == "bridge_max") { if (numOk) p.bridge_max = num; }
				else if (key == "hug") { if (numOk) p.hug = num; }
				else if (key == "depth_straight") { if (numOk) p.depth_straight = num; }
				else if (key == "chain_px") { if (numOk) p.chain_px = num; }
				else if (key == "chain_ang") { if (numOk) p.chain_ang = num; }
				else if (key == "chain_max") { if (numOk) p.chain_max = num; }
				else if (key == "chain_min") { if (numOk) p.chain_min = num; }
				else if (ParseOptT2R3(key, num, numOk, p)) { }
				else
				{
					err = QString("error:unknown_opt:") + key;
					return false;
				}
			}
			return true;
		}

		// GOLD tanda 2 (D-E SS14.4b): ancho de un AcceptedRun EN u=0 (atFront)
		// o u=1 (u=1), reconstruyendo EXACTAMENTE lo que el rasterizador
		// dibujo en ese extremo (mismo TaperProfile + misma aguja de D-C si
		// tail_frac>0) -- es "w_extremo_a"/"w_extremo_b" del puente (E14).
		double EndWidthAtBoundary(const AcceptedRun& ar, bool atFront, const WdParams& P)
		{
			double u = atFront ? 0.0 : 1.0;
			double w = ar.wRun * TaperProfile(u, ar.up, P.taper_exp);
			if (P.tail_frac > 0.0)
			{
				bool freeStart = ar.up > 0.0;
				bool freeEnd = ar.up < 1.0;
				double ttStart = freeStart ? Clamp01(u / P.tail_frac) : 1.0;
				double ttEnd = freeEnd ? Clamp01((1.0 - u) / P.tail_frac) : 1.0;
				double tt = std::min(ttStart, ttEnd);
				double needle = std::pow(tt, std::max(1e-6, P.tail_exp));
				w *= needle;
			}
			return w;
		}
	}

	QString Mi3WhiteDetailsRunOpt(const QString& srcPng, const QString& outPng, const QString& opts)
	{
		WdParams P;
		QString perr;
		if (!ParseOpts(opts, P, perr))
			return perr;

		QImage img(srcPng);
		if (img.isNull())
			return QString("error:cant_load_src:") + srcPng;
		img = img.convertToFormat(QImage::Format_RGBA8888);
		IntType W = img.width(), H = img.height();
		if (W <= 0 || H <= 0)
			return QString("error:empty_image");
		double k = std::max(0.15, (double)H / 2160.0);

		// angle < 0 => auto (defensivo: los llamadores GML ya resuelven esto
		// antes de construir la cadena opts, pero el motor se resuelve solo
		// tambien si le llega -1 directamente, p.ej. desde whitedetails.runopt)
		double angleDeg = P.angle;
		if (angleDeg < 0.0)
		{
			double a = Mi3WhiteDetailsAutoAngle();
			angleDeg = (a == -999.0) ? 135.0 : a;
		}
		double angRad = angleDeg * DEGTORAD;
		Vec2 L2{ std::cos(angRad), -std::sin(angRad) };

		// GOLD tanda 3 (E-A, SPEC_WHITE_DETAILS_GOLD.md SS20): la imagen de
		// TRABAJO de toda la deteccion/scoring (Y[], de la que salen
		// contraste E3, luminancia interior, cresta E9, etc.) es el sujeto
		// con la CAPA DE CURVAS de Antonio ya compuesta encima -- exactamente
		// la misma capa que E10 emite en out_curves: la LUT (BuildCurvesLUT)
		// aplicada SOLO dentro de la banda de borde (curves_width + rampa,
		// por curves_strength y curves_alpha), no en todo el sujeto.
		//
		// Medido (afinado tanda 3 r1, ver SPEC SS20.3): el "Curvas 7" que el
		// PSD trae horneado en reg/sujeto_reg.png -- la imagen sobre la que
		// se calibro TODO el faro -- vive en esa banda y solo en esa banda.
		// Contra el sujeto crudo del export real, |diff| medio = 66.75 en la
		// banda plena, 23.08 en la rampa y 3.07 fuera (ruido de render). Por
		// eso el control V del barrido de aislamiento t2 (que es literalmente
		// escena_subject + escena_wdcurvas compuesta) reproducia el oro:
		// F=0.887 contra F=0.886 del banco. Aplicar la LUT a TODO el sujeto
		// deja la imagen de trabajo ~70 niveles por encima de cualquier cosa
		// que el faro haya visto: F cae a 0.854 y aparece un falso positivo
		// nuevo en el rizo de la capa (masa 1172).
		//
		// `img` (raw) se deja intacto para out_curves (mas abajo, misma LUT,
		// mismo hash de siempre) y para el bake (compone sobre el archivo
		// ORIGINAL). El banco alimenta una version CRUDA equivalente
		// (wd_uncurve_banda.py invierte la curva SOLO en la banda) para no
		// aplicarla dos veces -- ver C6 (SPEC SS20).
		std::array<uint8_t, 256> lut = BuildCurvesLUT();
		std::vector<uint8_t> M((size_t)(W * H), 0), notM((size_t)(W * H), 0);
		std::vector<float> Y((size_t)(W * H), 0.f);
		for (IntType y = 0; y < H; y++)
		{
			const uchar* line = img.constScanLine((int)y);
			for (IntType x = 0; x < W; x++)
			{
				size_t idx = (size_t)(y * W + x);
				bool in = line[(size_t)x * 4 + 3] >= 128;
				M[idx] = in ? 1 : 0; notM[idx] = in ? 0 : 1;
			}
		}
		std::vector<float> Dext = ChamferDistance(notM, W, H);

		// Y[] = luminancia del sujeto CON la capa de curvas compuesta. La
		// banda es bit a bit la de E10 (mismo cw/ramp/M), y el color es el
		// "source-over" de esa capa sobre el pixel crudo:
		//   cur = raw + (lut(raw) - raw) * curves_strength
		//   eff = raw + (cur - raw) * band * curves_alpha
		{
			double cwY = P.curves_width * k;
			double rampY = P.curves_ramp * k;
			for (IntType y = 0; y < H; y++)
			{
				const uchar* line = img.constScanLine((int)y);
				for (IntType x = 0; x < W; x++)
				{
					size_t idx = (size_t)(y * W + x);
					double d = (double)Dext[idx];
					double band;
					if (d <= cwY) band = 1.0;
					else if (d <= cwY + rampY) band = rampY > 1e-9 ? 1.0 - (d - cwY) / rampY : 0.0;
					else band = 0.0;
					if (!M[idx]) band = 0.0;
					double a = Clamp01(band) * P.curves_alpha;

					double c[3];
					for (IntType ch = 0; ch < 3; ch++)
					{
						double raw = (double)line[(size_t)x * 4 + (size_t)ch];
						if (a <= 0.0) { c[ch] = raw; continue; }
						double cur = ClampRange(raw + ((double)lut[(size_t)line[(size_t)x * 4 + (size_t)ch]] - raw)
							* P.curves_strength, 0.0, 255.0);
						c[ch] = raw + (cur - raw) * a;
					}
					Y[idx] = (float)(0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]);
				}
			}
		}

		std::vector<float> Ybg;
		bool bgOk = false;
		if (!P.bg.isEmpty())
		{
			QImage bgImg(P.bg);
			if (!bgImg.isNull())
			{
				bgImg = bgImg.convertToFormat(QImage::Format_RGBA8888);
				if (bgImg.width() == W && bgImg.height() == H)
				{
					Ybg.assign((size_t)(W * H), 0.f);
					for (IntType y = 0; y < H; y++)
					{
						const uchar* line = bgImg.constScanLine((int)y);
						for (IntType x = 0; x < W; x++)
						{
							size_t idx = (size_t)(y * W + x);
							double r = line[(size_t)x * 4 + 0], g = line[(size_t)x * 4 + 1], b = line[(size_t)x * 4 + 2];
							Ybg[idx] = (float)(0.2126 * r + 0.7152 * g + 0.0722 * b);
						}
					}
					bgOk = true;
				}
			}
		}

		// ---- GOLD tanda 2 (D-A SS13.2): mapa de PROFUNDIDAD opcional ----
		// depth="" (o no carga, o no casa W,H) => E12 queda completamente
		// inerte, info lleva "depth=off" y el comportamiento es EXACTAMENTE
		// el de la tanda 1 (requisito del director, gate G22).
		std::vector<float> Dsp;
		bool depthOk = false;
		bool depthJsonOk = false;
		double depthNear = 0.0, depthFar = 0.0;
		if (!P.depth.isEmpty())
		{
			QImage depthImg(P.depth);
			if (!depthImg.isNull())
			{
				depthImg = depthImg.convertToFormat(QImage::Format_RGBA8888);
				if (depthImg.width() == W && depthImg.height() == H)
				{
					Dsp.assign((size_t)(W * H), 0.f);
					for (IntType y = 0; y < H; y++)
					{
						const uchar* line = depthImg.constScanLine((int)y);
						for (IntType x = 0; x < W; x++)
							Dsp[(size_t)(y * W + x)] = (float)line[(size_t)x * 4 + 0]; // gris: R=G=B
					}
					depthOk = true;
				}
			}
			if (depthOk && !P.depthJson.isEmpty())
			{
				// Solo para auditoria (near/far en el info); "si falta, no pasa
				// nada: el motor sigue" (SS13.1) -- el algoritmo trabaja en
				// disparidad cruda y no depende de este sidecar.
				QFile jf(P.depthJson);
				if (jf.open(QIODevice::ReadOnly))
				{
					QByteArray blob = jf.readAll();
					jf.close();
					QJsonDocument doc = QJsonDocument::fromJson(blob);
					if (doc.isObject())
					{
						QJsonObject jo = doc.object();
						if (jo.contains("near") && jo.contains("far"))
						{
							depthNear = jo.value("near").toDouble();
							depthFar = jo.value("far").toDouble();
							depthJsonOk = true;
						}
					}
				}
			}
		}
		// D-A SS13.5.1 (ultima vineta): las dos etapas de bordes internos
		// compiten por el mismo sitio; si depth esta activo, internal (E9,
		// luminancia) se fuerza a 0, y el info lo dice ("int=depth").
		bool internalActive = (P.internal != 0.0) && !depthOk;
		// GOLD tanda 2 (D-A SS14.5): dcand/dlam para el info -- candidatos de
		// profundidad generados vs. finalmente rasterizados.
		IntType dcandCount = 0;
		IntType dlamCount = 0;

		// ---- E1: seleccion de OBJETOS (mata el loro) ----
		double minArea = 300.0 * k * k;
		std::vector<Component> compsAll;
		FindComponents(M, W, H, minArea, compsAll);
		std::vector<Component> comps;
		if (P.obj_min_frac > 0.0 && !compsAll.empty())
		{
			IntType areaMax = 0;
			for (auto& c : compsAll) areaMax = std::max(areaMax, c.area);
			double thresh = P.obj_min_frac * (double)areaMax;
			for (auto& c : compsAll) if ((double)c.area >= thresh) comps.push_back(c);
		}
		else comps = compsAll;

		// GOLD r3: mascara de los objetos SELECCIONADOS. E1 sacaba al loro de las
		// laminas de silueta pero E9 (bordes internos) seguia mirando M entera:
		// en cuanto sep_px liberaba una plaza de internal_max se colaba una
		// lamina sobre el loro (medido en el barrido r23). La seleccion de
		// objetos vale tambien para las internas.
		std::vector<uint8_t> Msel((size_t)(W * H), 0);
		{
			std::vector<uint8_t> selScratch((size_t)(W * H), 0);
			for (auto& c : comps)
			{
				std::vector<IPt> pxs = CollectComponentPixels(M, W, H, c.seed, selScratch);
				for (auto& p : pxs) Msel[(size_t)(p.y * W + p.x)] = 1;
			}
		}

		IntType wn = std::max<IntType>(1, (IntType)std::llround(6.0 * k));           // E2: ventana de tangente
		IntType rSm = std::max<IntType>(1, (IntType)std::llround(3.0 * k));          // E2: suavizado de posiciones
		IntType wr4 = std::max<IntType>(1, (IntType)std::llround(12.0 * k));         // E4: ventana de rectitud
		IntType ws = std::max<IntType>(1, (IntType)std::llround(P.smooth_win * k));  // E4: suavizado del campo de score
		IntType cwWin = std::max<IntType>(2, (IntType)std::llround(10.0 * k));       // E8: ventana de esquina
		double minRunK = P.min_run * k;
		double gapMergeK = P.gap_merge * k;

		std::vector<CompData> compData;
		std::vector<RunCand> candidates;
		std::vector<IPt> allContourPts;

		for (IntType ci = 0; ci < (IntType)comps.size(); ci++)
		{
			std::vector<IPt> contour = TraceContour(M, W, H, comps[(size_t)ci].seed);
			if ((IntType)contour.size() < 16) continue;
			allContourPts.insert(allContourPts.end(), contour.begin(), contour.end());

			std::vector<Vec2> C0(contour.size());
			for (size_t i = 0; i < contour.size(); i++) C0[i] = Vec2{ (double)contour[i].x, (double)contour[i].y };

			// ---- E2: contorno resampleado y suavizado ----
			std::vector<Vec2> Qc = ResampleClosed(C0, 1.0);
			std::vector<Vec2> S = BoxCircular2D(BoxCircular2D(Qc, rSm), rSm);
			IntType m = (IntType)S.size();
			if (m < 8) continue; // componente degenerada tras el resample

			std::vector<Vec2> normal;
			ComputeTangentNormal(S, wn, M, W, H, normal);

			// ---- E3 (contraste) + E4 (score) ----
			std::vector<double> c01(m, 0.0);
			std::vector<double> lobeArr(m, 0.0);
			std::vector<float> sArr(m, 0.f);
			// GOLD tanda 2 (D-B, SS14.1a): score SIN el factor de luz, en
			// paralelo a sArr (que SI lo lleva). Se suaviza con BoxCircular
			// (misma ventana ws) para dar sbarNL, y su media sobre un run
			// (sNLMean) es lo que usa la Q nueva en vez de sbarMean -- asi la
			// luz entra en Q UNA sola vez (via qLight), no dos.
			std::vector<float> sNLArr(m, 0.f);
			for (IntType j = 0; j < m; j++)
			{
				double lumIn = 0.0; IntType cntIn = 0;
				double dOffs[3] = { 4.0 * k, 6.0 * k, 8.0 * k };
				for (double d : dOffs)
				{
					Vec2 sp = S[(size_t)j] - normal[(size_t)j] * d;
					double yv;
					if (SampleYInMask(Y, M, W, H, sp.x, sp.y, yv)) { lumIn += yv; cntIn++; }
				}
				double lumInMean = cntIn > 0 ? lumIn / cntIn : 0.0;
				double c;
				if (bgOk)
				{
					double lumOut = 0.0; double dOffs2[3] = { 6.0 * k, 8.0 * k, 12.0 * k };
					for (double d : dOffs2)
					{
						Vec2 sp = S[(size_t)j] + normal[(size_t)j] * d;
						lumOut += SampleYClamped(Ybg, W, H, sp.x, sp.y);
					}
					lumOut /= 3.0;
					double con = lumInMean - lumOut;
					c = Clamp01((con - CON_LO) / (CON_HI - CON_LO));
				}
				else
				{
					c = Clamp01((lumInMean - IN_LO) / (IN_HI - IN_LO));
				}
				c01[(size_t)j] = c;

				bool onBorder = (S[(size_t)j].x <= 1.0 || S[(size_t)j].y <= 1.0 ||
					S[(size_t)j].x >= (double)W - 2.0 || S[(size_t)j].y >= (double)H - 2.0);
				if (onBorder) { sArr[(size_t)j] = 0.f; continue; }

				double lightLobe = std::pow(std::max(0.0, Dot(normal[(size_t)j], L2)), P.light_pow);
				lobeArr[(size_t)j] = lightLobe;
				double straight = StraightnessAt(S, j, wr4);
				double straightF = P.straight_floor + (1.0 - P.straight_floor) * straight;
				// GOLD r3: suelo de contraste (ver contrast_bias en WdParams).
				double cTerm = P.contrast_bias + (1.0 - P.contrast_bias) * std::pow(c, P.contrast_pow);
				double sv = cTerm * (P.light_bias + (1.0 - P.light_bias) * lightLobe) * straightF;
				sArr[(size_t)j] = (float)sv;
				// GOLD tanda 2 (D-B): mismo score, SIN el factor de luz.
				sNLArr[(size_t)j] = (float)(cTerm * straightF);
			}
			std::vector<float> sbar = BoxCircular(sArr, ws);
			std::vector<float> sbarNL = BoxCircular(sNLArr, ws);

			CompData cd;
			cd.S = S; cd.normal = normal; cd.sbar = sbar; cd.sbarNL = sbarNL; cd.c01 = c01; cd.lobe = lobeArr;

			// ---- E8 (parte 1): esquinas convexas candidatas de esta componente ----
			std::vector<double> turnDeg(m, 0.0);
			std::vector<uint8_t> isConvex(m, 0);
			for (IntType j = 0; j < m; j++)
			{
				IntType a = ((j - cwWin) % m + m) % m;
				IntType b = ((j + cwWin) % m + m) % m;
				Vec2 dirIn = S[(size_t)j] - S[(size_t)a];
				Vec2 dirOut = S[(size_t)b] - S[(size_t)j];
				turnDeg[(size_t)j] = AngleBetweenDeg(dirIn, dirOut);
				Vec2 dirInN = Normalize(dirIn), dirOutN = Normalize(dirOut);
				// convexo = el giro se aleja de M: (dirIn-dirOut) alineado con
				// la normal EXTERIOR (ver derivacion en el reporte de la mision)
				bool outward = Dot(dirInN - dirOutN, normal[(size_t)j]) > 0.0;
				isConvex[(size_t)j] = (turnDeg[(size_t)j] >= P.v_angle_min && outward) ? 1 : 0;
			}
			std::vector<IntType> cSorted;
			for (IntType j = 0; j < m; j++) if (isConvex[(size_t)j]) cSorted.push_back(j);
			std::stable_sort(cSorted.begin(), cSorted.end(), [&turnDeg](IntType a, IntType b)
				{ return turnDeg[(size_t)a] > turnDeg[(size_t)b]; });
			double sepMin = 20.0 * k;
			for (IntType cIdx : cSorted)
			{
				bool tooClose = false;
				for (IntType used : cd.cornerIdx)
				{
					double dd = std::min(ArcLenFwd(S, used, cIdx), ArcLenFwd(S, cIdx, used));
					if (dd < sepMin) { tooClose = true; break; }
				}
				if (!tooClose) cd.cornerIdx.push_back(cIdx);
			}
			std::sort(cd.cornerIdx.begin(), cd.cornerIdx.end());
			cd.usedCorner.assign(cd.cornerIdx.size(), 0);
			cd.turnDeg = turnDeg;

			compData.push_back(cd);
			IntType compIdx = (IntType)compData.size() - 1;
			const std::vector<Vec2>& Sref = compData[(size_t)compIdx].S;

			// ---- E5: runs con fusion a traves de muescas ----
			IntType cutJ = 0; float minVal = sbar[0];
			for (IntType j = 1; j < m; j++) if (sbar[(size_t)j] < minVal) { minVal = sbar[(size_t)j]; cutJ = j; }
			std::vector<IntType> order(m);
			for (IntType t = 0; t < m; t++) order[(size_t)t] = (cutJ + 1 + t) % m;

			std::vector<std::vector<IntType>> runs0;
			{
				// paso 1: semillas duras (sbar >= T_SEED), en rangos [rs,re) de `order`
				std::vector<std::pair<IntType, IntType>> seg;
				IntType t = 0;
				while (t < m)
				{
					if (sbar[(size_t)order[(size_t)t]] >= (float)P.t_seed)
					{
						IntType rs = t;
						while (t < m && sbar[(size_t)order[(size_t)t]] >= (float)P.t_seed) t++;
						seg.push_back(std::make_pair(rs, t));
					}
					else t++;
				}
				for (auto& sg : seg)
					runs0.push_back(std::vector<IntType>(order.begin() + sg.first, order.begin() + sg.second));
			}

			for (IntType iter = 0; iter < 8; iter++)
			{
				IntType nRuns = (IntType)runs0.size();
				if (nRuns <= 1) break;
				bool changed = false;
				std::vector<std::vector<IntType>> merged;
				std::vector<uint8_t> consumed((size_t)nRuns, 0);
				for (IntType i = 0; i < nRuns; i++)
				{
					if (consumed[(size_t)i]) continue;
					std::vector<IntType> cur = runs0[(size_t)i];
					consumed[(size_t)i] = 1;
					IntType i1 = (i + 1) % nRuns;
					while (i1 != i && !consumed[(size_t)i1])
					{
						IntType endCur = cur.back();
						IntType startNext = runs0[(size_t)i1].front();
						double gap = ArcLenFwd(Sref, endCur, startNext);
						Vec2 dirA = Sref[(size_t)cur.back()] - Sref[(size_t)cur.front()];
						Vec2 dirB = Sref[(size_t)runs0[(size_t)i1].back()] - Sref[(size_t)runs0[(size_t)i1].front()];
						double ang = AngleBetweenDeg(dirA, dirB);
						double gapMean = MeanSbarFwd(sbar, endCur, startNext);
						// GOLD tanda 2 (D-E SS14.4a): el suelo del hueco era
						// 0.5*t_seed fijo; gap_sbar_frac lo hace ajustable
						// (default 0.35). gap_sbar_frac=0.5 = comportamiento
						// tanda 1 (junto con gap_merge=25, ver G22).
						if (gap <= gapMergeK && ang <= GAP_ANG && gapMean >= P.gap_sbar_frac * P.t_seed)
						{
							AppendGapIndices(cur, endCur, startNext, m);
							cur.insert(cur.end(), runs0[(size_t)i1].begin(), runs0[(size_t)i1].end());
							consumed[(size_t)i1] = 1;
							changed = true;
							i1 = (i1 + 1) % nRuns;
						}
						else break;
					}
					merged.push_back(cur);
				}
				runs0 = merged;
				if (!changed) break;
			}

			// ---- E5b (GOLD r1, afinado Opus): PARTIR los runs en las esquinas ----
			// El manual no envuelve una esquina: la lamina MUERE en el vertice
			// y (a veces) nace otra al otro lado -- eso es el acento en V. El
			// motor r0 fusionaba a traves de picos de 90 grados y producia
			// pocos trazos larguisimos que trazaban la silueta entera.
			// Se parte (a) en los maximos locales de giro >= turn_split y
			// (b) donde el giro ACUMULADO del sub-run supera turn_max. El
			// indice de corte va en LOS DOS sub-runs, asi los dos se tocan en
			// el vertice (que es como estan los pares en V del faro, <=4.1 px).
			if (P.turn_split < 180.0 || P.turn_max < 360.0)
			{
				std::vector<std::vector<IntType>> splitRuns;
				for (auto& run : runs0)
				{
					IntType rn = (IntType)run.size();
					if (rn < 5) { splitRuns.push_back(run); continue; }
					std::vector<uint8_t> cutHere((size_t)rn, 0);
					for (IntType p = 1; p + 1 < rn; p++)
					{
						double td = cd.turnDeg[(size_t)run[(size_t)p]];
						if (td < P.turn_split) continue;
						IntType q0 = std::max<IntType>(0, p - cwWin), q1 = std::min<IntType>(rn - 1, p + cwWin);
						bool isMax = true;
						for (IntType q = q0; q <= q1; q++)
						{
							if (q == p) continue;
							double tq = cd.turnDeg[(size_t)run[(size_t)q]];
							if (tq > td || (tq == td && q < p)) { isMax = false; break; }
						}
						// GOLD r2 (afinado Opus, R1): una MUESCA de la silueta
						// blocky tiene giros LOCALES de 90 grados pero devuelve
						// el contorno a la MISMA direccion. El faro cruza esas
						// muescas de un solo trazo (la banda baja del manual
						// pasa el escalon entera y sale gruesa); el motor r1
						// partia ahi y salian dos trozos finos. Si la direccion
						// GRUESA (ventana turn_win) apenas cambia en el corte,
						// no se parte.
						if (isMax && P.turn_keep > 0.0)
						{
							IntType wideN = std::max<IntType>(2, (IntType)std::llround(P.turn_win * k));
							IntType a0 = std::max<IntType>(0, p - wideN), a1 = std::min<IntType>(rn - 1, p + wideN);
							if (a0 < p && a1 > p)
							{
								double coarse = AngleBetweenDeg(
									Sref[(size_t)run[(size_t)p]] - Sref[(size_t)run[(size_t)a0]],
									Sref[(size_t)run[(size_t)a1]] - Sref[(size_t)run[(size_t)p]]);
								if (coarse < P.turn_keep) isMax = false;
							}
						}
						if (isMax) cutHere[(size_t)p] = 1;
					}
					IntType sub = 0;
					for (IntType p = 1; p < rn; p++)
					{
						bool doCut = cutHere[(size_t)p] != 0;
						if (!doCut && (p - sub) >= 4)
						{
							IntType mid = sub + (p - sub) / 2;
							double bend = AngleBetweenDeg(Sref[(size_t)run[(size_t)mid]] - Sref[(size_t)run[(size_t)sub]],
								Sref[(size_t)run[(size_t)p]] - Sref[(size_t)run[(size_t)mid]]);
							if (bend > P.turn_max) doCut = true;
						}
						if (!doCut) continue;
						splitRuns.push_back(std::vector<IntType>(run.begin() + sub, run.begin() + p + 1));
						sub = p;
					}
					if (sub < rn - 1)
						splitRuns.push_back(std::vector<IntType>(run.begin() + sub, run.end()));
				}
				runs0 = splitRuns;
			}

			// ---- E5c (GOLD r3, R1/R2): EXTENSION DE EXTREMOS con histeresis ----
			// El seeding usa el umbral DURO T_SEED, y por eso la capa arrancaba
			// ~130 px por debajo del hombro y el ala del gorro moria ~40 px antes
			// de la muesca (los dos delatores medidos de la r2). Cada extremo se
			// alarga mientras sbar >= ext_thr*T_SEED, con tope de ext_max px.
			// Va DESPUES de fusionar y partir a proposito: si se hace sobre las
			// semillas, alarga runs que luego se fusionan y el partido cae en
			// sitios distintos (medido: la capa se rompia en tres trozos).
			// La ocupacion se calcula sobre los runs ORIGINALES y cada extremo
			// se para en la mitad del hueco libre, asi el resultado no depende
			// del orden en que se recorren los runs (determinismo, R6).
			if (P.ext_max > 0.0 && P.ext_thr > 0.0 && !runs0.empty())
			{
				float extT = (float)(P.ext_thr * P.t_seed);
				double extMaxK = P.ext_max * k;
				std::vector<uint8_t> occupied((size_t)m, 0);
				for (auto& run : runs0)
					for (IntType idx : run) occupied[(size_t)idx] = 1;
				std::vector<std::vector<IntType>> grown;
				grown.reserve(runs0.size());
				for (auto& run : runs0)
				{
					if (run.empty()) { grown.push_back(run); continue; }
					// hueco libre a cada lado (para no pasar del punto medio)
					double gapBack = 0.0;
					{
						IntType cur = run.front();
						for (IntType g = 0; g < m; g++)
						{
							IntType prev = ((cur - 1) % m + m) % m;
							if (occupied[(size_t)prev]) break;
							gapBack += Length(Sref[(size_t)cur] - Sref[(size_t)prev]);
							cur = prev;
						}
					}
					double gapFwd = 0.0;
					{
						IntType cur = run.back();
						for (IntType g = 0; g < m; g++)
						{
							IntType nxt = (cur + 1) % m;
							if (occupied[(size_t)nxt]) break;
							gapFwd += Length(Sref[(size_t)nxt] - Sref[(size_t)cur]);
							cur = nxt;
						}
					}
					// GOLD r4 (R1/R3): la extension se PARA en una esquina. E5b
					// acaba de partir el run ahi a proposito porque el faro no
					// envuelve un vertice; sin este freno la extension lo vuelve
					// a cruzar y sale el "rizo" (medido: 609 px sobrantes en el
					// extremo bajo de la capa). ext_turn=360 = como r3.
					std::vector<IntType> pre, post;
					{
						double lim = std::min(extMaxK, 0.5 * gapBack);
						double acc = 0.0; IntType cur = run.front();
						while (acc < lim)
						{
							IntType prev = ((cur - 1) % m + m) % m;
							if (occupied[(size_t)prev] || sbar[(size_t)prev] < extT) break;
							if (cd.turnDeg[(size_t)prev] >= P.ext_turn) break;
							acc += Length(Sref[(size_t)cur] - Sref[(size_t)prev]);
							if (acc > lim) break;
							pre.push_back(prev);
							cur = prev;
						}
					}
					{
						double lim = std::min(extMaxK, 0.5 * gapFwd);
						double acc = 0.0; IntType cur = run.back();
						while (acc < lim)
						{
							IntType nxt = (cur + 1) % m;
							if (occupied[(size_t)nxt] || sbar[(size_t)nxt] < extT) break;
							if (cd.turnDeg[(size_t)nxt] >= P.ext_turn) break;
							acc += Length(Sref[(size_t)nxt] - Sref[(size_t)cur]);
							if (acc > lim) break;
							post.push_back(nxt);
							cur = nxt;
						}
					}
					std::vector<IntType> out;
					out.reserve(pre.size() + run.size() + post.size());
					for (size_t q = pre.size(); q-- > 0;) out.push_back(pre[q]);
					out.insert(out.end(), run.begin(), run.end());
					out.insert(out.end(), post.begin(), post.end());
					grown.push_back(out);
				}
				runs0 = grown;
			}

			// ---- E6 (pre-filtro de candidatos de esta componente) ----
			for (auto& run : runs0)
			{
				if (run.empty()) continue;
				double lenR = ArcLenFwd(Sref, run.front(), run.back());
				double sum = 0.0;
				for (IntType idx : run) sum += sbar[(size_t)idx];
				double sbarMean = sum / (double)run.size();
				if (lenR < minRunK || sbarMean < P.t_lo) continue;
				// GOLD r2: el lobulo MEDIO del run entra en Q (ver q_light_bias).
				double lobeSum = 0.0;
				for (IntType idx : run) lobeSum += lobeArr[(size_t)idx];
				double lobeMean = lobeSum / (double)run.size();
				// GOLD r4 (R2): el contraste CRUDO entra en Q por su cuenta y el
				// peso de sbarMean es regulable (ver q_sbar_bias / q_con_bias).
				double c01Sum = 0.0;
				for (IntType idx : run) c01Sum += cd.c01[(size_t)idx];
				double c01Mean = c01Sum / (double)run.size();
				// GOLD tanda 2 (D-B SS14.1a): qStr pesa sNLMean (sbar SIN luz),
				// no sbarMean (que lleva la luz dentro) -- si no, con
				// q_sbar_bias < 1 la luz entraria dos veces (una aqui, otra en
				// qLight). Con el default q_sbar_bias=1.0 este termino no
				// influye (qSbar=1) y Q coincide EXACTO con la de la tanda 1
				// (aviso del implementador, SS14.1 nota "Q identica a la r3"/G22).
				double sNLSum = 0.0;
				for (IntType idx : run) sNLSum += cd.sbarNL[(size_t)idx];
				double sNLMean = sNLSum / (double)run.size();
				double Qr = ComputeQCand(lenR, c01Mean, lobeMean, sNLMean,
					P.q_len, P.q_con_bias, P.q_light_bias, P.q_light_pow, P.q_sbar_bias);
				RunCand rc; rc.kind = 0; rc.compIdx = compIdx; rc.idx = run; rc.lenR = lenR; rc.sbarMean = sbarMean;
				rc.sNLMean = sNLMean; rc.lobeMean = lobeMean; rc.c01Mean = c01Mean; rc.Q = Qr;
				candidates.push_back(rc);
			}
		}

		// ---- GOLD tanda 2 (D-A SS13.3-13.5): E12, crestas de DISCONTINUIDAD
		// de profundidad (bordes de OCLUSION). Va despues de E5c (el bucle por
		// componente de arriba, que ya incluye E5c) y antes del top-K de E6:
		// los candidatos kind=1 se anaden a `candidates` y compiten en el
		// MISMO top-K que la silueta (SS13.4). depthOk=false => bloque entero
		// inerte, cero candidatos kind=1, byte-identico a la tanda 1 (G22).
		if (depthOk)
		{
			double intDepthMinK = P.int_depth_min * k;
			std::vector<float> rmax((size_t)(W * H), 0.f);
			std::vector<uint8_t> dirStepIdx((size_t)(W * H), 0);
			static const IntType ddx[4] = { 1, 0, 1, 1 };
			static const IntType ddy[4] = { 0, 1, 1, -1 };
			for (IntType y = 0; y < H; y++)
			{
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					if (!Msel[(size_t)idx] || (double)Dext[(size_t)idx] <= intDepthMinK) continue;
					float best = 0.f; IntType bestD = 0;
					for (IntType dd = 0; dd < 4; dd++)
					{
						IntType ax = x - ddx[dd], ay = y - ddy[dd];
						IntType bx = x + ddx[dd], by = y + ddy[dd];
						if (ax < 0 || ax >= W || ay < 0 || ay >= H || bx < 0 || bx >= W || by < 0 || by >= H) continue;
						float av = Dsp[(size_t)(ay * W + ax)], bv = Dsp[(size_t)(by * W + bx)];
						float mn = std::min(av, bv), mx2 = std::max(av, bv);
						if (mn < (float)P.int_depth_gmin) continue;
						float rstep = (mx2 > 1e-6f) ? (std::abs(av - bv) / mx2) : 0.f;
						if (rstep > best) { best = rstep; bestD = dd; }
					}
					rmax[(size_t)idx] = best;
					dirStepIdx[(size_t)idx] = (uint8_t)bestD;
				}
			}
			std::vector<uint8_t> candMaskD((size_t)(W * H), 0);
			for (IntType i = 0; i < W * H; i++)
				if (Msel[(size_t)i] && (double)Dext[(size_t)i] > intDepthMinK && rmax[(size_t)i] >= (float)P.int_depth_rel)
					candMaskD[(size_t)i] = 1;
			std::vector<uint8_t> ridgeD((size_t)(W * H), 0);
			for (IntType y = 0; y < H; y++)
			{
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					if (!candMaskD[(size_t)idx]) continue;
					IntType dd = (IntType)dirStepIdx[(size_t)idx];
					IntType ax = x - ddx[dd], ay = y - ddy[dd];
					IntType bx = x + ddx[dd], by = y + ddy[dd];
					float gA = (ax >= 0 && ax < W && ay >= 0 && ay < H) ? rmax[(size_t)(ay * W + ax)] : 0.f;
					float gB = (bx >= 0 && bx < W && by >= 0 && by < H) ? rmax[(size_t)(by * W + bx)] : 0.f;
					if (rmax[(size_t)idx] >= gA && rmax[(size_t)idx] >= gB)
						ridgeD[(size_t)idx] = 1;
				}
			}
			// GOLD tanda 2, afinado t2r1 (D-A): ENLACE de la cresta a traves de
			// huecos. MEDIDO sobre escena_depth.png en la zona ORO del pecho
			// (id9, el trazo de 404 px): la discontinuidad de profundidad de la
			// capa sobre el torso EXISTE (rmax p99 = 0.1498 bajo el trazo del
			// faro) pero llega FRAGMENTADA -- a rel=0.018 la cresta se parte en
			// tres trozos de diagonal 85 / 66 / 146 px, todos por debajo de
			// min_run=100, y el candidato NUNCA nace. El hueco entre trozos es
			// de 2..12 px (medido: 1634->1636 y 1700->1712). Con un enlace de
			// radio 4 los tres se funden en UNA componente de 955 px y diagonal
			// 319 px (rel=0.018), que es el 79 % del trazo del faro.
			// Es la MISMA idea que D-E para los runs de contorno (fusionar a
			// traves de muescas), aplicada a la cresta de profundidad.
			// depth_gap=0 => comportamiento anterior EXACTO (mascara sin
			// dilatar, mismas componentes, mismo orden de descubrimiento).
			IntType depthGapI = (IntType)std::llround(P.depth_gap * k);
			std::vector<uint8_t> ridgeLink = ridgeD;
			if (depthGapI > 0)
			{
				std::vector<uint8_t> tmpDil((size_t)(W * H), 0);
				for (IntType it = 0; it < depthGapI; it++)
				{
					tmpDil = ridgeLink;
					for (IntType y = 0; y < H; y++)
					{
						for (IntType x = 0; x < W; x++)
						{
							if (ridgeLink[(size_t)(y * W + x)]) continue;
							bool hit = false;
							for (IntType dy = -1; dy <= 1 && !hit; dy++)
							{
								for (IntType dx = -1; dx <= 1; dx++)
								{
									IntType xx = x + dx, yy = y + dy;
									if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
									if (ridgeLink[(size_t)(yy * W + xx)]) { hit = true; break; }
								}
							}
							if (hit) tmpDil[(size_t)(y * W + x)] = 1;
						}
					}
					ridgeLink.swap(tmpDil);
				}
			}
			std::vector<Component> ridgeCompsD;
			// El area minima se comprueba sobre los pixeles de cresta REALES
			// (no sobre los dilatados), asi que aqui basta con exigir 1 px.
			FindComponents(ridgeLink, W, H, 1.0, ridgeCompsD);
			std::vector<uint8_t> visitScratchD((size_t)(W * H), 0);
			for (auto& rcD : ridgeCompsD)
			{
				std::vector<IPt> ptsLink = CollectComponentPixels(ridgeLink, W, H, rcD.seed, visitScratchD);
				std::vector<IPt> ptsD;
				ptsD.reserve(ptsLink.size());
				for (auto& p : ptsLink)
					if (ridgeD[(size_t)(p.y * W + p.x)]) ptsD.push_back(p);
				if ((double)ptsD.size() < P.depth_min_px * k * k)
					continue;
				std::vector<Vec2> spineD;
				double lenD = 0.0;
				if (!RidgeToSpine(ptsD, k, P.min_run * k, spineD, lenD))
					continue;

				// SS13.4: el lado del trazo es el lado CERCANO (disparidad
				// CRECIENTE = mas cerca de la camara) -- misma linea de codigo
				// que dirNorm de E9, con el buffer de disparidad en vez del de
				// luminancia (SS13.4 "es gratis").
				double sumGx = 0.0, sumGy = 0.0;
				for (auto& p : ptsD)
				{
					IntType px = p.x, py = p.y;
					double gxL = (px > 0) ? (double)Dsp[(size_t)(py * W + (px - 1))] : (double)Dsp[(size_t)(py * W + px)];
					double gxR = (px < W - 1) ? (double)Dsp[(size_t)(py * W + (px + 1))] : (double)Dsp[(size_t)(py * W + px)];
					double gyU = (py > 0) ? (double)Dsp[(size_t)((py - 1) * W + px)] : (double)Dsp[(size_t)(py * W + px)];
					double gyD = (py < H - 1) ? (double)Dsp[(size_t)((py + 1) * W + px)] : (double)Dsp[(size_t)(py * W + px)];
					sumGx += (gxR - gxL);
					sumGy += (gyD - gyU);
				}
				Vec2 dirNear = Normalize(Vec2{ sumGx, sumGy });
				if (Length(dirNear) < 1e-6) continue;

				double rstepSum = 0.0;
				for (auto& p : ptsD) rstepSum += (double)rmax[(size_t)(p.y * W + p.x)];
				double rstepMean = ptsD.empty() ? 0.0 : rstepSum / (double)ptsD.size();
				double c01D = Clamp01((rstepMean - P.int_depth_rel) / std::max(1e-9, P.int_depth_rel_hi - P.int_depth_rel));
				double lobeD = std::pow(std::max(0.0, Dot(dirNear, L2)), P.light_pow);
				Vec2 chordD = spineD.back() - spineD.front();
				double straightD = Clamp01((Length(chordD) / std::max(1e-9, lenD) - 0.80) / 0.18);
				double straightFD = P.straight_floor + (1.0 - P.straight_floor) * straightD;
				double cTermD = P.contrast_bias + (1.0 - P.contrast_bias) * std::pow(c01D, P.contrast_pow);
				double sNLD = cTermD * straightFD;
				double sbarD = sNLD * (P.light_bias + (1.0 - P.light_bias) * lobeD);
				// mismo pre-filtro que las candidatas de silueta (E6): largo y
				// sbar minimos antes de competir por el top-K.
				if (lenD < P.min_run * k || sbarD < P.t_lo) continue;

				IntType nSp = (IntType)spineD.size();
				std::vector<double> profD((size_t)nSp);
				for (IntType i = 0; i < nSp; i++)
				{
					IntType px = (IntType)std::llround(spineD[(size_t)i].x);
					IntType py = (IntType)std::llround(spineD[(size_t)i].y);
					px = px < 0 ? 0 : (px >= W ? W - 1 : px);
					py = py < 0 ? 0 : (py >= H ? H - 1 : py);
					profD[(size_t)i] = (double)rmax[(size_t)(py * W + px)];
				}

				RunCand rc;
				rc.kind = 1; rc.compIdx = -1;
				rc.spine = spineD; rc.dirDraw = dirNear; rc.prof = profD;
				rc.lenR = lenD; rc.sbarMean = sbarD; rc.sNLMean = sNLD;
				rc.c01Mean = c01D; rc.lobeMean = lobeD;
				rc.Q = ComputeQCand(lenD, c01D, lobeD, sNLD,
					P.q_len, P.q_con_bias, P.q_light_bias, P.q_light_pow, P.q_sbar_bias);
				candidates.push_back(rc);
				dcandCount++;
			}
		}

		// ---- E6: seleccion TOP-K global, con espaciado y rampa suave ----
		std::stable_sort(candidates.begin(), candidates.end(), [](const RunCand& a, const RunCand& b)
			{
				if (a.Q != b.Q) return a.Q > b.Q;
				if (a.compIdx != b.compIdx) return a.compIdx < b.compIdx;
				IntType sa = a.idx.empty() ? 0 : a.idx.front();
				IntType sb = b.idx.empty() ? 0 : b.idx.front();
				return sa < sb;
			});

		// GOLD tanda 2 (D-B SS14.1b): Kf REAL (sin redondear) para el
		// densGain continuo; K entero como siempre.
		double Kf = P.k_base + P.k_span * P.density;
		IntType K = (IntType)std::llround(Kf);
		K = std::max<IntType>(4, std::min<IntType>(24, K));

		std::vector<RunCand> accepted;
		// GOLD r3 (R2): puntos decimados de cada lamina aceptada, para el gate de
		// separacion EUCLIDEA `sep_px` (min_gap solo mide arco dentro de una
		// componente y no ve dos laminas pegadas por caminos distintos).
		std::vector<std::vector<Vec2>> accPts;
		double sepK = P.sep_px * k;
		// GOLD tanda 2 (D-A SS13.5.1): decima por compIdx/idx (kind 0) o por
		// el spine explicito ya a paso 2k (kind 1, "decima spine con el mismo
		// paso 4k").
		auto decimateRun = [&](const RunCand& rc)
		{
			std::vector<Vec2> out;
			size_t st = (size_t)std::max<IntType>(1, (IntType)std::llround(4.0 * k));
			if (rc.kind == 0)
			{
				const std::vector<Vec2>& Sc = compData[(size_t)rc.compIdx].S;
				for (size_t q = 0; q < rc.idx.size(); q += st) out.push_back(Sc[(size_t)rc.idx[q]]);
				if (!rc.idx.empty()) out.push_back(Sc[(size_t)rc.idx.back()]);
			}
			else
			{
				for (size_t q = 0; q < rc.spine.size(); q += st) out.push_back(rc.spine[q]);
				if (!rc.spine.empty()) out.push_back(rc.spine.back());
			}
			return out;
		};
		auto minDistPoly = [](const std::vector<Vec2>& A, const std::vector<Vec2>& B)
		{
			double best = 1e18;
			for (const Vec2& pa : A)
				for (const Vec2& pb : B)
				{
					double d = Length(pa - pb);
					if (d < best) best = d;
				}
			return best;
		};
		// GOLD r4: lineas del volcado de diagnostico (vacio si no hay dbg_runs).
		QStringList dbgLines;
		// GOLD r4 (R4): la exencion de par en V solo vale para TICKS cortos.
		// GOLD tanda 2 (D-A SS13.5.1): "la exencion de par en V no aplica a
		// kind=1" -- pairOk se fuerza a false para cualquier candidato de
		// profundidad.
		double pairLenMaxK = (P.pair_len_max > 0.0) ? P.pair_len_max * k : 1e18;
		IntType depthAccepted = 0;
		IntType depthMaxI = (IntType)std::llround(P.depth_max);
		// afinado t2r2 (D-G, CADENAS): cuerda de un candidato de cualquier
		// kind, para medir el GIRO entre dos laminas que se tocan.
		auto chordOf = [&](const RunCand& rc)
		{
			if (rc.kind == 0 && rc.compIdx >= 0 && !rc.idx.empty())
			{
				const std::vector<Vec2>& Sc = compData[(size_t)rc.compIdx].S;
				return Sc[(size_t)rc.idx.back()] - Sc[(size_t)rc.idx.front()];
			}
			if (!rc.spine.empty()) return rc.spine.back() - rc.spine.front();
			return Vec2{ 0.0, 0.0 };
		};
		// El giro se PLIEGA a [0,90]: dos cuerdas antiparalelas (180) son un
		// duplicado encima, no una esquina, y no deben eximirse.
		auto chainTurnOk = [&](const RunCand& a, const RunCand& b)
		{
			double ang = AngleBetweenDeg(chordOf(a), chordOf(b));
			if (ang > 90.0) ang = 180.0 - ang;
			return ang >= P.chain_ang;
		};
		double chainPxK = P.chain_px * k;
		double chainMinK = P.chain_min * k;
		IntType chainMaxI = (IntType)std::llround(P.chain_max);
		IntType chainUsed = 0;
		for (auto& cand : candidates)
		{
			bool chainWant = false;
			// afinado t2r3: la elegibilidad de CADENA pide largo Y contraste
			// medio (chain_c01=0 => solo largo, exactamente t2r2).
			bool chainLenOk = (cand.lenR >= chainMinK) && (cand.c01Mean >= P.chain_c01);
			// afinado t2r2 (D-G): diagnostico del veto (solo dbg_runs, no toca
			// el PNG ni `info`): quien bloquea, a que distancia y con que giro.
			IntType blkAi = -1; double blkD = 0.0, blkAng = 0.0;
			// GOLD tanda 2 (D-A SS13.5.1): "el tope depth_max se aplica al
			// aceptar" -- las de silueta siguen entrando sin limite propio.
			if (cand.kind == 1 && depthAccepted >= depthMaxI)
				continue;
			bool tooClose = false;
			bool pairOk = (cand.kind == 0) && (cand.lenR <= pairLenMaxK);
			// GOLD tanda 2 (D-A SS13.5.1): el gate de separacion usa int_sep
			// cuando ALGUNO de los dos es kind=1, sep_px cuando los dos son
			// kind=0 (la misma regla que ya tenia E9, movida al sitio correcto).
			for (auto& a : accepted)
			{
				// El hueco de ARCO (min_gap) solo tiene sentido DENTRO de la
				// misma componente de silueta; kind=1 no tiene compIdx valido
				// (-1) y se salta este bloque por completo.
				if (a.kind != 0 || cand.kind != 0 || a.compIdx != cand.compIdx) continue;
				const std::vector<Vec2>& Sc = compData[(size_t)cand.compIdx].S;
				double d = RunGapDist(Sc, a.idx.front(), a.idx.back(), cand.idx.front(), cand.idx.back());
				if (d >= P.min_gap * k) continue;
				// GOLD r1: EXENCION de par en V. Dos laminas que se tocan en un
				// vertice convexo son el rasgo del faro (3 pares a 77/82/89
				// grados), no un duplicado: min_gap no debe matarlas.
				if (pairOk && d <= P.pair_dist * k)
				{
					double angPair = AngleBetweenDeg(Sc[(size_t)a.idx.back()] - Sc[(size_t)a.idx.front()],
						Sc[(size_t)cand.idx.back()] - Sc[(size_t)cand.idx.front()]);
					if (angPair >= P.pair_angle) continue;
				}
				// afinado t2r2 (D-G): la misma exencion, pero por CADENA (se
				// tocan y giran), valida para cualquier largo y cualquier kind.
				if (chainPxK > 0.0 && chainLenOk && chainUsed < chainMaxI && d <= chainPxK && chainTurnOk(a, cand))
				{
					chainWant = true; continue;
				}
				{
					double angB = AngleBetweenDeg(chordOf(a), chordOf(cand));
					if (angB > 90.0) angB = 180.0 - angB;
					blkAi = -2; blkD = d; blkAng = angB;
				}
				tooClose = true; break;
			}
			// GOLD r3 (R2): separacion EUCLIDEA, tambien ENTRE componentes, con
			// la misma exencion de par en V que min_gap (solo si ambos kind=0).
			std::vector<Vec2> candPts;
			if (!tooClose)
			{
				candPts = decimateRun(cand);
				for (size_t ai = 0; ai < accepted.size(); ai++)
				{
					bool eitherDepth = (cand.kind == 1) || (accepted[ai].kind == 1);
					double sepThis = eitherDepth ? ((P.int_sep >= 0.0) ? P.int_sep * k : sepK) : sepK;
					if (sepThis <= 0.0) continue;
					double d = minDistPoly(candPts, accPts[ai]);
					if (d >= sepThis) continue;
					if (pairOk && accepted[ai].kind == 0 && d <= P.pair_dist * k)
					{
						const std::vector<Vec2>& Sc = compData[(size_t)cand.compIdx].S;
						const std::vector<Vec2>& Sa = compData[(size_t)accepted[ai].compIdx].S;
						double angPair = AngleBetweenDeg(
							Sa[(size_t)accepted[ai].idx.back()] - Sa[(size_t)accepted[ai].idx.front()],
							Sc[(size_t)cand.idx.back()] - Sc[(size_t)cand.idx.front()]);
						if (angPair >= P.pair_angle) continue;
					}
					// afinado t2r2 (D-G): CADENA -- se tocan (<= chain_px) y hay
					// giro real (>= chain_ang tras plegar a [0,90]). Es el caso
					// medido del HOMBRO (candidato de 386 px a ~40 px de la
					// lamina de profundidad del brazo, giro 79 grados).
					if (chainPxK > 0.0 && chainLenOk && chainUsed < chainMaxI && d <= chainPxK
						&& chainTurnOk(accepted[ai], cand))
					{
						chainWant = true; continue;
					}
					{
						double angB = AngleBetweenDeg(chordOf(accepted[ai]), chordOf(cand));
						if (angB > 90.0) angB = 180.0 - angB;
						blkAi = (IntType)ai; blkD = d; blkAng = angB;
					}
					tooClose = true; break;
				}
			}
			if (!P.dbgRuns.isEmpty())
			{
				Vec2 p0, p1;
				if (cand.kind == 0)
				{
					const std::vector<Vec2>& Sd = compData[(size_t)cand.compIdx].S;
					p0 = Sd[(size_t)cand.idx.front()]; p1 = Sd[(size_t)cand.idx.back()];
				}
				else { p0 = cand.spine.front(); p1 = cand.spine.back(); }
				dbgLines.append(QString("cand kind=%1 comp=%2 x0=%3 y0=%4 x1=%5 y1=%6 len=%7 sbar=%8 lobe=%9 c01=%10 Q=%11 %12")
					.arg(cand.kind).arg(cand.compIdx)
					.arg(p0.x, 0, 'f', 1).arg(p0.y, 0, 'f', 1)
					.arg(p1.x, 0, 'f', 1).arg(p1.y, 0, 'f', 1)
					.arg(cand.lenR, 0, 'f', 1).arg(cand.sbarMean, 0, 'f', 4)
					.arg(cand.lobeMean, 0, 'f', 4).arg(cand.c01Mean, 0, 'f', 4).arg(cand.Q, 0, 'f', 4)
					.arg(tooClose ? QString("RECHAZADO=separacion blk=%1 d=%2 ang=%3 lenOk=%4 chUsed=%5")
						.arg(blkAi).arg(blkD, 0, 'f', 1).arg(blkAng, 0, 'f', 1)
						.arg(chainLenOk ? 1 : 0).arg(chainUsed)
						: (chainWant ? QString("ok=cadena") : QString("ok"))));
			}
			if (tooClose) continue;
			accepted.push_back(cand);
			accPts.push_back(candPts.empty() ? decimateRun(cand) : candPts);
			if (chainWant) chainUsed++;
			if (cand.kind == 1) depthAccepted++;
			if ((IntType)accepted.size() == K + 2) break;
		}
		// GOLD tanda 2 (D-B SS14.1b): Qcut solo "muerde" (recorta de verdad)
		// cuando SOBRAN candidatos sobre K; si no, aK=1 para todos (rampa
		// intacta) y el "premio" es densGain sobre el ANCHO (ver mas abajo).
		//
		// MEDIDO (gate G22): con TODOS los mandos nuevos neutros, esta escena
		// da nAcc=9 < K=13, y el Qcut de la tanda 1 en ese caso NO era 0: era
		// `accepted.back().Q` (0.2461 medido, dbg_runs), que le daba a la
		// lamina mas debil de las 9 un aK de smoothstep(~0.5), NO 1.0. La
		// pseudocodigo literal del spec (Qcut=0 si nAcc<=K) es el nucleo real
		// de D-B(b) (aK=1 siempre que no sobren candidatos) y se APLICA tal
		// cual cuando dens_gain>0 (el caso real de uso). Pero con
		// `dens_gain<=0` el propio spec documenta la clave como "= tanda 1"
		// (ver WdParams: "dens_gain=0 => densGain siempre 1, comportamiento
		// tanda 1"): interpretacion minima anotada, esa equivalencia se
		// extiende a Qcut tambien (no solo al multiplicador), que es lo unico
		// que hace que G22 sea alcanzable por un opt ya listado en el propio
		// gate en vez de necesitar uno nuevo no declarado. Con dens_gain=0 el
		// Qcut queda IDENTICO al de la tanda 1, byte a byte.
		IntType nAcc = (IntType)accepted.size();
		double Qcut = 0.0;
		double densGain = 1.0;
		if (P.dens_gain <= 0.0)
		{
			if (nAcc > 0)
				Qcut = (nAcc >= K) ? accepted[(size_t)(K - 1)].Q : accepted.back().Q;
		}
		else if (nAcc > K)
		{
			Qcut = accepted[(size_t)(K - 1)].Q;
		}
		else
		{
			double headroom = Clamp01(1.0 - (double)nAcc / std::max(1e-6, Kf));
			densGain = 1.0 + P.dens_gain * headroom;
		}

		// GOLD r1 (G5): rango OBSERVADO de sbarMean entre los aceptados, para
		// que la dispersion de ancho entre laminas no dependa de contrast_pow.
		double sbAccLo = 0.0, sbAccHi = 0.0;
		double lenAccLo = 0.0, lenAccHi = 0.0; // GOLD r2: idem para el reparto por largo (w_len)
		{
			bool firstAcc = true;
			for (auto& cand : accepted)
			{
				if (firstAcc)
				{
					sbAccLo = sbAccHi = cand.sbarMean;
					lenAccLo = lenAccHi = cand.lenR;
					firstAcc = false; continue;
				}
				if (cand.sbarMean < sbAccLo) sbAccLo = cand.sbarMean;
				if (cand.sbarMean > sbAccHi) sbAccHi = cand.sbarMean;
				if (cand.lenR < lenAccLo) lenAccLo = cand.lenR;
				if (cand.lenR > lenAccHi) lenAccHi = cand.lenR;
			}
		}

		// GOLD tanda 2 (D-C SS14.2a): rango OBSERVADO de `prom` (contraste x
		// largo^0.15) entre TODOS los aceptados (silueta y profundidad) --
		// mismo truco que sbAccLo/Hi, para w_mode=1. Se calcula siempre (barato)
		// aunque solo se USE con w_mode!=0.
		double promAccLo = 0.0, promAccHi = 0.0;
		{
			bool firstP = true;
			for (auto& cand : accepted)
			{
				double prom = std::pow(Clamp01(cand.c01Mean), P.w_rank_con)
					* std::pow(std::max(1e-6, cand.lenR) / 100.0, P.w_rank_len);
				cand.prom = prom;
				if (firstP) { promAccLo = promAccHi = prom; firstP = false; continue; }
				if (prom < promAccLo) promAccLo = prom;
				if (prom > promAccHi) promAccHi = prom;
			}
		}

		std::vector<float> aW((size_t)(W * H), 0.f);
		// GOLD tanda 2 (D-A SS13.5.1): aI ahora se declara AQUI (antes solo
		// vivia dentro de E9) porque las laminas de profundidad (kind=1,
		// E12) tambien pintan en aI y se rasterizan en ESTE bucle, antes de
		// que corra E9 mas abajo.
		std::vector<float> aI((size_t)(W * H), 0.f);
		// GOLD r1: acumulador de cobertura por tira (entra y sale a cero).
		std::vector<float> accStrip((size_t)(W * H), 0.f);
		// GOLD tanda 2 (D-C SS14.2b): segundo acumulador (cobertura*alfa)
		// para el terminal a aguja (RasterizeStripTail/RasterizeTriAddW).
		std::vector<float> accStripW((size_t)(W * H), 0.f);
		std::vector<AcceptedRun> acceptedRuns;
		for (auto& cand : accepted)
		{
			double aK = Smoothstep(Qcut * (1.0 - P.hyst), Qcut * (1.0 + P.hyst), cand.Q);
			double aLen = Smoothstep(P.min_run * k, 2.0 * P.min_run * k, cand.lenR);
			double aScore = Smoothstep(P.t_lo, P.t_hi, cand.sbarMean);
			double alphaRaw = Clamp01(aK * aLen * aScore);
			if (alphaRaw < 0.05 * std::max(1e-6, P.alpha_sat)) continue;
			// GOLD r1: saturacion de la rampa (ver alpha_sat en WdParams).
			double alphaR = Clamp01(alphaRaw / std::max(1e-6, P.alpha_sat));
			if (alphaR < 0.05) continue;

			std::vector<Vec2> spineR, normalR, mDir; std::vector<double> sbarR;
			IntType Nrun; double up;
			// afinado t2r3 (D-H): indices REALMENTE dibujados por esta lamina.
			// Con trim_rel=0 es exactamente cand.idx (t2r2 byte a byte).
			std::vector<IntType> idxUse = cand.idx;
			double lenUse = cand.lenR;
			if (cand.kind == 0)
			{
				CompData& cd = compData[(size_t)cand.compIdx];
				// ---- afinado t2r3 (D-H): RECORTE AL LOBULO DOMINANTE ----
				if (P.trim_rel > 0.0 && (IntType)idxUse.size() >= 6)
				{
					IntType rn = (IntType)idxUse.size();
					IntType srcI = (IntType)std::llround(P.trim_src);
					std::vector<double> u((size_t)rn, 0.0);
					for (IntType i = 0; i < rn; i++)
					{
						IntType id = idxUse[(size_t)i];
						double v;
						if (srcI == 1) v = cd.lobe[(size_t)id];
						else if (srcI == 2) v = cd.c01[(size_t)id];
						else if (srcI == 3) v = cd.lobe[(size_t)id] * cd.c01[(size_t)id];
						else v = (double)cd.sbar[(size_t)id];
						u[(size_t)i] = v;
					}
					// mismo suavizado relativo que E7 (ventana rn/8) para que el
					// pico no lo decida un pixel del aliasing del escalon.
					IntType wSm = std::max<IntType>(1, rn / 8);
					std::vector<double> us((size_t)rn, 0.0);
					for (IntType i = 0; i < rn; i++)
					{
						double sum = 0.0; IntType cnt = 0;
						for (IntType q = -wSm; q <= wSm; q++)
						{
							IntType ii = i + q; if (ii < 0 || ii >= rn) continue;
							sum += u[(size_t)ii]; cnt++;
						}
						us[(size_t)i] = cnt > 0 ? sum / (double)cnt : u[(size_t)i];
					}
					IntType pk = 0; double pv = us[0]; double mu = 0.0;
					for (IntType i = 0; i < rn; i++)
					{
						mu += us[(size_t)i];
						if (us[(size_t)i] > pv) { pv = us[(size_t)i]; pk = i; }
					}
					mu /= (double)rn;
					double flat = (pv > 1e-9) ? (mu / pv) : 1.0;
					bool trimOn = (pv >= P.trim_abs) && (flat <= P.trim_flat);
					double thr = P.trim_rel * pv;
					IntType a = pk, b = pk; double lenNew = cand.lenR; bool dropIt = false;
					if (trimOn)
					{
						while (a > 0 && us[(size_t)(a - 1)] >= thr) a--;
						while (b < rn - 1 && us[(size_t)(b + 1)] >= thr) b++;
						lenNew = ArcLenFwd(cd.S, idxUse[(size_t)a], idxUse[(size_t)b]);
						bool cortoOk = (lenNew >= P.trim_min * k);
						bool quitaOk = (lenNew >= (1.0 - ClampRange(P.trim_keep, 0.0, 1.0)) * cand.lenR);
						if (!cortoOk)
						{
							// el lobulo dominante no llega a lamina. Primero se
							// intenta el RESCATE POR ESQUINA (trim_tail): el
							// tramo terminal que entra en la esquina gruesa del
							// run. Si no hay esquina, se tira (trim_drop=1) o
							// se deja como estaba (trim_drop=0, t2r2).
							bool rescatada = false;
							if (P.trim_tail > 0.0)
							{
								IntType wideN = std::max<IntType>(2, (IntType)std::llround(P.turn_win * k));
								IntType bestI = -1; double bestT = P.trim_corner;
								for (IntType p2 = 1; p2 + 1 < rn; p2++)
								{
									IntType a0 = p2 - wideN, a1 = p2 + wideN;
									if (a0 < 0 || a1 > rn - 1) continue;
									double coarse = AngleBetweenDeg(
										cd.S[(size_t)idxUse[(size_t)p2]] - cd.S[(size_t)idxUse[(size_t)a0]],
										cd.S[(size_t)idxUse[(size_t)a1]] - cd.S[(size_t)idxUse[(size_t)p2]]);
									if (coarse > bestT) { bestT = coarse; bestI = p2; }
								}
								if (bestI > 0)
								{
									double want = P.trim_tail * k;
									IntType s = bestI; double acc = 0.0;
									while (s > 0 && acc < want)
									{
										acc += Length(cd.S[(size_t)idxUse[(size_t)s]] - cd.S[(size_t)idxUse[(size_t)(s - 1)]]);
										s--;
									}
									if (acc >= 0.5 * want && (bestI - s) >= 4)
									{
										idxUse = std::vector<IntType>(idxUse.begin() + s, idxUse.begin() + bestI + 1);
										lenUse = acc; rescatada = true;
										a = s; b = bestI; lenNew = acc;
									}
								}
							}
							if (!rescatada && P.trim_drop > 0.5) dropIt = true;
						}
						else if ((a > 0 || b < rn - 1) && quitaOk)
						{
							idxUse = std::vector<IntType>(idxUse.begin() + a, idxUse.begin() + b + 1);
							lenUse = lenNew;
						}
					}
					if (!P.dbgRuns.isEmpty())
						dbgLines.append(QString("trim comp=%1 len=%2 pico=%3 media=%4 flat=%5 on=%6 a=%7 b=%8 lenNew=%9 drop=%10")
							.arg(cand.compIdx).arg(cand.lenR, 0, 'f', 1).arg(pv, 0, 'f', 4)
							.arg(mu, 0, 'f', 4).arg(flat, 0, 'f', 4).arg(trimOn ? 1 : 0)
							.arg(a).arg(b).arg(lenNew, 0, 'f', 1).arg(dropIt ? 1 : 0));
					if (dropIt) continue;
				}
				Nrun = std::max<IntType>(8, (IntType)std::llround(lenUse / (2.0 * k)));
				ResampleRunPNS(cd.S, cd.normal, cd.sbar, idxUse, Nrun, spineR, normalR, sbarR);
				// GOLD r1: la lamina es una CUERDA RECTA, no un calco del contorno.
				// afinado t2r2: se guarda el spine SIN enderezar para poder
				// apoyar despues la cuerda en el punto mas saliente (hug).
				std::vector<Vec2> spinePre = spineR;
				StraightenSpine(spineR, normalR, P.straighten);
				HugSpineOutward(spineR, spinePre, normalR, P.hug);

				// ---- E7: localizar el pico ('up') sobre el perfil de sbar suavizado ----
				IntType smWin = std::max<IntType>(1, Nrun / 8);
				std::vector<double> sbarSmooth((size_t)Nrun, 0.0);
				for (IntType i = 0; i < Nrun; i++)
				{
					double sum = 0.0; IntType cnt = 0;
					for (IntType u = -smWin; u <= smWin; u++)
					{
						IntType ii = i + u; if (ii < 0 || ii >= Nrun) continue;
						sum += sbarR[(size_t)ii]; cnt++;
					}
					sbarSmooth[(size_t)i] = cnt > 0 ? sum / (double)cnt : sbarR[(size_t)i];
				}
				IntType upIdx = 0; double upVal = sbarSmooth[0];
				for (IntType i = 1; i < Nrun; i++) if (sbarSmooth[(size_t)i] > upVal) { upVal = sbarSmooth[(size_t)i]; upIdx = i; }
				up = SnapPeakPos(Nrun > 1 ? (double)upIdx / (double)(Nrun - 1) : 0.0);

				// GOLD r2: reubicar el pico segun up_mode (ver WdParams). El faro
				// engorda hacia la esquina convexa / hacia la luz y afila el otro
				// extremo a aguja; el pico de sbar sale invertido en la capa.
				IntType upModeI = (IntType)std::llround(P.up_mode);
				if (upModeI != 0 && Nrun > 1)
				{
					bool decided = false;
					if (upModeI == 1 || upModeI == 3 || upModeI == 5)
					{
						double dFront = 1e18, dBack = 1e18;
						for (IntType cj : cd.cornerIdx)
						{
							double df = std::min(ArcLenFwd(cd.S, idxUse.front(), cj), ArcLenFwd(cd.S, cj, idxUse.front()));
							double db = std::min(ArcLenFwd(cd.S, idxUse.back(), cj), ArcLenFwd(cd.S, cj, idxUse.back()));
							if (df < dFront) dFront = df;
							if (db < dBack) dBack = db;
						}
						double lim = P.up_corner_dist * k;
						if (dFront <= lim || dBack <= lim)
						{
							up = (dFront <= dBack) ? 0.0 : 1.0;
							decided = true;
						}
					}
					if (!decided && (upModeI == 2 || upModeI == 3))
					{
						// el extremo mas "arriba" en la direccion de la luz
						double pf = Dot(spineR[0], L2);
						double pb = Dot(spineR[(size_t)Nrun - 1], L2);
						up = (pf >= pb) ? 0.0 : 1.0;
						decided = true;
					}
					if (!decided && (upModeI == 4 || upModeI == 5))
					{
						// GOLD r2: solo la componente VERTICAL de la luz. Medido en
						// las cuatro laminas del faro que se pueden emparejar una a
						// una (ala del gorro, capa, banda baja, bota), el pico de
						// ancho cae SIEMPRE en el extremo mas alto en pantalla con
						// el sol arriba; la proyeccion completa sobre L2 (modo 2) se
						// la lleva el extremo mas a la izquierda y sale al reves en
						// el gorro.
						Vec2 Lv{ 0.0, (std::sin(angRad) >= 0.0) ? -1.0 : 1.0 };
						double pf = Dot(spineR[0], Lv);
						double pb = Dot(spineR[(size_t)Nrun - 1], Lv);
						up = (pf >= pb) ? 0.0 : 1.0;
						decided = true;
					}
				}
				mDir.assign((size_t)Nrun, Vec2{ 0.0, 0.0 });
				for (IntType i = 0; i < Nrun; i++) mDir[(size_t)i] = normalR[(size_t)i] * -1.0; // interior
			}
			else
			{
				// GOLD tanda 2 (D-A SS13.3/13.5.1): kind=1 (profundidad, E12).
				// El spine ya viene resampleado a 2k (RidgeToSpine); la
				// direccion de trazo es CONSTANTE (dirDraw = lado cercano,
				// SS13.4) y el pico 'up' sale del perfil de rstep, mismo
				// patron que E9/InternalCand (argmax del perfil).
				spineR = cand.spine;
				Nrun = (IntType)spineR.size();
				mDir.assign((size_t)Nrun, cand.dirDraw);
				// afinado t2r2: enderezar tambien la lamina de PROFUNDIDAD. La
				// cresta de disparidad sigue el escalon del z-buffer pixel a
				// pixel y salia "temblorosa" (veredicto visual t2r1). La normal
				// de una lamina kind=1 es CONSTANTE (-dirDraw), asi que
				// StraightenSpine solo proyecta el spine sobre su recta de
				// ajuste y deja la normal como estaba. depth_straight<0 =>
				// usar el mismo `straighten` que la silueta; 0 => como t2r1.
				{
					double dstr = (P.depth_straight < 0.0) ? P.straighten : P.depth_straight;
					if (dstr > 0.0 && Nrun >= 3)
					{
						std::vector<Vec2> nTmp((size_t)Nrun, Vec2{ -cand.dirDraw.x, -cand.dirDraw.y });
						StraightenSpine(spineR, nTmp, dstr);
						mDir.assign((size_t)Nrun, Vec2{ -nTmp[0].x, -nTmp[0].y });
					}
				}
				IntType upIdx = 0; double upVal = cand.prof.empty() ? 0.0 : cand.prof[0];
				for (IntType i = 1; i < (IntType)cand.prof.size(); i++)
					if (cand.prof[(size_t)i] > upVal) { upVal = cand.prof[(size_t)i]; upIdx = i; }
				up = SnapPeakPos(Nrun > 1 ? (double)upIdx / (double)(Nrun - 1) : 0.0);
			}

			// ---- GOLD tanda 2 (D-C SS14.2a): ancho por RANGO de prominencia
			// (w_mode!=0) o formula de la tanda 1 (w_mode=0; con dens_gain=0
			// tambien, es byte-identica a la tanda 1 -- gate G22). ----
			double wRun;
			if (P.w_mode != 0.0)
			{
				double promN = (promAccHi - promAccLo) > 1e-9
					? Clamp01((cand.prom - promAccLo) / (promAccHi - promAccLo)) : 0.5;
				double gRank = P.w_rank_lo + (P.w_rank_hi - P.w_rank_lo) * std::pow(promN, std::max(1e-6, P.w_rank_exp));
				wRun = P.width * k * alphaR * gRank * densGain;
				alphaR *= (P.a_rank_lo + (1.0 - P.a_rank_lo) * promN);
			}
			else
			{
				double wScore;
				if (P.w_rel != 0.0 && (sbAccHi - sbAccLo) > 1e-9)
					wScore = Clamp01((cand.sbarMean - sbAccLo) / (sbAccHi - sbAccLo));
				else
					wScore = Clamp01((cand.sbarMean - P.t_lo) / (P.w_ref - P.t_lo));
				// GOLD r2: mezcla con el reparto por LARGO (w_len).
				double wnorm = wScore;
				if (P.w_len > 0.0)
				{
					double wLenN = (lenAccHi - lenAccLo) > 1e-9
						? Clamp01((cand.lenR - lenAccLo) / (lenAccHi - lenAccLo)) : wScore;
					double mix = ClampRange(P.w_len, 0.0, 1.0);
					wnorm = (1.0 - mix) * wScore + mix * wLenN;
				}
				wRun = P.width * k * alphaR * (P.w_lo + (P.w_hi - P.w_lo) * wnorm);
				// GOLD tanda 2 (D-B SS14.1b): densGain=1 cuando dens_gain=0,
				// multiplicar por 1.0 es bit-exacto (G22).
				wRun *= densGain;
			}

			std::vector<double> wProf((size_t)Nrun);
			for (IntType i = 0; i < Nrun; i++)
			{
				double u = Nrun > 1 ? (double)i / (double)(Nrun - 1) : 0.0;
				wProf[(size_t)i] = wRun * TaperProfile(u, up, P.taper_exp);
			}

			// ---- GOLD tanda 2 (D-C SS14.2b): terminal a AGUJA. tail_frac<=0
			// usa el camino VIEJO (RasterizeStrip, alfa unico) intacto --
			// byte-identico a la tanda 1 (gate G22, ver comentario en
			// ApplyNeedleTail sobre por que son DOS funciones separadas). ----
			std::vector<float>& targetBuf = (cand.kind == 0) ? aW : aI;
			if (P.tail_frac > 0.0)
			{
				std::vector<double> aArr;
				ApplyNeedleTail(wProf, aArr, up, P.tail_frac, P.tail_exp, P.tail_alpha_lo, alphaR);
				RasterizeStripTail(targetBuf, W, H, spineR, mDir, wProf, aArr, accStrip, accStripW);
			}
			else
			{
				RasterizeStrip(targetBuf, W, H, spineR, mDir, wProf, alphaR, accStrip);
			}

			// afinado t2r3 (D-H): los remates / puentes / ticks cuelgan de lo
			// que la lamina DIBUJA, no del run completo del que salio.
			AcceptedRun ar; ar.kind = cand.kind; ar.compIdx = cand.compIdx; ar.idx = idxUse;
			ar.spine = spineR; ar.dirDraw = mDir.empty() ? Vec2{ 0.0, 0.0 } : mDir[0];
			ar.lenR = cand.lenR; ar.sbarMean = cand.sbarMean; ar.Q = cand.Q; ar.alphaR = alphaR;
			ar.wRun = wRun; ar.up = up; ar.prom = cand.prom;
			acceptedRuns.push_back(ar);
			if (cand.kind == 1) dlamCount++;

			// GOLD r4: volcado de diagnostico (solo si el llamador pide
			// dbg_runs). Sin el, la unica forma de saber POR QUE entra una
			// lamina que el faro no pinta era adivinar: aqui salen las
			// magnitudes medidas de cada lamina aceptada.
			if (!P.dbgRuns.isEmpty())
			{
				// GOLD tanda 2 (D-C SS14.5): cand.c01Mean YA es la misma media
				// que antes se recalculaba aqui desde cd.c01 (mismos indices,
				// misma suma) -- se reusa tal cual, tambien valido para kind=1.
				double promNRep = (promAccHi - promAccLo) > 1e-9
					? Clamp01((cand.prom - promAccLo) / (promAccHi - promAccLo)) : 0.5;
				double gRankRep = P.w_rank_lo + (P.w_rank_hi - P.w_rank_lo) * std::pow(promNRep, std::max(1e-6, P.w_rank_exp));
				dbgLines.append(QString(
					"lamina kind=%1 comp=%2 x0=%3 y0=%4 x1=%5 y1=%6 len=%7 sbar=%8 lobe=%9 c01=%10 Q=%11 alpha=%12 w=%13 prom=%14 promN=%15 gRank=%16 rank=%17")
					.arg(cand.kind).arg(cand.compIdx)
					.arg(spineR.front().x, 0, 'f', 1).arg(spineR.front().y, 0, 'f', 1)
					.arg(spineR.back().x, 0, 'f', 1).arg(spineR.back().y, 0, 'f', 1)
					.arg(cand.lenR, 0, 'f', 1).arg(cand.sbarMean, 0, 'f', 4)
					.arg(cand.lobeMean, 0, 'f', 4).arg(cand.c01Mean, 0, 'f', 4)
					.arg(cand.Q, 0, 'f', 4).arg(alphaR, 0, 'f', 3).arg(wRun, 0, 'f', 2)
					.arg(cand.prom, 0, 'f', 4).arg(promNRep, 0, 'f', 4).arg(gRankRep, 0, 'f', 4)
					.arg((IntType)acceptedRuns.size()));
				// afinado t2r3 (D-H): PERFIL punto a punto del run ORIGINAL
				// (antes del recorte), decimado a 20 muestras. Es el
				// instrumento que dice DONDE esta el lobulo que el faro pinta.
				if (cand.kind == 0 && cand.idx.size() >= 2)
				{
					CompData& cdp = compData[(size_t)cand.compIdx];
					IntType rn = (IntType)cand.idx.size();
					QStringList sm;
					for (IntType q = 0; q < 20; q++)
					{
						IntType i = (IntType)std::llround((double)(rn - 1) * (double)q / 19.0);
						IntType id = cand.idx[(size_t)i];
						sm.append(QString("%1,%2,%3,%4,%5")
							.arg(cdp.S[(size_t)id].x, 0, 'f', 0).arg(cdp.S[(size_t)id].y, 0, 'f', 0)
							.arg((double)cdp.sbar[(size_t)id], 0, 'f', 3)
							.arg(cdp.lobe[(size_t)id], 0, 'f', 3)
							.arg(cdp.c01[(size_t)id], 0, 'f', 3));
					}
					dbgLines.append(QString("prof rank=%1 nIdx=%2 nUse=%3 lenUse=%4 [x,y,sbar,lobe,c01] %5")
						.arg((IntType)acceptedRuns.size()).arg(rn).arg((IntType)idxUse.size())
						.arg(lenUse, 0, 'f', 1).arg(sm.join(" ")));
				}
			}
		}

		// ---- GOLD tanda 2 (D-D SS14.3): E13, REMATES EN L. Va ANTES de E8
		// (los ticks) y DESPUES del rasterizado de las laminas madre (arriba
		// ya termino): el remate tiene PRIORIDAD sobre el tick en una
		// esquina, y E8 ya se salta las esquinas marcadas en cd.usedCorner.
		// Solo laminas kind=0 (una lamina interna/profundidad no forma
		// remate con la silueta: son bordes distintos). remate_max=0 =>
		// comportamiento tanda 1 (el bucle no entra, gate G22). ----
		IntType remateCount = 0;
		IntType remateMaxI = (IntType)std::llround(P.remate_max);
		// afinado t2r1 (MEDIDO): recorrer las madres por Q le da los dos huecos
		// de remate_max a la CAPA y al GORRO, y la BOTA -- que es donde el faro
		// pinta sus DOS remates en L (id18/id22 y id19/id20) -- se queda sin
		// ninguno. Por PROMINENCIA (la magnitud de D-C: contraste x largo) la
		// bota va primero (prom 1.2786 contra 1.1392 del gorro y 1.0425 de la
		// capa) y se lleva sus dos esquinas, que es justo lo que hace el faro.
		// remate_order=0 => orden por Q (comportamiento anterior).
		std::vector<IntType> remOrder;
		for (IntType i = 0; i < (IntType)acceptedRuns.size(); i++) remOrder.push_back(i);
		if (P.remate_order != 0.0)
			std::stable_sort(remOrder.begin(), remOrder.end(),
				[&acceptedRuns](IntType a, IntType b)
				{ return acceptedRuns[(size_t)a].prom > acceptedRuns[(size_t)b].prom; });
		for (IntType arI : remOrder)
		{
			AcceptedRun& ar = acceptedRuns[(size_t)arI];
			if (remateCount >= remateMaxI) break;
			if (ar.kind != 0 || ar.idx.empty()) continue;
			CompData& cd = compData[(size_t)ar.compIdx];
			IntType mComp = (IntType)cd.S.size();
			for (IntType side = 0; side < 2 && remateCount < remateMaxI; side++)
			{
				IntType endPt = (side == 0) ? ar.idx.front() : ar.idx.back();
				IntType bestC = -1; double bestD = 1e18;
				for (IntType civ = 0; civ < (IntType)cd.cornerIdx.size(); civ++)
				{
					if (cd.usedCorner[(size_t)civ]) continue;
					double dd = std::min(ArcLenFwd(cd.S, endPt, cd.cornerIdx[(size_t)civ]),
						ArcLenFwd(cd.S, cd.cornerIdx[(size_t)civ], endPt));
					if (dd < bestD) { bestD = dd; bestC = civ; }
				}
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("remate? comp=%1 side=%2 end=(%3,%4) esquinas=%5 bestD=%6 lim=%7")
						.arg(ar.compIdx).arg(side)
						.arg(cd.S[(size_t)endPt].x, 0, 'f', 1).arg(cd.S[(size_t)endPt].y, 0, 'f', 1)
						.arg((int)cd.cornerIdx.size()).arg(bestD, 0, 'f', 1).arg(P.remate_dist * k, 0, 'f', 1));
				// afinado t2r1 (BUG MEDIDO nº2): el vertice bota/pierna --
				// justo el que D-D existe para rematar -- NO esta en
				// cd.cornerIdx: cornerIdx solo guarda esquinas CONVEXAS
				// (isConvex, "outward"), y ese vertice es un entrante. Medido
				// en dbg: para la horizontal de la bota, side=1 end=(1846.3,
				// 1524.4) da bestD=302.7 px con lim=16 => nunca hay esquina.
				// El indice de la esquina, sin embargo, YA lo tenemos: E5b
				// PARTIO el run ahi, asi que el extremo de la lamina ES el
				// vertice. Se usa la esquina registrada cuando la hay (para
				// poder marcarla y que E8 no le cuelgue ademas un tick) y el
				// propio extremo cuando no la hay.
				bool haveCorner = (bestC >= 0 && bestD <= P.remate_dist * k);
				IntType cornerJ = haveCorner ? cd.cornerIdx[(size_t)bestC] : endPt;

				// Sentido: AVANZAR desde la esquina hacia el lado LIBRE (el
				// que no ocupa `ar`): si la esquina se alcanza desde endPt
				// caminando ADELANTE, el remate continua ADELANTE (fresca);
				// si se alcanza caminando ATRAS, el remate continua ATRAS.
				// afinado t2r1 (BUG MEDIDO nº4): con la esquina PEGADA al
				// extremo (bestD=0, el caso normal: E5b parte justo ahi) los
				// dos arcos valen 0 y `dFwd <= dBwd` daba SIEMPRE "adelante",
				// asi que en el extremo `front` el remate se dibujaba ENCIMA
				// de su propia madre (medido: dos remates de 150 px calcados
				// sobre la lamina (1869,1841)-(1867,2009)). El sentido no
				// depende del arco: la lamina ocupa idx.front()..idx.back()
				// caminando ADELANTE, luego salir por `front` es ATRAS y salir
				// por `back` es ADELANTE. Siempre.
				bool goForward = (side == 1);

				double lenAdj = 0.0; IntType cur = cornerJ; IntType guard = 0;
				double capLen = P.remate_len_max * k;
				double pairKrem = P.pair_dist * k;
				while (guard < mComp)
				{
					IntType nxt = goForward ? (cur + 1) % mComp : ((cur - 1) % mComp + mComp) % mComp;
					// afinado t2r1 (BUG MEDIDO nº3): el giro de LA PROPIA
					// esquina rompia el bucle en el primer paso y lenAdj salia
					// 0.0 en TODAS las esquinas (medido en dbg: 6 de 6). El
					// giro se mide con una ventana (cwWin) centrada, asi que
					// los primeros ~20 px despues del vertice siguen "girando".
					// Se ignora el corte mientras no nos hayamos alejado del
					// vertice mas que pair_dist -- el mismo radio con el que el
					// resto del algoritmo considera "compartir vertice".
					if (lenAdj > pairKrem && cd.turnDeg[(size_t)nxt] >= P.turn_split) break;
					lenAdj += Length(cd.S[(size_t)nxt] - cd.S[(size_t)cur]);
					cur = nxt;
					guard++;
					if (lenAdj >= capLen) break;
				}
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("remate?? comp=%1 side=%2 lenAdj=%3 min=%4 fwd=%5")
						.arg(ar.compIdx).arg(side).arg(lenAdj, 0, 'f', 1)
						.arg(P.remate_min_run * k, 0, 'f', 1).arg(goForward ? 1 : 0));
				if (lenAdj < P.remate_min_run * k) continue;
				double lenL = ClampRange(P.remate_frac * lenAdj, P.remate_min_run * k, P.remate_len_max * k);

				std::vector<IntType> spineIdx;
				spineIdx.push_back(cornerJ);
				double accArc = 0.0; cur = cornerJ; guard = 0;
				while (accArc < lenL && guard < mComp)
				{
					IntType nxt = goForward ? (cur + 1) % mComp : ((cur - 1) % mComp + mComp) % mComp;
					accArc += Length(cd.S[(size_t)nxt] - cd.S[(size_t)cur]);
					spineIdx.push_back(nxt);
					cur = nxt;
					guard++;
				}
				if (spineIdx.size() < 2) continue;

				// SS14.3: "respeta sep_px contra las aceptadas (si choca, no
				// se emite)".
				std::vector<Vec2> remPts;
				{
					size_t st = (size_t)std::max<IntType>(1, (IntType)std::llround(4.0 * k));
					for (size_t q = 0; q < spineIdx.size(); q += st) remPts.push_back(cd.S[(size_t)spineIdx[q]]);
					remPts.push_back(cd.S[(size_t)spineIdx.back()]);
				}
				// afinado t2r1: el remate es un TRAZO, no un calco del
				// contorno. Sin gate de contraste el primero que sale es el
				// bajo de la CAPA (la madre de mayor Q) y ahi el faro no pinta
				// nada: medido, la masa FALSA de la zona "rizo del bajo de la
				// capa" se dispara de 465 a 2628 px y la P de MASA cae de
				// 0.920 a 0.779. remate_c01=0 => sin gate.
				double c01Rem = 0.0, sbarRem0 = 0.0;
				for (IntType si : spineIdx) { c01Rem += cd.c01[(size_t)si]; sbarRem0 += cd.sbar[(size_t)si]; }
				c01Rem /= std::max<size_t>(1, spineIdx.size());
				sbarRem0 /= std::max<size_t>(1, spineIdx.size());
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("remate??? comp=%1 side=%2 lenL=%3 c01=%4 sbar=%5")
						.arg(ar.compIdx).arg(side).arg(lenL, 0, 'f', 1)
						.arg(c01Rem, 0, 'f', 4).arg(sbarRem0, 0, 'f', 4));
				if (c01Rem < P.remate_c01) continue;
				// afinado t2r1 (BUG MEDIDO, rem=0 en TODAS las corridas): el
				// remate ARRANCA EN LA ESQUINA que comparte con su lamina
				// madre (SS14.3 lo exige: "comparte vertice"), asi que su
				// distancia a la madre es ~0 por CONSTRUCCION y el test de
				// sep_px (250 px por defecto) lo mataba SIEMPRE. Con
				// remate_max=2 y sep_px>0 nunca se emitio ni uno -- y de eso
				// dependia recuperar la pierna (id20) tras subir pair_len_max
				// a 90, que es lo que hundio el recall de MASA de 0.748 a
				// 0.626. La regla correcta es la de los pares en V: no cuenta
				// como choque lo que COMPARTE el vertice (a <= pair_dist de la
				// esquina); todo lo demas sigue respetando sep_px.
				// `remate_sep` (nueva, afinado t2r1): sep_px=250 es un radio
				// pensado para NO repetir el mismo rasgo dos veces; aplicado al
				// remate mata tambien lo que solo esta ENCADENADO (medido: el
				// remate de la pierna, 296 px, choca con la lamina de la
				// espinilla que empieza 20 px mas abajo). remate_sep<0 => usa
				// sep_px (comportamiento anterior); 0 => sin gate euclideo.
				double sepRem = (P.remate_sep >= 0.0) ? P.remate_sep * k : sepK;
				if (sepRem > 0.0)
				{
					Vec2 cornerPt = cd.S[(size_t)cornerJ];
					double pairK = P.pair_dist * k;
					bool clash = false;
					for (auto& ap : accPts)
					{
						if (ap.empty()) continue;
						double dCorner = 1e18;
						for (const Vec2& q : ap)
						{
							double dq = Length(q - cornerPt);
							if (dq < dCorner) dCorner = dq;
						}
						if (dCorner <= pairK) continue; // comparte vertice: es el par en L
						if (minDistPoly(remPts, ap) < sepRem) { clash = true; break; }
					}
					if (clash) continue;
				}

				IntType Nrem = std::max<IntType>(8, (IntType)std::llround(lenL / (2.0 * k)));
				std::vector<Vec2> spineRem, normalRem; std::vector<double> sbarRem;
				ResampleRunPNS(cd.S, cd.normal, cd.sbar, spineIdx, Nrem, spineRem, normalRem, sbarRem);
				// SS14.3: "geometria: LA MISMA de E7 (recta, un solo pico,
				// taper, terminal de D-C)".
				StraightenSpine(spineRem, normalRem, P.straighten);

				double wRunL = P.remate_width * ar.wRun;
				double upL = 0.0; // pico EN el vertice, se afila alejandose
				std::vector<double> wProfL((size_t)Nrem);
				std::vector<Vec2> mDirL((size_t)Nrem);
				for (IntType i = 0; i < Nrem; i++)
				{
					double u = Nrem > 1 ? (double)i / (double)(Nrem - 1) : 0.0;
					wProfL[(size_t)i] = wRunL * TaperProfile(u, upL, P.taper_exp);
					mDirL[(size_t)i] = normalRem[(size_t)i] * -1.0;
				}
				if (P.tail_frac > 0.0)
				{
					std::vector<double> aArrL;
					ApplyNeedleTail(wProfL, aArrL, upL, P.tail_frac, P.tail_exp, P.tail_alpha_lo, ar.alphaR);
					RasterizeStripTail(aW, W, H, spineRem, mDirL, wProfL, aArrL, accStrip, accStripW);
				}
				else
				{
					RasterizeStrip(aW, W, H, spineRem, mDirL, wProfL, ar.alphaR, accStrip);
				}

				if (haveCorner) cd.usedCorner[(size_t)bestC] = 1;
				accPts.push_back(remPts);
				remateCount++;
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("remate comp=%1 x0=%2 y0=%3 x1=%4 y1=%5 len=%6 w=%7")
						.arg(ar.compIdx)
						.arg(spineRem.front().x, 0, 'f', 1).arg(spineRem.front().y, 0, 'f', 1)
						.arg(spineRem.back().x, 0, 'f', 1).arg(spineRem.back().y, 0, 'f', 1)
						.arg(lenL, 0, 'f', 1).arg(wRunL, 0, 'f', 2));
			}
		}

		// ---- GOLD tanda 2 (D-E SS14.4b): E14, PUENTE de laminas colineales
		// tras la seleccion. Solo entre laminas kind=0 de la MISMA
		// componente, recorridas en el orden de ACEPTACION (Q desc,
		// desempate total): "un puente por par, sin recursion".
		// bridge_max=0 => comportamiento tanda 1 (gate G22). ----
		IntType bridgeCount = 0;
		IntType bridgeMaxI = (IntType)std::llround(P.bridge_max);
		std::vector<uint8_t> bridgedFlag(acceptedRuns.size(), 0);
		for (size_t ib = 0; ib < acceptedRuns.size() && bridgeCount < bridgeMaxI; ib++)
		{
			if (acceptedRuns[ib].kind != 0 || acceptedRuns[ib].idx.empty() || bridgedFlag[ib]) continue;
			for (size_t jb = ib + 1; jb < acceptedRuns.size() && bridgeCount < bridgeMaxI; jb++)
			{
				if (acceptedRuns[jb].kind != 0 || acceptedRuns[jb].idx.empty() || bridgedFlag[jb]) continue;
				AcceptedRun& A = acceptedRuns[ib];
				AcceptedRun& B = acceptedRuns[jb];
				if (A.compIdx != B.compIdx) continue;
				CompData& cd = compData[(size_t)A.compIdx];
				double dFwdAB = ArcLenFwd(cd.S, A.idx.back(), B.idx.front());
				double dFwdBA = ArcLenFwd(cd.S, B.idx.back(), A.idx.front());
				bool useAtoB = dFwdAB <= dFwdBA;
				double gap = useAtoB ? dFwdAB : dFwdBA;
				if (gap > P.bridge_gap * k) continue;
				Vec2 dirA = cd.S[(size_t)A.idx.back()] - cd.S[(size_t)A.idx.front()];
				Vec2 dirB = cd.S[(size_t)B.idx.back()] - cd.S[(size_t)B.idx.front()];
				double ang = AngleBetweenDeg(dirA, dirB);
				if (ang > P.bridge_ang) continue;

				IntType fromIdx = useAtoB ? A.idx.back() : B.idx.back();
				IntType toIdx = useAtoB ? B.idx.front() : A.idx.front();
				std::vector<IntType> gapIdx;
				gapIdx.push_back(fromIdx);
				IntType mComp = (IntType)cd.S.size();
				IntType cur = fromIdx; IntType guard = 0;
				while (cur != toIdx && guard < mComp + 2)
				{
					cur = (cur + 1) % mComp;
					gapIdx.push_back(cur);
					guard++;
				}
				if (gapIdx.size() < 2) continue;

				// SS14.4b: "w(u) = lerp(w_extremo_a, w_extremo_b, u)" -- los
				// anchos reales en los DOS extremos que miran al hueco.
				double wEndFrom = EndWidthAtBoundary(useAtoB ? A : B, false, P);
				double wEndTo = EndWidthAtBoundary(useAtoB ? B : A, true, P);

				IntType Nb = std::max<IntType>(4, (IntType)std::llround(gap / (2.0 * k)));
				std::vector<Vec2> spineBr, normalBr; std::vector<double> sbarBr;
				ResampleRunPNS(cd.S, cd.normal, cd.sbar, gapIdx, Nb, spineBr, normalBr, sbarBr);
				// SS14.4b: "sin taper: es una continuacion" -- ni
				// StraightenSpine ni TaperProfile, solo interpolacion LINEAL
				// del ancho entre los dos extremos.
				std::vector<double> wProfBr((size_t)Nb);
				std::vector<Vec2> mDirBr((size_t)Nb);
				for (IntType i = 0; i < Nb; i++)
				{
					double u = Nb > 1 ? (double)i / (double)(Nb - 1) : 0.0;
					wProfBr[(size_t)i] = wEndFrom + (wEndTo - wEndFrom) * u;
					mDirBr[(size_t)i] = normalBr[(size_t)i] * -1.0;
				}
				double alphaBr = std::min(A.alphaR, B.alphaR);
				RasterizeStrip(aW, W, H, spineBr, mDirBr, wProfBr, alphaBr, accStrip);

				bridgedFlag[ib] = 1; bridgedFlag[jb] = 1;
				bridgeCount++;
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("puente comp=%1 x0=%2 y0=%3 x1=%4 y1=%5 gap=%6 wA=%7 wB=%8")
						.arg(A.compIdx)
						.arg(spineBr.front().x, 0, 'f', 1).arg(spineBr.front().y, 0, 'f', 1)
						.arg(spineBr.back().x, 0, 'f', 1).arg(spineBr.back().y, 0, 'f', 1)
						.arg(gap, 0, 'f', 1).arg(wEndFrom, 0, 'f', 2).arg(wEndTo, 0, 'f', 2));
			}
		}

		// ---- E8 (parte 2): acentos en V, en orden de Q descendente ----
		IntType vaccCount = 0;
		for (auto& ar : acceptedRuns)
		{
			if (vaccCount >= (IntType)P.v_max) break;
			// GOLD tanda 2 (D-A): los acentos en V solo aplican a laminas de
			// silueta (kind=0); compIdx=-1 en kind=1 no es un indice valido.
			if (ar.kind != 0) continue;
			CompData& cd = compData[(size_t)ar.compIdx];
			if (cd.cornerIdx.empty() || ar.idx.empty()) continue;
			IntType startI = ar.idx.front(), endI = ar.idx.back();

			for (IntType side = 0; side < 2 && vaccCount < (IntType)P.v_max; side++)
			{
				IntType endPt = side == 0 ? startI : endI;
				IntType bestC = -1; double bestD = 1e18;
				for (IntType civ = 0; civ < (IntType)cd.cornerIdx.size(); civ++)
				{
					if (cd.usedCorner[(size_t)civ]) continue;
					double dd = std::min(ArcLenFwd(cd.S, endPt, cd.cornerIdx[(size_t)civ]),
						ArcLenFwd(cd.S, cd.cornerIdx[(size_t)civ], endPt));
					if (dd < bestD) { bestD = dd; bestC = civ; }
				}
				if (bestC < 0 || bestD > P.v_dist * k) continue;
				IntType cornerJ = cd.cornerIdx[(size_t)bestC];
				if (cd.c01[(size_t)cornerJ] < P.v_contrast_min) continue;

				// GOLD r3 (R4): el tick del faro es CORTO y no escala con la madre
				// (ver v_len_frac/v_len_min/v_len_max en WdParams).
				double lenV = ClampRange(P.v_len_frac * ar.lenR, P.v_len_min * k, P.v_len_max * k);
				bool goForward = (endPt == endI);
				std::vector<IntType> vIdx;
				{
					IntType mComp = (IntType)cd.S.size();
					vIdx.push_back(cornerJ);
					double acc = 0.0; IntType cur = cornerJ; IntType guard = 0;
					while (acc < lenV && guard < mComp)
					{
						IntType nxt = goForward ? (cur + 1) % mComp : ((cur - 1) % mComp + mComp) % mComp;
						acc += Length(cd.S[(size_t)nxt] - cd.S[(size_t)cur]);
						vIdx.push_back(nxt);
						cur = nxt;
						guard++;
					}
				}
				if (vIdx.size() < 2) continue;

				// GOLD r2 (afinado Opus): el acento en V solo va en esquinas
				// convexas ILUMINADAS. El motor r1 lo emitia siempre que la
				// lamina madre tocaba una esquina, y por eso el gorro salia
				// SIMETRICO (el faro solo pinta el flanco izquierdo: el brazo
				// derecho del pico era este acento, no una lamina elegida).
				double vLobe = 0.0;
				for (IntType vj : vIdx) vLobe += cd.lobe[(size_t)vj];
				vLobe /= (double)vIdx.size();
				if (vLobe < P.v_light_min) continue;

				IntType Nv = std::max<IntType>(8, (IntType)std::llround(lenV / (2.0 * k)));
				std::vector<Vec2> spineV, normalV; std::vector<double> sbarV;
				ResampleRunPNS(cd.S, cd.normal, cd.sbar, vIdx, Nv, spineV, normalV, sbarV);
				double wRunV = P.v_width * ar.wRun;
				std::vector<double> wProf((size_t)Nv);
				std::vector<Vec2> mDirV((size_t)Nv);
				for (IntType i = 0; i < Nv; i++)
				{
					double u = Nv > 1 ? (double)i / (double)(Nv - 1) : 0.0;
					wProf[(size_t)i] = wRunV * TaperProfile(u, 0.0, P.taper_exp); // pico en la esquina (u=0)
					mDirV[(size_t)i] = normalV[(size_t)i] * -1.0;
				}
				RasterizeStrip(aW, W, H, spineV, mDirV, wProf, ar.alphaR, accStrip);

				cd.usedCorner[(size_t)bestC] = 1;
				vaccCount++;
			}
		}

		// ---- E9: bordes internos (opcional, P.internal) ----
		IntType internalCount = 0;
		if (internalActive)
		{
			IntType rb = std::max<IntType>(1, (IntType)std::llround(2.0 * k));
			std::vector<float> Yb((size_t)(W * H), 0.f);
			for (IntType y = 0; y < H; y++)
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					if (!M[(size_t)idx]) continue;
					double sum = 0.0; IntType cnt = 0;
					for (IntType dy = -rb; dy <= rb; dy++)
						for (IntType dx = -rb; dx <= rb; dx++)
						{
							IntType ny = y + dy, nx = x + dx;
							if (ny < 0 || ny >= H || nx < 0 || nx >= W) continue;
							IntType nidx = ny * W + nx;
							if (!M[(size_t)nidx]) continue;
							sum += Y[(size_t)nidx]; cnt++;
						}
					Yb[(size_t)idx] = cnt > 0 ? (float)(sum / (double)cnt) : Y[(size_t)idx];
				}

			std::vector<float> Gdx((size_t)(W * H), 0.f), Gdy((size_t)(W * H), 0.f), Gmag((size_t)(W * H), 0.f);
			auto YbAt = [&](IntType yy, IntType xx) -> float
			{
				yy = yy < 0 ? 0 : (yy >= H ? H - 1 : yy);
				xx = xx < 0 ? 0 : (xx >= W ? W - 1 : xx);
				return Yb[(size_t)(yy * W + xx)];
			};
			for (IntType y = 0; y < H; y++)
				for (IntType x = 0; x < W; x++)
				{
					float gx = (YbAt(y - 1, x + 1) + 2 * YbAt(y, x + 1) + YbAt(y + 1, x + 1))
						- (YbAt(y - 1, x - 1) + 2 * YbAt(y, x - 1) + YbAt(y + 1, x - 1));
					float gy = (YbAt(y + 1, x - 1) + 2 * YbAt(y + 1, x) + YbAt(y + 1, x + 1))
						- (YbAt(y - 1, x - 1) + 2 * YbAt(y - 1, x) + YbAt(y - 1, x + 1));
					IntType idx = y * W + x;
					Gdx[(size_t)idx] = gx; Gdy[(size_t)idx] = gy; Gmag[(size_t)idx] = std::sqrt(gx * gx + gy * gy);
				}

			std::vector<uint8_t> cand((size_t)(W * H), 0);
			double intDepthPx = P.int_depth_min * k;
			for (IntType i = 0; i < W * H; i++)
				if (Msel[(size_t)i] && (double)Dext[(size_t)i] > intDepthPx && Gmag[(size_t)i] >= (float)P.int_grad_min)
					cand[(size_t)i] = 1;

			std::vector<uint8_t> ridge((size_t)(W * H), 0);
			static const IntType sdx[8] = { 1,1,0,-1,-1,-1,0,1 };
			static const IntType sdy8[8] = { 0,1,1,1,0,-1,-1,-1 };
			for (IntType y = 0; y < H; y++)
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					if (!cand[(size_t)idx]) continue;
					float gx = Gdx[(size_t)idx], gy = Gdy[(size_t)idx];
					float gl = std::sqrt(gx * gx + gy * gy);
					if (gl < 1e-6f) continue;
					double ang = std::atan2((double)gy, (double)gx);
					IntType sector = ((IntType)std::llround(ang / (kPi / 4.0))) & 7;
					IntType nx1 = x + sdx[(size_t)sector], ny1 = y + sdy8[(size_t)sector];
					IntType nx2 = x - sdx[(size_t)sector], ny2 = y - sdy8[(size_t)sector];
					float g1 = (nx1 >= 0 && nx1 < W && ny1 >= 0 && ny1 < H) ? Gmag[(size_t)(ny1 * W + nx1)] : 0.f;
					float g2 = (nx2 >= 0 && nx2 < W && ny2 >= 0 && ny2 < H) ? Gmag[(size_t)(ny2 * W + nx2)] : 0.f;
					if (Gmag[(size_t)idx] >= g1 && Gmag[(size_t)idx] >= g2)
						ridge[(size_t)idx] = 1;
				}

			std::vector<Component> ridgeComps;
			FindComponents(ridge, W, H, P.int_min_px * k * k, ridgeComps);

			std::vector<uint8_t> visitScratch((size_t)(W * H), 0);
			std::vector<InternalCand> intCands;
			for (auto& rc : ridgeComps)
			{
				std::vector<IPt> pts = CollectComponentPixels(ridge, W, H, rc.seed, visitScratch);
				// GOLD tanda 2 (D-A SS13.3): construccion de la espina
				// factorizada a RidgeToSpine (compartida con E12).
				std::vector<Vec2> spineInt;
				double sLen = 0.0;
				if (!RidgeToSpine(pts, k, P.min_run * k, spineInt, sLen))
					continue;
				// GOLD tanda 2: nSpine ya no lo declara el bloque PCA inline
				// (factorizado a RidgeToSpine) -- se deriva del spine devuelto.
				IntType nSpine = (IntType)spineInt.size();

				double sumGx = 0.0, sumGy = 0.0;
				for (auto& p : pts) { sumGx += Gdx[(size_t)(p.y * W + p.x)]; sumGy += Gdy[(size_t)(p.y * W + p.x)]; }
				Vec2 dirNorm = Normalize(Vec2{ sumGx, sumGy });
				if (Length(dirNorm) < 1e-6) continue;

				std::vector<double> deltaProf((size_t)nSpine, 0.0);
				double meanDelta = 0.0;
				for (IntType i = 0; i < nSpine; i++)
				{
					Vec2 pp = spineInt[(size_t)i] + dirNorm * (4.0 * k);
					Vec2 pm = spineInt[(size_t)i] - dirNorm * (4.0 * k);
					double yp = SampleYClamped(Y, W, H, pp.x, pp.y);
					double ym = SampleYClamped(Y, W, H, pm.x, pm.y);
					double dlt = std::abs(yp - ym);
					deltaProf[(size_t)i] = dlt;
					meanDelta += dlt;
				}
				meanDelta /= (double)nSpine;

				double contrastNorm = Clamp01((meanDelta - CON_LO) / (CON_HI - CON_LO));
				double lightTerm = P.light_bias + (1.0 - P.light_bias) * std::pow(std::max(0.0, Dot(dirNorm, L2)), P.light_pow);

				InternalCand icand;
				icand.spine = spineInt; icand.dir = dirNorm; icand.lenInt = sLen;
				// GOLD r3: mismo suelo de contraste que en la silueta.
				double cTermInt = P.contrast_bias + (1.0 - P.contrast_bias) * contrastNorm;
				icand.scoreInt = cTermInt * lightTerm; icand.deltaProfile = deltaProf;
				intCands.push_back(icand);
			}

			std::stable_sort(intCands.begin(), intCands.end(), [&P](const InternalCand& a, const InternalCand& b)
				{
					double qa = std::pow(a.lenInt / 100.0, P.q_len) * a.scoreInt;
					double qb = std::pow(b.lenInt / 100.0, P.q_len) * b.scoreInt;
					return qa > qb;
				});

			IntType internalMax = (IntType)P.internal_max;
			// GOLD r3 (R2): las internas tambien respetan sep_px. La "escuadra de
			// la cadera" de la r2 era una horizontal de silueta con una vertical
			// INTERNA pegada encima: min_gap no las veia (caminos distintos) y
			// internal_max solo limita cuantas, no donde.
			std::vector<std::vector<Vec2>> intPts;
			for (auto& icand : intCands)
			{
				if (internalCount >= internalMax) break;
				double alphaRInt = Clamp01(icand.scoreInt);
				if (alphaRInt < 0.05) continue;
				std::vector<Vec2> icPts;
				if (sepK > 0.0)
				{
					size_t st = (size_t)std::max<IntType>(1, (IntType)std::llround(2.0 * k));
					for (size_t q = 0; q < icand.spine.size(); q += st) icPts.push_back(icand.spine[q]);
					if (!icand.spine.empty()) icPts.push_back(icand.spine.back());
					double intSepK = (P.int_sep >= 0.0) ? P.int_sep * k : sepK;
					bool clash = false;
					for (auto& ap : accPts) if (minDistPoly(icPts, ap) < intSepK) { clash = true; break; }
					if (!clash)
						for (auto& ip : intPts) if (minDistPoly(icPts, ip) < sepK) { clash = true; break; }
					if (clash) continue;
					intPts.push_back(icPts);
				}

				IntType Ni = (IntType)icand.spine.size();
				IntType upIdx = 0; double upVal = icand.deltaProfile[0];
				for (IntType i = 1; i < Ni; i++) if (icand.deltaProfile[(size_t)i] > upVal) { upVal = icand.deltaProfile[(size_t)i]; upIdx = i; }
				double up = SnapPeakPos(Ni > 1 ? (double)upIdx / (double)(Ni - 1) : 0.0);

				double wnorm = Clamp01((icand.scoreInt - P.t_lo) / (P.w_ref - P.t_lo));
				double wRunInt = P.width * k * alphaRInt * (P.w_lo + (P.w_hi - P.w_lo) * wnorm) * 0.80;

				std::vector<double> wProf((size_t)Ni);
				std::vector<Vec2> mDir((size_t)Ni, icand.dir);
				for (IntType i = 0; i < Ni; i++)
				{
					double u = Ni > 1 ? (double)i / (double)(Ni - 1) : 0.0;
					wProf[(size_t)i] = wRunInt * TaperProfile(u, up, P.taper_exp);
				}
				RasterizeStrip(aI, W, H, icand.spine, mDir, wProf, alphaRInt, accStrip);
				if (!P.dbgRuns.isEmpty())
					dbgLines.append(QString("interna x0=%1 y0=%2 x1=%3 y1=%4 len=%5 score=%6 alpha=%7 w=%8")
						.arg(icand.spine.front().x, 0, 'f', 1).arg(icand.spine.front().y, 0, 'f', 1)
						.arg(icand.spine.back().x, 0, 'f', 1).arg(icand.spine.back().y, 0, 'f', 1)
						.arg(icand.lenInt, 0, 'f', 1).arg(icand.scoreInt, 0, 'f', 4)
						.arg(alphaRInt, 0, 'f', 3).arg(wRunInt, 0, 'f', 2));
				internalCount++;
			}
		}

		// ---- E10: capa de curvas (sustituto del inner outline) ----
		// GOLD tanda 3 (E-A): reusa el `lut` calculado arriba (misma LUT,
		// misma funcion pura) en vez de recalcularlo -- byte-identico a
		// antes, sin la llamada duplicada.
		IntType curvesEmitted = 0;
		if (!P.outCurves.isEmpty())
		{
			double cw = P.curves_width * k;
			double ramp = P.curves_ramp * k;
			QImage curvesImg(W, H, QImage::Format_RGBA8888);
			for (IntType y = 0; y < H; y++)
			{
				const uchar* srcLine = img.constScanLine((int)y);
				uchar* dstLine = curvesImg.scanLine((int)y);
				for (IntType x = 0; x < W; x++)
				{
					IntType idx = y * W + x;
					double d = (double)Dext[(size_t)idx];
					double band;
					if (d <= cw) band = 1.0;
					else if (d <= cw + ramp) band = ramp > 1e-9 ? 1.0 - (d - cw) / ramp : 0.0;
					else band = 0.0;
					if (!M[(size_t)idx]) band = 0.0;

					if (band > 0.0)
					{
						double r = srcLine[(size_t)x * 4 + 0], g = srcLine[(size_t)x * 4 + 1], b = srcLine[(size_t)x * 4 + 2];
						double curR = (double)lut[(size_t)(uchar)r], curG = (double)lut[(size_t)(uchar)g], curB = (double)lut[(size_t)(uchar)b];
						double outR = ClampRange(r + (curR - r) * P.curves_strength, 0.0, 255.0);
						double outG = ClampRange(g + (curG - g) * P.curves_strength, 0.0, 255.0);
						double outB = ClampRange(b + (curB - b) * P.curves_strength, 0.0, 255.0);
						dstLine[(size_t)x * 4 + 0] = (uchar)std::llround(outR);
						dstLine[(size_t)x * 4 + 1] = (uchar)std::llround(outG);
						dstLine[(size_t)x * 4 + 2] = (uchar)std::llround(outB);
						dstLine[(size_t)x * 4 + 3] = (uchar)std::llround(255.0 * Clamp01(band) * P.curves_alpha);
					}
					else
					{
						dstLine[(size_t)x * 4 + 0] = 0; dstLine[(size_t)x * 4 + 1] = 0;
						dstLine[(size_t)x * 4 + 2] = 0; dstLine[(size_t)x * 4 + 3] = 0;
					}
				}
			}
			if (curvesImg.save(P.outCurves))
				curvesEmitted = 1;
		}

		// ---- E11: salidas, recortes e info ----
		// GOLD r1: con `straighten` la lamina es una cuerda; en un tramo
		// convexo su centro se separa del borde mas que el ancho (la sagita
		// de la cuerda). El recorte por profundidad pasa a ser una red de
		// seguridad (clip_depth), no el ancho de la cuna.
		double wedgeClipDist = std::max(P.width * k + 3.0, P.clip_depth * k);
		for (IntType i = 0; i < W * H; i++)
			if (!M[(size_t)i] || (double)Dext[(size_t)i] > wedgeClipDist)
				aW[(size_t)i] = 0.f;
		for (IntType i = 0; i < W * H; i++)
			if (!M[(size_t)i]) aI[(size_t)i] = 0.f;

		std::vector<float> aA((size_t)(W * H), 0.f);
		if (P.outline_alpha > 0.0)
		{
			double outlineLimit = P.outline_width * k + 1.0;
			for (IntType i = 0; i < W * H; i++)
			{
				if (!M[(size_t)i]) continue;
				double d = (double)Dext[(size_t)i];
				if (d <= outlineLimit)
					aA[(size_t)i] = (float)(P.outline_alpha * Clamp01(outlineLimit - d));
			}
		}

		IntType wedgepx = 0;
		std::vector<uint8_t> lamSeed((size_t)(W * H), 0);
		for (IntType i = 0; i < W * H; i++)
		{
			double comb = std::max((double)aW[(size_t)i], std::max((double)aI[(size_t)i], (double)aA[(size_t)i]));
			if (comb > 0.0) wedgepx++;
			if (std::max((double)aW[(size_t)i], (double)aI[(size_t)i]) > 0.06) lamSeed[(size_t)i] = 1;
		}
		std::vector<float> Dcov = ChamferDistance(lamSeed, W, H);
		double covLimit = 6.0 * k;
		IntType coveredCount = 0, totalCount = (IntType)allContourPts.size();
		for (auto& p : allContourPts)
		{
			if (p.y < 0 || p.y >= H || p.x < 0 || p.x >= W) continue;
			if ((double)Dcov[(size_t)(p.y * W + p.x)] <= covLimit) coveredCount++;
		}
		double cov = totalCount > 0 ? (double)coveredCount / (double)totalCount : 0.0;

		QImage out(W, H, QImage::Format_RGBA8888);
		for (IntType y = 0; y < H; y++)
		{
			uchar* line = out.scanLine((int)y);
			for (IntType x = 0; x < W; x++)
			{
				size_t idx = (size_t)(y * W + x);
				double av = std::max((double)aW[idx], std::max((double)aI[idx], (double)aA[idx]));
				IntType ai = (IntType)std::llround(255.0 * Clamp01(av));
				line[(size_t)x * 4 + 0] = 255;
				line[(size_t)x * 4 + 1] = 255;
				line[(size_t)x * 4 + 2] = 255;
				line[(size_t)x * 4 + 3] = (uchar)ai;
			}
		}
		if (!out.save(outPng))
			return QString("error:save_failed:") + outPng;

		if (P.bake != 0.0)
		{
			QImage baked = img.copy();
			for (IntType y = 0; y < H; y++)
			{
				uchar* bline = baked.scanLine((int)y);
				const uchar* oline = out.constScanLine((int)y);
				for (IntType x = 0; x < W; x++)
				{
					double sa = oline[(size_t)x * 4 + 3] / 255.0;
					double da = bline[(size_t)x * 4 + 3] / 255.0;
					double outA = sa + da * (1.0 - sa);
					double r = 0, g = 0, b = 0;
					if (outA > 1e-9)
					{
						r = (255.0 * sa + bline[(size_t)x * 4 + 0] * da * (1.0 - sa)) / outA;
						g = (255.0 * sa + bline[(size_t)x * 4 + 1] * da * (1.0 - sa)) / outA;
						b = (255.0 * sa + bline[(size_t)x * 4 + 2] * da * (1.0 - sa)) / outA;
					}
					bline[(size_t)x * 4 + 0] = (uchar)std::llround(Clamp01(r / 255.0) * 255.0);
					bline[(size_t)x * 4 + 1] = (uchar)std::llround(Clamp01(g / 255.0) * 255.0);
					bline[(size_t)x * 4 + 2] = (uchar)std::llround(Clamp01(b / 255.0) * 255.0);
					bline[(size_t)x * 4 + 3] = (uchar)std::llround(Clamp01(outA) * 255.0);
				}
			}
			if (!baked.save(srcPng))
				return QString("error:bake_save_failed:") + srcPng;
		}

		QString conMode = bgOk ? "bg" : "in";
		QString info = QString("ok cov=%1 runs=%2 lam=%3 vacc=%4 int=%5 wedgepx=%6 K=%7 con=%8 curves=%9")
			.arg(cov, 0, 'f', 3)
			.arg((IntType)candidates.size())
			.arg((IntType)acceptedRuns.size())
			.arg(vaccCount)
			.arg(internalCount)
			.arg(wedgepx)
			.arg(K)
			.arg(conMode)
			.arg(curvesEmitted);
		info += QString(" ang=%1 dens=%2 w=%3 lb=%4 mg=%5 mr=%6")
			.arg(angleDeg, 0, 'f', 1)
			.arg(P.density, 0, 'f', 3)
			.arg(P.width, 0, 'f', 2)
			.arg(P.light_bias, 0, 'f', 2)
			.arg(P.min_gap, 0, 'f', 1)
			.arg(P.min_run, 0, 'f', 1);
		if (!P.bg.isEmpty() && !bgOk)
			info += " bg=off";
		// GOLD tanda 2 (D-A SS14.5): auditoria de profundidad/D-B/D-D/D-E.
		// El formato de la tanda 1 se CONSERVA entero; esto se ANADE al
		// final (nunca se reordena: los parsers del arnes cuentan con el
		// prefijo).
		if (!P.depth.isEmpty() && !depthOk)
			info += " depth=off";
		else if (depthOk)
		{
			info += QString(" depth=on near=%1 far=%2")
				.arg(depthNear, 0, 'g', 4).arg(depthFar, 0, 'g', 4);
			if (!depthJsonOk)
				info += " depth_json=off";
		}
		if (depthOk)
			info += " int=depth";
		info += QString(" dcand=%1 dlam=%2 rem=%3 brg=%4 Kf=%5 dgain=%6")
			.arg(dcandCount).arg(dlamCount).arg(remateCount).arg(bridgeCount)
			.arg(Kf, 0, 'f', 2).arg(densGain, 0, 'f', 3);

		// GOLD r4: volcado de diagnostico. No participa en el PNG ni en `info`
		// (pureza R6 intacta); solo se escribe si el llamador dio una ruta.
		if (!P.dbgRuns.isEmpty())
		{
			QFile dbgFile(P.dbgRuns);
			if (dbgFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
			{
				QByteArray blob = (info + "\n" + dbgLines.join("\n") + "\n").toLatin1();
				dbgFile.write(blob);
				dbgFile.close();
			}
		}
		return info;
	}

	// DEPRECADO: ver Mi3WhiteDetails.hpp / SPEC_WHITE_DETAILS_GOLD.md C2.
	// Wrapper de compatibilidad sobre la API vieja de 10 argumentos: arma la
	// cadena "opts" equivalente y delega en el motor GOLD. 'g' con 10 digitos
	// significativos evita perdida de precision en el roundtrip numero->texto.
	QString Mi3WhiteDetailsRun(const QString& srcPng, const QString& outPng,
		double angleDeg, double density, double wedgeWidth, double outlineWidth,
		double outlineAlpha, double minRunLen, IntType seed, IntType bake)
	{
		QString opts = QString("angle=%1;density=%2;width=%3;outline_width=%4;outline_alpha=%5;min_run=%6;seed=%7;bake=%8")
			.arg(angleDeg, 0, 'g', 10)
			.arg(density, 0, 'g', 10)
			.arg(wedgeWidth, 0, 'g', 10)
			.arg(outlineWidth, 0, 'g', 10)
			.arg(outlineAlpha, 0, 'g', 10)
			.arg(minRunLen, 0, 'g', 10)
			.arg((double)seed, 0, 'g', 10)
			.arg((double)bake, 0, 'g', 10);
		return Mi3WhiteDetailsRunOpt(srcPng, outPng, opts);
	}

	// SPEC_WHITE_DETAILS_GOLD.md SS1: SE CONSERVA TAL CUAL (sin cambios).
	double Mi3WhiteDetailsAutoAngle()
	{
		const CinemaEnvState& env = CinemaSceneEnv();
		const CinemaCameraState& cam = CinemaSceneCamera();
		if (!env.valid || !cam.valid)
			return -999.0;

		const float* m = cam.viewProj; // 4x4 GPU column-major (ver Render/Matrix.hpp)
		float sx = env.sunDir[0], sy = env.sunDir[1], sz = env.sunDir[2];
		float hx = m[0] * sx + m[4] * sy + m[8] * sz;
		float hy = m[1] * sx + m[5] * sy + m[9] * sz;
		float hz = m[2] * sx + m[6] * sy + m[10] * sz;
		float hw = m[3] * sx + m[7] * sy + m[11] * sz;

		float vlen = std::sqrt(hx * hx + hy * hy);
		float scaleRef = std::max(std::abs(hz), std::max(std::abs(hw), 1.0f));
		if (vlen < 1e-3f * scaleRef)
			return -999.0;

		double angDeg = std::atan2((double)hy, (double)hx) * (double)RADTODEG;
		angDeg = std::fmod(angDeg + 360.0, 360.0);
		return angDeg;
	}
}
