#include "CinemaOracle.hpp"

#include "CinemaBvh.hpp"
#include "CinemaKernelM0.hpp"
#include "CinemaScene.hpp"

#include <cmath>
#include <cstring>
#include <vector>

// CinemaOracle: implementacion (contrato completo en CinemaOracle.hpp).
//
// Todo el integrador esta portado linea a linea del kernel HLSL (CSPathTrace
// de CinemaKernelM0.hpp) mas las extensiones M1/M2 documentadas en el
// header. Se trabaja en float (no double) para quedar cerca de la aritmetica
// GPU; el RNG es bit-exacto por construccion (solo enteros uint32 y un
// escalado por potencia de 2 exacta).

namespace CppProject
{
	namespace
	{
		// -----------------------------------------------------------------
		// Constantes (espejo de las #define del kernel + host)
		// -----------------------------------------------------------------

		const float ORACLE_PI = 3.14159265358979f;
		const float ORACLE_TWO_PI = 6.28318530717959f;
		const float ORACLE_T_MAX = 1.0e30f;
		const float ORACLE_T_EPS = 1.0e-4f;
		const uint32_t ORACLE_MAX_TRANSP = 32;   // PT_MAX_TRANSP del kernel
		// R15: espejo de PT_MAX_TRANSP_HOLDOUT (presupuesto ampliado del rayo de
		// camara que ya cruzo un objeto invisible-a-camara).
		const uint32_t ORACLE_MAX_TRANSP_HOLDOUT = 256;
		const uint32_t ORACLE_INVALID = 0xFFFFFFFFu;

		// Ruleta rusa (diseno seccion 6): desde bounce >= 3 (0-based)
		const uint32_t ORACLE_RR_START = 3;
		const float ORACLE_RR_MIN = 0.05f;
		const float ORACLE_RR_MAX = 0.95f;

		// M2: materiales (espejo de las PT_* del kernel)
		const float ORACLE_ROUGH_MIN = 0.03f;      // PT_ROUGH_MIN
		const float ORACLE_F0_DIELECTRIC = 0.04f;  // PT_F0_DIELECTRIC
		const float ORACLE_LEAF_TRANSMIT = 0.3f;   // PT_LEAF_TRANSMIT
		const float ORACLE_WATER_IOR = 1.33f;      // PT_WATER_IOR
		const float ORACLE_WATER_ROUGH = 0.07f;    // PT_WATER_ROUGH
		const float ORACLE_GLASS_F0 = 0.04f;       // PT_GLASS_F0
		const float ORACLE_WATER_ABS_DIST = 16.0f; // PT_WATER_ABS_DIST
		const float ORACLE_SIGMA_FLOOR = 1.0e-3f;  // PT_SIGMA_FLOOR
		const float ORACLE_SEG_MAX = 1.0e6f;       // PT_SEG_MAX

		// kinds de superficie (PT_KIND_* del kernel)
		const uint32_t ORACLE_KIND_STD = 0;
		const uint32_t ORACLE_KIND_LEAF = 1;
		const uint32_t ORACLE_KIND_WATER = 2;
		const uint32_t ORACLE_KIND_GLASS = 3;
		const uint32_t ORACLE_KIND_COVER = 4; // espejo de PT_KIND_COVER

		// Duplicadas de CinemaRender.cpp (DEBEN casar; ver nota del header)
		const float ORACLE_SUN_ANGLE_DEG = 2.0f;
		const float ORACLE_SUN_IRRADIANCE_SCALE = 5.0f;

		const IntType ORACLE_MAX_DIM = 16384;

		// -----------------------------------------------------------------
		// Vectores minimos
		// -----------------------------------------------------------------

		struct OVec
		{
			float x = 0.f, y = 0.f, z = 0.f;
		};

		struct ORand
		{
			float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
		};

		inline OVec OMake(float x, float y, float z) { OVec v; v.x = x; v.y = y; v.z = z; return v; }
		inline OVec OAdd(OVec a, OVec b) { return OMake(a.x + b.x, a.y + b.y, a.z + b.z); }
		inline OVec OSub(OVec a, OVec b) { return OMake(a.x - b.x, a.y - b.y, a.z - b.z); }
		inline OVec OMul(OVec a, OVec b) { return OMake(a.x * b.x, a.y * b.y, a.z * b.z); }
		inline OVec OScale(OVec a, float s) { return OMake(a.x * s, a.y * s, a.z * s); }
		inline float ODot(OVec a, OVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline OVec OCross(OVec a, OVec b)
		{
			return OMake(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
		}
		inline float OLen(OVec a) { return std::sqrt(ODot(a, a)); }
		inline OVec ONormalize(OVec a)
		{
			float len = OLen(a);
			return (len > 0.f) ? OScale(a, 1.f / len) : a;
		}
		inline float OClamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
		inline float OLum(OVec c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }
		inline BoolType OFinite(OVec c)
		{
			return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
		}

		// lerp del kernel (HLSL lerp)
		inline OVec OLerp(OVec a, OVec b, float t)
		{
			return OAdd(a, OScale(OSub(b, a), t));
		}

		// -----------------------------------------------------------------
		// RNG pcg4d: bit-exacto contra Pcg4d/Rand4 del kernel
		// -----------------------------------------------------------------

		inline void Pcg4d(uint32_t v[4])
		{
			v[0] = v[0] * 1664525u + 1013904223u;
			v[1] = v[1] * 1664525u + 1013904223u;
			v[2] = v[2] * 1664525u + 1013904223u;
			v[3] = v[3] * 1664525u + 1013904223u;
			v[0] += v[1] * v[3]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1]; v[3] += v[1] * v[2];
			v[0] ^= v[0] >> 16u;
			v[1] ^= v[1] >> 16u;
			v[2] ^= v[2] >> 16u;
			v[3] ^= v[3] >> 16u;
			v[0] += v[1] * v[3]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1]; v[3] += v[1] * v[2];
		}

		// ctr = sampleIdx + seed * 0x9E3779B9 (uint32, con wraparound)
		inline ORand Rand4(uint32_t pixelX, uint32_t pixelY, uint32_t ctr, uint32_t dim)
		{
			uint32_t v[4] = { pixelX, pixelY, ctr, dim };
			Pcg4d(v);
			ORand r;
			r.x = (float)(v[0] >> 8u) * (1.0f / 16777216.0f);
			r.y = (float)(v[1] >> 8u) * (1.0f / 16777216.0f);
			r.z = (float)(v[2] >> 8u) * (1.0f / 16777216.0f);
			r.w = (float)(v[3] >> 8u) * (1.0f / 16777216.0f);
			return r;
		}

		// -----------------------------------------------------------------
		// Decodificacion de atributos (espejo del kernel)
		// -----------------------------------------------------------------

		// Espejo del DecodeNormal888 del kernel: inversa EXACTA del empaquetado
		// del motor (byte = (n+1)/2 * 255), no la division por 127 de antes.
		inline OVec DecodeNormal888(uint32_t n)
		{
			return OMake(
				(float)(n & 255u) * (2.f / 255.f) - 1.f,
				(float)((n >> 8u) & 255u) * (2.f / 255.f) - 1.f,
				(float)((n >> 16u) & 255u) * (2.f / 255.f) - 1.f);
		}

		inline OVec DecodeColorRgb(uint32_t c)
		{
			return OMake(
				(float)(c & 255u) / 255.f,
				(float)((c >> 8u) & 255u) / 255.f,
				(float)((c >> 16u) & 255u) / 255.f);
		}

		inline float DecodeVertEmissive(uint32_t dataFlags)
		{
			return (float)((dataFlags >> 8u) & 255u) / 255.f;
		}

		inline OVec PtVertPos(const PTVert& v) { return OMake(v.px, v.py, v.pz); }

		// -----------------------------------------------------------------
		// Muestreo (espejo del kernel: BuildOnb Duff 2017, coseno, cono)
		// -----------------------------------------------------------------

		inline void BuildOnb(OVec n, OVec& t, OVec& b)
		{
			float s = (n.z >= 0.f) ? 1.f : -1.f;
			float a = -1.f / (s + n.z);
			float c = n.x * n.y * a;
			t = OMake(1.f + s * n.x * n.x * a, s * c, -s * n.x);
			b = OMake(c, s + n.y * n.y * a, -n.y);
		}

		inline OVec CosineSampleHemisphere(float r1, float r2, OVec n)
		{
			float phi = ORACLE_TWO_PI * r1;
			float sr = std::sqrt(r2);
			OVec t, b;
			BuildOnb(n, t, b);
			OVec d = OAdd(OAdd(OScale(t, std::cos(phi) * sr), OScale(b, std::sin(phi) * sr)),
				OScale(n, std::sqrt(std::max(0.f, 1.f - r2))));
			return ONormalize(d);
		}

		inline OVec SampleCone(float r1, float r2, OVec axis, float cosMax)
		{
			float cosTheta = 1.f - r1 * (1.f - cosMax);
			float sinTheta = std::sqrt(std::max(0.f, 1.f - cosTheta * cosTheta));
			float phi = ORACLE_TWO_PI * r2;
			OVec t, b;
			BuildOnb(axis, t, b);
			return ONormalize(OAdd(OAdd(OScale(t, std::cos(phi) * sinTheta),
				OScale(b, std::sin(phi) * sinTheta)), OScale(axis, cosTheta)));
		}

		// -----------------------------------------------------------------
		// M2: Fresnel y GGX (portes exactos de SchlickF3 / FresnelDielectric
		// / GgxD / GgxLambda / SampleGgxVndf del kernel, mismo orden de
		// operaciones)
		// -----------------------------------------------------------------

		inline OVec SchlickF3(OVec f0, float cosX)
		{
			float p = OClamp01(1.f - cosX);
			float p2 = p * p;
			float p5 = p2 * p2 * p;
			return OMake(
				f0.x + (1.f - f0.x) * p5,
				f0.y + (1.f - f0.y) * p5,
				f0.z + (1.f - f0.z) * p5);
		}

		// Fresnel dielectrico exacto (etaRel = n_incidente / n_transmitido);
		// el caller detecta TIR antes (sin2T >= 1 -> F = 1)
		inline float FresnelDielectric(float cosI, float cosT, float etaRel)
		{
			float rs = (etaRel * cosI - cosT) / std::max(etaRel * cosI + cosT, 1.0e-8f);
			float rp = (cosI - etaRel * cosT) / std::max(cosI + etaRel * cosT, 1.0e-8f);
			return OClamp01(0.5f * (rs * rs + rp * rp));
		}

		inline float GgxD(float cosNH, float alphaG)
		{
			float a2 = alphaG * alphaG;
			float d = cosNH * cosNH * (a2 - 1.f) + 1.f;
			return a2 / std::max(ORACLE_PI * d * d, 1.0e-12f);
		}

		inline float GgxLambda(float cosX, float alphaG)
		{
			float c2 = cosX * cosX;
			float t2 = std::max(0.f, 1.f - c2) / std::max(c2, 1.0e-8f);
			return 0.5f * (std::sqrt(1.f + alphaG * alphaG * t2) - 1.f);
		}

		// Muestreo VNDF (Heitz 2018) en marco local (z = normal), woL.z > 0
		inline OVec SampleGgxVndf(OVec woL, float alphaG, float r1, float r2)
		{
			OVec vh = ONormalize(OMake(alphaG * woL.x, alphaG * woL.y, woL.z));
			float lensq = vh.x * vh.x + vh.y * vh.y;
			OVec t1v = (lensq > 1.0e-12f)
				? OScale(OMake(-vh.y, vh.x, 0.f), 1.f / std::sqrt(lensq))
				: OMake(1.f, 0.f, 0.f);
			OVec t2v = OCross(vh, t1v);
			float rr = std::sqrt(r1);
			float phi = ORACLE_TWO_PI * r2;
			float t1 = rr * std::cos(phi);
			float t2 = rr * std::sin(phi);
			float s = 0.5f * (1.f + vh.z);
			t2 = (1.f - s) * std::sqrt(std::max(0.f, 1.f - t1 * t1)) + s * t2;
			OVec nh = OAdd(OAdd(OScale(t1v, t1), OScale(t2v, t2)),
				OScale(vh, std::sqrt(std::max(0.f, 1.f - t1 * t1 - t2 * t2))));
			return ONormalize(OMake(alphaG * nh.x, alphaG * nh.y, std::max(1.0e-6f, nh.z)));
		}

		// -----------------------------------------------------------------
		// Frame (espejo CPU del cbuffer PTFrame que veria el kernel)
		// -----------------------------------------------------------------

		struct OracleFrame
		{
			float invViewProj[16] = {};
			OVec camPos;
			OVec sunDir;
			float cosSunMax = 1.f;
			float sunOmega = 0.f;
			OVec sunRadiance;  // radiancia del DISCO (ya dividida por omega y escalada)
			OVec skyZenith, skyHorizon, ambient;
			float nightAlpha = 0.f;
			uint32_t maxBounces = 3;
			float clampInd = 0.f;      // res_clamp.z (0 = off)
			float emissiveScale = 0.f; // res_clamp.w
			float width = 0.f, height = 0.f;
			uint32_t seed = 0;
			// R6 DoF (espejo de dofParams del cbuffer): x foco, y radio lente,
			// z on. apertureRadius 0 o dofOn 0 => rayo pinhole identico.
			BoolType dofOn = false;
			float focusDist = 0.f;
			float apertureRadius = 0.f;
			// DOF-FIEL A1/A5 (espejo de lensParams del cbuffer): apertura
			// poligonal. Todos 0 => disco uniforme, camino bit-identico.
			float blades = 0.f, lensRot = 0.f, lensRatio = 0.f, lensBias = 0.f;
			// DOF-FIEL A2 (espejo de camFwd del cbuffer): eje optico unitario,
			// para refocalizar por PLANO en vez de por esfera.
			OVec camFwd;
			// CAPAS SLICING (espejo de bandParams del cbuffer): ventana de
			// profundidad del segmento de camara. bandOn=false => bit-identico.
			BoolType bandOn = false;
			float bandT0 = 0.f;
			float bandT1 = ORACLE_T_MAX;   // usa la constante de "sin limite" del fichero
			// R14 (espejo de FRAMEF_ORTHO): camara ORTOGRAFICA -> el rayo
			// primario nace en el plano near del pixel, no en camPos.
			BoolType ortho = false;
			// R12 niebla (espejo de fogParams0/1/2 del cbuffer). fogShow 0 =>
			// el camino es el de R11, bit a bit.
			BoolType fogShow = false;
			OVec fogColor, fogColorSun;
			float fogDistance = 0.f, fogSize = 0.f, fogHeight = 0.f;
			float fogMixGamma = 0.f;
			// F5 (C1, espejo de fogParams1.w del cbuffer): peso del grupo
			// ENTORNO en la NEE de "otra fuente". 0 = OFF = camino bit-identico.
			float envLight = 0.f;
			// FONDO DE PELICULA (espejo de FRAMEF_BACKDROP en counts.w): lo que
			// la camara ve DETRAS del sujeto no pinta ni cubre. El COLOR no llega
			// hasta aqui a proposito -- lo pone el resolve, DESPUES del tonemap, y
			// cinema.parity compara la media LINEAL del acumulador, que es justo
			// esto. false = OFF = camino bit-identico.
			BoolType backdrop = false;
		};

		// DOF-FIEL A1: espejo LITERAL de LensApertureSample del kernel (mismo
		// orden de operaciones, std::sqrt/std::cos/std::sin, ORACLE_PI/
		// ORACLE_TWO_PI). blades<3 es bit-identico a la formula de disco
		// unitario uniforme de antes de esta ronda.
		inline void OLensApertureSample(float ux, float uy, float blades, float rotRad, float ratio,
			float& outX, float& outY)
		{
			float px, py;
			if (blades < 3.f)
			{
				float r = std::sqrt(ux);
				float ang = ORACLE_TWO_PI * uy;
				px = r * std::cos(ang);
				py = r * std::sin(ang);
			}
			else
			{
				float n = std::floor(blades);
				float sector = std::floor(ux * n);
				float su = ux * n - sector;
				float a = ORACLE_PI / n;
				float s = std::sqrt(su);
				float t = uy * s;
				float w0 = 1.f - s;
				px = (w0 + t) * std::cos(a);
				py = (w0 - t) * std::sin(a);
				float sang = sector * 2.f * a;
				float cs = std::cos(sang), sn = std::sin(sang);
				float rx = cs * px - sn * py;
				float ry = sn * px + cs * py;
				px = rx; py = ry;
				float areaPoly = 0.5f * n * std::sin(ORACLE_TWO_PI / n);
				float rescale = std::sqrt(ORACLE_PI / areaPoly);
				px *= rescale; py *= rescale;
			}
			py *= (1.f - ratio);
			float cr = std::cos(rotRad), sr = std::sin(rotRad);
			outX = cr * px - sr * py;
			outY = sr * px + cr * py;
		}

		// R6: espejo de FaceCulled del kernel. ng del winding ALMACENADO (sin
		// flip). CULL_BACK descarta dot(ng,dir) > 0; CULL_FRONT descarta < 0.
		inline BoolType OFaceCulled(uint32_t flags, OVec p0, OVec p1, OVec p2, OVec dir)
		{
			uint32_t cf = flags & (CinemaKernelM0::MATF_CULL_BACK | CinemaKernelM0::MATF_CULL_FRONT);
			if (cf == 0)
				return false;
			float sd = ODot(OCross(OSub(p1, p0), OSub(p2, p0)), dir);
			if ((cf & CinemaKernelM0::MATF_CULL_BACK) && sd > 0.f)
				return true;
			if ((cf & CinemaKernelM0::MATF_CULL_FRONT) && sd < 0.f)
				return true;
			return false;
		}

		// Espejo de SkyRadiance del kernel
		inline OVec SkyRadiance(const OracleFrame& fr, OVec dir)
		{
			float up = dir.z;
			OVec grad;
			if (up >= 0.f)
				grad = OLerp(fr.skyHorizon, fr.skyZenith, OClamp01(up));
			else
				grad = OLerp(fr.skyHorizon, fr.ambient, OClamp01(-up));
			return OLerp(grad, OScale(grad, 0.05f), OClamp01(fr.nightAlpha));
		}

		// R12: espejo 1:1 de FogFactor / FogColorFor / FogMix del kernel
		// (CinemaKernelM0.hpp). Formula exacta del raster shader_high_fog.fsh y
		// del inscatter bicolor de shader_high_fog_apply.fsh. lerp(0.55, 1, h)
		// se escribe como 0.55 + 0.45*h (el lerp del kernel compila a eso).
		inline float OFogFactor(const OracleFrame& fr, float dist, float worldZ)
		{
			float sigma = 1.6f / std::max(fr.fogDistance, 1.f);
			float clearZone = fr.fogDistance * 0.04f;
			float dens = 1.f - std::exp(-std::max(dist - clearZone, 0.f) * sigma);
			float hfall = std::exp(-std::max(worldZ - fr.fogHeight, 0.f) / std::max(fr.fogSize, 1.f));
			return OClamp01(dens * (0.55f + 0.45f * hfall));
		}

		inline OVec OFogColorFor(const OracleFrame& fr, OVec viewDir)
		{
			float m = std::pow(OClamp01(ODot(viewDir, fr.sunDir)), 8.f);
			return OLerp(fr.fogColor, fr.fogColorSun, OClamp01(m * 0.7f));
		}

		inline OVec OFogMix(const OracleFrame& fr, OVec radiance, OVec fogCol, float fog)
		{
			float g = fr.fogMixGamma;
			OVec c = OMake(std::max(radiance.x, 0.f), std::max(radiance.y, 0.f), std::max(radiance.z, 0.f));
			OVec f = OMake(std::max(fogCol.x, 0.f), std::max(fogCol.y, 0.f), std::max(fogCol.z, 0.f));
			if (g <= 0.f)
				return OLerp(c, f, fog);
			float invG = 1.f / g;
			OVec cd = OMake(std::pow(c.x, invG), std::pow(c.y, invG), std::pow(c.z, invG));
			OVec fd = OMake(std::pow(f.x, invG), std::pow(f.y, invG), std::pow(f.z, invG));
			OVec d = OLerp(cd, fd, fog);
			return OMake(std::pow(std::max(d.x, 0.f), g), std::pow(std::max(d.y, 0.f), g),
				std::pow(std::max(d.z, 0.f), g));
		}

		inline OVec ClampIndirect(const OracleFrame& fr, OVec c)
		{
			if (fr.clampInd <= 0.f)
				return c;
			return OMake(std::min(c.x, fr.clampInd), std::min(c.y, fr.clampInd), std::min(c.z, fr.clampInd));
		}

		// v' = M * v con el almacenamiento column-major del motor
		// (m[c*4+r]; misma convencion que Matrix::operator*(VecType) y que el
		// mul(invViewProj, v) del kernel)
		inline void MulMatVec(const float m[16], float x, float y, float z, float w, float out[4])
		{
			out[0] = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
			out[1] = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
			out[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
			out[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
		}

		// -----------------------------------------------------------------
		// Vista de escena CPU + lista NEE de "otra luz"
		// -----------------------------------------------------------------

		struct OracleScene
		{
			const PTVert* verts = nullptr;
			const PTTri* tris = nullptr;      // array REORDENADO (bvh.tris, orden GPU)
			uint32_t numTris = 0;
			const PTNode* nodes = nullptr;
			const PTMaterial* mats = nullptr;
			uint32_t numMats = 0;
			const QVector<QImage>* slices = nullptr;
			const std::vector<PTLight>* lights = nullptr;

			// Seleccion de "otra luz" (contrato en CinemaOracle.hpp): la CDF
			// emisiva es LA DE CinemaScene (la misma subida a GPU como emiCdf);
			// aqui solo se derivan pesos y mapas auxiliares.
			uint32_t numLights = 0;
			const std::vector<uint32_t>* emiTriIdx = nullptr; // indices en tris (orden GPU)
			const std::vector<float>* emiCdf = nullptr;       // acumulada SIN normalizar
			float emiPower = 0.f;    // wEmi del kernel: emiCdf.back() CRUDO (dominio de la CDF)
			float lightPower = 0.f;  // wLights del kernel: suma float de LightPowerWeight
			float pEmi = 0.f;        // wEmiEff / (wEmiEff + wLights + wEnvEff), wEmiEff = wEmi * escala * pi
			float pEnv = 0.f;        // F5 (C1): wEnvEff / (wEmiEff + wLights + wEnvEff); 0 = OFF
			std::vector<float> emiArea;     // paralela a emiTriIdx (area mundo)
			std::vector<float> emiPickPdf;  // paralela: pEmi * EmiSlotProb(j)
			std::vector<uint32_t> triToEmi; // tri (orden GPU) -> j en emiTriIdx, o ORACLE_INVALID
		};

		// Muestreo nearest del atlas CPU: mismo resultado que SampleLevel mip 0
		// point-clamp sobre la Texture2DArray SRGB (los slices QImage guardan
		// exactamente los bytes subidos; sRGB -> lineal con la formula exacta).
		inline float SrgbToLinearF(float c)
		{
			return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		ORand SampleAlbedo(const OracleScene& sc, const PTMaterial& mat, float rawU, float rawV)
		{
			ORand out;
			out.x = 1.f; out.y = 1.f; out.z = 1.f; out.w = 1.f; // blanco si el atlas no esta

			float lu = rawU, lv = rawV;
			if (mat.flags & CinemaKernelM0::MATF_WRAP_UV)
			{
				lu = lu - std::floor(lu); // frac() de HLSL
				lv = lv - std::floor(lv);
			}
			float u = mat.uvRect[0] + lu * mat.uvRect[2];
			float v = mat.uvRect[1] + lv * mat.uvRect[3];

			if (!sc.slices || (int)mat.sliceAlbedo >= sc.slices->size())
				return out;
			const QImage& img = (*sc.slices)[(int)mat.sliceAlbedo];
			IntType w = img.width(), h = img.height();
			if (w <= 0 || h <= 0)
				return out;

			// Point sampling con address CLAMP: texel = floor(uv * dim),
			// acotado al rango valido
			IntType ix = (IntType)std::floor(u * (float)w);
			IntType iy = (IntType)std::floor(v * (float)h);
			ix = ix < 0 ? 0 : (ix >= w ? w - 1 : ix);
			iy = iy < 0 ? 0 : (iy >= h ? h - 1 : iy);

			const uchar* px = img.constScanLine((int)iy) + ix * 4; // RGBA8888
			out.x = SrgbToLinearF((float)px[0] / 255.f);
			out.y = SrgbToLinearF((float)px[1] / 255.f);
			out.z = SrgbToLinearF((float)px[2] / 255.f);
			out.w = (float)px[3] / 255.f; // el alpha de un formato sRGB es lineal
			return out;
		}

		// M2: fetch de DATOS (material/normal map) en bytes CRUDOS. Espejo de
		// SampleDataTexel del kernel: alli el atlas SRGB devuelve lineal y el
		// kernel lo re-codifica con la OETF exacta para recuperar el byte;
		// aqui el byte se lee directo del QImage (mismo valor modulo redondeo
		// float del roundtrip). rect = uvRectMat/uvRectNrm; mismo WRAP_UV y
		// nearest-clamp que el albedo.
		ORand SampleDataTexel(const OracleScene& sc, uint32_t matFlags,
			const float rect[4], uint32_t slice, float rawU, float rawV)
		{
			ORand out;
			out.x = 1.f; out.y = 1.f; out.z = 1.f; out.w = 1.f;

			float lu = rawU, lv = rawV;
			if (matFlags & CinemaKernelM0::MATF_WRAP_UV)
			{
				lu = lu - std::floor(lu);
				lv = lv - std::floor(lv);
			}
			float u = rect[0] + lu * rect[2];
			float v = rect[1] + lv * rect[3];

			if (!sc.slices || (int)slice >= sc.slices->size())
				return out;
			const QImage& img = (*sc.slices)[(int)slice];
			IntType w = img.width(), h = img.height();
			if (w <= 0 || h <= 0)
				return out;

			IntType ix = (IntType)std::floor(u * (float)w);
			IntType iy = (IntType)std::floor(v * (float)h);
			ix = ix < 0 ? 0 : (ix >= w ? w - 1 : ix);
			iy = iy < 0 ? 0 : (iy >= h ? h - 1 : iy);

			const uchar* px = img.constScanLine((int)iy) + ix * 4; // RGBA8888
			out.x = (float)px[0] / 255.f;
			out.y = (float)px[1] / 255.f;
			out.z = (float)px[2] / 255.f;
			out.w = (float)px[3] / 255.f;
			return out;
		}

		// UV cruda interpolada de un triangulo (espejo de TriRawUv)
		inline void TriRawUv(const OracleScene& sc, const PTTri& tri, float bu, float bv, float& outU, float& outV)
		{
			const PTVert& v0 = sc.verts[tri.i0];
			const PTVert& v1 = sc.verts[tri.i1];
			const PTVert& v2 = sc.verts[tri.i2];
			float bw = 1.f - bu - bv;
			outU = v0.u * bw + v1.u * bu + v2.u * bv;
			outV = v0.v * bw + v1.v * bu + v2.v * bv;
		}

		// -----------------------------------------------------------------
		// Interseccion (espejo de SafeInvDir / IntersectAabb / IntersectTri)
		// -----------------------------------------------------------------

		inline OVec SafeInvDir(OVec d)
		{
			OVec s = OMake(d.x >= 0.f ? 1.f : -1.f, d.y >= 0.f ? 1.f : -1.f, d.z >= 0.f ? 1.f : -1.f);
			return OMake(
				s.x / std::max(std::fabs(d.x), 1.0e-12f),
				s.y / std::max(std::fabs(d.y), 1.0e-12f),
				s.z / std::max(std::fabs(d.z), 1.0e-12f));
		}

		inline float IntersectAabb(const PTNode& n, OVec org, OVec invDir, float tMax)
		{
			float t0x = (n.bminx - org.x) * invDir.x;
			float t0y = (n.bminy - org.y) * invDir.y;
			float t0z = (n.bminz - org.z) * invDir.z;
			float t1x = (n.bmaxx - org.x) * invDir.x;
			float t1y = (n.bmaxy - org.y) * invDir.y;
			float t1z = (n.bmaxz - org.z) * invDir.z;
			float tnx = std::min(t0x, t1x), tny = std::min(t0y, t1y), tnz = std::min(t0z, t1z);
			float tfx = std::max(t0x, t1x), tfy = std::max(t0y, t1y), tfz = std::max(t0z, t1z);
			float tNear = std::max(std::max(tnx, tny), std::max(tnz, 0.f));
			float tFar = std::min(std::min(tfx, tfy), std::min(tfz, tMax));
			return (tNear <= tFar) ? tNear : ORACLE_T_MAX;
		}

		inline BoolType IntersectTri(OVec org, OVec dir, OVec p0, OVec p1, OVec p2,
			float tMin, float tMax, float& t, float& bu, float& bv)
		{
			t = 0.f; bu = 0.f; bv = 0.f;
			OVec e1 = OSub(p1, p0);
			OVec e2 = OSub(p2, p0);
			OVec pv = OCross(dir, e2);
			float det = ODot(e1, pv);
			if (std::fabs(det) < 1.0e-12f)
				return false;
			float invDet = 1.f / det;
			OVec tv = OSub(org, p0);
			bu = ODot(tv, pv) * invDet;
			if (bu < 0.f || bu > 1.f)
				return false;
			OVec qv = OCross(tv, e1);
			bv = ODot(dir, qv) * invDet;
			if (bv < 0.f || bu + bv > 1.f)
				return false;
			t = ODot(e2, qv) * invDet;
			return (t > tMin && t < tMax);
		}

		// -----------------------------------------------------------------
		// TraceClosest con alpha-test in-stack (espejo del kernel M0/M1)
		// -----------------------------------------------------------------

		struct OracleHit
		{
			float t = ORACLE_T_MAX;
			uint32_t tri = ORACLE_INVALID;
			float bu = 0.f, bv = 0.f;
		};

		// R15 (holdout, espejo 1:1 del kernel): isCameraRay marca el segmento de
		// CAMARA (primario + continuaciones rectas por vidrio fino). Con el, los
		// triangulos MATF_CAM_INVISIBLE se ignoran y el rayo CONTINUA, sin gastar
		// presupuesto de transparencia. TraceShadowColored NUNCA filtra.
		OracleHit TraceClosest(const OracleScene& sc, OVec org, OVec dir, float tMin, BoolType isCameraRay)
		{
			OracleHit best;
			OVec invDir = SafeInvDir(dir);
			uint32_t transpLeft = ORACLE_MAX_TRANSP;
			BoolType holdoutCrossed = false; // R15: espejo de PT_MAX_TRANSP_HOLDOUT
			uint32_t stack[PT_BVH_TRACE_STACK];
			uint32_t sp = 0;
			uint32_t node = 0;

			while (true)
			{
				const PTNode& n = sc.nodes[node];
				if (n.leftOrFirst & PT_NODE_LEAF_BIT)
				{
					uint32_t first = n.leftOrFirst & ~PT_NODE_LEAF_BIT;
					for (uint32_t i = 0; i < n.rightOrCount; i++)
					{
						uint32_t triIdx = first + i;
						const PTTri& tri = sc.tris[triIdx];
						OVec p0 = PtVertPos(sc.verts[tri.i0]);
						OVec p1 = PtVertPos(sc.verts[tri.i1]);
						OVec p2 = PtVertPos(sc.verts[tri.i2]);
						float t, bu, bv;
						if (IntersectTri(org, dir, p0, p1, p2, tMin, best.t, t, bu, bv))
						{
							const PTMaterial& mat = sc.mats[tri.matId];
							// R15 holdout (espejo del kernel): invisible SOLO al
							// rayo de camara; el rayo sigue sin gastar presupuesto.
							if (isCameraRay && (mat.flags & CinemaKernelM0::MATF_CAM_INVISIBLE))
							{
								if (!holdoutCrossed)
								{
									holdoutCrossed = true;
									transpLeft = ORACLE_MAX_TRANSP_HOLDOUT;
								}
								continue;
							}
							// R6: backface culling del raster (espejo del kernel);
							// cara culled -> el rayo sigue, sin gastar presupuesto.
							if (OFaceCulled(mat.flags, p0, p1, p2, dir))
								continue;
							BoolType accept = true;
							if ((mat.flags & CinemaKernelM0::MATF_ALPHA_TEST) && transpLeft > 0)
							{
								float u, v;
								TriRawUv(sc, tri, bu, bv, u, v);
								if (SampleAlbedo(sc, mat, u, v).w < 0.5f)
								{
									accept = false;
									transpLeft--;
								}
							}
							else if ((mat.flags & (CinemaKernelM0::MATF_ALPHA_BLEND | CinemaKernelM0::MATF_WATER))
								== CinemaKernelM0::MATF_ALPHA_BLEND && transpLeft > 0)
							{
								// Espejo del kernel: texel fantasma del overlay
								// (aEff ~ 0) -> se ignora el hit, sin velo Fresnel
								float u, v;
								TriRawUv(sc, tri, bu, bv, u, v);
								float aEff = SampleAlbedo(sc, mat, u, v).w * mat.blendA;
								if (aEff < 0.004f)
								{
									accept = false;
									transpLeft--;
								}
							}
							if (accept)
							{
								best.t = t;
								best.tri = triIdx;
								best.bu = bu;
								best.bv = bv;
							}
						}
					}
					if (sp == 0)
						break;
					node = stack[--sp];
				}
				else
				{
					uint32_t c0 = n.leftOrFirst;
					uint32_t c1 = n.rightOrCount;
					float d0 = IntersectAabb(sc.nodes[c0], org, invDir, best.t);
					float d1 = IntersectAabb(sc.nodes[c1], org, invDir, best.t);
					if (d0 > d1)
					{
						float dtmp = d0; d0 = d1; d1 = dtmp;
						uint32_t ctmp = c0; c0 = c1; c1 = ctmp;
					}
					if (d0 == ORACLE_T_MAX)
					{
						if (sp == 0)
							break;
						node = stack[--sp];
					}
					else
					{
						node = c0;
						if (d1 < ORACLE_T_MAX && sp < PT_BVH_TRACE_STACK)
							stack[sp++] = c1;
					}
				}
			}

			return best;
		}

		// -----------------------------------------------------------------
		// TraceShadowColored (M1): transmitancia RGB del shadow ray. MISMAS
		// ramas que el TraceShadow float3 del kernel:
		//   ALPHA_TEST (con presupuesto): alpha < 0.5 -> pasa libre; texel
		//     solido -> OCLUYE (aunque el material tambien sea ALPHA_BLEND)
		//   si no, ALPHA_BLEND / WATER -> trans *= texel.rgb * blend.rgb
		//     * (1 - texel.a * blendA); early-out con la SUMA r+g+b < 1e-4
		//   resto -> opaco (0)
		// El producto es conmutativo: no hace falta orden near-first.
		// -----------------------------------------------------------------

		// F1 4a + RONDA 5 (fix del acantilado): oBlkMat = matId del triangulo que
		// corto el rayo, ORACLE_INVALID si no lo corto nadie. Espejo 1:1 del
		// kernel -- la explicacion larga (por que la IDENTIDAD del bloqueador y
		// no un booleano "es emisivo", y por que matId y no el slot de la CDF)
		// esta en CinemaKernelM0.hpp, sobre la variante RayQuery de TraceShadow.
		OVec TraceShadowColored(const OracleScene& sc, OVec org, OVec dir, float tMin, float tMax,
			uint32_t& oBlkMat)
		{
			oBlkMat = ORACLE_INVALID;
			OVec trans = OMake(1.f, 1.f, 1.f);
			if (tMax <= tMin)
				return trans;

			OVec invDir = SafeInvDir(dir);
			uint32_t transpLeft = ORACLE_MAX_TRANSP;
			uint32_t stack[PT_BVH_TRACE_STACK];
			uint32_t sp = 0;
			uint32_t node = 0;

			while (true)
			{
				const PTNode& n = sc.nodes[node];
				if (n.leftOrFirst & PT_NODE_LEAF_BIT)
				{
					uint32_t first = n.leftOrFirst & ~PT_NODE_LEAF_BIT;
					for (uint32_t i = 0; i < n.rightOrCount; i++)
					{
						const PTTri& tri = sc.tris[first + i];
						OVec p0 = PtVertPos(sc.verts[tri.i0]);
						OVec p1 = PtVertPos(sc.verts[tri.i1]);
						OVec p2 = PtVertPos(sc.verts[tri.i2]);
						float t, bu, bv;
						if (!IntersectTri(org, dir, p0, p1, p2, tMin, tMax, t, bu, bv))
							continue;

						const PTMaterial& mat = sc.mats[tri.matId];
						// R8: facing DESDE LA LUZ (-dir) en el pase de sombra
						// (espejo del kernel; shadow map = viewpoint luz).
						if (OFaceCulled(mat.flags, p0, p1, p2, OScale(dir, -1.f)))
							continue;
						if (mat.flags & CinemaKernelM0::MATF_NO_SHADOW)
							continue; // R4: nubes no proyectan sombra (espejo del kernel)
						if ((mat.flags & CinemaKernelM0::MATF_ALPHA_TEST) && transpLeft > 0)
						{
							float u, v;
							TriRawUv(sc, tri, bu, bv, u, v);
							if (SampleAlbedo(sc, mat, u, v).w < 0.5f)
							{
								transpLeft--;
								continue; // texel fantasma del cutout
							}
							// Texel solido de un cutout: OCLUYE (rama else-if
							// del kernel: no cae al tinte aunque el material
							// tambien sea ALPHA_BLEND)
						}
						else if ((mat.flags & (CinemaKernelM0::MATF_ALPHA_BLEND | CinemaKernelM0::MATF_WATER))
							&& transpLeft > 0)
						{
							// Sombra coloreada: el cristal/agua filtra
							float u, v;
							TriRawUv(sc, tri, bu, bv, u, v);
							ORand texel = SampleAlbedo(sc, mat, u, v);
							float aEff = OClamp01(texel.w * mat.blendA);
							// COBERTURA: (1-aEff) SIN tinte (espejo del kernel)
							if (mat.flags & CinemaKernelM0::MATF_COVERAGE)
								trans = OScale(trans, 1.f - aEff);
							else
							{
								OVec tint = OMake(texel.x * mat.blendR, texel.y * mat.blendG, texel.z * mat.blendB);
								trans = OMul(trans, OScale(tint, 1.f - aEff));
							}
							transpLeft--;
							if (trans.x + trans.y + trans.z < 1.0e-4f)
							{
								oBlkMat = tri.matId;
								return OMake(0.f, 0.f, 0.f); // filtro ya opaco
							}
							continue;
						}
						oBlkMat = tri.matId;
						return OMake(0.f, 0.f, 0.f); // ocluido opaco
					}
					if (sp == 0)
						break;
					node = stack[--sp];
				}
				else
				{
					uint32_t c0 = n.leftOrFirst;
					uint32_t c1 = n.rightOrCount;
					float d0 = IntersectAabb(sc.nodes[c0], org, invDir, tMax);
					float d1 = IntersectAabb(sc.nodes[c1], org, invDir, tMax);
					BoolType h0 = (d0 < ORACLE_T_MAX);
					BoolType h1 = (d1 < ORACLE_T_MAX);
					if (h0 && h1)
					{
						node = c0;
						if (sp < PT_BVH_TRACE_STACK)
							stack[sp++] = c1;
					}
					else if (h0)
						node = c0;
					else if (h1)
						node = c1;
					else
					{
						if (sp == 0)
							break;
						node = stack[--sp];
					}
				}
			}

			return trans;
		}

		// Sobrecarga historica: identica, sin el dato del F1 (espejo del kernel).
		OVec TraceShadowColored(const OracleScene& sc, OVec org, OVec dir, float tMin, float tMax)
		{
			uint32_t blkMatIgnored;
			return TraceShadowColored(sc, org, dir, tMin, tMax, blkMatIgnored);
		}

		// -----------------------------------------------------------------
		// Superficie M2 (espejo de SurfaceM2 / EvalSurface del kernel):
		// normal map, metallic/roughness (draw o material map SEUS/LabPBR) y
		// clasificacion de kind (agua > cristal > hoja > estandar). albedo
		// conserva la semantica M1 (emision + AOV).
		// -----------------------------------------------------------------

		struct OracleSurface
		{
			OVec pos;
			OVec ns;         // normal de shading (normal map aplicado; hacia el rayo)
			OVec ng;         // normal geometrica (volteada hacia el rayo)
			OVec albedo;     // texel * blend.rgb * colorVert
			OVec diffColor;  // kind 0: albedo * (1 - metallic); resto: albedo
			OVec f0;         // kind 0: lerp(0.04, albedo, metallic)
			OVec glassTint;  // kind 3: texel.rgb * blend.rgb * (1 - aEff)
			float glassAlpha = 0.f; // kind 3: aEff (cobertura del film)
			OVec waterSigma; // kind 2: sigma de Beer del blendColor
			float alphaG = 0.f;    // kind 0: GGX alpha = roughness^2 (floor)
			float emissive = 0.f;
			uint32_t kind = ORACLE_KIND_STD;
			float frontFace = 0.f; // 1 = cara frontal geometrica (pre-volteo)
		};

		OracleSurface EvalSurface(const OracleScene& sc, const OracleHit& hit, OVec rayDir)
		{
			const PTTri& tri = sc.tris[hit.tri];
			const PTVert& v0 = sc.verts[tri.i0];
			const PTVert& v1 = sc.verts[tri.i1];
			const PTVert& v2 = sc.verts[tri.i2];
			const PTMaterial& mat = sc.mats[tri.matId];

			float bw = 1.f - hit.bu - hit.bv;
			OVec p0 = PtVertPos(v0), p1 = PtVertPos(v1), p2 = PtVertPos(v2);

			OracleSurface s;
			s.pos = OAdd(OAdd(OScale(p0, bw), OScale(p1, hit.bu)), OScale(p2, hit.bv));

			s.ng = OCross(OSub(p1, p0), OSub(p2, p0));
			float ngLen = OLen(s.ng);
			s.ng = (ngLen > 1.0e-20f) ? OScale(s.ng, 1.f / ngLen) : OMake(0.f, 0.f, 1.f);

			// Cara frontal ANTES del volteo (decide entrar/salir del agua)
			s.frontFace = (ODot(s.ng, rayDir) <= 0.f) ? 1.f : 0.f;

			OVec nsRaw = OAdd(OAdd(
				OScale(DecodeNormal888(v0.normal888), bw),
				OScale(DecodeNormal888(v1.normal888), hit.bu)),
				OScale(DecodeNormal888(v2.normal888), hit.bv));
			float nsLen = OLen(nsRaw);
			s.ns = (nsLen > 1.0e-6f) ? OScale(nsRaw, 1.f / nsLen) : s.ng;

			if (ODot(s.ng, rayDir) > 0.f)
				s.ng = OScale(s.ng, -1.f);
			if (ODot(s.ns, rayDir) > 0.f)
				s.ns = OScale(s.ns, -1.f);

			float u, v;
			TriRawUv(sc, tri, hit.bu, hit.bv, u, v);
			ORand texel = SampleAlbedo(sc, mat, u, v);

			// Normal map (M2): espejo exacto del bloque del kernel
			if (mat.flags & CinemaKernelM0::MATF_HAS_NRMTEX)
			{
				ORand nrmTexel = SampleDataTexel(sc, mat.flags, mat.uvRectNrm, mat.sliceNrm, u, v);
				if (nrmTexel.w >= 0.01f)
				{
					OVec tRaw = OAdd(OAdd(
						OScale(DecodeNormal888(v0.tangent888), bw),
						OScale(DecodeNormal888(v1.tangent888), hit.bu)),
						OScale(DecodeNormal888(v2.tangent888), hit.bv));
					OVec tOrt = OSub(tRaw, OScale(s.ns, ODot(tRaw, s.ns)));
					float tLen = OLen(tOrt);
					if (tLen > 1.0e-6f)
					{
						tOrt = OScale(tOrt, 1.f / tLen);
						OVec bOrt = OCross(tOrt, s.ns);
						float nx = nrmTexel.x * 2.f - 1.f;
						float ny = nrmTexel.y * 2.f - 1.f;
						float nz = std::sqrt(std::max(0.f, 1.f - (nx * nx + ny * ny)));
						OVec nPer = ONormalize(OAdd(OAdd(
							OScale(tOrt, nx), OScale(bOrt, -ny)), OScale(s.ns, nz)));
						if (ODot(nPer, s.ng) > 0.f)
							s.ns = nPer;
					}
				}
			}

			OVec vertRgb = OAdd(OAdd(
				OScale(DecodeColorRgb(v0.colorABGR), bw),
				OScale(DecodeColorRgb(v1.colorABGR), hit.bu)),
				OScale(DecodeColorRgb(v2.colorABGR), hit.bv));

			s.albedo = OMul(OMake(texel.x * mat.blendR, texel.y * mat.blendG, texel.z * mat.blendB), vertRgb);

			float eVert = DecodeVertEmissive(v0.dataFlags) * bw
				+ DecodeVertEmissive(v1.dataFlags) * hit.bu
				+ DecodeVertEmissive(v2.dataFlags) * hit.bv;
			s.emissive = std::max(mat.emissive, eVert);

			// --- clasificacion de kind (WATER > ALPHA_BLEND > hoja > estandar) ---
			uint32_t leafBits = (v0.dataFlags | v1.dataFlags | v2.dataFlags) & 4u;
			s.glassTint = OMake(0.f, 0.f, 0.f);
			s.waterSigma = OMake(0.f, 0.f, 0.f);
			s.diffColor = s.albedo;
			s.f0 = OMake(ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC);
			s.alphaG = 0.f;

			if (mat.flags & CinemaKernelM0::MATF_WATER)
			{
				s.kind = ORACLE_KIND_WATER;
				// clamp(blend.rgb, PT_SIGMA_FLOOR, 1) del kernel
				float blr = std::min(std::max(mat.blendR, ORACLE_SIGMA_FLOOR), 1.f);
				float blg = std::min(std::max(mat.blendG, ORACLE_SIGMA_FLOOR), 1.f);
				float blb = std::min(std::max(mat.blendB, ORACLE_SIGMA_FLOOR), 1.f);
				s.waterSigma = OMake(
					-std::log(blr) / ORACLE_WATER_ABS_DIST,
					-std::log(blg) / ORACLE_WATER_ABS_DIST,
					-std::log(blb) / ORACLE_WATER_ABS_DIST);
			}
			else if ((mat.flags & (CinemaKernelM0::MATF_ALPHA_BLEND | CinemaKernelM0::MATF_COVERAGE))
				== (CinemaKernelM0::MATF_ALPHA_BLEND | CinemaKernelM0::MATF_COVERAGE)
				&& OClamp01(texel.w * mat.blendA) < 0.999f)
			{
				// COBERTURA (espejo EXACTO de la rama PT_KIND_COVER del kernel)
				s.kind = ORACLE_KIND_COVER;
				s.glassAlpha = OClamp01(texel.w * mat.blendA);
				float metallicC = mat.metallic;
				float roughnessC = mat.roughness;
				if (mat.flags & CinemaKernelM0::MATF_HAS_MATTEX)
				{
					ORand mtC = SampleDataTexel(sc, mat.flags, mat.uvRectMat, mat.sliceMat, u, v);
					if (mat.flags & CinemaKernelM0::MATF_MAT_LABPBR)
					{
						metallicC = (mtC.y > 0.898f) ? 1.f : 0.f;
						float smC = 1.f - mtC.x;
						roughnessC = smC * smC;
					}
					else
					{
						roughnessC = 1.f - mtC.x;
						metallicC = mtC.y;
					}
				}
				metallicC = OClamp01(metallicC);
				roughnessC = roughnessC < ORACLE_ROUGH_MIN ? ORACLE_ROUGH_MIN : (roughnessC > 1.f ? 1.f : roughnessC);
				s.diffColor = OScale(s.albedo, 1.f - metallicC);
				s.f0 = OLerp(OMake(ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC), s.albedo, metallicC);
				s.alphaG = roughnessC * roughnessC;
			}
			else if ((mat.flags & CinemaKernelM0::MATF_ALPHA_BLEND)
				&& OClamp01(texel.w * mat.blendA) < 0.999f)
			{
				// Texels DEPTH2 con aEff ~ 1 (marcos opacos) caen al kind
				// estandar, como el kernel
				s.kind = ORACLE_KIND_GLASS;
				float aEff = OClamp01(texel.w * mat.blendA);
				s.glassTint = OScale(OMake(texel.x * mat.blendR, texel.y * mat.blendG, texel.z * mat.blendB), 1.f - aEff);
				s.glassAlpha = aEff;
			}
			else if ((mat.flags & CinemaKernelM0::MATF_SUBSURFACE) || leafBits != 0)
			{
				s.kind = ORACLE_KIND_LEAF;
			}
			else
			{
				s.kind = ORACLE_KIND_STD;
				float metallic = mat.metallic;
				float roughness = mat.roughness;
				if (mat.flags & CinemaKernelM0::MATF_HAS_MATTEX)
				{
					ORand mt = SampleDataTexel(sc, mat.flags, mat.uvRectMat, mat.sliceMat, u, v);
					if (mat.flags & CinemaKernelM0::MATF_MAT_LABPBR)
					{
						metallic = (mt.y > 0.898f) ? 1.f : 0.f;
						float sm = 1.f - mt.x;
						roughness = sm * sm;
					}
					else
					{
						roughness = 1.f - mt.x;
						metallic = mt.y;
					}
				}
				metallic = OClamp01(metallic);
				roughness = roughness < ORACLE_ROUGH_MIN ? ORACLE_ROUGH_MIN : (roughness > 1.f ? 1.f : roughness);
				s.diffColor = OScale(s.albedo, 1.f - metallic);
				s.f0 = OLerp(OMake(ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC, ORACLE_F0_DIELECTRIC), s.albedo, metallic);
				s.alphaG = roughness * roughness;
			}

			return s;
		}

		// -----------------------------------------------------------------
		// M2: BSDF (portes EXACTOS de EvalBsdfCosPdf / NeeShadowOrg /
		// SampleBsdf del kernel; mismos guards y epsilons)
		// -----------------------------------------------------------------

		// fcos = f * |cos efectivo|; pdf = pdf de MEZCLA de SampleBsdf.
		// Solo kinds 0/1 (los delta no reciben NEE ni llegan aqui).
		void EvalBsdfCosPdf(const OracleSurface& s, OVec wo, OVec wi, OVec& fcos, float& pdf)
		{
			fcos = OMake(0.f, 0.f, 0.f);
			pdf = 0.f;

			if (s.kind == ORACLE_KIND_LEAF)
			{
				float cosR = ODot(s.ns, wi);
				if (cosR > 0.f)
				{
					fcos = OScale(s.albedo, (1.f - ORACLE_LEAF_TRANSMIT) * cosR / ORACLE_PI);
					pdf = (1.f - ORACLE_LEAF_TRANSMIT) * cosR / ORACLE_PI;
				}
				else if (cosR < 0.f)
				{
					fcos = OScale(s.albedo, ORACLE_LEAF_TRANSMIT * (-cosR) / ORACLE_PI);
					pdf = ORACLE_LEAF_TRANSMIT * (-cosR) / ORACLE_PI;
				}
				return;
			}

			// kind 0: Lambert difuso + GGX sobre el hemisferio de ns
			float cosO = ODot(s.ns, wo);
			float cosI = ODot(s.ns, wi);
			if (cosO <= 1.0e-6f || cosI <= 1.0e-6f)
				return;

			float wSpec = OLum(SchlickF3(s.f0, cosO));
			float wDiff = OLum(s.diffColor);
			float wSum = wSpec + wDiff;
			if (wSum <= 0.f)
				return;
			float pSpec = wSpec / wSum;

			// Lobulo difuso
			fcos = OScale(s.diffColor, cosI / ORACLE_PI);
			pdf = (1.f - pSpec) * (cosI / ORACLE_PI);

			// Lobulo GGX single-scatter (f*cos = F * D * G2 / (4 * cosO))
			OVec h = OAdd(wo, wi);
			float hLen = OLen(h);
			if (hLen > 1.0e-8f)
			{
				h = OScale(h, 1.f / hLen);
				float cosNH = ODot(s.ns, h);
				float cosOH = ODot(wo, h);
				if (cosNH > 0.f && cosOH > 0.f)
				{
					float d = GgxD(cosNH, s.alphaG);
					float lo = GgxLambda(cosO, s.alphaG);
					float li = GgxLambda(cosI, s.alphaG);
					float g2 = 1.f / (1.f + lo + li);
					float g1 = 1.f / (1.f + lo);
					OVec fr = SchlickF3(s.f0, cosOH);
					float specScale = d * g2 / std::max(4.f * cosO, 1.0e-8f);
					fcos = OAdd(fcos, OScale(fr, specScale));
					pdf += pSpec * (g1 * d / std::max(4.f * cosO, 1.0e-8f));
				}
			}

			// COBERTURA: espejo del kernel -- la parte no-delta se escala por
			// aEff en fcos y en pdf (el peso fcos/pdf queda intacto).
			if (s.kind == ORACLE_KIND_COVER)
			{
				fcos = OScale(fcos, s.glassAlpha);
				pdf *= s.glassAlpha;
			}
		}

		// Origen del shadow ray de NEE (hojas retroiluminadas saltan de lado)
		inline OVec NeeShadowOrg(const OracleSurface& s, float offs, OVec wi)
		{
			float sgn = (s.kind == ORACLE_KIND_LEAF && ODot(wi, s.ng) < 0.f) ? -1.f : 1.f;
			return OAdd(s.pos, OScale(s.ng, offs * sgn));
		}

		// Muestreo del BSDF M2. rHemiZ/rHemiW = .zw de la dim k=0; rLobe =
		// dim k=3. Devuelve false si el camino muere.
		BoolType SampleBsdf(const OracleSurface& s, OVec wo,
			float rHemiZ, float rHemiW, const ORand& rLobe,
			OVec& wi, OVec& weight, float& pdfOut, BoolType& isDelta, BoolType& refracted)
		{
			wi = OMake(0.f, 0.f, 1.f);
			weight = OMake(0.f, 0.f, 0.f);
			pdfOut = 0.f;
			isDelta = false;
			refracted = false;

			if (s.kind == ORACLE_KIND_WATER)
			{
				isDelta = true;
				OVec t, b;
				BuildOnb(s.ns, t, b);
				float aG = ORACLE_WATER_ROUGH * ORACLE_WATER_ROUGH;
				OVec woL = OMake(ODot(wo, t), ODot(wo, b), ODot(wo, s.ns));
				OVec m = s.ns;
				if (woL.z > 1.0e-6f)
				{
					OVec hL = SampleGgxVndf(woL, aG, rLobe.y, rLobe.z);
					m = OAdd(OAdd(OScale(t, hL.x), OScale(b, hL.y)), OScale(s.ns, hL.z));
				}
				float cosI = ODot(wo, m);
				if (cosI <= 1.0e-6f)
				{
					m = s.ns;
					cosI = std::max(ODot(wo, m), 1.0e-6f);
				}
				float etaRel = (s.frontFace > 0.5f) ? (1.f / ORACLE_WATER_IOR) : ORACLE_WATER_IOR;
				float sin2T = etaRel * etaRel * std::max(0.f, 1.f - cosI * cosI);
				float fres = 1.f; // TIR
				float cosT = 0.f;
				if (sin2T < 1.f)
				{
					cosT = std::sqrt(1.f - sin2T);
					fres = FresnelDielectric(cosI, cosT, etaRel);
				}
				if (rLobe.x < fres)
				{
					wi = ONormalize(OSub(OScale(m, 2.f * cosI), wo));
					if (ODot(wi, s.ng) <= 0.f)
						wi = ONormalize(OSub(OScale(s.ns, 2.f * ODot(wo, s.ns)), wo));
					if (ODot(wi, s.ng) <= 0.f)
						return false;
				}
				else
				{
					wi = ONormalize(OSub(OScale(m, etaRel * cosI - cosT), OScale(wo, etaRel)));
					if (ODot(wi, s.ng) >= 0.f)
					{
						float cosI2 = std::max(ODot(wo, s.ns), 1.0e-6f);
						float sin2T2 = etaRel * etaRel * std::max(0.f, 1.f - cosI2 * cosI2);
						if (sin2T2 >= 1.f)
							return false;
						float cosT2 = std::sqrt(1.f - sin2T2);
						wi = ONormalize(OSub(OScale(s.ns, etaRel * cosI2 - cosT2), OScale(wo, etaRel)));
						if (ODot(wi, s.ng) >= 0.f)
							return false;
					}
					refracted = true;
				}
				weight = OMake(1.f, 1.f, 1.f);
				return true;
			}

			if (s.kind == ORACLE_KIND_GLASS)
			{
				isDelta = true;
				float cosI = std::max(ODot(s.ns, wo), 0.f);
				float p = OClamp01(1.f - cosI);
				float p2 = p * p;
				float fres = ORACLE_GLASS_F0 + (1.f - ORACLE_GLASS_F0) * (p2 * p2 * p);
				if (rLobe.x < fres)
				{
					wi = ONormalize(OSub(OScale(s.ns, 2.f * ODot(wo, s.ns)), wo));
					if (ODot(wi, s.ng) <= 0.f)
						return false;
					weight = OMake(1.f, 1.f, 1.f);
				}
				else
				{
					wi = OScale(wo, -1.f);
					weight = s.glassTint;
					if (weight.x + weight.y + weight.z <= 0.f)
						return false;
					refracted = true;
				}
				return true;
			}

			// COBERTURA: espejo EXACTO del kernel (misma dimension de RNG y
			// mismo reescalado; sin esto la paridad GPU/CPU se rompe).
			ORand rLobeC = rLobe;
			if (s.kind == ORACLE_KIND_COVER)
			{
				if (rLobeC.x >= s.glassAlpha)
				{
					isDelta = true;
					wi = OScale(wo, -1.f);
					weight = OMake(1.f, 1.f, 1.f);
					refracted = true;
					return true;
				}
				rLobeC.x = rLobeC.x / std::max(s.glassAlpha, 1.0e-6f);
			}

			// kinds 0/1: mezcla muestreable, peso = fcos/pdf
			if (s.kind == ORACLE_KIND_LEAF)
			{
				if (rLobeC.x < ORACLE_LEAF_TRANSMIT)
					wi = CosineSampleHemisphere(rHemiZ, rHemiW, OScale(s.ns, -1.f));
				else
					wi = CosineSampleHemisphere(rHemiZ, rHemiW, s.ns);
			}
			else
			{
				float cosO = ODot(s.ns, wo);
				if (cosO <= 1.0e-6f)
					return false;
				float wSpec = OLum(SchlickF3(s.f0, cosO));
				float wDiff = OLum(s.diffColor);
				float wSum = wSpec + wDiff;
				if (wSum <= 0.f)
					return false;
				float pSpec = wSpec / wSum;
				if (rLobeC.x < pSpec)
				{
					OVec t, b;
					BuildOnb(s.ns, t, b);
					OVec woL = OMake(ODot(wo, t), ODot(wo, b), cosO);
					OVec hL = SampleGgxVndf(woL, s.alphaG, rLobeC.y, rLobeC.z);
					OVec h = OAdd(OAdd(OScale(t, hL.x), OScale(b, hL.y)), OScale(s.ns, hL.z));
					wi = ONormalize(OSub(OScale(h, 2.f * ODot(wo, h)), wo));
				}
				else
					wi = CosineSampleHemisphere(rHemiZ, rHemiW, s.ns);
			}

			OVec fcos;
			float pdf;
			EvalBsdfCosPdf(s, wo, wi, fcos, pdf);
			if (pdf <= 1.0e-12f || fcos.x + fcos.y + fcos.z <= 0.f)
				return false;
			weight = OScale(fcos, 1.f / pdf);
			pdfOut = pdf;
			return true;
		}

		// -----------------------------------------------------------------
		// Falloff / cono / peso de potencia: portes EXACTOS de los helpers
		// M1 del kernel (LightFalloff / SpotMask / LightPowerWeight de
		// CinemaKernelM0.hpp, que a su vez replican el forward)
		// -----------------------------------------------------------------

		float LightFalloff(float dist, float range, float fadeSize)
		{
			float fade = fadeSize < 1.0e-4f ? 1.0e-4f : (fadeSize > 1.f ? 1.f : fadeSize);
			float r = std::max(range, 1.0e-4f);
			return 1.f - OClamp01((dist - r * (1.f - fade)) / (r * fade));
		}

		// cosA = coseno entre el eje del spot y la direccion luz -> punto.
		// cosOuter == -2 (marca point light del contrato) devuelve 1.
		float SpotMask(float cosA, float cosOuter, float sharpness)
		{
			if (cosOuter <= -1.5f)
				return 1.f; // point light
			if (cosA <= 1.0e-4f)
				return 0.f; // detras del plano del spot
			float sinA = std::sqrt(std::max(0.f, 1.f - cosA * cosA));
			float sinO = std::sqrt(std::max(0.f, 1.f - cosOuter * cosOuter));
			float tanHalf = std::max(sinO / std::max(cosOuter, 1.0e-4f), 1.0e-6f);
			float radial = 0.5f * (sinA / cosA) / tanHalf;
			return 1.f - OClamp01((radial - 0.5f * sharpness) / (0.5f * std::max(0.01f, 1.f - sharpness)));
		}

		// Peso de potencia para la seleccion proporcional de luces
		float LightPowerWeight(const PTLight& l)
		{
			return OLum(OMake(l.colR, l.colG, l.colB)) * std::max(l.range * l.range, 1.0e-4f);
		}

		// -----------------------------------------------------------------
		// Vista NEE de "otra luz": consume la CDF emisiva de CinemaScene (la
		// misma que se sube a GPU) y deriva el reparto por grupos del header.
		// -----------------------------------------------------------------

		void BuildNeeView(OracleScene& sc, float emissiveScale, const OracleFrame& fr)
		{
			const std::vector<PTLight>& lights = *sc.lights;
			sc.numLights = (uint32_t)lights.size();

			// wEmi / wLights / pEmi EXACTOS del CSPathTrace del kernel:
			// wEmi = ultimo elemento de la CDF CRUDA; el split de grupo usa la
			// potencia EFECTIVA wEmiEff = wEmi * emissiveScale * pi (la CDF
			// del host no incluye la escala ni el pi del flujo Lambert);
			// wLights = suma EN FLOAT y en orden de LightPowerWeight. pEmi
			// entra igual en la pdf de la NEE y en el peso MIS del hit: solo
			// cambia el reparto de muestras, jamas sesga.
			sc.emiPower = sc.emiCdf->empty() ? 0.f : std::max(sc.emiCdf->back(), 0.f);
			float wEmiEff = sc.emiPower * emissiveScale * ORACLE_PI;
			float wLights = 0.f;
			for (const PTLight& light : lights)
				wLights += LightPowerWeight(light);
			sc.lightPower = wLights;
			// F5 (C1): espejo EXACTO del bloque de CSPathTrace. envK viaja en
			// fogParams1.w; el domo apagado (luminancia 0) pesa 0 aunque envK > 0;
			// wEnvEff se suma AL FINAL para que con 0.0 exacto pEmi no mueva un ulp.
			float envKO = fr.envLight;
			OVec envAvgO = OScale(OAdd(OAdd(fr.skyZenith, fr.skyHorizon), fr.ambient), 1.f / 3.f);
			envAvgO = OLerp(envAvgO, OScale(envAvgO, 0.05f), OClamp01(fr.nightAlpha));
			float wEnvEff = 0.f;
			if (envKO > 0.f && OLum(envAvgO) > 0.f)
				wEnvEff = (wEmiEff + wLights > 0.f) ? (envKO * (wEmiEff + wLights)) : 1.f;
			float wOther = wEmiEff + wLights + wEnvEff;
			sc.pEmi = (wOther > 0.f) ? wEmiEff / wOther : 0.f;
			sc.pEnv = (wOther > 0.f) ? wEnvEff / wOther : 0.f;

			// Mapas auxiliares por tri emisor: area mundo (la misma cross que
			// EmissiveRadianceAt) y pdf de seleccion pEmi * EmiSlotProb
			size_t numEmi = sc.emiTriIdx->size();
			sc.triToEmi.assign(sc.numTris, ORACLE_INVALID);
			sc.emiArea.assign(numEmi, 0.f);
			sc.emiPickPdf.assign(numEmi, 0.f);
			for (size_t j = 0; j < numEmi; j++)
			{
				uint32_t triIdx = (*sc.emiTriIdx)[j];
				if (triIdx >= sc.numTris)
					continue;
				const PTTri& tri = sc.tris[triIdx];
				OVec p0 = PtVertPos(sc.verts[tri.i0]);
				OVec p1 = PtVertPos(sc.verts[tri.i1]);
				OVec p2 = PtVertPos(sc.verts[tri.i2]);
				sc.emiArea[j] = 0.5f * OLen(OCross(OSub(p1, p0), OSub(p2, p0)));
				float lo = j ? (*sc.emiCdf)[j - 1] : 0.f;
				float slotProb = std::max((*sc.emiCdf)[j] - lo, 0.f) / std::max(sc.emiPower, 1.0e-20f);
				sc.emiPickPdf[j] = sc.pEmi * slotProb;
				sc.triToEmi[triIdx] = (uint32_t)j;
			}
		}

		// Primer slot con cdf[slot] >= target (porte exacto de SampleEmiCdf
		// del kernel; CDF acumulada SIN normalizar, target en [0, W_emi))
		uint32_t SampleEmiCdf(const std::vector<float>& cdf, float target)
		{
			uint32_t lo = 0, hi = (uint32_t)cdf.size() - 1;
			while (lo < hi)
			{
				uint32_t mid = (lo + hi) >> 1;
				if (cdf[mid] >= target)
					hi = mid;
				else
					lo = mid + 1;
			}
			return lo;
		}

		// -----------------------------------------------------------------
		// NEE de "otra fuente": porte EXACTO del SampleOtherLight del kernel
		// M2 (CinemaKernelM0.hpp; mismos guards, epsilons y orden). Devuelve
		// la contribucion del BSDF COMPLETO (fcos de EvalBsdfCosPdf) SIN
		// throughput ni clamp. Solo kinds 0/1 (el caller gatea los delta).
		//   rl.x = seleccion de grupo y, remapeada, de fuente
		//   rl.yz = punto por area del tri emisor o cono hacia la esfera
		// selfTri: el tri que se esta sombreando (un emisor no recibe NEE
		// sobre si mismo).
		// -----------------------------------------------------------------

		// F1 4a: oUnshadowed = el estimador GEMELO (el MISMO termino con trans = 1).
		// Espejo 1:1 del out param del kernel.
		OVec SampleOtherLight(const OracleScene& sc, const OracleFrame& fr,
			const OracleSurface& s, OVec wo, float offs, uint32_t selfTri, const ORand& rl,
			BoolType envMisOn, OVec& oUnshadowed)
		{
			OVec zero = OMake(0.f, 0.f, 0.f);
			oUnshadowed = zero;

			// ----- grupo ENTORNO (F5 / C1), espejo 1:1 del kernel -----
			// Cola del intervalo [1-pEnv, 1): con pEnv = 0 el reparto es el de
			// siempre. Va ANTES del guard historico (una escena SOLO-DOMO no
			// tiene emisivos ni luces y ese guard mataria el unico grupo vivo).
			if (sc.pEnv > 0.f && rl.x >= 1.f - sc.pEnv)
			{
				OVec wiE = CosineSampleHemisphere(rl.y, rl.z, s.ns);
				float cosN = ODot(wiE, s.ns);
				if (cosN <= 1.0e-6f)
					return zero;
				OVec fcosE;
				float pdfBsdfE;
				EvalBsdfCosPdf(s, wo, wiE, fcosE, pdfBsdfE);
				if (fcosE.x + fcosE.y + fcosE.z <= 0.f)
					return zero;
				float pdfNeeE = sc.pEnv * cosN / ORACLE_PI;
				if (pdfNeeE <= 1.0e-20f)
					return zero;
				// Espejo del kernel: en el ULTIMO rebote el rayo de BSDF no se
				// traza, asi que la NEE de entorno se lleva el peso 1 entero.
				float misWE = envMisOn ? (pdfNeeE / (pdfNeeE + pdfBsdfE)) : 1.f;
				OVec leE = SkyRadiance(fr, wiE);
				if (leE.x + leE.y + leE.z <= 0.f)
					return zero;
				OVec baseV = OScale(OMul(leE, fcosE), misWE / pdfNeeE);
				OVec transE = TraceShadowColored(sc, NeeShadowOrg(s, offs, wiE), wiE, 0.f, ORACLE_T_MAX);
				// El domo no tiene geometria en el BVH: nunca hay auto-sombra.
				oUnshadowed = baseV;
				if (transE.x + transE.y + transE.z <= 0.f)
					return zero;
				return OScale(OMul(OMul(transE, leE), fcosE), misWE / pdfNeeE);
			}

			float wTotal = sc.emiPower + sc.lightPower;
			if (wTotal <= 0.f)
				return zero;

			if (rl.x < sc.pEmi)
			{
				// ----- grupo emisivos: busqueda binaria en la CDF -----
				float rIn = std::min(rl.x / std::max(sc.pEmi, 1.0e-20f), 0.99999994f);
				uint32_t slot = SampleEmiCdf(*sc.emiCdf, rIn * sc.emiPower);
				uint32_t triIdx = (*sc.emiTriIdx)[slot];
				if (triIdx == selfTri || triIdx >= sc.numTris)
					return zero;
				const PTTri& tri = sc.tris[triIdx];

				// Punto uniforme por area (mapeo sqrt estandar sobre rl.yz)
				float su = std::sqrt(std::max(rl.y, 0.f));
				float bu = 1.f - su;
				float bv = rl.z * su;
				float bw = 1.f - bu - bv;

				// EmissiveRadianceAt del kernel: posicion, normal geometrica
				// sin orientar, area y radiancia (cutout alpha < 0.5 = agujero)
				const PTVert& v0 = sc.verts[tri.i0];
				const PTVert& v1 = sc.verts[tri.i1];
				const PTVert& v2 = sc.verts[tri.i2];
				const PTMaterial& mat = sc.mats[tri.matId];
				OVec p0 = PtVertPos(v0), p1 = PtVertPos(v1), p2 = PtVertPos(v2);
				OVec lp = OAdd(OAdd(OScale(p0, bw), OScale(p1, bu)), OScale(p2, bv));

				OVec ngRaw = OCross(OSub(p1, p0), OSub(p2, p0));
				float ngLen = OLen(ngRaw);
				float area = 0.5f * ngLen;
				OVec lng = (ngLen > 1.0e-20f) ? OScale(ngRaw, 1.f / ngLen) : OMake(0.f, 0.f, 1.f);

				float eu = v0.u * bw + v1.u * bu + v2.u * bv;
				float ev = v0.v * bw + v1.v * bu + v2.v * bv;
				ORand texel = SampleAlbedo(sc, mat, eu, ev);
				OVec le = zero;
				if (!((mat.flags & CinemaKernelM0::MATF_ALPHA_TEST) && texel.w < 0.5f))
				{
					OVec vertRgb = OAdd(OAdd(
						OScale(DecodeColorRgb(v0.colorABGR), bw),
						OScale(DecodeColorRgb(v1.colorABGR), bu)),
						OScale(DecodeColorRgb(v2.colorABGR), bv));
					float eVert = DecodeVertEmissive(v0.dataFlags) * bw
						+ DecodeVertEmissive(v1.dataFlags) * bu
						+ DecodeVertEmissive(v2.dataFlags) * bv;
					float e = std::max(mat.emissive, eVert);
					le = OScale(OMul(OMake(texel.x * mat.blendR, texel.y * mat.blendG, texel.z * mat.blendB), vertRgb),
						e * fr.emissiveScale);
				}
				if (le.x + le.y + le.z <= 0.f || area <= 1.0e-12f)
					return zero;

				OVec toL = OSub(lp, s.pos);
				float dist2 = ODot(toL, toL);
				float dist = std::sqrt(dist2);
				if (dist <= 1.0e-4f)
					return zero;
				OVec wi = OScale(toL, 1.f / dist);
				float cosL = std::fabs(ODot(lng, wi)); // emision de dos caras
				OVec fcos;
				float pdfBsdf;
				EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdf);
				if (fcos.x + fcos.y + fcos.z <= 0.f || cosL <= 1.0e-6f)
					return zero;

				// pdf TOTAL de la estrategia NEE en angulo solido; el factor
				// pEmi * EmiSlotProb esta precomputado en emiPickPdf
				float pdfNee = sc.emiPickPdf[slot] * dist2 / std::max(area * cosL, 1.0e-12f);
				if (pdfNee <= 1.0e-20f)
					return zero;
				float misW = pdfNee / (pdfNee + pdfBsdf);

				OVec baseE = OScale(OMul(le, fcos), misW / pdfNee);
				uint32_t blkMatE;
				OVec trans = TraceShadowColored(sc, NeeShadowOrg(s, offs, wi), wi, 0.f,
					dist * 0.9999f, blkMatE);
				// F1 4a + RONDA 5 (fix del acantilado): es auto-sombra -- y por
				// tanto NO sombra -- SOLO si el que tapa es el MISMO objeto
				// emisivo que estoy muestreando (mismo matId = mismo draw). Un
				// emisivo AJENO si proyecta sombra (espejo del kernel).
				oUnshadowed = (blkMatE == tri.matId) ? OMul(trans, baseE) : baseE;
				if (trans.x + trans.y + trans.z <= 0.f)
					return zero;
				// BIT-IDENTIDAD: expresion EXACTA de siempre (espejo del kernel).
				return OScale(OMul(OMul(trans, le), fcos), misW / pdfNee);
			}

			// ----- grupo luces analiticas: seleccion lineal por potencia -----
			if (sc.numLights == 0 || sc.lightPower <= 0.f)
				return zero;
			// F5: pLit = 1 - pEmi - pEnv. Con pEnv = 0 es 1 - pEmi bit a bit.
			float pLit = 1.f - sc.pEmi - sc.pEnv;
			float rInL = std::min((rl.x - sc.pEmi) / std::max(pLit, 1.0e-20f), 0.99999994f);
			float target = rInL * sc.lightPower;
			float run = 0.f;
			uint32_t pick = sc.numLights - 1;
			for (uint32_t lj = 0; lj < sc.numLights; lj++)
			{
				run += LightPowerWeight((*sc.lights)[lj]);
				if (target < run)
				{
					pick = lj;
					break;
				}
			}
			const PTLight& light = (*sc.lights)[pick];
			float pSel = pLit * LightPowerWeight(light) / sc.lightPower;
			if (light.type == 2u)
			{
				float side = std::max(light.sizeRadius, 0.f);
				if (side <= 1.0e-6f)
					return zero;
				OVec ln = ONormalize(OMake(light.dirx, light.diry, light.dirz));
				OVec helper = std::abs(ln.y) < 0.999f ? OMake(0.f, 1.f, 0.f) : OMake(1.f, 0.f, 0.f);
				OVec tangent = ONormalize(OCross(helper, ln));
				OVec bitangent = OCross(ln, tangent);
				float halfSide = 0.5f * side;
				OVec lp = OAdd(OMake(light.posx, light.posy, light.posz),
					OAdd(OScale(tangent, (2.f * rl.y - 1.f) * halfSide),
						OScale(bitangent, (2.f * rl.z - 1.f) * halfSide)));
				OVec toL = OSub(lp, s.pos);
				float dist2 = ODot(toL, toL);
				if (dist2 <= 1.0e-8f)
					return zero;
				float dist = std::sqrt(dist2);
				OVec wi = OScale(toL, 1.f / dist);
				float cosL = std::max(ODot(ln, OScale(wi, -1.f)), 0.f);
				float att = LightFalloff(dist, light.range, light.fadeSize);
				if (cosL <= 0.f || att <= 0.f)
					return zero;
				OVec fcos;
				float pdfBsdfL;
				EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdfL);
				if (fcos.x + fcos.y + fcos.z <= 0.f)
					return zero;
				float geometry = cosL * (side * side) / std::max(dist2, 1.0e-8f);
				OVec lightCol = OMake(light.colR, light.colG, light.colB);
				OVec baseL = OScale(OMul(lightCol, fcos), (att * geometry) / std::max(pSel, 1.0e-20f));
				OVec trans = TraceShadowColored(sc, NeeShadowOrg(s, offs, wi), wi, 0.f,
					std::max(dist - ORACLE_T_EPS, ORACLE_T_EPS));
				oUnshadowed = baseL;
				if (trans.x + trans.y + trans.z <= 0.f)
					return zero;
				return OScale(OMul(OMul(trans, lightCol), fcos),
					(att * geometry) / std::max(pSel, 1.0e-20f));
			}

			OVec toC = OSub(OMake(light.posx, light.posy, light.posz), s.pos);
			float dist = OLen(toC);
			if (dist <= 1.0e-4f)
				return zero;
			OVec wiC = OScale(toC, 1.f / dist); // hacia el CENTRO de la luz

			// Falloff y cono del forward, medidos al centro (como el raster)
			float att = LightFalloff(dist, light.range, light.fadeSize);
			OVec axis = OMake(light.dirx, light.diry, light.dirz);
			float mask = SpotMask(ODot(axis, OScale(wiC, -1.f)), light.cosOuter, light.sharpness);
			if (att <= 0.f || mask <= 0.f)
				return zero;

			// Direccion de sombra: delta = al centro; size > 0 = cono hacia
			// la esfera (solo penumbra; la energia no cambia con el tamano)
			OVec wi = wiC;
			if (light.sizeRadius > 0.f && dist > light.sizeRadius)
			{
				float sinMax = OClamp01(light.sizeRadius / dist);
				float cosMax = std::sqrt(std::max(0.f, 1.f - sinMax * sinMax));
				wi = SampleCone(rl.y, rl.z, wiC, cosMax);
			}
			OVec fcos;
			float pdfBsdfL;
			EvalBsdfCosPdf(s, wo, wi, fcos, pdfBsdfL); // pdf sin uso: NEE pura
			if (fcos.x + fcos.y + fcos.z <= 0.f)
				return zero;

			float tMaxSh = std::max(dist - light.sizeRadius, ORACLE_T_EPS);
			// col * att * mask como irradiancia E; respuesta del BSDF
			// fcos * E; sin MIS (las luces no estan en el BVH)
			OVec lightCol = OMake(light.colR, light.colG, light.colB);
			OVec baseL = OScale(OMul(lightCol, fcos), (att * mask) / std::max(pSel, 1.0e-20f));
			OVec trans = TraceShadowColored(sc, NeeShadowOrg(s, offs, wi), wi, 0.f, tMaxSh);
			// F1 4a + RONDA 5 (fix del acantilado): una luz ANALITICA no tiene
			// geometria en el BVH -> NUNCA hay auto-sombra aqui (espejo del
			// kernel).
			oUnshadowed = baseL;
			if (trans.x + trans.y + trans.z <= 0.f)
				return zero;
			// BIT-IDENTIDAD: expresion EXACTA de siempre (espejo del kernel).
			return OScale(OMul(OMul(trans, lightCol), fcos),
				(att * mask) / std::max(pSel, 1.0e-20f));
		}

		// -----------------------------------------------------------------
		// Un sample de un pixel: integrador M2 completo (espejo 1:1 del
		// CSPathTrace del kernel)
		// -----------------------------------------------------------------

		void TracePathSample(const OracleScene& sc, const OracleFrame& fr,
			uint32_t px, uint32_t py, uint32_t sampleIdx, float out[4])
		{
			uint32_t ctr = sampleIdx + fr.seed * 0x9E3779B9u;

			// --- rayo primario (espejo del kernel) ---
			ORand rj = Rand4(px, py, ctr, CinemaOracleDims::DIM_CAMERA);
			float ndcX = (((float)px + rj.x) / fr.width) * 2.f - 1.f;
			float ndcY = 1.f - (((float)py + rj.y) / fr.height) * 2.f;

			float pNear[4], pFar[4];
			MulMatVec(fr.invViewProj, ndcX, ndcY, 0.f, 1.f, pNear);
			MulMatVec(fr.invViewProj, ndcX, ndcY, 1.f, 1.f, pFar);
			OVec nearW = OScale(OMake(pNear[0], pNear[1], pNear[2]), 1.f / pNear[3]);
			OVec farW = OScale(OMake(pFar[0], pFar[1], pFar[2]), 1.f / pFar[3]);
			OVec rayDir = ONormalize(OSub(farW, nearW));
			// R14 ORTHO (espejo EXACTO del kernel): la direccion ya sale
			// paralela de la inversa; lo que cambia es el ORIGEN, que pasa a
			// ser el punto del plano near de ESTE pixel.
			OVec rayOrg = fr.ortho ? nearW : fr.camPos;

			// R6/DOF-FIEL A1/A2 DoF (espejo EXACTO del kernel; rj.z/rj.w = dim 0,
			// sin dims extra). dofOn=0 o radio 0 => rayo pinhole identico.
			// A1: OLensApertureSample sustituye OLensDiskSample (blades<3 es
			// bit-identico). A2: el punto focal se calcula sobre el PLANO de
			// foco perpendicular a fr.camFwd, no sobre una esfera.
			float lensBiasT = 1.f;
			if (fr.dofOn && fr.apertureRadius > 0.f)
			{
				float ldx, ldy;
				OLensApertureSample(rj.z, rj.w, fr.blades, fr.lensRot, fr.lensRatio, ldx, ldy);
				ldx *= fr.apertureRadius;
				ldy *= fr.apertureRadius;
				OVec up0 = (std::fabs(rayDir.z) < 0.999f) ? OMake(0.f, 0.f, 1.f) : OMake(1.f, 0.f, 0.f);
				OVec lensR = ONormalize(OCross(up0, rayDir));
				OVec lensU = OCross(rayDir, lensR);
				float cosAx = ODot(rayDir, fr.camFwd);
				float ft = fr.focusDist / std::max(cosAx, 1.0e-4f);
				OVec focal = OAdd(rayOrg, OScale(rayDir, ft));
				rayOrg = OAdd(rayOrg, OAdd(OScale(lensR, ldx), OScale(lensU, ldy)));
				rayDir = ONormalize(OSub(focal, rayOrg));
				// A5: apodizacion "edge highlight" (espejo del kernel).
				if (fr.lensBias > 0.f)
				{
					float rr = OClamp01(std::sqrt(ldx * ldx + ldy * ldy) / std::max(fr.apertureRadius, 1.0e-6f));
					lensBiasT = 1.f + fr.lensBias * (rr - 0.5f);
				}
			}

			OVec color = OMake(0.f, 0.f, 0.f);
			OVec throughput = OMake(1.f, 1.f, 1.f);
			if (fr.lensBias > 0.f)
				throughput = OScale(throughput, lensBiasT);
			float alpha = 0.f;
				BoolType stillPrimary = true;
				float tCam = 0.f;   // CAPAS SLICING: distancia acumulada del segmento de camara

			// F1 4a SHADOW CATCHER (espejo 1:1 del kernel; ver el banner de
			// MATF_SHADOW_CATCHER en CinemaKernelM0.hpp). `color` pasa a ser el
			// NUMERADOR en cuanto catcherPath se enciende; catchDen es el
			// denominador montado con las MISMAS direcciones muestreadas.
			BoolType catcherPath = false;
			BoolType catcherSeg = false;
			OVec catchDen = OMake(0.f, 0.f, 0.f);
			float catchOpacity = 1.f;
			OVec catchSkyBehind = OMake(0.f, 0.f, 0.f);

			// R12 niebla (espejo del kernel). El oraculo nunca corre con film
			// alpha (fondo opaco), asi que el gate es solo fogShow.
			BoolType fogOn = fr.fogShow;
			float fogF = 0.f;
			OVec fogCol = OMake(0.f, 0.f, 0.f);

			float pdfSunNee = (fr.sunOmega > 1.0e-6f) ? 1.f / fr.sunOmega : 0.f;
			// pdf de MEZCLA del rayo de BSDF que genero el vertice actual; < 0
			// marca el rayo de camara. prevDelta marca rebote DELTA previo
			// (agua/cristal): emision y disco solar entran con peso 1.
			float prevPdfBsdf = -1.f;
			BoolType prevDelta = false;
			// F5 (C1, espejo del kernel): pdf de la NEE de entorno para la
			// direccion que genero el vertice actual (lado BSDF del MIS del domo).
			// 0 = ninguna NEE de entorno compitio -> el cielo entra con peso 1.
			float prevPdfEnvNee = 0.f;

			// Estado de medio del agua (Beer-Lambert), espejo del kernel
			BoolType inWater = false;
			OVec waterSigma = OMake(0.f, 0.f, 0.f);

			for (uint32_t bounce = 0; bounce < fr.maxBounces; bounce++)
			{
				// R15 (holdout): stillPrimary == "sigo siendo rayo de camara"
				// (espejo exacto del kernel).
				float tMinSeg = (bounce == 0) ? 0.f : ORACLE_T_EPS;
				if (fr.bandOn && stillPrimary && fr.bandT0 > 0.f)
					tMinSeg = std::max(tMinSeg, fr.bandT0 - tCam);
				OracleHit hit = TraceClosest(sc, rayOrg, rayDir, tMinSeg, stillPrimary);

				// CAPAS SLICING (espejo 1:1 del kernel): el primer hit no-
				// atravesado que cae mas alla de la ventana mata el sample. El
				// oraculo no tiene AOVs: solo muere el sample (break).
				if (fr.bandOn && stillPrimary && hit.tri != ORACLE_INVALID
					&& (tCam + hit.t) >= fr.bandT1)
					break;

				// --- Beer-Lambert del segmento recorrido DENTRO del agua ---
				if (inWater)
				{
					float segT = std::min((hit.tri == ORACLE_INVALID) ? ORACLE_SEG_MAX : hit.t, ORACLE_SEG_MAX);
					throughput = OMul(throughput, OMake(
						std::exp(-waterSigma.x * segT),
						std::exp(-waterSigma.y * segT),
						std::exp(-waterSigma.z * segT)));
				}

				if (hit.tri == ORACLE_INVALID)
				{
					// --- miss: cielo (peso 1) y, desde bounce 1, la mitad
					// BSDF del MIS del disco solar; tras un rebote delta el
					// disco entra con peso 1 (el disco NO se pinta en
					// primarios: el fondo del raster tampoco lo trae) ---
					OVec esc = SkyRadiance(fr, rayDir);
					// F5 (C1): la otra mitad del MIS del domo. Va ANTES del disco
					// solar (que tiene su propio MIS). Con el flag apagado
					// prevPdfEnvNee es 0 y esta rama no se ejecuta nunca.
					if (prevPdfEnvNee > 0.f && prevPdfBsdf > 0.f)
						esc = OScale(esc, prevPdfBsdf / (prevPdfBsdf + prevPdfEnvNee));
					if (bounce > 0)
					{
						if (pdfSunNee > 0.f && ODot(rayDir, fr.sunDir) >= fr.cosSunMax)
						{
							if (prevDelta)
								esc = OAdd(esc, fr.sunRadiance);
							else if (prevPdfBsdf > 0.f)
								esc = OAdd(esc, OScale(fr.sunRadiance,
									prevPdfBsdf / (prevPdfBsdf + pdfSunNee)));
						}
					}
					// ALPHA (espejo del kernel): un rayo que SIGUE SIENDO DE
					// CAMARA y se escapa esta viendo el FONDO, en el rebote
					// que sea -- el rebote 0 solo era el caso particular de
					// "no habia nada delante". El oraculo siempre corre con
					// fondo OPACO (sin film alpha): el fondo aporta color
					// (tenido por lo atravesado) y el pixel queda LLENO.
					// FONDO DE PELICULA (espejo del kernel): con el flag encendido
					// esta rama se comporta como el film transparente -- ni color
					// ni cobertura. `esc` (el cielo) sigue INTACTO para el
					// denominador del catcher y para los rayos que ya no son de
					// camara: el fondo NO ilumina.
					if (stillPrimary)
					{
						if (!fr.backdrop)
						{
							color = OAdd(color, (bounce == 0)
								? esc
								: ClampIndirect(fr, OMul(throughput, esc)));
							alpha = 1.f;
						}
					}
					else
						color = OAdd(color, ClampIndirect(fr, OMul(throughput, esc)));
					// F1 4a: sin ocluyentes el rayo habria hecho lo mismo -> el
					// denominador recibe el termino IDENTICO (sombra 0 exacta).
					if (catcherSeg)
					{
						catchDen = OAdd(catchDen, ClampIndirect(fr, OMul(throughput, esc)));
						catcherSeg = false;
					}
					break;
				}

				// R12 niebla: se resuelve en el PRIMER hit real (espejo exacto
				// del kernel; la traversal ya descarto ghost-texels/cutouts)
				if (fogOn && bounce == 0)
				{
					OVec hitPos = OAdd(rayOrg, OScale(rayDir, hit.t));
					fogF = OFogFactor(fr, hit.t, hitPos.z);
					fogCol = OFogColorFor(fr, rayDir);
				}

				// F1 MATTE (espejo 1:1 del kernel): el rayo de camara muere aqui.
				// El oraculo no tiene AOVs; el resto es identico. Con film OPACO
				// (unico modo del oraculo) el pixel acaba NEGRO por el alpha=1
				// forzado del final -- exactamente lo mismo que hace el kernel.
				if (stillPrimary && (sc.mats[sc.tris[hit.tri].matId].flags & CinemaKernelM0::MATF_CAM_MATTE))
					break;

				OracleSurface s = EvalSurface(sc, hit, rayDir);

				// Cobertura del film (espejo del kernel): se resuelve CAPA A
				// CAPA con OVER, no como un maximo sobre la cadena. COBERTURA
				// y VIDRIO deciden su aporte TRAS muestrear el lobo (abajo).
				// F1 4a SHADOW CATCHER (espejo 1:1 del kernel): solo en el vertice
				// primario del bounce 0, donde `color` vale exactamente 0.
				BoolType isCatcher = (bounce == 0) && stillPrimary
					&& ((sc.mats[sc.tris[hit.tri].matId].flags & CinemaKernelM0::MATF_SHADOW_CATCHER) != 0)
					&& (s.kind == ORACLE_KIND_STD || s.kind == ORACLE_KIND_LEAF
						|| s.kind == ORACLE_KIND_COVER);
				if (isCatcher)
				{
					catcherPath = true;
					catchOpacity = sc.mats[sc.tris[hit.tri].matId].shadowOpacity;
					catchSkyBehind = SkyRadiance(fr, rayDir);
					stillPrimary = false;
				}
				else if (stillPrimary)
				{
					if (s.kind != ORACLE_KIND_COVER && s.kind != ORACLE_KIND_GLASS)
					{
						alpha = 1.f;
						stillPrimary = false;
					}
				}

				// --- emision por hit de BSDF, con peso MIS balance si el tri
				// esta en la lista NEE (mismos gates que el kernel: primario,
				// rebote delta previo o tri fuera de lista = peso 1) ---
				if (s.emissive > 0.f)
				{
					float misW = 1.f;
					if (!prevDelta && prevPdfBsdf > 0.f && sc.pEmi > 0.f
						&& hit.tri < sc.triToEmi.size()
						&& sc.triToEmi[hit.tri] != ORACLE_INVALID)
					{
						uint32_t slot = sc.triToEmi[hit.tri];
						float area = sc.emiArea[slot];
						float cosL = std::fabs(ODot(s.ng, rayDir)); // ng hacia el rayo: abs da |cosL|
						float pdfNee = sc.emiPickPdf[slot] * (hit.t * hit.t)
							/ std::max(area * cosL, 1.0e-12f);
						misW = prevPdfBsdf / (prevPdfBsdf + pdfNee);
					}
					OVec le = OScale(OMul(throughput, s.albedo), s.emissive * fr.emissiveScale * misW);
					color = OAdd(color, (bounce == 0) ? le : ClampIndirect(fr, le));
					// F1 4a: una FUENTE no es un ocluyente -> mismo termino al
					// denominador (aporte de sombra 0).
					// RONDA 5 (SEGUNDO ACANTILADO): "es una FUENTE" ya no es
					// "emite algo" sino "emite AL MENOS lo que el domo abierto
					// daria en esa direccion". Criterio continuo, sin
					// acantilado, y bit-identico al de siempre cuando la fuente
					// es un softbox de verdad o cuando el cielo es negro.
					// Espejo del kernel (explicacion larga alli).
					OVec emiCruda = OScale(s.albedo, s.emissive * fr.emissiveScale);
					BoolType esFuente = (OLum(emiCruda) >= OLum(SkyRadiance(fr, rayDir)));
					if (catcherSeg && esFuente)
					{
						catchDen = OAdd(catchDen, ClampIndirect(fr, le));
						catcherSeg = false;
					}
				}
				// F1 4a: y si no era fuente, es OCLUYENTE: la referencia sin sujeto
				// para esa direccion es el domo analitico (cero rayos extra).
				if (catcherSeg)
				{
					// F5 (C1): el gemelo ve el domo con el MISMO peso MIS que el
					// numerador (espejo del kernel). Con el flag apagado la
					// expresion es la de siempre.
					OVec refEnv = OMul(throughput, SkyRadiance(fr, rayDir));
					if (prevPdfEnvNee > 0.f && prevPdfBsdf > 0.f)
						refEnv = OScale(refEnv, prevPdfBsdf / (prevPdfBsdf + prevPdfEnvNee));
					catchDen = OAdd(catchDen, ClampIndirect(fr, refEnv));
					catcherSeg = false;
				}

				// --- RNG del bounce (contrato de dims del header) ---
				uint32_t dimBase = CinemaOracleDims::DIM_BOUNCE_BASE
					+ CinemaOracleDims::DIM_BOUNCE_STRIDE * bounce;
				ORand rs = Rand4(px, py, ctr, dimBase + CinemaOracleDims::DIM_SUN_BSDF);
				ORand rLobe = Rand4(px, py, ctr, dimBase + CinemaOracleDims::DIM_LOBE);
				float offs = std::max(ORACLE_T_EPS, hit.t * 1.0e-4f);
				OVec wo = OScale(rayDir, -1.f);

				// --- NEE (solo kinds no-delta: agua/cristal no la reciben) ---
				if (s.kind == ORACLE_KIND_STD || s.kind == ORACLE_KIND_LEAF
				|| s.kind == ORACLE_KIND_COVER)
				{
					// --- NEE del sol: SIEMPRE 1 rayo al disco (splitting) ---
					OVec sunL;
					float sunWeight;
					float misSun = 1.f;
					if (fr.sunOmega > 1.0e-6f)
					{
						// Disco de area: pdf = 1/omega -> peso = omega
						sunL = SampleCone(rs.x, rs.y, fr.sunDir, fr.cosSunMax);
						sunWeight = fr.sunOmega;
					}
					else
					{
						// Sol delta: irradiancia preintegrada, sin MIS
						sunL = fr.sunDir;
						sunWeight = 1.f;
					}

					OVec fcosSun;
					float pdfSunB;
					EvalBsdfCosPdf(s, wo, sunL, fcosSun, pdfSunB);
					if (fcosSun.x + fcosSun.y + fcosSun.z > 0.f)
					{
						if (fr.sunOmega > 1.0e-6f)
							misSun = pdfSunNee / (pdfSunNee + pdfSunB);
						OVec cBase = OScale(OMul(OMul(throughput, fr.sunRadiance), fcosSun),
							sunWeight * misSun);
						OVec sunTrans = TraceShadowColored(sc, NeeShadowOrg(s, offs, sunL),
							sunL, 0.f, ORACLE_T_MAX);
						// F1 4a: el gemelo del sol, FUERA del gate de trans (la umbra
						// es justo donde el numerador no suma nada). RONDA 5 (fix del
						// acantilado): el sol NO tiene geometria en el BVH -> nunca se
						// tapa a si mismo (espejo del kernel).
						if (isCatcher)
							catchDen = OAdd(catchDen, cBase);
						if (sunTrans.x + sunTrans.y + sunTrans.z > 0.f)
						{
							// BIT-IDENTIDAD: expresion EXACTA de siempre.
							OVec c = OScale(OMul(OMul(OMul(throughput, sunTrans), fr.sunRadiance), fcosSun),
								sunWeight * misSun);
							color = OAdd(color, (bounce == 0) ? c : ClampIndirect(fr, c));
						}
					}

					// --- NEE de OTRA fuente: 1 de {emisivos CDF, point/spot}
					// por potencia (mismo gate que el kernel) ---
					if (sc.pEmi > 0.f || sc.lightPower > 0.f || sc.pEnv > 0.f)
					{
						ORand rl = Rand4(px, py, ctr, dimBase + CinemaOracleDims::DIM_LIGHT);
						OVec unsh;
						BoolType envMisOn = (bounce + 1) < fr.maxBounces;
						OVec cl = OMul(throughput, SampleOtherLight(sc, fr, s, wo, offs, hit.tri, rl, envMisOn, unsh));
						color = OAdd(color, (bounce == 0) ? cl : ClampIndirect(fr, cl));
						if (isCatcher)   // F1 4a: el gemelo de la otra fuente
							catchDen = OAdd(catchDen, OMul(throughput, unsh));
					}
				}

				// --- continuar el camino: muestreo del BSDF M2 ---
				OVec newDir, bsdfWeight;
				float pdfNew;
				BoolType isDelta, refracted;
				if (!SampleBsdf(s, wo, rs.z, rs.w, rLobe, newDir, bsdfWeight, pdfNew, isDelta, refracted))
					break;

				// Cobertura del film con el lobo ya muestreado (espejo del
				// kernel): interactuar con la capa llena el pixel; pasar de
				// largo conserva el primario (la COBERTURA no aporta nada --
				// su propio lobulo es el estimador; el VIDRIO aporta su aEff
				// ANALITICO con OVER, porque su transmision no es estocastica
				// en la cobertura).
				if (stillPrimary && (s.kind == ORACLE_KIND_GLASS || s.kind == ORACLE_KIND_COVER))
				{
					if (!refracted)
					{
						alpha = 1.f;
						stillPrimary = false;
					}
					else if (s.kind == ORACLE_KIND_GLASS)
						alpha = alpha + (1.f - alpha) * std::min(std::max(s.glassAlpha, 0.f), 1.f);
				}

				// Estado del medio: la refraccion del agua entra/sale segun la cara
				if (s.kind == ORACLE_KIND_WATER && refracted)
				{
					inWater = (s.frontFace > 0.5f);
					waterSigma = s.waterSigma;
				}

				throughput = OMul(throughput, bsdfWeight);

				// F1 4a: sale la continuacion del catcher; el PROXIMO vertice (o
				// miss) decide su termino del denominador, y solo ese.
				if (isCatcher)
					catcherSeg = true;

				// --- ruleta rusa desde el bounce 3 (dim RNG propia), tras el
				// peso del rebote, como el kernel ---
				if (bounce >= ORACLE_RR_START)
				{
					float pCont = OLum(throughput);
					pCont = pCont < ORACLE_RR_MIN ? ORACLE_RR_MIN : (pCont > ORACLE_RR_MAX ? ORACLE_RR_MAX : pCont);
					if (Rand4(px, py, ctr, dimBase + CinemaOracleDims::DIM_RR).x >= pCont)
						break;
					throughput = OScale(throughput, 1.f / pCont);
				}

				rayDir = newDir;
				prevPdfBsdf = isDelta ? -1.f : pdfNew;
				prevDelta = isDelta;
				// F5 (C1, espejo del kernel): pdf de la NEE de entorno para la
				// direccion recien muestreada. Solo si el kind recibe NEE y el
				// lobo no es delta; bajo s.ns (hoja) da 0 -> peso 1 al lado BSDF.
				prevPdfEnvNee = 0.f;
				if (sc.pEnv > 0.f && !isDelta
					&& (s.kind == ORACLE_KIND_STD || s.kind == ORACLE_KIND_LEAF
						|| s.kind == ORACLE_KIND_COVER))
					prevPdfEnvNee = sc.pEnv * std::max(ODot(newDir, s.ns), 0.f) / ORACLE_PI;
				// CAPAS SLICING: acumular la distancia recorrida por el segmento
				// de camara (espejo 1:1 del kernel).
				if (stillPrimary)
					tCam += hit.t;
				// Origen del rebote: al lado geometrico hacia el que sale el rayo
				float sgnOrg = (ODot(newDir, s.ng) >= 0.f) ? 1.f : -1.f;
				rayOrg = OAdd(s.pos, OScale(s.ng, offs * sgnOrg));
			}

			// F1 4a: RESOLVE DEL SHADOW CATCHER (espejo 1:1 del kernel). El
			// oraculo SIEMPRE corre con film OPACO, asi que el alpha acaba en 1 y
			// lo que compara cinema.parity es el COLOR: el fondo atenuado por la
			// sombra. Es decir, la paridad SI mide el cociente.
			if (catcherPath)
			{
				OVec num = OMake(std::max(color.x, 0.f), std::max(color.y, 0.f), std::max(color.z, 0.f));
				OVec den = OMake(std::max(catchDen.x, 0.f), std::max(catchDen.y, 0.f), std::max(catchDen.z, 0.f));
				float ar = (den.x > 1.0e-6f) ? OClamp01(1.f - num.x / den.x) : 0.f;
				float ag = (den.y > 1.0e-6f) ? OClamp01(1.f - num.y / den.y) : 0.f;
				float ab = (den.z > 1.0e-6f) ? OClamp01(1.f - num.z / den.z) : 0.f;
				alpha = OClamp01(std::max(ar, std::max(ag, ab)) * catchOpacity);
				// FONDO DE PELICULA (espejo del kernel): el catcher se queda en
				// holdout puro (color 0, alpha = sombra) tambien con film opaco.
				// El fondo lo pone el resolve con OVER en display.
				color = fr.backdrop ? OMake(0.f, 0.f, 0.f) : OScale(catchSkyBehind, 1.f - alpha);
			}

			// R12 niebla: out = mix(radiancia del camino, color de niebla, fog),
			// sobre la radiancia COMPLETA del sample (espejo del kernel)
			if (fogOn && fogF > 0.f)
				color = OFogMix(fr, color, fogCol, fogF);

			// INVARIANTE del render OPACO (espejo del kernel): el oraculo
			// SIEMPRE corre con fondo opaco, asi que el alpha es 1 sin
			// excepciones. Cubre los caminos que mueren todavia primarios
			// (SampleBsdf sin lobo valido, ruleta rusa, presupuesto de
			// rebotes agotado dentro de una pila de capas).
			// FONDO DE PELICULA (espejo del kernel): con el flag encendido el film
			// llega al resolve con la COBERTURA REAL, que es lo que necesita el
			// OVER contra el color del fondo -- y lo que la paridad compara.
			if (!fr.backdrop)
				alpha = 1.f;

			// Saneo NaN/inf por sample (criterio (e) de M0)
			if (!OFinite(color))
				color = OMake(0.f, 0.f, 0.f);
			if (!std::isfinite(alpha))
				alpha = 0.f;

			out[0] = color.x;
			out[1] = color.y;
			out[2] = color.z;
			out[3] = alpha;
		}

		// -----------------------------------------------------------------
		// Estado del ultimo render (para CinemaOracleCompare / LastImage)
		// -----------------------------------------------------------------

		std::vector<float> oracleImage;
		IntType oracleW = 0, oracleH = 0;
	}

	// ---------------------------------------------------------------------
	// API
	// ---------------------------------------------------------------------

	BoolType CinemaOracleTraceCameraRay(const float org[3], const float dir[3],
		float tMin, float tMax, float* outT, IntType* outTriIdx,
		float* outBu, float* outBv)
	{
		if (outT) *outT = 0.f;
		if (outTriIdx) *outTriIdx = -1;
		if (outBu) *outBu = 0.f;
		if (outBv) *outBv = 0.f;

		const CinemaBvh& bvh = CinemaSceneGetBvh();
		const std::vector<PTVert>& verts = CinemaSceneGetVerts();
		const std::vector<PTMaterial>& mats = CinemaSceneGetMaterials();
		const QVector<QImage>& slices = CinemaSceneGetAtlasSlices();
		if (!bvh.IsValid() || verts.empty() || bvh.tris.empty() || mats.empty())
			return false;

		// Vista MINIMA de escena: TraceClosest no usa luces ni CDF emisiva, asi
		// que NO se llama a BuildNeeView (seria trabajo tirado por rayo).
		OracleScene sc;
		sc.verts = verts.data();
		sc.tris = bvh.tris.data();
		sc.numTris = (uint32_t)bvh.tris.size();
		sc.nodes = bvh.nodes.data();
		sc.mats = mats.data();
		sc.numMats = (uint32_t)mats.size();
		sc.slices = &slices;

		OracleHit h = TraceClosest(sc, OMake(org[0], org[1], org[2]),
			OMake(dir[0], dir[1], dir[2]), tMin, true);
		if (h.tri == ORACLE_INVALID || !(h.t > 0.f) || h.t > tMax)
			return false;
		if (outT) *outT = h.t;
		if (outTriIdx) *outTriIdx = (IntType)h.tri;
		if (outBu) *outBu = h.bu;
		if (outBv) *outBv = h.bv;
		return true;
	}

	QString CinemaOracleRender(IntType width, IntType height, IntType spp,
		IntType seed, IntType bounces, RealType clampIndirect,
		RealType emissiveScale, float* outRgba)
	{
		if (width < 1 || height < 1 || width > ORACLE_MAX_DIM || height > ORACLE_MAX_DIM || spp < 1)
		{
			WARNING("Cinema oracle: invalid size/spp");
			return "invalid_arg";
		}

		const CinemaBvh& bvh = CinemaSceneGetBvh();
		const std::vector<PTVert>& verts = CinemaSceneGetVerts();
		const std::vector<PTMaterial>& mats = CinemaSceneGetMaterials();
		const QVector<QImage>& slices = CinemaSceneGetAtlasSlices();
		if (!bvh.IsValid() || verts.empty() || mats.empty() || slices.isEmpty())
		{
			WARNING("Cinema oracle: no built scene (run pt_build first)");
			return "scene_not_built";
		}

		const CinemaCameraState& cam = CinemaSceneCamera();
		if (!cam.valid)
		{
			WARNING("Cinema oracle: no valid camera");
			return "camera_not_set";
		}
		const CinemaEnvState& env = CinemaSceneEnv();
		if (!env.valid)
			WARNING("Cinema oracle: no environment set, sol y cielo negros");

		// ---- frame: espejo EXACTO de FillFrame de CinemaRender.cpp ----
		OracleFrame fr;
		memcpy(fr.invViewProj, cam.invViewProj, sizeof(fr.invViewProj));
		fr.camPos = OMake(cam.pos[0], cam.pos[1], cam.pos[2]);
		fr.ortho = cam.ortho; // R14 (espejo de FRAMEF_ORTHO en FillFrame)

		OVec sd = OMake(env.sunDir[0], env.sunDir[1], env.sunDir[2]);
		float sdLen = OLen(sd);
		// Division POR COMPONENTE como FillFrame (sd[c] /= len): x / len y
		// x * (1 / len) difieren en el ultimo ulp y este espejo es CPU vs CPU
		// (el cbuffer se llena en CPU), asi que puede y debe ser bit-exacto.
		if (sdLen > 1.0e-6f)
			sd = OMake(sd.x / sdLen, sd.y / sdLen, sd.z / sdLen);
		else
			sd = OMake(0.f, 0.f, 1.f);
		fr.sunDir = sd;
		// FIX-A (sun_softness): el disco solar ya no es una constante duplicada.
		// El oraculo lee los MISMOS campos derivados que FillFrame
		// (CinemaSceneEnvSet los calcula una vez, en doble): asi es imposible que
		// las dos formulas se desincronicen, que era el riesgo real para
		// cinema.parity. ORACLE_SUN_ANGLE_DEG queda como el mismo fallback que
		// CINEMA_SUN_ANGLE_DEG usa cuando no hubo pt_env_set.
		if (env.valid)
		{
			fr.cosSunMax = env.sunCosAng;
			fr.sunOmega = env.sunOmega;
		}
		else
		{
			fr.cosSunMax = std::cos(ORACLE_SUN_ANGLE_DEG * ORACLE_PI / 180.f);
			fr.sunOmega = ORACLE_TWO_PI * (1.f - fr.cosSunMax);
		}
		// FIX-A: mismo regimen delta que FillFrame (y que :1734 mas abajo): con
		// omega <= 1e-6 sunRadiance ES la irradiancia, no se divide.
		float omega = (fr.sunOmega > 1.0e-6f) ? fr.sunOmega : 1.f;
		fr.sunRadiance = OMake(
			env.sunRadiance[0] * ORACLE_SUN_IRRADIANCE_SCALE / omega,
			env.sunRadiance[1] * ORACLE_SUN_IRRADIANCE_SCALE / omega,
			env.sunRadiance[2] * ORACLE_SUN_IRRADIANCE_SCALE / omega);

		fr.skyZenith = OMake(env.skyZenith[0], env.skyZenith[1], env.skyZenith[2]);
		fr.skyHorizon = OMake(env.skyHorizon[0], env.skyHorizon[1], env.skyHorizon[2]);
		fr.ambient = OMake(env.ambient[0], env.ambient[1], env.ambient[2]);
		fr.nightAlpha = env.nightAlpha;
		// F5 (C1): espejo de frame.fogParams1[3] en FillFrame. Sin env valido, 0.
		fr.envLight = env.valid ? env.envLight : 0.f;
		// FONDO DE PELICULA: espejo del bit FRAMEF_BACKDROP que pone FillFrame.
		// El oraculo SIEMPRE corre con film OPACO (nunca hay alphaFilm), asi que
		// la condicion se reduce a env valido + ajuste encendido -- exactamente
		// lo que evalua CinemaBackdropActive(false) en el host. Sin este espejo,
		// cinema.parity se pondria ROJA en cuanto un proyecto con el fondo
		// encendido pasara por el arnes (el kernel haria holdout y el oraculo no).
		fr.backdrop = env.valid && env.backdrop > 0;

		IntType b = bounces < 1 ? 1 : (bounces > 8 ? 8 : bounces);
		fr.maxBounces = (uint32_t)b;
		fr.clampInd = clampIndirect > 0.0 ? (float)clampIndirect : 0.f;
		fr.emissiveScale = emissiveScale > 0.0 ? (float)emissiveScale : 0.f;
		fr.width = (float)width;
		fr.height = (float)height;
		fr.seed = (uint32_t)seed;

		// R6 DoF (espejo de FillFrame). cam.* ya saneado por CinemaSceneCameraSet.
		fr.dofOn = cam.dofOn;
		fr.focusDist = cam.focusDist;
		fr.apertureRadius = cam.apertureRadius;
		// DOF-FIEL A1/A5 (espejo de FillFrame/lensParams).
		fr.blades = cam.blades;
		fr.lensRot = cam.lensRot;
		fr.lensRatio = cam.lensRatio;
		fr.lensBias = cam.lensBias;
		// DOF-FIEL A2 (espejo de FillFrame/camFwd).
		fr.camFwd = OMake(cam.camFwd[0], cam.camFwd[1], cam.camFwd[2]);

		// CAPAS SLICING (espejo de FillFrame/bandParams)
		fr.bandOn = CinemaSceneBandWindowActive();
		fr.bandT0 = CinemaSceneBandWindowT0();
		fr.bandT1 = CinemaSceneBandWindowT1();   // <= 0 en el setter => +inf ya resuelto

		// R12 niebla (espejo de FillFrame). El oraculo no tiene film alpha, asi
		// que el gate es el fogShow del env tal cual.
		fr.fogShow = env.fogShow;
		fr.fogColor = OMake(env.fogColor[0], env.fogColor[1], env.fogColor[2]);
		fr.fogColorSun = OMake(env.fogColorSun[0], env.fogColorSun[1], env.fogColorSun[2]);
		fr.fogDistance = env.fogDistance;
		fr.fogSize = env.fogSize;
		fr.fogHeight = env.fogHeight;
		fr.fogMixGamma = CinemaSceneFogMixGamma();

		// ---- vista de escena + seleccion de otra luz (CDF de CinemaScene) ----
		OracleScene sc;
		sc.verts = verts.data();
		sc.tris = bvh.tris.data();
		sc.numTris = (uint32_t)bvh.tris.size();
		sc.nodes = bvh.nodes.data();
		sc.mats = mats.data();
		sc.numMats = (uint32_t)mats.size();
		sc.slices = &slices;
		sc.lights = &CinemaSceneLights();
		sc.emiTriIdx = &CinemaSceneGetEmissiveTris();
		sc.emiCdf = &CinemaSceneGetEmissiveCdf();
		BuildNeeView(sc, fr.emissiveScale, fr);

		// ---- render: OpenMP por filas; determinista (RNG counter-based) ----
		oracleImage.assign((size_t)width * height * 4, 0.f);
		oracleW = width;
		oracleH = height;
		float* img = oracleImage.data();
		float invSpp = 1.f / (float)spp;
		IntType numSamples = spp;

		#pragma OPENMP_FOR
		for (IntType y = 0; y < height; y++)
		{
			for (IntType x = 0; x < width; x++)
			{
				// Acumulacion secuencial en float, en orden de sample (mismo
				// orden de sumas que el acumulador GPU)
				float acc[4] = { 0.f, 0.f, 0.f, 0.f };
				for (IntType smp = 0; smp < numSamples; smp++)
				{
					float one[4];
					TracePathSample(sc, fr, (uint32_t)x, (uint32_t)y, (uint32_t)smp, one);
					acc[0] += one[0];
					acc[1] += one[1];
					acc[2] += one[2];
					acc[3] += one[3];
				}
				// NORMALIZACION DEL FILM: la MISMA que el resolve de produccion
				// (CinemaRenderFinish) y que el lado GPU del arnes. El color se
				// DES-PREMULTIPLICA dividiendo por la SUMA DE ALPHAS (con suelo
				// 1) y el alpha que sale es la COBERTURA MEDIA. Este oraculo
				// dividia los CUATRO canales por el numero de samples (film
				// PREMULTIPLICADO): identico mientras el alpha valia 1 por
				// sample, distinto en cuanto hay cobertura parcial en el film.
				// Ver la nota larga en CinemaRenderParity.
				float sumA = std::isfinite(acc[3]) ? acc[3] : 0.f;
				float weight = sumA > 1.f ? sumA : 1.f;
				float* dst = img + ((size_t)y * width + x) * 4;
				dst[0] = acc[0] / weight;
				dst[1] = acc[1] / weight;
				dst[2] = acc[2] / weight;
				dst[3] = sumA * invSpp;
			}
		}

		if (outRgba)
			memcpy(outRgba, oracleImage.data(), oracleImage.size() * sizeof(float));

		DEBUG("Cinema oracle: " + NumStr(width) + "x" + NumStr(height) + " @ " + NumStr(spp)
			+ " spp, NEE: " + NumStr((IntType)sc.numLights) + " luces + "
			+ NumStr((IntType)sc.emiTriIdx->size()) + " tris emisores (pEmi "
			+ NumStr((RealType)sc.pEmi, 3) + ")");
		return QString();
	}

	CinemaOracleCompareResult CinemaOracleCompare(const float* gpuRgba,
		IntType width, IntType height, RealType rmseThreshold)
	{
		CinemaOracleCompareResult res;
		if (!gpuRgba || width < 1 || height < 1
			|| width != oracleW || height != oracleH || oracleImage.empty())
		{
			WARNING("Cinema oracle compare: no matching oracle render");
			return res;
		}

		const float* oracle = oracleImage.data();
		size_t numPix = (size_t)width * height;
		double sumSq = 0.0;
		size_t badValues = 0;

		double maxDiff = 0.0;
		size_t maxIdx = 0, maxCh = 0;
		// LOS CUATRO CANALES, alpha incluido. Durante anos esto comparaba solo
		// RGB (c < 3) y el canal de COBERTURA quedaba fuera del gate: un canal
		// que no se compara es un canal donde puede vivir cualquier bug -- y de
		// hecho ahi vivio el de la normalizacion del film. El alpha de las dos
		// imagenes es la cobertura media (suma_alpha / spp) en ambos lados.
		for (size_t i = 0; i < numPix; i++)
		{
			const float* g = gpuRgba + i * 4;
			const float* o = oracle + i * 4;
			for (size_t c = 0; c < 4; c++)
			{
				double d = (double)g[c] - (double)o[c];
				if (!std::isfinite(d))
				{
					badValues++;
					continue;
				}
				sumSq += d * d;
				double ad = d < 0.0 ? -d : d;
				if (ad > maxDiff)
				{
					maxDiff = ad;
					maxIdx = i;
					maxCh = c;
				}
			}
		}
		res.maxDiff = (RealType)maxDiff;
		res.maxX = (IntType)(maxIdx % (size_t)width);
		res.maxY = (IntType)(maxIdx / (size_t)width);
		res.maxC = (IntType)maxCh;

		if (badValues)
		{
			WARNING("Cinema oracle compare: " + NumStr((IntType)badValues) + " non-finite values");
			res.rmse = 1.0e30;
			res.ok = false;
			return res;
		}

		res.rmse = std::sqrt(sumSq / ((double)numPix * 4.0));
		res.ok = (res.rmse <= rmseThreshold);
		return res;
	}

	const float* CinemaOracleLastImage(IntType* outWidth, IntType* outHeight)
	{
		if (outWidth)
			*outWidth = oracleW;
		if (outHeight)
			*outHeight = oracleH;
		return oracleImage.empty() ? nullptr : oracleImage.data();
	}
}
