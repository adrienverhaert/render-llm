#include "CinemaDenoise.hpp"

#include <QLibrary>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

// CinemaDenoise: implementacion (contrato en CinemaDenoise.hpp).
// OIDN 2.x CPU cargado en runtime + fallback a-trous propio.
//
// Politica de errores (misma que CinemaScene/CinemaGpu): nunca crashear.
// DLL/simbolo ausente -> WARNING una sola vez y degradacion.

namespace CppProject
{
	namespace
	{
		// -----------------------------------------------------------------
		// OIDN dinamico. NO se incluye ningun header de OIDN: los tipos y
		// constantes de la API C 2.x se replican aqui (contrato estable,
		// OpenImageDenoise/oidn.h). Handles opacos = punteros.
		// -----------------------------------------------------------------

		typedef void* OidnDevice;
		typedef void* OidnFilter;
		typedef void* OidnBuffer; // M7: handle opaco de buffer de device (OIDN 2.x)

		const int OIDN_DEVICE_TYPE_CPU_V = 1;  // OIDNDeviceType::OIDN_DEVICE_TYPE_CPU
		const int OIDN_DEVICE_TYPE_CUDA_V = 3; // OIDNDeviceType::OIDN_DEVICE_TYPE_CUDA (M7)
		const int OIDN_FORMAT_FLOAT3_V = 3;    // OIDNFormat::OIDN_FORMAT_FLOAT3
		const int OIDN_ERROR_NONE_V = 0;       // OIDNError::OIDN_ERROR_NONE

		// La API C de OIDN es cdecl (OIDN_API_CALL vacio en x64): punteros
		// a funcion planos. Firmas de OpenImageDenoise 2.x.
		typedef OidnDevice (*PfnOidnNewDevice)(int type);
		typedef void (*PfnOidnCommitDevice)(OidnDevice device);
		typedef int (*PfnOidnGetDeviceError)(OidnDevice device, const char** outMessage);
		typedef OidnFilter (*PfnOidnNewFilter)(OidnDevice device, const char* type);
		typedef void (*PfnOidnSetSharedFilterImage)(OidnFilter filter, const char* name,
			void* devPtr, int format, size_t width, size_t height,
			size_t byteOffset, size_t bytePixelStride, size_t byteRowStride);
		typedef void (*PfnOidnSetFilterBool)(OidnFilter filter, const char* name, bool value);
		typedef void (*PfnOidnCommitFilter)(OidnFilter filter);
		typedef void (*PfnOidnExecuteFilter)(OidnFilter filter);
		typedef void (*PfnOidnReleaseFilter)(OidnFilter filter);
		typedef void (*PfnOidnReleaseDevice)(OidnDevice device);
		// M7: device buffers (para devices GPU sin acceso coherente a memoria del
		// sistema, p.ej. CUDA en GPU discreta). En CPU se sigue usando el camino de
		// punteros compartidos (bit-identico al de siempre).
		typedef OidnBuffer (*PfnOidnNewBuffer)(OidnDevice device, size_t byteSize);
		typedef void (*PfnOidnReleaseBuffer)(OidnBuffer buffer);
		typedef void (*PfnOidnWriteBuffer)(OidnBuffer buffer, size_t byteOffset, size_t byteSize, const void* srcHost);
		typedef void (*PfnOidnReadBuffer)(OidnBuffer buffer, size_t byteOffset, size_t byteSize, void* dstHost);
		typedef void (*PfnOidnSetFilterImage)(OidnFilter filter, const char* name,
			OidnBuffer buffer, int format, size_t width, size_t height,
			size_t byteOffset, size_t bytePixelStride, size_t byteRowStride);
		typedef bool (*PfnOidnGetDeviceBool)(OidnDevice device, const char* name);

		struct OidnApi
		{
			PfnOidnNewDevice NewDevice = nullptr;
			PfnOidnCommitDevice CommitDevice = nullptr;
			PfnOidnGetDeviceError GetDeviceError = nullptr;
			PfnOidnNewFilter NewFilter = nullptr;
			PfnOidnSetSharedFilterImage SetSharedFilterImage = nullptr;
			PfnOidnSetFilterBool SetFilterBool = nullptr;
			PfnOidnCommitFilter CommitFilter = nullptr;
			PfnOidnExecuteFilter ExecuteFilter = nullptr;
			PfnOidnReleaseFilter ReleaseFilter = nullptr;
			PfnOidnReleaseDevice ReleaseDevice = nullptr;
			// M7 (device buffers): opcionales; su ausencia deshabilita el device GPU
			PfnOidnNewBuffer NewBuffer = nullptr;
			PfnOidnReleaseBuffer ReleaseBuffer = nullptr;
			PfnOidnWriteBuffer WriteBuffer = nullptr;
			PfnOidnReadBuffer ReadBuffer = nullptr;
			PfnOidnSetFilterImage SetFilterImage = nullptr;
			PfnOidnGetDeviceBool GetDeviceBool = nullptr;
		};

		OidnApi oidn;

		// Estado de carga: 0 = sin intentar, 1 = OK, -1 = fallo (el fallo
		// es permanente para la sesion: una DLL no aparece sola a mitad de
		// ejecucion y asi el WARNING sale UNA sola vez)
		IntType oidnLoadState = 0;

		// Device cacheado entre renders (crearlo y comitearlo carga los pesos de la
		// red: hacerlo una vez por sesion, no por frame). M7: puede ser CPU o CUDA.
		OidnDevice oidnDevice = nullptr;
		BoolType oidnUsesBuffers = false; // M7: device GPU sin memoria de sistema -> buffers de device
		QString oidnLabel = "cpu";        // etiqueta del device realmente en uso

		// Resuelve un simbolo o marca el fallo (con WARNING del nombre)
		template <typename T>
		BoolType OidnResolve(QLibrary& lib, T& fn, const char* name)
		{
			fn = (T)lib.resolve(name);
			if (!fn)
				WARNING(QString("OIDN: simbolo ") + name + " no encontrado en OpenImageDenoise.dll (denoise degrada a a-trous)");
			return fn != nullptr;
		}

		// Carga OpenImageDenoise.dll (junto al exe: LoadLibrary busca el
		// directorio de la aplicacion primero) y resuelve los ~10 simbolos
		// C. Un solo intento por sesion; WARNING una sola vez.
		BoolType OidnLoad()
		{
			if (oidnLoadState != 0)
				return oidnLoadState > 0;
			oidnLoadState = -1; // pesimista hasta acabar

			static QLibrary lib("OpenImageDenoise");
			if (!lib.load())
			{
				WARNING("OIDN: OpenImageDenoise.dll no disponible junto al exe (denoise degrada a a-trous): " + lib.errorString());
				return false;
			}

			BoolType ok = true;
			ok = OidnResolve(lib, oidn.NewDevice, "oidnNewDevice") && ok;
			ok = OidnResolve(lib, oidn.CommitDevice, "oidnCommitDevice") && ok;
			ok = OidnResolve(lib, oidn.GetDeviceError, "oidnGetDeviceError") && ok;
			ok = OidnResolve(lib, oidn.NewFilter, "oidnNewFilter") && ok;
			ok = OidnResolve(lib, oidn.SetSharedFilterImage, "oidnSetSharedFilterImage") && ok;
			ok = OidnResolve(lib, oidn.CommitFilter, "oidnCommitFilter") && ok;
			ok = OidnResolve(lib, oidn.ExecuteFilter, "oidnExecuteFilter") && ok;
			ok = OidnResolve(lib, oidn.ReleaseFilter, "oidnReleaseFilter") && ok;
			ok = OidnResolve(lib, oidn.ReleaseDevice, "oidnReleaseDevice") && ok;

			// hdr/cleanAux: en 2.x el export es oidnSetFilterBool; el nombre
			// 1.x oidnSetFilter1b se acepta como fallback por si el usuario
			// coloca una 1.4 (mismo prototipo)
			oidn.SetFilterBool = (PfnOidnSetFilterBool)lib.resolve("oidnSetFilterBool");
			if (!oidn.SetFilterBool)
				oidn.SetFilterBool = (PfnOidnSetFilterBool)lib.resolve("oidnSetFilter1b");
			if (!oidn.SetFilterBool)
			{
				WARNING("OIDN: ni oidnSetFilterBool ni oidnSetFilter1b exportados (denoise degrada a a-trous)");
				ok = false;
			}

			// M7: simbolos de device buffers (OPCIONALES). Sin ellos no se puede
			// alimentar un device GPU y el motor cae a CPU (punteros compartidos).
			oidn.NewBuffer = (PfnOidnNewBuffer)lib.resolve("oidnNewBuffer");
			oidn.ReleaseBuffer = (PfnOidnReleaseBuffer)lib.resolve("oidnReleaseBuffer");
			oidn.WriteBuffer = (PfnOidnWriteBuffer)lib.resolve("oidnWriteBuffer");
			oidn.ReadBuffer = (PfnOidnReadBuffer)lib.resolve("oidnReadBuffer");
			oidn.SetFilterImage = (PfnOidnSetFilterImage)lib.resolve("oidnSetFilterImage");
			oidn.GetDeviceBool = (PfnOidnGetDeviceBool)lib.resolve("oidnGetDeviceBool");

			if (!ok)
				return false;

			oidnLoadState = 1;
			DEBUG("OIDN: OpenImageDenoise.dll cargada, simbolos resueltos");
			return true;
		}

		// Crea y comitea el device OIDN (una vez). M7: selecciona el tipo por
		// MI3_OIDN_DEVICE = "cpu" | "cuda" (fuerza uno); ausente/otro = AUTO
		// (intenta CUDA con device buffers y cae LIMPIO a CPU). CUDA solo se intenta
		// si la DLL exporta los simbolos de buffer (device_cuda.dll vendorizada). Si
		// todo falla, WARNING una vez y fallo permanente de sesion (el caller degrada
		// a a-trous). Guarda oidnUsesBuffers (device GPU sin memoria de sistema) y
		// oidnLabel (cpu|cuda) para el status.
		BoolType OidnEnsureDevice()
		{
			if (oidnDevice)
				return true;
			if (!OidnLoad())
				return false;

			const char* envd = std::getenv("MI3_OIDN_DEVICE");
			BoolType forceCpu = envd && (!strcmp(envd, "cpu") || !strcmp(envd, "CPU"));
			BoolType forceCuda = envd && (!strcmp(envd, "cuda") || !strcmp(envd, "CUDA"));
			BoolType buffersOk = oidn.NewBuffer && oidn.ReleaseBuffer && oidn.WriteBuffer
				&& oidn.ReadBuffer && oidn.SetFilterImage;

			// Orden de intento: CUDA (si procede y hay buffers), luego CPU.
			int types[2];
			const char* labels[2];
			int nTries = 0;
			if (!forceCpu && buffersOk)
			{
				types[nTries] = OIDN_DEVICE_TYPE_CUDA_V;
				labels[nTries] = "cuda";
				nTries++;
			}
			if (!forceCuda)
			{
				types[nTries] = OIDN_DEVICE_TYPE_CPU_V;
				labels[nTries] = "cpu";
				nTries++;
			}
			if (nTries == 0)
			{
				WARNING("OIDN: MI3_OIDN_DEVICE=cuda pero la DLL no exporta los simbolos de buffer (denoise degrada a a-trous)");
				oidnLoadState = -1;
				return false;
			}

			for (int i = 0; i < nTries; i++)
			{
				OidnDevice dev = oidn.NewDevice(types[i]);
				if (!dev)
				{
					DEBUG(QString("OIDN: oidnNewDevice(") + labels[i] + ") devolvio null");
					continue;
				}
				oidn.CommitDevice(dev);
				const char* msg = nullptr;
				if (oidn.GetDeviceError(dev, &msg) != OIDN_ERROR_NONE_V)
				{
					DEBUG(QString("OIDN: commit del device ") + labels[i] + " fallo ("
						+ QString(msg ? msg : "sin mensaje") + ")");
					oidn.ReleaseDevice(dev);
					continue;
				}
				// Un device GPU sin memoria de sistema coherente necesita buffers de
				// device (CPU siempre usa punteros compartidos = camino de siempre).
				BoolType sysmem = (types[i] == OIDN_DEVICE_TYPE_CPU_V);
				if (types[i] != OIDN_DEVICE_TYPE_CPU_V && oidn.GetDeviceBool)
					sysmem = oidn.GetDeviceBool(dev, "systemMemorySupported");
				BoolType needBuffers = !sysmem;
				if (needBuffers && !buffersOk)
				{
					oidn.ReleaseDevice(dev); // defensivo (buffersOk ya gatea CUDA)
					continue;
				}
				oidnDevice = dev;
				oidnUsesBuffers = needBuffers;
				oidnLabel = labels[i];
				DEBUG("OIDN: device '" + oidnLabel + "' listo (buffers="
					+ QString::number(oidnUsesBuffers ? 1 : 0) + ")");
				return true;
			}

			WARNING("OIDN: ningun device (cuda/cpu) pudo crearse/comitear (faltan OpenImageDenoise_core/_device_*/tbb12 junto al exe?; denoise degrada a a-trous)");
			oidnLoadState = -1;
			return false;
		}

		// -----------------------------------------------------------------
		// Empaquetado RGBA plano -> float3 plano, con saneo (OIDN y el
		// a-trous asumen entradas finitas; un NaN colado envenena el filtro)
		// -----------------------------------------------------------------

		inline float SaneF(float v)
		{
			return std::isfinite(v) ? v : 0.f;
		}

		// clampLo/clampHi con hi < lo = sin clamp superior (color HDR)
		void PackFloat3(const float* rgba, size_t numPix, float lo, float hi, std::vector<float>& out)
		{
			out.resize(numPix * 3);
			for (size_t i = 0; i < numPix; i++)
			{
				for (size_t c = 0; c < 3; c++)
				{
					float v = SaneF(rgba[i * 4 + c]);
					if (v < lo)
						v = lo;
					if (hi > lo && v > hi)
						v = hi;
					out[i * 3 + c] = v;
				}
			}
		}

		// -----------------------------------------------------------------
		// Motor OIDN: receta canonica (diseno seccion 9)
		//   prefilter albedo (RT albedo-only, in-place)
		//   prefilter normal (RT normal-only, in-place)
		//   filtro principal color+albedo+normal, hdr=true, cleanAux=true
		// Sin aux (punteros null): filtro color-only hdr=true.
		// -----------------------------------------------------------------

		// Camino de PUNTEROS COMPARTIDOS (device CPU, o GPU con memoria de sistema):
		// byte a byte el de siempre. Asume oidnDevice ya creado (OidnRun lo garantiza).
		BoolType OidnRunShared(IntType w, IntType h, const float* colorRgba,
			const float* albedoRgba, const float* normalDepthRgba, float* outRgba)
		{
			size_t numPix = (size_t)w * (size_t)h;
			size_t sw = (size_t)w, sh = (size_t)h;

			// float3 planos: color HDR >= 0, albedo [0,1], normal [-1,1]
			std::vector<float> col, alb, nrm, outF(numPix * 3);
			PackFloat3(colorRgba, numPix, 0.f, -1.f, col);
			BoolType haveAux = albedoRgba != nullptr && normalDepthRgba != nullptr;
			if (haveAux)
			{
				PackFloat3(albedoRgba, numPix, 0.f, 1.f, alb);
				PackFloat3(normalDepthRgba, numPix, -1.f, 1.f, nrm);
			}

			// Prefilters de aux (in-place sobre alb/nrm): el filtro
			// principal referencia los MISMOS buffers, asi que ejecutarlos
			// antes deja las guias limpias que exige cleanAux
			OidnFilter fAlb = nullptr, fNrm = nullptr;
			if (haveAux)
			{
				fAlb = oidn.NewFilter(oidnDevice, "RT");
				fNrm = oidn.NewFilter(oidnDevice, "RT");
			}
			OidnFilter fMain = oidn.NewFilter(oidnDevice, "RT");
			if (!fMain || (haveAux && (!fAlb || !fNrm)))
			{
				WARNING("OIDN: oidnNewFilter(RT) fallo (denoise degrada a a-trous)");
				if (fAlb)
					oidn.ReleaseFilter(fAlb);
				if (fNrm)
					oidn.ReleaseFilter(fNrm);
				if (fMain)
					oidn.ReleaseFilter(fMain);
				return false;
			}

			// strides 0 = packed (los calcula OIDN del formato)
			if (haveAux)
			{
				oidn.SetSharedFilterImage(fAlb, "albedo", alb.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetSharedFilterImage(fAlb, "output", alb.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.CommitFilter(fAlb);

				oidn.SetSharedFilterImage(fNrm, "normal", nrm.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetSharedFilterImage(fNrm, "output", nrm.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.CommitFilter(fNrm);
			}

			oidn.SetSharedFilterImage(fMain, "color", col.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			if (haveAux)
			{
				oidn.SetSharedFilterImage(fMain, "albedo", alb.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetSharedFilterImage(fMain, "normal", nrm.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			}
			oidn.SetSharedFilterImage(fMain, "output", outF.data(), OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			oidn.SetFilterBool(fMain, "hdr", true);
			if (haveAux)
				oidn.SetFilterBool(fMain, "cleanAux", true);
			oidn.CommitFilter(fMain);

			if (fAlb)
				oidn.ExecuteFilter(fAlb);
			if (fNrm)
				oidn.ExecuteFilter(fNrm);
			oidn.ExecuteFilter(fMain);

			const char* msg = nullptr;
			BoolType ok = oidn.GetDeviceError(oidnDevice, &msg) == OIDN_ERROR_NONE_V;
			if (!ok)
				WARNING("OIDN: fallo en filtro (denoise degrada a a-trous): " + QString(msg ? msg : "sin mensaje"));

			if (fAlb)
				oidn.ReleaseFilter(fAlb);
			if (fNrm)
				oidn.ReleaseFilter(fNrm);
			oidn.ReleaseFilter(fMain);

			if (ok)
			{
				for (size_t i = 0; i < numPix; i++)
				{
					outRgba[i * 4 + 0] = SaneF(outF[i * 3 + 0]);
					outRgba[i * 4 + 1] = SaneF(outF[i * 3 + 1]);
					outRgba[i * 4 + 2] = SaneF(outF[i * 3 + 2]);
					outRgba[i * 4 + 3] = colorRgba[i * 4 + 3];
				}
			}
			return ok;
		}

		// Camino de DEVICE BUFFERS (device GPU, p.ej. CUDA): misma receta y filtros
		// que OidnRunShared, pero las imagenes viven en buffers de device (write host
		// -> device, execute en GPU, read device -> host). Cualquier fallo devuelve
		// false y el caller cae a CPU/a-trous. Asume oidnDevice ya creado.
		BoolType OidnRunBuffers(IntType w, IntType h, const float* colorRgba,
			const float* albedoRgba, const float* normalDepthRgba, float* outRgba)
		{
			size_t numPix = (size_t)w * (size_t)h;
			size_t sw = (size_t)w, sh = (size_t)h;
			size_t bytes3 = numPix * 3 * sizeof(float);

			std::vector<float> col, alb, nrm, outF(numPix * 3);
			PackFloat3(colorRgba, numPix, 0.f, -1.f, col);
			BoolType haveAux = albedoRgba != nullptr && normalDepthRgba != nullptr;
			if (haveAux)
			{
				PackFloat3(albedoRgba, numPix, 0.f, 1.f, alb);
				PackFloat3(normalDepthRgba, numPix, -1.f, 1.f, nrm);
			}

			OidnBuffer bCol = oidn.NewBuffer(oidnDevice, bytes3);
			OidnBuffer bOut = oidn.NewBuffer(oidnDevice, bytes3);
			OidnBuffer bAlb = haveAux ? oidn.NewBuffer(oidnDevice, bytes3) : nullptr;
			OidnBuffer bNrm = haveAux ? oidn.NewBuffer(oidnDevice, bytes3) : nullptr;
			auto releaseBufs = [&]()
			{
				if (bCol) oidn.ReleaseBuffer(bCol);
				if (bOut) oidn.ReleaseBuffer(bOut);
				if (bAlb) oidn.ReleaseBuffer(bAlb);
				if (bNrm) oidn.ReleaseBuffer(bNrm);
			};
			if (!bCol || !bOut || (haveAux && (!bAlb || !bNrm)))
			{
				WARNING("OIDN: oidnNewBuffer fallo (device " + oidnLabel + "; denoise degrada a a-trous)");
				releaseBufs();
				return false;
			}

			oidn.WriteBuffer(bCol, 0, bytes3, col.data());
			if (haveAux)
			{
				oidn.WriteBuffer(bAlb, 0, bytes3, alb.data());
				oidn.WriteBuffer(bNrm, 0, bytes3, nrm.data());
			}

			OidnFilter fAlb = nullptr, fNrm = nullptr;
			if (haveAux)
			{
				fAlb = oidn.NewFilter(oidnDevice, "RT");
				fNrm = oidn.NewFilter(oidnDevice, "RT");
			}
			OidnFilter fMain = oidn.NewFilter(oidnDevice, "RT");
			if (!fMain || (haveAux && (!fAlb || !fNrm)))
			{
				WARNING("OIDN: oidnNewFilter(RT) fallo (device " + oidnLabel + "; denoise degrada a a-trous)");
				if (fAlb) oidn.ReleaseFilter(fAlb);
				if (fNrm) oidn.ReleaseFilter(fNrm);
				if (fMain) oidn.ReleaseFilter(fMain);
				releaseBufs();
				return false;
			}

			// Prefilters de aux in-place (buffer entrada = buffer salida)
			if (haveAux)
			{
				oidn.SetFilterImage(fAlb, "albedo", bAlb, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetFilterImage(fAlb, "output", bAlb, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.CommitFilter(fAlb);
				oidn.SetFilterImage(fNrm, "normal", bNrm, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetFilterImage(fNrm, "output", bNrm, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.CommitFilter(fNrm);
			}
			oidn.SetFilterImage(fMain, "color", bCol, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			if (haveAux)
			{
				oidn.SetFilterImage(fMain, "albedo", bAlb, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
				oidn.SetFilterImage(fMain, "normal", bNrm, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			}
			oidn.SetFilterImage(fMain, "output", bOut, OIDN_FORMAT_FLOAT3_V, sw, sh, 0, 0, 0);
			oidn.SetFilterBool(fMain, "hdr", true);
			if (haveAux)
				oidn.SetFilterBool(fMain, "cleanAux", true);
			oidn.CommitFilter(fMain);

			if (fAlb) oidn.ExecuteFilter(fAlb);
			if (fNrm) oidn.ExecuteFilter(fNrm);
			oidn.ExecuteFilter(fMain);

			const char* msg = nullptr;
			BoolType ok = oidn.GetDeviceError(oidnDevice, &msg) == OIDN_ERROR_NONE_V;
			if (!ok)
				WARNING("OIDN: fallo en filtro (device " + oidnLabel + "; denoise degrada a a-trous): " + QString(msg ? msg : "sin mensaje"));
			if (ok)
				oidn.ReadBuffer(bOut, 0, bytes3, outF.data());

			if (fAlb) oidn.ReleaseFilter(fAlb);
			if (fNrm) oidn.ReleaseFilter(fNrm);
			oidn.ReleaseFilter(fMain);
			releaseBufs();

			if (ok)
			{
				for (size_t i = 0; i < numPix; i++)
				{
					outRgba[i * 4 + 0] = SaneF(outF[i * 3 + 0]);
					outRgba[i * 4 + 1] = SaneF(outF[i * 3 + 1]);
					outRgba[i * 4 + 2] = SaneF(outF[i * 3 + 2]);
					outRgba[i * 4 + 3] = colorRgba[i * 4 + 3];
				}
			}
			return ok;
		}

		// Dispatcher: asegura el device (CPU o CUDA segun MI3_OIDN_DEVICE) y elige el
		// camino (buffers para GPU sin memoria de sistema; punteros compartidos si no).
		BoolType OidnRun(IntType w, IntType h, const float* colorRgba,
			const float* albedoRgba, const float* normalDepthRgba, float* outRgba)
		{
			if (!OidnEnsureDevice())
				return false;
			if (oidnUsesBuffers)
				return OidnRunBuffers(w, h, colorRgba, albedoRgba, normalDepthRgba, outRgba);
			return OidnRunShared(w, h, colorRgba, albedoRgba, normalDepthRgba, outRgba);
		}

		// -----------------------------------------------------------------
		// Fallback A-TROUS edge-aware (diseno seccion 9): wavelet B3 5x5
		// con hueco creciente (step 1,2,4,8), pesos por luminancia
		// comprimida x/(1+x) (robusta en HDR), coseno de normales,
		// profundidad RELATIVA y distancia de albedo. Guias fijas (AOVs),
		// color en ping-pong. OpenMP por filas (patron OPENMP_FOR).
		// -----------------------------------------------------------------

		const IntType ATROUS_ITERATIONS = 4;      // steps 1, 2, 4, 8 (diseno: 3-5)
		const float ATROUS_NORMAL_POW = 32.f;     // exponente del coseno de normales
		const float ATROUS_SIGMA_DEPTH = 0.05f;   // sigma de profundidad relativa
		const float ATROUS_SIGMA_COLOR = 0.25f;   // sigma de luminancia comprimida
		const float ATROUS_SIGMA_ALBEDO = 0.2f;   // sigma de distancia rgb de albedo
		const float ATROUS_NORMAL_EPS2 = 1.0e-8f; // normal^2 bajo esto = "sin normal" (cielo)

		inline float AtrousLumC(const float* c)
		{
			// luminancia Rec.709 comprimida a [0,1) con x/(1+x): las
			// diferencias en HDR no revientan el sigma
			float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
			return l / (1.f + l);
		}

		inline IntType ClampI(IntType v, IntType lo, IntType hi)
		{
			return v < lo ? lo : (v > hi ? hi : v);
		}

		BoolType AtrousRun(IntType w, IntType h, const float* colorRgba,
			const float* albedoRgba, const float* normalDepthRgba, float* outRgba)
		{
			// B3 spline 1D; el peso 2D del tap es kernel[i] * kernel[j]
			const float kernel1d[5] = { 1.f / 16.f, 4.f / 16.f, 6.f / 16.f, 4.f / 16.f, 1.f / 16.f };

			size_t numPix = (size_t)w * (size_t)h;
			std::vector<float> cur, next(numPix * 3);
			PackFloat3(colorRgba, numPix, 0.f, -1.f, cur);

			const float sigDepth2 = 2.f * ATROUS_SIGMA_DEPTH * ATROUS_SIGMA_DEPTH;
			const float sigColor2 = 2.f * ATROUS_SIGMA_COLOR * ATROUS_SIGMA_COLOR;
			const float sigAlbedo2 = 2.f * ATROUS_SIGMA_ALBEDO * ATROUS_SIGMA_ALBEDO;

			for (IntType iter = 0; iter < ATROUS_ITERATIONS; iter++)
			{
				IntType step = (IntType)1 << iter;
				const float* src = cur.data();
				float* dstIt = next.data();

				#pragma OPENMP_FOR
				for (IntType y = 0; y < h; y++)
				{
					for (IntType x = 0; x < w; x++)
					{
						size_t p = (size_t)y * (size_t)w + (size_t)x;
						const float* cp = src + p * 3;
						float lp = AtrousLumC(cp);

						float npx = 0.f, npy = 0.f, npz = 0.f, zp = 0.f, np2 = 0.f;
						if (normalDepthRgba)
						{
							const float* nd = normalDepthRgba + p * 4;
							npx = SaneF(nd[0]);
							npy = SaneF(nd[1]);
							npz = SaneF(nd[2]);
							zp = SaneF(nd[3]);
							np2 = npx * npx + npy * npy + npz * npz;
						}
						const float* ap = albedoRgba ? albedoRgba + p * 4 : nullptr;

						float sumR = 0.f, sumG = 0.f, sumB = 0.f, sumW = 0.f;
						for (IntType j = -2; j <= 2; j++)
						{
							IntType qy = ClampI(y + j * step, 0, h - 1);
							for (IntType i = -2; i <= 2; i++)
							{
								IntType qx = ClampI(x + i * step, 0, w - 1);
								size_t q = (size_t)qy * (size_t)w + (size_t)qx;
								const float* cq = src + q * 3;

								float wgt = kernel1d[i + 2] * kernel1d[j + 2];

								// peso de color: luminancia comprimida
								float dl = lp - AtrousLumC(cq);
								wgt *= std::exp(-(dl * dl) / sigColor2);

								if (normalDepthRgba)
								{
									const float* nd = normalDepthRgba + q * 4;
									float nqx = SaneF(nd[0]), nqy = SaneF(nd[1]), nqz = SaneF(nd[2]);
									float nq2 = nqx * nqx + nqy * nqy + nqz * nqz;
									// normales: coseno^pow; cielo-cielo
									// (ambas ~0) = 1, cielo-geometria = 0
									if (np2 > ATROUS_NORMAL_EPS2 && nq2 > ATROUS_NORMAL_EPS2)
									{
										float d = (npx * nqx + npy * nqy + npz * nqz) / std::sqrt(np2 * nq2);
										wgt *= std::pow(d < 0.f ? 0.f : (d > 1.f ? 1.f : d), ATROUS_NORMAL_POW);
									}
									else if (np2 > ATROUS_NORMAL_EPS2 || nq2 > ATROUS_NORMAL_EPS2)
										wgt = 0.f;
									// profundidad relativa (invariante de
									// escala: t va de metros a far plane)
									float zq = SaneF(nd[3]);
									float azp = zp < 0.f ? -zp : zp;
									float azq = zq < 0.f ? -zq : zq;
									float dz = (zp - zq) / (1.f + (azp > azq ? azp : azq));
									wgt *= std::exp(-(dz * dz) / sigDepth2);
								}

								if (ap)
								{
									const float* aq = albedoRgba + q * 4;
									float da0 = SaneF(ap[0]) - SaneF(aq[0]);
									float da1 = SaneF(ap[1]) - SaneF(aq[1]);
									float da2 = SaneF(ap[2]) - SaneF(aq[2]);
									float da = da0 * da0 + da1 * da1 + da2 * da2;
									wgt *= std::exp(-da / sigAlbedo2);
								}

								sumR += cq[0] * wgt;
								sumG += cq[1] * wgt;
								sumB += cq[2] * wgt;
								sumW += wgt;
							}
						}

						float* dp = dstIt + p * 3;
						if (sumW > 1.0e-12f)
						{
							dp[0] = sumR / sumW;
							dp[1] = sumG / sumW;
							dp[2] = sumB / sumW;
						}
						else
						{
							dp[0] = cp[0];
							dp[1] = cp[1];
							dp[2] = cp[2];
						}
					}
				}

				cur.swap(next);
			}

			for (size_t i = 0; i < numPix; i++)
			{
				outRgba[i * 4 + 0] = cur[i * 3 + 0];
				outRgba[i * 4 + 1] = cur[i * 3 + 1];
				outRgba[i * 4 + 2] = cur[i * 3 + 2];
				outRgba[i * 4 + 3] = colorRgba[i * 4 + 3];
			}
			return true;
		}
	}

	// ---------------------------------------------------------------------
	// API publica
	// ---------------------------------------------------------------------

	QString CinemaDenoiseRun(IntType w, IntType h,
		const float* colorRgba, const float* albedoRgba,
		const float* normalDepthRgba, float* outRgba, BoolType preferOidn)
	{
		if (w < 1 || h < 1 || !colorRgba || !outRgba)
		{
			WARNING("CinemaDenoiseRun: argumentos invalidos (w=" + QString::number(w)
				+ " h=" + QString::number(h) + ")");
			if (colorRgba && outRgba && outRgba != colorRgba && w > 0 && h > 0)
				memcpy(outRgba, colorRgba, (size_t)w * (size_t)h * 4 * sizeof(float));
			return QString();
		}

		if (preferOidn && OidnRun(w, h, colorRgba, albedoRgba, normalDepthRgba, outRgba))
			return QString("oidn-") + oidnLabel; // M7: oidn-cuda | oidn-cpu (evidencia A/B)

		if (AtrousRun(w, h, colorRgba, albedoRgba, normalDepthRgba, outRgba))
			return QString("atrous");

		// Fallo total (no deberia ocurrir: el a-trous no tiene dependencias)
		WARNING("CinemaDenoiseRun: ambos motores fallaron, salida = color sin denoise");
		if (outRgba != colorRgba)
			memcpy(outRgba, colorRgba, (size_t)w * (size_t)h * 4 * sizeof(float));
		return QString();
	}

	BoolType CinemaDenoiseIsCudaAvailable()
	{
		// Inicializa (idempotente) el device OIDN y reporta si es CUDA. Si la DLL de
		// OIDN no esta o cae a CPU, oidnLabel != "cuda" -> false (el preview no hace
		// el checkpoint denoisado en CPU, que seria lag).
		if (!OidnEnsureDevice())
			return false;
		return oidnLabel == "cuda";
	}
}
