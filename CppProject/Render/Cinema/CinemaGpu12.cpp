#include "CinemaGpu12.hpp"

#include "CinemaKernelM0.hpp"
#include "CinemaTrimSource.hpp"  // TRIM: composicion del kernel por conjunto de efectos
#include "CinemaScene.hpp"
#include "CinemaBvh.hpp"

#include <QLibrary>
#include <QImage>

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>

#if API_D3D11

// D3D12 / DXGI 1.6 / dxc (Windows SDK 10.0.26100, headers DXR completos). SOLO
// esta unidad de traduccion los incluye; el resto de Cinema sigue siendo D3D11
// puro. windows.h ya viene por GraphicsApiHandler.hpp (via CinemaScene.hpp).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#undef max
#undef min

// CinemaGpu12: implementacion (contrato en CinemaGpu12.hpp). Ver la nota de
// arquitectura del header. Este backend NO altera el D3D11 ni el oraculo.

namespace CppProject
{
	namespace
	{
		using CinemaKernelM0::PTFRAME_SIZE;

		// -----------------------------------------------------------------
		// Estado del device / infraestructura (perezoso, cacheado por sesion)
		// -----------------------------------------------------------------
		bool g_initTried = false;
		bool g_available = false;
		QString g_why;          // motivo de no-disponible (para el log)
		QString g_backendInfo;  // descripcion del adaptador elegido

		ID3D12Device*               g_device = nullptr;
		ID3D12Device5*              g_device5 = nullptr;  // fase B: prebuild info de AS
		ID3D12CommandQueue*         g_queue = nullptr;
		ID3D12CommandAllocator*     g_alloc = nullptr;
		ID3D12GraphicsCommandList*  g_cmd = nullptr;
		ID3D12GraphicsCommandList4* g_cmd4 = nullptr;     // fase B: BuildRaytracingAccelerationStructure
		ID3D12Fence*                g_fence = nullptr;
		UINT64                      g_fenceVal = 0;
		HANDLE                      g_fenceEvent = nullptr;
		IDXGIAdapter3*              g_adapter3 = nullptr;

		// dxc dinamico (patron OIDN): un unico simbolo C exportado.
		int                   g_dxcState = 0; // 0 sin intentar, 1 OK, -1 fallo permanente
		DxcCreateInstanceProc g_dxcCreate = nullptr;

		// Pipeline fijo (kernel constante -> se compila una vez)
		ID3D12RootSignature* g_rootSig = nullptr;
		// TRIM: clave del CONJUNTO de efectos con el que se compilaron los PSOs.
		// 0 = ninguno (kernel de siempre).
		uint64_t g_trimKey = 0;
		ID3D12PipelineState* g_psoPath = nullptr;
		ID3D12PipelineState* g_psoClear = nullptr;

		// Heap de descriptores shader-visible: t0..t7 (8 SRV) en 0..7,
		// u0..u3 (4 UAV) en 8..11. La static sampler s0 va en la root signature.
		// (M7: u3 = gVarCount adaptive, contiguo tras u0..u2.)
		ID3D12DescriptorHeap* g_srvHeap = nullptr;
		UINT g_descSize = 0;
		const UINT SRV_BASE = 0;
		const UINT UAV_BASE = 8;
		const UINT HEAP_COUNT = 12;

		// Anillo de CBV en upload heap (camino de menor riesgo de fase A: PTFrame
		// integro por tile; se re-usa un slot solo tras drenar la GPU).
		ID3D12Resource*           g_cbvRing = nullptr;
		uint8_t*                  g_cbvMapped = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS g_cbvBaseVa = 0;
		const UINT CBV_SLOTS = 1024;
		// R12: el PTFrame paso de 256 a 272 B (float4 fogParams2 de la niebla).
		// La CBV de D3D12 exige direcciones multiplo de 256 -> el slot del anillo
		// sube a 512 (el sobrante no se lee: el shader solo declara 272 B).
		const UINT CBV_STRIDE = 512; // >= PTFRAME_SIZE y multiplo de la alineacion CBV

		// Recursos de escena (re-subidos desde CPU; CERO interop con D3D11)
		ID3D12Resource* g_verts = nullptr;
		ID3D12Resource* g_tris = nullptr;
		ID3D12Resource* g_nodes = nullptr;
		ID3D12Resource* g_mats = nullptr;
		ID3D12Resource* g_lights = nullptr;
		ID3D12Resource* g_emiTri = nullptr;
		ID3D12Resource* g_emiCdf = nullptr;
		ID3D12Resource* g_atlas = nullptr;
		// Fase B / M8-A: aceleracion por hardware. Con MI3_CINEMA_INSTANCED=0 se
		// arma UNA BLAS (verts en MUNDO) + TLAS de instancia IDENTIDAD. Con el
		// toggle ON (M8-A) se arma UNA BLAS POR DRAW desde los verts LOCALES + una
		// instancia por draw con su matriz de mundo -> mover un objeto = reescribir
		// la Transform de su instancia (fase B). Todas las BLAS viven en un unico
		// buffer g_blasBuf a offsets alineados (evita miles de recursos sueltos).
		// g_instBase (t9): base de tri GLOBAL por instancia (mapea la primitiva
		// LOCAL de cada BLAS al array gTris). g_emiO2W (t10): object->world por
		// emisor de la CDF de NEE (su sampling no pasa por RayQuery). g_asIndex es
		// el index buffer de las BLAS (uint3 por tri; con instancias, winding
		// FUENTE sin swap, orden de entrada por-draw -> se trocea por rango).
		ID3D12Resource* g_blas = nullptr;    // OFF: BLAS unica
		ID3D12Resource* g_blasBuf = nullptr; // ON: todas las BLAS a offsets
		ID3D12Resource* g_tlas = nullptr;
		ID3D12Resource* g_asIndex = nullptr;
		ID3D12Resource* g_instBase = nullptr; // t9: StructuredBuffer<uint>
		ID3D12Resource* g_emiO2W = nullptr;   // t10: StructuredBuffer<InstXform> (48 B)
		UINT g_numInstances = 0;
		double g_lastAsBuildMs = 0.0;
		double g_lastUpdateMs = 0.0;
		// M8-B ("escena viva"): copia CPU de los N instance-descs del ultimo build
		// instanciado (Transform + AccelerationStructure VA a la BLAS en g_blasBuf,
		// que persiste). Un update en vivo reescribe solo Transform y reconstruye la
		// TLAS reusando estas VAs -> sin tocar BLAS/verts. Se rellena en
		// BuildAccelStructuresInstanced.
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> g_instDescCpu;
		// M8-C: offset (bytes) de cada BLAS dentro de g_blasBuf (persistido del build
		// instanciado). El update PARCIAL de geometria (bend) reconstruye SOLO las
		// BLAS cambiadas EN SU MISMO offset (el count no cambia -> el tamano tampoco).
		std::vector<UINT64> g_blasOffset;
		bool g_sceneUp = false;
		// Firma de escena para detectar rebuild sin tocar CinemaScene: el SRV D3D11
		// de verts se re-crea en cada build, asi que su identidad + los conteos
		// distinguen una escena nueva.
		const void* g_sigVertsSrv = nullptr;
		UINT g_sigCounts[7] = { 0, 0, 0, 0, 0, 0, 0 };
		uint64_t g_sigBuildGen = 0; // FIX race capas: firma por generacion de build

		// Targets por resolucion
		IntType g_w = 0, g_h = 0;
		ID3D12Resource* g_accum = nullptr;
		ID3D12Resource* g_aovAlb = nullptr;
		ID3D12Resource* g_aovNrm = nullptr;
		ID3D12Resource* g_varCount = nullptr; // M7 adaptive (u3): media/M2/conteo/convergido por pixel
		ID3D12Resource* g_readback = nullptr; // 4 planos w*h*16 contiguos (accum, alb, nrm, varCount)

		// -----------------------------------------------------------------
		// Helpers de bajo nivel
		// -----------------------------------------------------------------

		QString HrStr(HRESULT hr)
		{
			return "0x" + QString::number((uint)hr, 16);
		}

		D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = type;
			hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			hp.CreationNodeMask = 1;
			hp.VisibleNodeMask = 1;
			return hp;
		}

		D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Alignment = 0;
			d.Width = bytes;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.SampleDesc.Quality = 0;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = flags;
			return d;
		}

		// Crea un committed buffer. Devuelve nullptr con WARNING en fallo.
		ID3D12Resource* CreateBuffer(D3D12_HEAP_TYPE heap, UINT64 bytes,
			D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState)
		{
			if (!g_device || !bytes)
				return nullptr;
			D3D12_HEAP_PROPERTIES hp = HeapProps(heap);
			D3D12_RESOURCE_DESC rd = BufferDesc(bytes, flags);
			ID3D12Resource* res = nullptr;
			HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
				&rd, initState, nullptr, IID_PPV_ARGS(&res));
			if (FAILED(hr))
			{
				WARNING("CinemaGpu12: CreateCommittedResource buffer " + NumStr((IntType)bytes)
					+ " B fallo (" + HrStr(hr) + ")");
				return nullptr;
			}
			return res;
		}

		void WaitForGpu()
		{
			if (!g_queue || !g_fence)
				return;
			g_fenceVal++;
			if (FAILED(g_queue->Signal(g_fence, g_fenceVal)))
				return;
			if (g_fence->GetCompletedValue() < g_fenceVal)
			{
				if (SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent)))
					WaitForSingleObject(g_fenceEvent, INFINITE);
			}
		}

		// Cierra la lista, la ejecuta y drena la GPU. Detecta device-removed.
		BoolType ExecuteAndWait()
		{
			if (FAILED(g_cmd->Close()))
			{
				WARNING("CinemaGpu12: Close de la command list fallo");
				return false;
			}
			ID3D12CommandList* lists[1] = { g_cmd };
			g_queue->ExecuteCommandLists(1, lists);
			WaitForGpu();
			HRESULT rr = g_device->GetDeviceRemovedReason();
			if (FAILED(rr))
			{
				WARNING("CinemaGpu12: DEVICE REMOVED (" + HrStr(rr) + ") -> backend DXR no disponible");
				g_available = false;
				g_why = "device_removed";
				return false;
			}
			return true;
		}

		BoolType BeginCmd()
		{
			if (FAILED(g_alloc->Reset()))
			{
				WARNING("CinemaGpu12: allocator Reset fallo");
				return false;
			}
			if (FAILED(g_cmd->Reset(g_alloc, nullptr)))
			{
				WARNING("CinemaGpu12: command list Reset fallo");
				return false;
			}
			return true;
		}

		// Liga root signature + heap + tablas SRV/UAV + PSO en la lista actual.
		void BindCompute(ID3D12PipelineState* pso)
		{
			g_cmd->SetComputeRootSignature(g_rootSig);
			ID3D12DescriptorHeap* heaps[1] = { g_srvHeap };
			g_cmd->SetDescriptorHeaps(1, heaps);
			D3D12_GPU_DESCRIPTOR_HANDLE base = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
			g_cmd->SetComputeRootDescriptorTable(1, base); // t0..t7 (slot 0 del heap)
			D3D12_GPU_DESCRIPTOR_HANDLE uav = base;
			uav.ptr += (UINT64)UAV_BASE * g_descSize;
			g_cmd->SetComputeRootDescriptorTable(2, uav);  // u0..u2 (slot 8 del heap)
			// Fase B: TLAS por root SRV (t8). CSPathTrace la usa; CSClear la ignora.
			if (g_tlas)
				g_cmd->SetComputeRootShaderResourceView(3, g_tlas->GetGPUVirtualAddress());
			// M8-A: gInstBase (t9) y gEmiO2W (t10). Siempre validos tras UploadScene
			// (con >=1 elemento incluso en OFF/sin emisores) para que el root SRV
			// nunca quede sin setear cuando el shader los referencia.
			if (g_instBase)
				g_cmd->SetComputeRootShaderResourceView(4, g_instBase->GetGPUVirtualAddress());
			if (g_emiO2W)
				g_cmd->SetComputeRootShaderResourceView(5, g_emiO2W->GetGPUVirtualAddress());
			g_cmd->SetPipelineState(pso);
		}

		// Barrera UAV sobre accumulador + AOVs + varCount (hazard RMW entre samples).
		void UavBarrierTargets()
		{
			D3D12_RESOURCE_BARRIER b[4] = {};
			ID3D12Resource* r[4] = { g_accum, g_aovAlb, g_aovNrm, g_varCount };
			for (int i = 0; i < 4; i++)
			{
				b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				b[i].UAV.pResource = r[i];
			}
			g_cmd->ResourceBarrier(4, b);
		}

		UINT GroupsFor(IntType pixels)
		{
			return (UINT)((pixels + (IntType)CinemaKernelM0::THREADGROUP_SIZE - 1)
				/ (IntType)CinemaKernelM0::THREADGROUP_SIZE);
		}

		// -----------------------------------------------------------------
		// dxc: carga dinamica (un intento por sesion) + compilacion DXIL
		// -----------------------------------------------------------------

		BoolType DxcLoad()
		{
			if (g_dxcState != 0)
				return g_dxcState > 0;
			g_dxcState = -1; // pesimista

			// dxil.dll debe estar junto al exe para que dxcompiler.dll firme el
			// DXIL. Cargarla explicitamente (best-effort) fuerza su presencia; si
			// no carga, dxcompiler la buscara igual en el dir del exe.
			static QLibrary dxil("dxil");
			if (!dxil.load())
				WARNING("CinemaGpu12: dxil.dll no cargo junto al exe (" + dxil.errorString()
					+ ") -- el DXIL podria salir sin firmar");

			static QLibrary dxc("dxcompiler");
			if (!dxc.load())
			{
				WARNING("CinemaGpu12: dxcompiler.dll no disponible junto al exe (backend DXR no disponible): "
					+ dxc.errorString());
				return false;
			}
			g_dxcCreate = (DxcCreateInstanceProc)dxc.resolve("DxcCreateInstance");
			if (!g_dxcCreate)
			{
				WARNING("CinemaGpu12: DxcCreateInstance no exportado por dxcompiler.dll");
				return false;
			}
			g_dxcState = 1;
			DEBUG("CinemaGpu12: dxcompiler.dll cargada, DxcCreateInstance resuelto");
			return true;
		}

		// Compila SOURCE con dxc para el entry dado -> blob DXIL firmado. Devuelve
		// nullptr con WARNING (y el log del compilador) en fallo. El caller es
		// dueno del blob (Release).
		IDxcBlob* DxcCompile(const char* source, const wchar_t* entry, const char* dbgName, BoolType rayquery)
		{
			if (!DxcLoad())
				return nullptr;

			IDxcUtils* utils = nullptr;
			IDxcCompiler3* comp = nullptr;
			if (FAILED(g_dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
				FAILED(g_dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&comp))))
			{
				WARNING("CinemaGpu12: DxcCreateInstance(Utils/Compiler) fallo");
				releaseAndReset(comp);
				releaseAndReset(utils);
				return nullptr;
			}

			DxcBuffer src = {};
			src.Ptr = source;
			src.Size = strlen(source);
			src.Encoding = DXC_CP_UTF8;

			// Flags OBLIGATORIOS (M6A_PORT_NOTES): -T cs_6_5, -E <entry>, -HV 2018
			// (paridad matematica con fxc, sin sorpresas de HLSL 2021), -Gis (IEEE
			// strictness: sin el, -O3 pliega el guard isfinite del acumulador),
			// -Ges (strict). NO -Zpr: mantener column-major como el D3D11.
			std::vector<const wchar_t*> args;
			args.push_back(L"-T"); args.push_back(L"cs_6_5");
			args.push_back(L"-E"); args.push_back(entry);
			args.push_back(L"-HV"); args.push_back(L"2018");
			args.push_back(L"-Gis");
			args.push_back(L"-Ges");
			// Fase B: activa el camino RayQuery inline del kernel (TraceClosest/
			// TraceShadow por hardware + gScene TLAS en t8). Solo dxc/D3D12 lo ve;
			// fxc/D3D11 compila el traversal software (fallback, oraculo intacto).
			if (rayquery)
			{
				args.push_back(L"-D");
				args.push_back(L"CINEMA_RAYQUERY=1");
			}
		#if DEBUG_MODE
			args.push_back(L"-Zi");
			args.push_back(L"-Qembed_debug");
		#else
			args.push_back(L"-O3");
		#endif

			IDxcResult* result = nullptr;
			HRESULT hr = comp->Compile(&src, args.data(), (UINT32)args.size(),
				nullptr, IID_PPV_ARGS(&result));

			IDxcBlob* dxil = nullptr;
			if (SUCCEEDED(hr) && result)
			{
				// Log del compilador (errores/warnings)
				IDxcBlobUtf8* errs = nullptr;
				result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
				HRESULT status = E_FAIL;
				result->GetStatus(&status);
				if (FAILED(status))
				{
					QString log = "(sin log)";
					if (errs && errs->GetStringLength())
						log = QString::fromUtf8(errs->GetStringPointer(), (int)errs->GetStringLength());
					WARNING("CinemaGpu12: dxc " + QString(dbgName) + " fallo\n\t" + log);
				}
				else
				{
					if (errs && errs->GetStringLength())
						DEBUG("CinemaGpu12: dxc " + QString(dbgName) + " log: "
							+ QString::fromUtf8(errs->GetStringPointer(), (int)errs->GetStringLength()));
					IDxcBlob* obj = nullptr;
					if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj)
						dxil = obj; // firmado por dxil.dll adyacente
					else
						WARNING("CinemaGpu12: dxc " + QString(dbgName) + " sin objeto DXIL de salida");
				}
				releaseAndReset(errs);
			}
			else
				WARNING("CinemaGpu12: IDxcCompiler3::Compile fallo para " + QString(dbgName) + " (" + HrStr(hr) + ")");

			releaseAndReset(result);
			releaseAndReset(comp);
			releaseAndReset(utils);
			return dxil;
		}

		// -----------------------------------------------------------------
		// Init del device (adaptador HIGH_PERFORMANCE con DXR tier >= 1.1)
		// -----------------------------------------------------------------

		BoolType EnsureDevice()
		{
			if (g_device)
				return true;

			IDXGIFactory6* factory = nullptr;
			if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory)
			{
				g_why = "dxgi_factory_failed";
				WARNING("CinemaGpu12: CreateDXGIFactory2 fallo");
				return false;
			}

			// Elegir el adaptador por ALTO RENDIMIENTO (la iGPU AMD NO debe ganar).
			ID3D12Device* dev = nullptr;
			IDXGIAdapter1* chosen = nullptr;
			QString chosenDesc;
			for (UINT i = 0;; i++)
			{
				IDXGIAdapter1* adapter = nullptr;
				if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
					IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
					break;
				if (!adapter)
					break;

				DXGI_ADAPTER_DESC1 desc = {};
				adapter->GetDesc1(&desc);
				if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				{
					releaseAndReset(adapter);
					continue;
				}

				ID3D12Device* cand = nullptr;
				if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&cand))) && cand)
				{
					D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
					BoolType dxr = SUCCEEDED(cand->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
						&opt5, sizeof(opt5))) && opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
					if (dxr)
					{
						dev = cand;
						chosen = adapter;
						chosenDesc = QString::fromWCharArray(desc.Description);
						chosenDesc += " (" + NumStr((IntType)(desc.DedicatedVideoMemory / (1024 * 1024))) + " MB)";
						break;
					}
					releaseAndReset(cand);
				}
				releaseAndReset(adapter);
			}

			if (!dev)
			{
				g_why = "no_d3d12_dxr_tier11";
				WARNING("CinemaGpu12: ningun adaptador con D3D12 + DXR tier 1.1");
				releaseAndReset(chosen);
				releaseAndReset(factory);
				return false;
			}

			g_device = dev;
			g_backendInfo = "D3D12 DXR / " + chosenDesc;

			// Fase B: ID3D12Device5 para GetRaytracingAccelerationStructurePrebuildInfo.
			if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&g_device5))) || !g_device5)
			{
				g_why = "no_id3d12device5";
				WARNING("CinemaGpu12: ID3D12Device5 no disponible (DXR incompleto)");
				releaseAndReset(chosen);
				releaseAndReset(factory);
				return false;
			}

			// IDXGIAdapter3 para el presupuesto de VRAM (opcional)
			if (chosen)
				chosen->QueryInterface(IID_PPV_ARGS(&g_adapter3));

			// Cola / fence / allocator / lista
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			BoolType ok = SUCCEEDED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)));
			ok = ok && SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)));
			ok = ok && SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_cmd)));
			// Fase B: ID3D12GraphicsCommandList4 para BuildRaytracingAccelerationStructure.
			ok = ok && g_cmd && SUCCEEDED(g_cmd->QueryInterface(IID_PPV_ARGS(&g_cmd4)));
			ok = ok && SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
			if (ok)
			{
				g_cmd->Close(); // se abre con Reset en cada uso
				g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				ok = g_fenceEvent != nullptr;
			}
			g_descSize = ok ? g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;

			releaseAndReset(chosen);
			releaseAndReset(factory);

			if (!ok)
			{
				g_why = "d3d12_infra_failed";
				WARNING("CinemaGpu12: creacion de cola/allocator/lista/fence fallo");
				return false;
			}

			// Log de VRAM (una vez)
			if (g_adapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
				if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
					DEBUG("CinemaGpu12: VRAM local budget " + NumStr((IntType)(mem.Budget / (1024 * 1024)))
						+ " MB, en uso " + NumStr((IntType)(mem.CurrentUsage / (1024 * 1024))) + " MB");
			}
			DEBUG("CinemaGpu12: device creado -> " + g_backendInfo);
			return true;
		}

		// -----------------------------------------------------------------
		// Root signature + PSOs + heap + anillo CBV (una vez)
		// -----------------------------------------------------------------

		BoolType EnsurePipeline()
		{
			// TRIM (fix oilslick-en-Cinema-congelado): el trimKey se calcula
			// SIEMPRE, incluso si rootsig/heap/anillo ya existen. Si no, la
			// PRIMERA llamada a EnsurePipeline() -- CinemaGpu12Available(),
			// que corre UNA vez al arrancar via g_initTried, ANTES de que
			// exista ninguna escena real (CinemaSceneTrimSlots() vacio) --
			// compila el PSO con CinemaTrimComposeSource({}) = el kernel SIN
			// ningun efecto (el marcador @@TRIM@@ queda como comentario,
			// CINEMA_TRIM nunca se #define). Con el early-return de antes,
			// esa PSO quedaba fija para el resto del proceso: la escena real
			// (con oilslick u otro efecto) nunca volvia a pasar por aqui, asi
			// que stTicks()/stWave()/stCycle() ni siquiera EXISTIAN en el
			// shader compilado -- el trim no podia responder al tiempo (ni a
			// nada) por mucho que el cbuffer trajera el valor correcto.
			uint64_t trimKey = CinemaTrimSetKey(CinemaSceneTrimSlots());
			if (g_rootSig && g_psoPath && g_psoClear && g_srvHeap && g_cbvRing && trimKey == g_trimKey)
				return true;

			// --- Root signature ---
			// param0: root CBV b0 (VA del slot del anillo por tile)
			// param1: tabla SRV t0..t7 ; param2: tabla UAV u0..u2 ; static sampler s0
			if (!g_rootSig)
			{
				D3D12_DESCRIPTOR_RANGE srvRange = {};
				srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				srvRange.NumDescriptors = 8; // t0..t7
				srvRange.BaseShaderRegister = 0;
				srvRange.RegisterSpace = 0;
				srvRange.OffsetInDescriptorsFromTableStart = 0;

				D3D12_DESCRIPTOR_RANGE uavRange = {};
				uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
				uavRange.NumDescriptors = 4; // u0..u3 (M7: +varCount)
				uavRange.BaseShaderRegister = 0;
				uavRange.RegisterSpace = 0;
				uavRange.OffsetInDescriptorsFromTableStart = 0;

				// param3 (fase B): root SRV con la TLAS en t8 (RayQuery inline). Un
				// acceleration structure se puede ligar como root descriptor SRV
				// apuntando a la VA de la TLAS. CSClear no lo usa (root param sin
				// setear es legal si el shader no lo referencia); CSPathTrace lo
				// setea siempre en BindCompute.
				// M8-A: params 4/5 = root SRV con gInstBase (t9) y gEmiO2W (t10). Se
				// ligan como root descriptors (VA directa, StructuredBuffer) igual
				// que la TLAS. CSClear no los usa (root param sin setear es legal si
				// el shader no lo referencia); CSPathTrace los setea en BindCompute.
				D3D12_ROOT_PARAMETER params[6] = {};
				params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				params[0].Descriptor.ShaderRegister = 0; // b0
				params[0].Descriptor.RegisterSpace = 0;
				params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[1].DescriptorTable.NumDescriptorRanges = 1;
				params[1].DescriptorTable.pDescriptorRanges = &srvRange;
				params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				params[2].DescriptorTable.NumDescriptorRanges = 1;
				params[2].DescriptorTable.pDescriptorRanges = &uavRange;
				params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[3].Descriptor.ShaderRegister = 8; // t8 = gScene (TLAS)
				params[3].Descriptor.RegisterSpace = 0;
				params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[4].Descriptor.ShaderRegister = 9; // t9 = gInstBase
				params[4].Descriptor.RegisterSpace = 0;
				params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				params[5].Descriptor.ShaderRegister = 10; // t10 = gEmiO2W
				params[5].Descriptor.RegisterSpace = 0;
				params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				D3D12_STATIC_SAMPLER_DESC samp = {};
				samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
				samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
				samp.MaxLOD = D3D12_FLOAT32_MAX;
				samp.ShaderRegister = 0; // s0
				samp.RegisterSpace = 0;
				samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				D3D12_ROOT_SIGNATURE_DESC rs = {};
				rs.NumParameters = 6;
				rs.pParameters = params;
				rs.NumStaticSamplers = 1;
				rs.pStaticSamplers = &samp;
				rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

				ID3DBlob* blob = nullptr;
				ID3DBlob* err = nullptr;
				HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
				if (FAILED(hr))
				{
					QString msg = err ? QString::fromLatin1((const char*)err->GetBufferPointer(), (int)err->GetBufferSize()) : "(sin log)";
					WARNING("CinemaGpu12: D3D12SerializeRootSignature fallo: " + msg);
					releaseAndReset(err);
					releaseAndReset(blob);
					return false;
				}
				releaseAndReset(err);
				hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
				releaseAndReset(blob);
				if (FAILED(hr))
				{
					WARNING("CinemaGpu12: CreateRootSignature fallo (" + HrStr(hr) + ")");
					return false;
				}
			}

			// --- PSOs (compilar el MISMO kernel a DXIL) ---
			// TRIM (D10): "el MISMO kernel" sigue siendo literal -- los dos
			// backends compilan EL MISMO TEXTO, el que compone
			// CinemaTrimComposeSource para el conjunto de efectos de la
			// escena. Sin efectos devuelve CinemaKernelM0::SOURCE tal cual.
			// trimKey ya se calculo arriba (antes del early-return): reusarlo
			// aqui, no volver a llamar CinemaTrimSetKey.
			{
				if (trimKey != g_trimKey && (g_psoPath || g_psoClear))
				{
					releaseAndReset(g_psoPath);
					releaseAndReset(g_psoClear);
				}
				g_trimKey = trimKey;
			}
			if (!g_psoPath || !g_psoClear)
			{
				const char* src = CinemaTrimComposeSource(CinemaSceneTrimSlots());
				IDxcBlob* dxilPath = DxcCompile(src, L"CSPathTrace", "CSPathTrace", true);
				IDxcBlob* dxilClear = DxcCompile(src, L"CSClear", "CSClear", true);
				if (!dxilPath || !dxilClear)
				{
					g_why = "dxc_compile_failed";
					releaseAndReset(dxilPath);
					releaseAndReset(dxilClear);
					return false;
				}
				D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
				pd.pRootSignature = g_rootSig;
				pd.CS.pShaderBytecode = dxilPath->GetBufferPointer();
				pd.CS.BytecodeLength = dxilPath->GetBufferSize();
				HRESULT h1 = g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoPath));
				pd.CS.pShaderBytecode = dxilClear->GetBufferPointer();
				pd.CS.BytecodeLength = dxilClear->GetBufferSize();
				HRESULT h2 = g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoClear));
				releaseAndReset(dxilPath);
				releaseAndReset(dxilClear);
				if (FAILED(h1) || FAILED(h2))
				{
					g_why = "pso_create_failed";
					WARNING("CinemaGpu12: CreateComputePipelineState fallo (path " + HrStr(h1) + ", clear " + HrStr(h2)
						+ ") -- dxil.dll firmando el DXIL?");
					releaseAndReset(g_psoPath);
					releaseAndReset(g_psoClear);
					return false;
				}
			}

			// --- Heap de descriptores (11 slots, shader-visible) ---
			if (!g_srvHeap)
			{
				D3D12_DESCRIPTOR_HEAP_DESC hd = {};
				hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				hd.NumDescriptors = HEAP_COUNT;
				hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap))))
				{
					WARNING("CinemaGpu12: CreateDescriptorHeap fallo");
					return false;
				}
			}

			// --- Anillo de CBV ---
			if (!g_cbvRing)
			{
				g_cbvRing = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)CBV_SLOTS * CBV_STRIDE,
					D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
				if (!g_cbvRing)
					return false;
				D3D12_RANGE none = { 0, 0 };
				if (FAILED(g_cbvRing->Map(0, &none, (void**)&g_cbvMapped)))
				{
					WARNING("CinemaGpu12: Map del anillo CBV fallo");
					releaseAndReset(g_cbvRing);
					return false;
				}
				g_cbvBaseVa = g_cbvRing->GetGPUVirtualAddress();
			}

			return true;
		}

		// -----------------------------------------------------------------
		// Descriptores de escena / targets en el heap
		// -----------------------------------------------------------------

		D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(UINT index)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += (SIZE_T)index * g_descSize;
			return h;
		}

		// SRV de structured buffer en el slot dado. res == nullptr -> descriptor
		// null (para t5/t6/t7 opcionales cuando el conteo es 0; el kernel no los
		// lee). count/stride definen la vista cuando res != nullptr.
		void WriteBufferSrv(UINT slot, ID3D12Resource* res, UINT stride, UINT count)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			d.Buffer.FirstElement = 0;
			d.Buffer.NumElements = res ? count : 1;
			d.Buffer.StructureByteStride = stride;
			d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			g_device->CreateShaderResourceView(res, &d, HeapCpu(slot));
		}

		void WriteAtlasSrv(UINT slot, ID3D12Resource* atlas, UINT slices)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			d.Texture2DArray.MostDetailedMip = 0;
			d.Texture2DArray.MipLevels = 1;
			d.Texture2DArray.FirstArraySlice = 0;
			d.Texture2DArray.ArraySize = slices;
			d.Texture2DArray.PlaneSlice = 0;
			d.Texture2DArray.ResourceMinLODClamp = 0.f;
			g_device->CreateShaderResourceView(atlas, &d, HeapCpu(slot));
		}

		void WriteBufferUav(UINT slot, ID3D12Resource* res, UINT count)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			d.Buffer.FirstElement = 0;
			d.Buffer.NumElements = count;
			d.Buffer.StructureByteStride = 16; // float4
			d.Buffer.CounterOffsetInBytes = 0;
			d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			g_device->CreateUnorderedAccessView(res, nullptr, &d, HeapCpu(slot));
		}

		// -----------------------------------------------------------------
		// Upload de escena
		// -----------------------------------------------------------------

		void ReleaseSceneResources()
		{
			releaseAndReset(g_verts);
			releaseAndReset(g_tris);
			releaseAndReset(g_nodes);
			releaseAndReset(g_mats);
			releaseAndReset(g_lights);
			releaseAndReset(g_emiTri);
			releaseAndReset(g_emiCdf);
			releaseAndReset(g_atlas);
			releaseAndReset(g_blas);
			releaseAndReset(g_blasBuf);
			releaseAndReset(g_tlas);
			releaseAndReset(g_asIndex);
			releaseAndReset(g_instBase);
			releaseAndReset(g_emiO2W);
			g_numInstances = 0;
			g_instDescCpu.clear(); // M8-B: sin snapshot valido tras liberar la escena
			g_blasOffset.clear();  // M8-C
			g_sceneUp = false;
		}

		// Crea un DEFAULT buffer en COPY_DEST, encola la copia desde un upload
		// temporal (que queda vivo en 'temps' hasta el wait) y la barrera a
		// NON_PIXEL_SHADER_RESOURCE. Devuelve el recurso DEFAULT o nullptr.
		ID3D12Resource* UploadStructured(const void* data, UINT stride, UINT count,
			std::vector<ID3D12Resource*>& temps, std::vector<D3D12_RESOURCE_BARRIER>& barriers)
		{
			UINT64 bytes = (UINT64)stride * count;
			if (!bytes || !data)
				return nullptr;
			ID3D12Resource* def = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, bytes,
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
			ID3D12Resource* up = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, bytes,
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (!def || !up)
			{
				releaseAndReset(def);
				releaseAndReset(up);
				return nullptr;
			}
			void* mapped = nullptr;
			D3D12_RANGE none = { 0, 0 };
			if (FAILED(up->Map(0, &none, &mapped)))
			{
				releaseAndReset(def);
				releaseAndReset(up);
				return nullptr;
			}
			memcpy(mapped, data, (size_t)bytes);
			up->Unmap(0, nullptr);
			g_cmd->CopyBufferRegion(def, 0, up, 0, bytes);

			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = def;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barriers.push_back(b);
			temps.push_back(up);
			return def;
		}

		// -----------------------------------------------------------------
		// Fase B: BLAS/TLAS. UNA BLAS (todos los tris en MUNDO, NON_OPAQUE) desde
		// g_verts (posicion @ offset 0, stride 48) + un index buffer tightly-
		// packed (uint3 por tri, en orden de bvh.tris -> PrimitiveIndex==triIdx)
		// que se crea aqui. TLAS de UNA instancia identidad. PREFER_FAST_TRACE.
		// Cronometrado con QueryPerformanceCounter (build SINCRONO -> ms reales).
		// Devuelve false con fallback si algo falla. El index buffer g_asIndex ya
		// esta subido y en NON_PIXEL_SHADER_RESOURCE (estado requerido por el
		// build de AS) antes de llamar a esta funcion.
		// -----------------------------------------------------------------
		BoolType BuildAccelStructures(UINT numVerts, UINT numTris)
		{
			g_lastAsBuildMs = 0.0;
			if (!g_device5 || !g_cmd4 || !g_verts || !g_asIndex || !numTris)
			{
				WARNING("CinemaGpu12: precondiciones de build de AS ausentes");
				return false;
			}

			// --- Geometria: triangulos indexados, NON_OPAQUE (candidatos) ---
			D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
			geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // NON_OPAQUE
			geo.Triangles.Transform3x4 = 0;
			geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geo.Triangles.IndexCount = numTris * 3;
			geo.Triangles.VertexCount = numVerts;
			geo.Triangles.IndexBuffer = g_asIndex->GetGPUVirtualAddress();
			geo.Triangles.VertexBuffer.StartAddress = g_verts->GetGPUVirtualAddress();
			geo.Triangles.VertexBuffer.StrideInBytes = CinemaKernelM0::PTVERT_STRIDE; // 48

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasIn = {};
			blasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			blasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			blasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			blasIn.NumDescs = 1;
			blasIn.pGeometryDescs = &geo;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo = {};
			g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasIn, &blasInfo);

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
			tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			tlasIn.NumDescs = 1;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
			g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);

			if (!blasInfo.ResultDataMaxSizeInBytes || !tlasInfo.ResultDataMaxSizeInBytes)
			{
				WARNING("CinemaGpu12: prebuild info de AS invalido");
				return false;
			}

			UINT64 scratchBytes = blasInfo.ScratchDataSizeInBytes > tlasInfo.ScratchDataSizeInBytes
				? blasInfo.ScratchDataSizeInBytes : tlasInfo.ScratchDataSizeInBytes;

			// Buffers de resultado (estado AS, nunca se transicionan) + scratch +
			// instance descs (upload). BLAS/TLAS son miembros persistentes.
			releaseAndReset(g_blas);
			releaseAndReset(g_tlas);
			g_blas = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, blasInfo.ResultDataMaxSizeInBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
			g_tlas = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, tlasInfo.ResultDataMaxSizeInBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
			ID3D12Resource* scratch = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, scratchBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ID3D12Resource* instBuf = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (!g_blas || !g_tlas || !scratch || !instBuf)
			{
				WARNING("CinemaGpu12: reserva de buffers de AS fallo");
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				releaseAndReset(g_blas);
				releaseAndReset(g_tlas);
				return false;
			}

			// Instancia identidad, mascara 0xFF, apunta a la BLAS.
			D3D12_RAYTRACING_INSTANCE_DESC inst = {};
			inst.Transform[0][0] = 1.0f;
			inst.Transform[1][1] = 1.0f;
			inst.Transform[2][2] = 1.0f;
			inst.InstanceID = 0;
			inst.InstanceMask = 0xFF;
			inst.InstanceContributionToHitGroupIndex = 0;
			inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			inst.AccelerationStructure = g_blas->GetGPUVirtualAddress();
			void* im = nullptr;
			D3D12_RANGE none = { 0, 0 };
			if (FAILED(instBuf->Map(0, &none, &im)))
			{
				WARNING("CinemaGpu12: Map del instance desc fallo");
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				return false;
			}
			memcpy(im, &inst, sizeof(inst));
			instBuf->Unmap(0, nullptr);
			tlasIn.InstanceDescs = instBuf->GetGPUVirtualAddress();

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
			blasDesc.Inputs = blasIn;
			blasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
			blasDesc.DestAccelerationStructureData = g_blas->GetGPUVirtualAddress();

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
			tlasDesc.Inputs = tlasIn;
			tlasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
			tlasDesc.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();

			LARGE_INTEGER freq, t0, t1;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t0);

			if (!BeginCmd())
			{
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				return false;
			}
			g_cmd4->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
			// Barrera UAV global: BLAS (y sus escrituras de scratch) completa antes
			// de que la TLAS lea la BLAS y reuse el mismo scratch.
			D3D12_RESOURCE_BARRIER uav = {};
			uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uav.UAV.pResource = nullptr; // global
			g_cmd->ResourceBarrier(1, &uav);
			g_cmd4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
			BoolType ok = ExecuteAndWait();

			QueryPerformanceCounter(&t1);
			g_lastAsBuildMs = freq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;

			releaseAndReset(scratch);
			releaseAndReset(instBuf);

			if (!ok)
			{
				releaseAndReset(g_blas);
				releaseAndReset(g_tlas);
				return false;
			}
			g_numInstances = 1;

			UINT64 asBytes = blasInfo.ResultDataMaxSizeInBytes + tlasInfo.ResultDataMaxSizeInBytes;
			DEBUG("CinemaGpu12: BLAS/TLAS build " + QString::number(g_lastAsBuildMs, 'f', 2) + " ms (tris "
				+ NumStr((IntType)numTris) + ", AS " + NumStr((IntType)(asBytes / (1024 * 1024))) + " MB, scratch "
				+ NumStr((IntType)(scratchBytes / (1024 * 1024))) + " MB)");
			if (g_adapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
				if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
					DEBUG("CinemaGpu12: VRAM tras AS en uso " + NumStr((IntType)(mem.CurrentUsage / (1024 * 1024)))
						+ " MB de " + NumStr((IntType)(mem.Budget / (1024 * 1024))) + " MB");
			}
			return true;
		}

		// -----------------------------------------------------------------
		// M8-A: BLAS por draw (verts LOCALES) + TLAS multi-instancia. Cada draw
		// valido -> su propia BLAS desde el sub-rango de g_asIndex (indices GLOBALES
		// winding-fuente) sobre g_verts; una instancia con la matriz de mundo del
		// draw. Todas las BLAS viven en g_blasBuf a offsets alineados a 256 B (evita
		// N recursos sueltos); scratch por-BLAS a offsets (builds independientes sin
		// barrera entre si) y una barrera UAV global antes de la TLAS. NON_OPAQUE
		// (el kernel replica los descartes custom por candidato). Sincrono ->
		// g_lastAsBuildMs reales. Fase C dedup por vbufferId (una BLAS por malla).
		// -----------------------------------------------------------------
		BoolType BuildAccelStructuresInstanced(UINT numVerts)
		{
			g_lastAsBuildMs = 0.0;
			if (!g_device5 || !g_cmd4 || !g_verts || !g_asIndex || !numVerts)
			{
				WARNING("CinemaGpu12: precondiciones de build de AS instanciado ausentes");
				return false;
			}
			const std::vector<PTDrawRange>& ranges = CinemaSceneGetDrawRanges();
			const std::vector<PTDraw>& draws = CinemaSceneGetDraws();
			const UINT N = (UINT)ranges.size();
			if (!N)
			{
				WARNING("CinemaGpu12: sin draws para el AS instanciado");
				return false;
			}

			auto Align256 = [](UINT64 v) -> UINT64 {
				const UINT64 a = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
				return (v + a - 1) & ~(a - 1);
			};
			const D3D12_GPU_VIRTUAL_ADDRESS vertsVa = g_verts->GetGPUVirtualAddress();
			const D3D12_GPU_VIRTUAL_ADDRESS idxVa = g_asIndex->GetGPUVirtualAddress();

			// --- Paso 1: geometrias + prebuild info por BLAS -> offsets ---
			std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos(N);
			std::vector<UINT64> blasOffset(N), scratchOffset(N);
			UINT64 blasCursor = 0, scratchCursor = 0;
			for (UINT i = 0; i < N; i++)
			{
				const PTDrawRange& rg = ranges[i];
				D3D12_RAYTRACING_GEOMETRY_DESC& geo = geos[i];
				geo = {};
				geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // NON_OPAQUE
				geo.Triangles.Transform3x4 = 0;
				geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
				geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				geo.Triangles.IndexCount = (UINT)rg.triCount * 3u;
				geo.Triangles.VertexCount = numVerts; // indices GLOBALES sobre g_verts
				geo.Triangles.IndexBuffer = idxVa + (UINT64)rg.triStart * 3ull * sizeof(uint32_t);
				geo.Triangles.VertexBuffer.StartAddress = vertsVa;
				geo.Triangles.VertexBuffer.StrideInBytes = CinemaKernelM0::PTVERT_STRIDE;

				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
				in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				in.NumDescs = 1;
				in.pGeometryDescs = &geo;
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
				g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
				if (!info.ResultDataMaxSizeInBytes)
				{
					WARNING("CinemaGpu12: prebuild info de BLAS " + NumStr((IntType)i) + " invalido");
					return false;
				}
				blasOffset[i] = blasCursor;
				blasCursor += Align256(info.ResultDataMaxSizeInBytes);
				scratchOffset[i] = scratchCursor;
				scratchCursor += Align256(info.ScratchDataSizeInBytes);
			}

			// --- Paso 1b: prebuild TLAS ---
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
			tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			tlasIn.NumDescs = N;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
			g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);
			if (!tlasInfo.ResultDataMaxSizeInBytes)
			{
				WARNING("CinemaGpu12: prebuild info de TLAS invalido");
				return false;
			}
			// La TLAS reusa el scratch (barrera UAV mediante) -> tamano = max de la
			// suma de scratch de BLAS y el scratch de la TLAS.
			UINT64 scratchBytes = scratchCursor > tlasInfo.ScratchDataSizeInBytes
				? scratchCursor : tlasInfo.ScratchDataSizeInBytes;

			// --- Paso 2: reservas ---
			releaseAndReset(g_blasBuf);
			releaseAndReset(g_tlas);
			g_blasBuf = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, blasCursor,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
			g_tlas = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, tlasInfo.ResultDataMaxSizeInBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
			ID3D12Resource* scratch = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, scratchBytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			ID3D12Resource* instBuf = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD,
				(UINT64)N * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (!g_blasBuf || !g_tlas || !scratch || !instBuf)
			{
				WARNING("CinemaGpu12: reserva de buffers de AS instanciado fallo");
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				releaseAndReset(g_blasBuf);
				releaseAndReset(g_tlas);
				return false;
			}
			const D3D12_GPU_VIRTUAL_ADDRESS blasBufVa = g_blasBuf->GetGPUVirtualAddress();

			// --- Paso 3: instancias (Transform = mundo del draw, fila-mayor) ---
			D3D12_RAYTRACING_INSTANCE_DESC* insts = nullptr;
			D3D12_RANGE none = { 0, 0 };
			if (FAILED(instBuf->Map(0, &none, (void**)&insts)))
			{
				WARNING("CinemaGpu12: Map de instance descs instanciado fallo");
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				return false;
			}
			g_instDescCpu.assign(N, D3D12_RAYTRACING_INSTANCE_DESC{});
			for (UINT i = 0; i < N; i++)
			{
				D3D12_RAYTRACING_INSTANCE_DESC& inst = insts[i];
				inst = {};
				CinemaInstXform x = CinemaMatrixToInstXform(draws[(size_t)ranges[i].drawIdx].world);
				memcpy(&inst.Transform[0][0], x.m, sizeof(x.m)); // 3x4 fila-mayor, layout identico
				inst.InstanceID = 0; // el kernel usa InstanceIndex, no InstanceID
				inst.InstanceMask = 0xFF;
				inst.InstanceContributionToHitGroupIndex = 0;
				inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
				inst.AccelerationStructure = blasBufVa + blasOffset[i];
				g_instDescCpu[i] = inst; // M8-B: snapshot para el update en vivo
			}
			instBuf->Unmap(0, nullptr);
			tlasIn.InstanceDescs = instBuf->GetGPUVirtualAddress();

			// --- Paso 4: build (N BLAS en paralelo por offset de scratch, barrera
			// UAV global, luego TLAS reusando el scratch) ---
			LARGE_INTEGER freq, t0, t1;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t0);
			if (!BeginCmd())
			{
				releaseAndReset(scratch);
				releaseAndReset(instBuf);
				return false;
			}
			const D3D12_GPU_VIRTUAL_ADDRESS scratchVa = scratch->GetGPUVirtualAddress();
			for (UINT i = 0; i < N; i++)
			{
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
				in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				in.NumDescs = 1;
				in.pGeometryDescs = &geos[i];
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d = {};
				d.Inputs = in;
				d.ScratchAccelerationStructureData = scratchVa + scratchOffset[i];
				d.DestAccelerationStructureData = blasBufVa + blasOffset[i];
				g_cmd4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
			}
			// Barrera UAV global: todas las BLAS (y sus escrituras de scratch)
			// completan antes de que la TLAS lea las BLAS y reuse el scratch.
			D3D12_RESOURCE_BARRIER uav = {};
			uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			uav.UAV.pResource = nullptr;
			g_cmd->ResourceBarrier(1, &uav);

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
			tlasDesc.Inputs = tlasIn;
			tlasDesc.ScratchAccelerationStructureData = scratchVa;
			tlasDesc.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
			g_cmd4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
			BoolType ok = ExecuteAndWait();

			QueryPerformanceCounter(&t1);
			g_lastAsBuildMs = freq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;

			releaseAndReset(scratch);
			releaseAndReset(instBuf);
			if (!ok)
			{
				releaseAndReset(g_blasBuf);
				releaseAndReset(g_tlas);
				return false;
			}
			g_numInstances = N;
			g_blasOffset = blasOffset; // M8-C: persistir offsets para el rebuild parcial

			UINT64 asBytes = blasCursor + tlasInfo.ResultDataMaxSizeInBytes;
			DEBUG("CinemaGpu12: BLAS/TLAS INSTANCIADO build " + QString::number(g_lastAsBuildMs, 'f', 2)
				+ " ms (" + NumStr((IntType)N) + " BLAS/instancias, AS " + NumStr((IntType)(asBytes / (1024 * 1024)))
				+ " MB, scratch " + NumStr((IntType)(scratchBytes / (1024 * 1024))) + " MB)");
			if (g_adapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
				if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
					DEBUG("CinemaGpu12: VRAM tras AS instanciado en uso " + NumStr((IntType)(mem.CurrentUsage / (1024 * 1024)))
						+ " MB de " + NumStr((IntType)(mem.Budget / (1024 * 1024))) + " MB");
			}
			return true;
		}

		BoolType UploadScene()
		{
			const CinemaSceneGpu& g = CinemaSceneGetGpu();
			const CinemaBvh& bvh = CinemaSceneGetBvh();
			const std::vector<PTMaterial>& mats = CinemaSceneGetMaterials();
			const std::vector<PTLight>& lights = CinemaSceneLights();
			const QVector<QImage>& slices = CinemaSceneGetAtlasSlices();

			// M8-A: eleccion de representacion. instanced -> verts LOCALES + tris
			// winding-fuente (trisLocal) + CDF emisiva instanciada; el kernel
			// transforma a mundo por instancia. OFF -> el camino de siempre (verts
			// MUNDO + bvh.tris reordenado), bit-identico.
			const BoolType instanced = CinemaSceneIsInstanced();
			const std::vector<PTVert>& verts = instanced ? CinemaSceneGetVertsLocal() : CinemaSceneGetVerts();
			const std::vector<PTTri>& tris = instanced ? CinemaSceneGetTrisLocal() : bvh.tris;
			const std::vector<uint32_t>& emiTri = instanced ? CinemaSceneGetEmissiveTrisLocal() : CinemaSceneGetEmissiveTris();
			const std::vector<float>& emiCdf = instanced ? CinemaSceneGetEmissiveCdfLocal() : CinemaSceneGetEmissiveCdf();

			if (verts.empty() || tris.empty() || bvh.nodes.empty() || mats.empty() || slices.isEmpty())
			{
				WARNING("CinemaGpu12: escena incompleta para el upload D3D12");
				return false;
			}

			ReleaseSceneResources();
			if (!BeginCmd())
				return false;

			std::vector<ID3D12Resource*> temps;
			std::vector<D3D12_RESOURCE_BARRIER> barriers;

			g_verts = UploadStructured(verts.data(), CinemaKernelM0::PTVERT_STRIDE, (UINT)verts.size(), temps, barriers);
			g_tris  = UploadStructured(tris.data(), CinemaKernelM0::PTTRI_STRIDE, (UINT)tris.size(), temps, barriers);
			g_nodes = UploadStructured(bvh.nodes.data(), CinemaKernelM0::PTNODE_STRIDE, (UINT)bvh.nodes.size(), temps, barriers);
			g_mats  = UploadStructured(mats.data(), CinemaKernelM0::PTMATERIAL_STRIDE, (UINT)mats.size(), temps, barriers);
			if (!lights.empty())
				g_lights = UploadStructured(lights.data(), CinemaKernelM0::PTLIGHT_STRIDE, (UINT)lights.size(), temps, barriers);
			if (!emiTri.empty())
				g_emiTri = UploadStructured(emiTri.data(), CinemaKernelM0::PTEMITRI_STRIDE, (UINT)emiTri.size(), temps, barriers);
			if (!emiCdf.empty())
				g_emiCdf = UploadStructured(emiCdf.data(), CinemaKernelM0::PTEMICDF_STRIDE, (UINT)emiCdf.size(), temps, barriers);

			// Fase B / M8-A: index buffer tightly-packed (uint3 por tri) para las
			// BLAS, en el MISMO orden que gTris -> PrimitiveIndex (local a la BLAS) +
			// gInstBase = triIdx global. Se sube como structured buffer stride 4 (la
			// BLAS solo usa su GPU VA como R32_UINT); queda en NON_PIXEL_SHADER_RESOURCE.
			std::vector<uint32_t> asIdx(tris.size() * 3);
			for (size_t i = 0; i < tris.size(); i++)
			{
				asIdx[i * 3 + 0] = tris[i].i0;
				asIdx[i * 3 + 1] = tris[i].i1;
				asIdx[i * 3 + 2] = tris[i].i2;
			}
			g_asIndex = UploadStructured(asIdx.data(), 4, (UINT)asIdx.size(), temps, barriers);

			// M8-A: gInstBase (t9) = base de tri GLOBAL por instancia; gEmiO2W (t10)
			// = object->world por emisor. Con instancias los construye a partir de
			// los rangos de draw / emiO2W; OFF -> gInstBase=[0] (BLAS unica) y gEmiO2W
			// = IDENTIDAD por emisor (la geometria emisora ya esta en mundo). Ambos
			// con >=1 elemento para que el root SRV nunca quede sin ligar.
			CinemaInstXform identityX = {}; identityX.m[0] = identityX.m[5] = identityX.m[10] = 1.f;
			std::vector<uint32_t> instBaseCpu;
			std::vector<CinemaInstXform> emiO2WCpu;
			if (instanced)
			{
				const std::vector<PTDrawRange>& ranges = CinemaSceneGetDrawRanges();
				instBaseCpu.reserve(ranges.size());
				for (size_t r = 0; r < ranges.size(); r++)
					instBaseCpu.push_back((uint32_t)ranges[r].triStart);
				const std::vector<CinemaInstXform>& eo = CinemaSceneGetEmissiveO2W();
				emiO2WCpu.assign(eo.begin(), eo.end()); // uno por emisor (== emiTri.size())
			}
			else
			{
				instBaseCpu.push_back(0u); // BLAS unica: PrimitiveIndex ya es global
				// El kernel indexa gEmiO2W[slot] para CADA emisor: una identidad por
				// emisor de MUNDO (su geometria ya esta en mundo -> transform no-op).
				emiO2WCpu.assign(emiTri.size(), identityX);
			}
			if (instBaseCpu.empty())
				instBaseCpu.push_back(0u);
			if (emiO2WCpu.empty())
				emiO2WCpu.push_back(identityX); // dummy: root SRV siempre valido
			g_instBase = UploadStructured(instBaseCpu.data(), 4, (UINT)instBaseCpu.size(), temps, barriers);
			g_emiO2W   = UploadStructured(emiO2WCpu.data(), (UINT)sizeof(CinemaInstXform), (UINT)emiO2WCpu.size(), temps, barriers);

			// --- Atlas Texture2DArray SRGB ---
			UINT numSlices = (UINT)slices.size();
			UINT aw = (UINT)slices[0].width();
			UINT ah = (UINT)slices[0].height();
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = aw;
			td.Height = ah;
			td.DepthOrArraySize = (UINT16)numSlices;
			td.MipLevels = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			td.SampleDesc.Count = 1;
			td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			td.Flags = D3D12_RESOURCE_FLAG_NONE;
			D3D12_HEAP_PROPERTIES defHp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			HRESULT ha = g_device->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &td,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_atlas));

			BoolType atlasOk = SUCCEEDED(ha) && g_atlas != nullptr;
			if (atlasOk)
			{
				// Footprints alineados a 256 B por slice
				std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(numSlices);
				std::vector<UINT> rows(numSlices);
				std::vector<UINT64> rowBytes(numSlices);
				UINT64 total = 0;
				g_device->GetCopyableFootprints(&td, 0, numSlices, 0, fps.data(), rows.data(), rowBytes.data(), &total);

				ID3D12Resource* up = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, total,
					D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
				uint8_t* mapped = nullptr;
				D3D12_RANGE none = { 0, 0 };
				if (up && SUCCEEDED(up->Map(0, &none, (void**)&mapped)))
				{
					for (UINT s = 0; s < numSlices; s++)
					{
						const QImage& img = slices[(int)s];
						UINT64 dstRowPitch = fps[s].Footprint.RowPitch;
						for (UINT y = 0; y < rows[s]; y++)
						{
							const uchar* srcRow = img.constScanLine((int)y);
							memcpy(mapped + fps[s].Offset + (UINT64)y * dstRowPitch, srcRow, (size_t)rowBytes[s]);
						}

						D3D12_TEXTURE_COPY_LOCATION dst = {};
						dst.pResource = g_atlas;
						dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						dst.SubresourceIndex = s;
						D3D12_TEXTURE_COPY_LOCATION src = {};
						src.pResource = up;
						src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
						src.PlacedFootprint = fps[s];
						g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					}
					up->Unmap(0, nullptr);
					temps.push_back(up);

					D3D12_RESOURCE_BARRIER b = {};
					b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					b.Transition.pResource = g_atlas;
					b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
					barriers.push_back(b);
				}
				else
				{
					releaseAndReset(up);
					atlasOk = false;
				}
			}

			BoolType ok = g_verts && g_tris && g_nodes && g_mats && atlasOk && g_asIndex
				&& g_instBase && g_emiO2W
				&& (lights.empty() || g_lights) && (emiTri.empty() || g_emiTri) && (emiCdf.empty() || g_emiCdf);

			if (ok)
			{
				if (!barriers.empty())
					g_cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());
				ok = ExecuteAndWait();
			}
			else
			{
				WARNING("CinemaGpu12: fallo creando recursos de escena D3D12");
				g_cmd->Close(); // descartar la lista a medio grabar
			}

			for (ID3D12Resource* t : temps)
				releaseAndReset(t);

			if (!ok)
			{
				ReleaseSceneResources();
				return false;
			}

			// Descriptores SRV t0..t7 en el heap. M8-A: los conteos de tris/emisores
			// salen de los arrays SUBIDOS (instanced usa trisLocal/emiTriIdxLocal,
			// de igual longitud que el camino mundo pero por robustez se toman aqui).
			WriteBufferSrv(SRV_BASE + 0, g_verts, CinemaKernelM0::PTVERT_STRIDE, (UINT)verts.size());
			WriteBufferSrv(SRV_BASE + 1, g_tris, CinemaKernelM0::PTTRI_STRIDE, (UINT)tris.size());
			WriteBufferSrv(SRV_BASE + 2, g_nodes, CinemaKernelM0::PTNODE_STRIDE, g.numNodes);
			WriteBufferSrv(SRV_BASE + 3, g_mats, CinemaKernelM0::PTMATERIAL_STRIDE, g.numMaterials);
			WriteAtlasSrv(SRV_BASE + 4, g_atlas, numSlices);
			WriteBufferSrv(SRV_BASE + 5, g_lights, CinemaKernelM0::PTLIGHT_STRIDE, g.numLights);
			WriteBufferSrv(SRV_BASE + 6, g_emiTri, CinemaKernelM0::PTEMITRI_STRIDE, (UINT)emiTri.size());
			WriteBufferSrv(SRV_BASE + 7, g_emiCdf, CinemaKernelM0::PTEMICDF_STRIDE, (UINT)emiCdf.size());

			// Fase B / M8-A: BLAS/TLAS (RayQuery inline). instanced -> N BLAS + N
			// instancias; OFF -> 1 BLAS + instancia identidad. Fallo -> escena no
			// valida -> CinemaRender cae limpio al backend D3D11.
			BoolType asOk = instanced
				? BuildAccelStructuresInstanced((UINT)verts.size())
				: BuildAccelStructures(g.numVerts, (UINT)tris.size());
			if (!asOk)
			{
				WARNING("CinemaGpu12: build de BLAS/TLAS fallo -> escena D3D12 no disponible");
				ReleaseSceneResources();
				return false;
			}

			g_sceneUp = true;
			DEBUG(QString("CinemaGpu12: escena subida (") + (instanced ? "INSTANCIADA " : "")
				+ "verts " + NumStr((IntType)verts.size()) + ", tris "
				+ NumStr((IntType)tris.size()) + ", instancias " + NumStr((IntType)g_numInstances)
				+ ", mats " + NumStr((IntType)g.numMaterials) + ", slices " + NumStr((IntType)numSlices) + ")");
			return true;
		}
	} // namespace anonimo

	// =====================================================================
	// API publica
	// =====================================================================

	BoolType CinemaGpu12Available(QString* whyOut)
	{
		if (!g_initTried)
		{
			g_initTried = true;
			g_available = EnsureDevice() && EnsurePipeline();
			// Puerta de sanidad: el selftest prueba dxc->DXIL firmado->PSO->
			// dispatch->readback de punta a punta antes de confiar en el backend.
			if (g_available)
			{
				QString stErr;
				if (!CinemaGpu12Selftest(&stErr))
				{
					g_available = false;
					g_why = "selftest_failed:" + stErr;
				}
			}
			if (g_available)
				DEBUG("CinemaGpu12: backend DXR DISPONIBLE (" + g_backendInfo + ")");
			else
				WARNING("CinemaGpu12: backend DXR NO disponible (motivo: " + g_why + ")");
		}
		if (whyOut)
			*whyOut = g_available ? QString() : g_why;
		return g_available;
	}

	QString CinemaGpu12BackendInfo()
	{
		return g_backendInfo.isEmpty() ? QString("D3D12 (sin adaptador)") : g_backendInfo;
	}

	BoolType CinemaGpu12EnsureScene()
	{
		if (!CinemaGpu12Available(nullptr))
			return false;
		const CinemaSceneGpu& g = CinemaSceneGetGpu();
		if (!g.IsValid())
		{
			WARNING("CinemaGpu12: escena no construida");
			return false;
		}
		// Firma: GENERACION monotona del build (CinemaSceneGpu::buildGen) + conteos
		// + identidad del SRV de verts. FIX race capas (bg==full): la identidad de
		// puntero del SRV NO discrimina builds consecutivos (el allocator puede
		// devolver la MISMA direccion tras liberar el SRV del build previo) y los
		// conteos son identicos entre pasadas del export por capas (solo cambian
		// flags de material) -> ~1/14 se reusaba la escena D3D12 de la pasada
		// ANTERIOR y la capa salia byte-identica a la previa (mismo seed). La
		// generacion se bumpea en cada upload GPU completo: cada rebuild re-sube.
		UINT counts[7] = { g.numVerts, g.numTris, g.numNodes, g.numMaterials, g.numSlices, g.numLights, g.numEmiTris };
		bool same = g_sceneUp && g_sigBuildGen == g.buildGen && g_sigVertsSrv == (const void*)g.vertsSrv;
		for (int i = 0; i < 7 && same; i++)
			same = (g_sigCounts[i] == counts[i]);
		if (same)
			return true;

		if (!UploadScene())
			return false;
		g_sigBuildGen = g.buildGen;
		g_sigVertsSrv = (const void*)g.vertsSrv;
		for (int i = 0; i < 7; i++)
			g_sigCounts[i] = counts[i];
		return true;
	}

	double CinemaGpu12LastUpdateMs()
	{
		return g_lastUpdateMs;
	}

	// M8-B ("escena viva"): update TRANSFORM-ONLY sin recaptura de geometria.
	// worlds[i] = object->world nuevo por instancia (paralelo a las draw-ranges);
	// emiO2W = o2w nuevo por emisor de la CDF (paralelo a la CDF emisiva local).
	// Reescribe solo INSTANCE_DESC.Transform (las BLAS y g_verts NO se tocan),
	// reconstruye la TLAS (a N<=~154 instancias es sub-ms; el rebuild completo es
	// mas robusto que el refit tras muchos updates) y re-sube gEmiO2W (t10) y
	// gLights (t5) desde el estado fresco. El signo del determinante de las
	// instancias espejadas viaja SOLO en la matriz -> el kernel lo compensa por
	// hit (M8-A), asi que mover un objeto espejado por keyframe queda correcto.
	BoolType CinemaGpu12UpdateInstances(const std::vector<CinemaInstXform>& worlds,
		const std::vector<CinemaInstXform>& emiO2W)
	{
		g_lastUpdateMs = 0.0;
		if (!CinemaGpu12Available(nullptr))
			return false;
		const UINT N = (UINT)worlds.size();
		if (!g_sceneUp || !g_tlas || !g_blasBuf || !g_device5 || !g_cmd4 ||
			N == 0 || g_numInstances != N || (UINT)g_instDescCpu.size() != N)
		{
			WARNING("CinemaGpu12: update en vivo sin escena instanciada valida");
			return false;
		}

		LARGE_INTEGER freq, t0, t1;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);

		// 1) Reescribir SOLO la Transform de cada instancia (la AccelerationStructure
		//    VA apunta a la BLAS en g_blasBuf, que persiste intacta).
		for (UINT i = 0; i < N; i++)
			memcpy(&g_instDescCpu[i].Transform[0][0], worlds[i].m, sizeof(worlds[i].m));

		// 2) Buffer de instance-descs (upload) para la TLAS.
		ID3D12Resource* instBuf = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD,
			(UINT64)N * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
			D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		if (!instBuf)
			return false;
		{
			void* m = nullptr;
			D3D12_RANGE none = { 0, 0 };
			if (FAILED(instBuf->Map(0, &none, &m)))
			{
				releaseAndReset(instBuf);
				return false;
			}
			memcpy(m, g_instDescCpu.data(), (size_t)N * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			instBuf->Unmap(0, nullptr);
		}

		// 3) Prebuild de la TLAS (mismo N -> mismo tamano que g_tlas ya reservado).
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
		tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		tlasIn.NumDescs = N;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
		g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);
		if (!tlasInfo.ResultDataMaxSizeInBytes)
		{
			releaseAndReset(instBuf);
			return false;
		}
		ID3D12Resource* scratch = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, tlasInfo.ScratchDataSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (!scratch)
		{
			releaseAndReset(instBuf);
			return false;
		}
		tlasIn.InstanceDescs = instBuf->GetGPUVirtualAddress();

		// 4) Re-subir gEmiO2W (t10 root SRV) + gLights (t5 tabla) y reconstruir la
		//    TLAS en una sola lista. gInstBase (t9) y las BLAS NO cambian.
		const std::vector<PTLight>& lights = CinemaSceneLights();
		if (!BeginCmd())
		{
			releaseAndReset(instBuf);
			releaseAndReset(scratch);
			return false;
		}
		std::vector<ID3D12Resource*> temps;
		std::vector<D3D12_RESOURCE_BARRIER> barriers;

		std::vector<CinemaInstXform> eo = emiO2W;
		if (eo.empty())
		{
			CinemaInstXform id = {}; id.m[0] = id.m[5] = id.m[10] = 1.f;
			eo.push_back(id); // dummy: el root SRV t10 nunca queda sin ligar
		}
		releaseAndReset(g_emiO2W);
		g_emiO2W = UploadStructured(eo.data(), (UINT)sizeof(CinemaInstXform), (UINT)eo.size(), temps, barriers);

		if (!lights.empty())
		{
			releaseAndReset(g_lights);
			g_lights = UploadStructured(lights.data(), CinemaKernelM0::PTLIGHT_STRIDE, (UINT)lights.size(), temps, barriers);
			if (g_lights)
				WriteBufferSrv(SRV_BASE + 5, g_lights, CinemaKernelM0::PTLIGHT_STRIDE, (UINT)lights.size());
		}

		if (!barriers.empty())
			g_cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
		tlasDesc.Inputs = tlasIn;
		tlasDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
		tlasDesc.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
		g_cmd4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		BoolType ok = ExecuteAndWait();

		for (size_t i = 0; i < temps.size(); i++)
			releaseAndReset(temps[i]);
		releaseAndReset(instBuf);
		releaseAndReset(scratch);

		QueryPerformanceCounter(&t1);
		g_lastUpdateMs = freq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;

		if (!ok || !g_emiO2W)
		{
			WARNING("CinemaGpu12: update en vivo fallo");
			return false;
		}
		DEBUG("CinemaGpu12: update en vivo " + QString::number(g_lastUpdateMs, 'f', 3)
			+ " ms (" + NumStr((IntType)N) + " instancias, TLAS rebuild)");
		return true;
	}

	// M8-C ("escena viva", C1): update PARCIAL de GEOMETRIA (bend). changedDraws =
	// indices de instancia cuya geometria LOCAL YA se re-aplano en CinemaScene
	// (scene.vertsLocal, MISMO count de verts/tris). Reescribe SOLO el rango de verts
	// de esas instancias en g_verts (CopyBufferRegion), reconstruye SOLO sus BLAS en
	// su MISMO offset de g_blasBuf (el count no cambia -> el tamano tampoco) y luego la
	// TLAS con las transforms nuevas (+ gEmiO2W/gLights). El resto de g_verts y las
	// demas BLAS NO se tocan -> update sub-ms por parte, resultado bit-identico a un
	// build frio del mismo frame. Fallback: false -> el caller hace pt_build completo.
	BoolType CinemaGpu12UpdateGeometry(const std::vector<CinemaInstXform>& worlds,
		const std::vector<CinemaInstXform>& emiO2W, const std::vector<int>& changedDraws)
	{
		g_lastUpdateMs = 0.0;
		if (!CinemaGpu12Available(nullptr))
			return false;
		const UINT N = (UINT)worlds.size();
		if (!g_sceneUp || !g_tlas || !g_blasBuf || !g_verts || !g_asIndex || !g_device5 || !g_cmd4 ||
			N == 0 || g_numInstances != N || (UINT)g_instDescCpu.size() != N ||
			(UINT)g_blasOffset.size() != N || changedDraws.empty())
		{
			WARNING("CinemaGpu12: update de geometria sin escena instanciada valida");
			return false;
		}
		const std::vector<PTVert>& vlocal = CinemaSceneGetVertsLocal();
		const std::vector<PTDrawRange>& ranges = CinemaSceneGetDrawRanges();
		if (vlocal.empty() || (UINT)ranges.size() != N)
			return false;
		const UINT totalVerts = (UINT)vlocal.size();
		const UINT M = (UINT)changedDraws.size();

		LARGE_INTEGER freq, t0, t1;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);

		auto Align256 = [](UINT64 v) -> UINT64 {
			const UINT64 a = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
			return (v + a - 1) & ~(a - 1);
		};
		const D3D12_GPU_VIRTUAL_ADDRESS vertsVa = g_verts->GetGPUVirtualAddress();
		const D3D12_GPU_VIRTUAL_ADDRESS idxVa = g_asIndex->GetGPUVirtualAddress();
		const D3D12_GPU_VIRTUAL_ADDRESS blasBufVa = g_blasBuf->GetGPUVirtualAddress();

		// --- Reservas + staging ANTES de abrir la command list (limpio ante fallo) ---
		std::vector<ID3D12Resource*> temps;
		auto cleanup = [&]() { for (size_t z = 0; z < temps.size(); z++) releaseAndReset(temps[z]); };

		// Un UPLOAD por draw cambiado con sus verts locales frescos.
		std::vector<ID3D12Resource*> vertUp(M, nullptr);
		std::vector<UINT64> vertDstOff(M, 0), vertBytes(M, 0);
		for (UINT k = 0; k < M; k++)
		{
			int i = changedDraws[k];
			if (i < 0 || (UINT)i >= N) { cleanup(); return false; }
			const PTDrawRange& rg = ranges[(size_t)i];
			if (rg.vertStart < 0 || (UINT)(rg.vertStart + rg.vertCount) > totalVerts) { cleanup(); return false; }
			UINT64 bytes = (UINT64)rg.vertCount * CinemaKernelM0::PTVERT_STRIDE;
			vertDstOff[k] = (UINT64)rg.vertStart * CinemaKernelM0::PTVERT_STRIDE;
			vertBytes[k] = bytes;
			if (!bytes) continue;
			ID3D12Resource* up = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
			if (!up) { cleanup(); return false; }
			void* mp = nullptr; D3D12_RANGE none = { 0, 0 };
			if (FAILED(up->Map(0, &none, &mp))) { releaseAndReset(up); cleanup(); return false; }
			memcpy(mp, &vlocal[(size_t)rg.vertStart], (size_t)bytes);
			up->Unmap(0, nullptr);
			vertUp[k] = up;
			temps.push_back(up);
		}

		// Prebuild de las BLAS cambiadas -> offsets de scratch.
		std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos(M);
		std::vector<UINT64> scratchOff(M, 0);
		UINT64 scratchCursor = 0;
		for (UINT k = 0; k < M; k++)
		{
			const PTDrawRange& rg = ranges[(size_t)changedDraws[k]];
			D3D12_RAYTRACING_GEOMETRY_DESC& geo = geos[k]; geo = {};
			geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // NON_OPAQUE (igual que el build)
			geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geo.Triangles.IndexCount = (UINT)rg.triCount * 3u;
			geo.Triangles.VertexCount = totalVerts; // indices GLOBALES sobre g_verts
			geo.Triangles.IndexBuffer = idxVa + (UINT64)rg.triStart * 3ull * sizeof(uint32_t);
			geo.Triangles.VertexBuffer.StartAddress = vertsVa;
			geo.Triangles.VertexBuffer.StrideInBytes = CinemaKernelM0::PTVERT_STRIDE;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
			in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			in.NumDescs = 1; in.pGeometryDescs = &geo;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
			g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
			scratchOff[k] = scratchCursor;
			scratchCursor += Align256(info.ScratchDataSizeInBytes);
		}

		// TLAS prebuild (mismo N -> mismo tamano que g_tlas ya reservado).
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
		tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		tlasIn.NumDescs = N;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
		g_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);
		UINT64 scratchBytes = scratchCursor > tlasInfo.ScratchDataSizeInBytes ? scratchCursor : tlasInfo.ScratchDataSizeInBytes;

		ID3D12Resource* scratch = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, scratchBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (!scratch) { cleanup(); return false; }
		temps.push_back(scratch);
		const D3D12_GPU_VIRTUAL_ADDRESS scratchVa = scratch->GetGPUVirtualAddress();

		// Instancias con las transforms nuevas.
		for (UINT i = 0; i < N; i++)
			memcpy(&g_instDescCpu[i].Transform[0][0], worlds[i].m, sizeof(worlds[i].m));
		ID3D12Resource* instBuf = CreateBuffer(D3D12_HEAP_TYPE_UPLOAD,
			(UINT64)N * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		if (!instBuf) { cleanup(); return false; }
		temps.push_back(instBuf);
		{
			void* m = nullptr; D3D12_RANGE none = { 0, 0 };
			if (FAILED(instBuf->Map(0, &none, &m))) { cleanup(); return false; }
			memcpy(m, g_instDescCpu.data(), (size_t)N * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			instBuf->Unmap(0, nullptr);
		}
		tlasIn.InstanceDescs = instBuf->GetGPUVirtualAddress();

		// --- Command list ---
		if (!BeginCmd()) { cleanup(); return false; }

		// 1) g_verts NON_PIXEL_SHADER_RESOURCE -> COPY_DEST, copiar rangos, volver.
		auto transVerts = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = g_verts;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = before;
			b.Transition.StateAfter = after;
			g_cmd->ResourceBarrier(1, &b);
		};
		transVerts(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		for (UINT k = 0; k < M; k++)
			if (vertUp[k] && vertBytes[k])
				g_cmd->CopyBufferRegion(g_verts, vertDstOff[k], vertUp[k], 0, vertBytes[k]);
		transVerts(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		// 2) rebuild PARCIAL de las BLAS cambiadas (mismo offset).
		for (UINT k = 0; k < M; k++)
		{
			int i = changedDraws[k];
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
			in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			in.NumDescs = 1; in.pGeometryDescs = &geos[k];
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d = {};
			d.Inputs = in;
			d.ScratchAccelerationStructureData = scratchVa + scratchOff[k];
			d.DestAccelerationStructureData = blasBufVa + g_blasOffset[(size_t)i];
			g_cmd4->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
		}
		// barrera UAV global: verts copiados + BLAS completas antes de que la TLAS lea.
		D3D12_RESOURCE_BARRIER uav = {};
		uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = nullptr;
		g_cmd->ResourceBarrier(1, &uav);

		// 3) re-subir gEmiO2W (t10) + gLights (t5), como el update transform-only.
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		std::vector<CinemaInstXform> eo = emiO2W;
		if (eo.empty()) { CinemaInstXform id = {}; id.m[0] = id.m[5] = id.m[10] = 1.f; eo.push_back(id); }
		releaseAndReset(g_emiO2W);
		g_emiO2W = UploadStructured(eo.data(), (UINT)sizeof(CinemaInstXform), (UINT)eo.size(), temps, barriers);
		const std::vector<PTLight>& lights = CinemaSceneLights();
		if (!lights.empty())
		{
			releaseAndReset(g_lights);
			g_lights = UploadStructured(lights.data(), CinemaKernelM0::PTLIGHT_STRIDE, (UINT)lights.size(), temps, barriers);
			if (g_lights)
				WriteBufferSrv(SRV_BASE + 5, g_lights, CinemaKernelM0::PTLIGHT_STRIDE, (UINT)lights.size());
		}
		if (!barriers.empty())
			g_cmd->ResourceBarrier((UINT)barriers.size(), barriers.data());

		// 4) TLAS rebuild (reusa el scratch, barrera UAV mediante).
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
		tlasDesc.Inputs = tlasIn;
		tlasDesc.ScratchAccelerationStructureData = scratchVa;
		tlasDesc.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
		g_cmd4->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		BoolType ok = ExecuteAndWait();
		cleanup();

		QueryPerformanceCounter(&t1);
		g_lastUpdateMs = freq.QuadPart ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
		if (!ok || !g_emiO2W)
		{
			WARNING("CinemaGpu12: update de geometria fallo");
			return false;
		}
		DEBUG("CinemaGpu12: update GEOMETRIA " + QString::number(g_lastUpdateMs, 'f', 3)
			+ " ms (" + NumStr((IntType)M) + " BLAS parciales de " + NumStr((IntType)N) + " + verts + TLAS)");
		return true;
	}

	void CinemaGpu12ReleaseTargets()
	{
		releaseAndReset(g_accum);
		releaseAndReset(g_aovAlb);
		releaseAndReset(g_aovNrm);
		releaseAndReset(g_varCount);
		releaseAndReset(g_readback);
		g_w = 0;
		g_h = 0;
	}

	BoolType CinemaGpu12Begin(IntType w, IntType h)
	{
		if (!CinemaGpu12Available(nullptr) || !g_sceneUp)
			return false;
		// TRIM (fix oilslick-en-Cinema-congelado, parte 2): CinemaGpu12Available()
		// solo llama a EnsurePipeline() la PRIMERA vez del proceso (guarda
		// g_initTried) -- antes de que exista ninguna escena real, cuando
		// CinemaSceneTrimSlots() esta vacio. El chequeo de trimKey que
		// EnsurePipeline() hace ahora (ver ahi) nunca se re-ejecutaba porque
		// nada volvia a llamarla. Aqui SI hay escena real (g_sceneUp), asi
		// que reintentarla en cada Begin() -- barata cuando no cambio nada,
		// recompila el PSO cuando la escena trae un efecto de trim distinto
		// (o el primero) del que vio la sonda de disponibilidad.
		if (!EnsurePipeline())
			return false;
		if (w < 1 || h < 1)
			return false;

		if (w != g_w || h != g_h || !g_accum)
		{
			CinemaGpu12ReleaseTargets();
			UINT64 planeBytes = (UINT64)w * h * 16; // float4
			g_accum  = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, planeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			g_aovAlb = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, planeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			g_aovNrm = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, planeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			g_varCount = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, planeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); // M7 (u3)
			g_readback = CreateBuffer(D3D12_HEAP_TYPE_READBACK, planeBytes * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
			if (!g_accum || !g_aovAlb || !g_aovNrm || !g_varCount || !g_readback)
			{
				CinemaGpu12ReleaseTargets();
				WARNING("CinemaGpu12: reserva de targets D3D12 fallo (" + NumStr(w) + "x" + NumStr(h) + ")");
				return false;
			}
			g_w = w;
			g_h = h;

			// Descriptores UAV u0..u3 (M7: +varCount)
			UINT count = (UINT)(w * h);
			WriteBufferUav(UAV_BASE + 0, g_accum, count);
			WriteBufferUav(UAV_BASE + 1, g_aovAlb, count);
			WriteBufferUav(UAV_BASE + 2, g_aovNrm, count);
			WriteBufferUav(UAV_BASE + 3, g_varCount, count);

			if (g_adapter3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO mem = {};
				if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem)))
					DEBUG("CinemaGpu12: targets " + NumStr((IntType)(planeBytes * 4 / (1024 * 1024)))
						+ " MB; VRAM en uso " + NumStr((IntType)(mem.CurrentUsage / (1024 * 1024))) + " MB");
			}
		}

		// CSClear del frame entero: PTFrame con res_clamp.x/y en el slot 0.
		if (!BeginCmd())
			return false;
		memset(g_cbvMapped, 0, CBV_STRIDE);
		float* res = (float*)(g_cbvMapped + 224); // offset de res_clamp
		res[0] = (float)w;
		res[1] = (float)h;
		BindCompute(g_psoClear);
		g_cmd->SetComputeRootConstantBufferView(0, g_cbvBaseVa);
		g_cmd->Dispatch(GroupsFor(w), GroupsFor(h), 1);
		UavBarrierTargets();
		return ExecuteAndWait();
	}

	BoolType CinemaGpu12Sample(const void* frame256, IntType sampleIdxBase,
		IntType sppCount, IntType tileSize, IntType flushBudgetPix)
	{
		if (!g_available || !g_accum || !frame256)
			return false;
		if (sppCount < 1)
			sppCount = 1;
		if (tileSize < 1)
			tileSize = g_w > g_h ? g_w : g_h;

		if (!BeginCmd())
			return false;
		BindCompute(g_psoPath);

		UINT ringIdx = 0;
		IntType pixelsSinceFlush = 0;

		// Cierra el chunk actual (drena la GPU), reabre lista y re-liga estado, y
		// resetea el anillo CBV (seguro: la GPU quedo drenada). Devuelve false si
		// falla la re-apertura.
		auto flushChunk = [&]() -> BoolType
		{
			if (!ExecuteAndWait())
				return false;
			if (!BeginCmd())
				return false;
			BindCompute(g_psoPath);
			ringIdx = 0;
			pixelsSinceFlush = 0;
			return true;
		};

		for (IntType s = 0; s < sppCount; s++)
		{
			// Barrera UAV entre samples consecutivos (RMW sobre los mismos
			// pixeles). Antes del sample 0 no hace falta (Begin/anterior Sample
			// drenaron), pero es inofensiva.
			if (s > 0)
				UavBarrierTargets();

			for (IntType ty = 0; ty < g_h; ty += tileSize)
			{
				for (IntType tx = 0; tx < g_w; tx += tileSize)
				{
					IntType tileW = g_w - tx < tileSize ? g_w - tx : tileSize;
					IntType tileH = g_h - ty < tileSize ? g_h - ty : tileSize;

					// Copiar el PTFrame al slot del anillo y fijar tile.xy/z.
					uint8_t* slot = g_cbvMapped + (UINT64)ringIdx * CBV_STRIDE;
					memcpy(slot, frame256, PTFRAME_SIZE);
					uint32_t* tile = (uint32_t*)(slot + 208); // offset de 'tile'
					tile[0] = (uint32_t)tx;
					tile[1] = (uint32_t)ty;
					tile[2] = (uint32_t)(sampleIdxBase + s);
					// tile[3] (semilla) ya viene en frame256

					g_cmd->SetComputeRootConstantBufferView(0, g_cbvBaseVa + (UINT64)ringIdx * CBV_STRIDE);
					g_cmd->Dispatch(GroupsFor(tileW), GroupsFor(tileH), 1);
					ringIdx++;
					pixelsSinceFlush += tileW * tileH;

					if (ringIdx >= CBV_SLOTS || pixelsSinceFlush >= flushBudgetPix)
					{
						if (!flushChunk())
							return false;
					}
				}
			}
		}

		return ExecuteAndWait();
	}

	BoolType CinemaGpu12Readback(float* accum, float* aovAlbedo, float* aovNormalDepth, float* varCount)
	{
		if (!g_available || !g_accum || !g_readback)
			return false;

		UINT64 planeBytes = (UINT64)g_w * g_h * 16;
		if (!BeginCmd())
			return false;

		// accum/AOVs/varCount: UNORDERED_ACCESS -> COPY_SOURCE, copiar a los 4 planos
		// del readback, y volver a UNORDERED_ACCESS. (M7: 4o plano = varCount, para
		// el resolve por conteo por pixel, el heatmap y la terminacion temprana.)
		ID3D12Resource* srcs[4] = { g_accum, g_aovAlb, g_aovNrm, g_varCount };
		D3D12_RESOURCE_BARRIER pre[4] = {};
		D3D12_RESOURCE_BARRIER post[4] = {};
		for (int i = 0; i < 4; i++)
		{
			pre[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			pre[i].Transition.pResource = srcs[i];
			pre[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			pre[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			pre[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			post[i] = pre[i];
			post[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			post[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}
		g_cmd->ResourceBarrier(4, pre);
		for (int i = 0; i < 4; i++)
			g_cmd->CopyBufferRegion(g_readback, (UINT64)i * planeBytes, srcs[i], 0, planeBytes);
		g_cmd->ResourceBarrier(4, post);
		if (!ExecuteAndWait())
			return false;

		uint8_t* mapped = nullptr;
		D3D12_RANGE readAll = { 0, (SIZE_T)(planeBytes * 4) };
		if (FAILED(g_readback->Map(0, &readAll, (void**)&mapped)))
		{
			WARNING("CinemaGpu12: Map del readback fallo");
			return false;
		}
		if (accum)
			memcpy(accum, mapped, (size_t)planeBytes);
		if (aovAlbedo)
			memcpy(aovAlbedo, mapped + planeBytes, (size_t)planeBytes);
		if (aovNormalDepth)
			memcpy(aovNormalDepth, mapped + planeBytes * 2, (size_t)planeBytes);
		if (varCount)
			memcpy(varCount, mapped + planeBytes * 3, (size_t)planeBytes);
		D3D12_RANGE noWrite = { 0, 0 };
		g_readback->Unmap(0, &noWrite);
		return true;
	}

	// ---------------------------------------------------------------------
	// Selftest: kernel cs_6_5 trivial via dxc con su propia root signature
	// (root CBV b0 + root UAV u0), dispatch, readback y verificacion CPU.
	// ---------------------------------------------------------------------

	BoolType CinemaGpu12Selftest(QString* errorOut)
	{
		QString err;
		auto done = [&](BoolType ok) -> BoolType
		{
			if (errorOut)
				*errorOut = ok ? QString() : err;
			if (ok)
				DEBUG("CinemaGpu12: selftest DXR OK");
			else
				WARNING("CinemaGpu12: selftest DXR fallo: " + err);
			return ok;
		};

		if (!EnsureDevice())
			return done((err = g_why, false));
		if (!DxcLoad())
			return done((err = "dxc_no_disponible", false));

		const UINT N = 64;
		const UINT SEED = 7;
		static const char* SELFTEST_HLSL = R"hlsl(
cbuffer P : register(b0) { uint4 uParams; }; // x=w, y=h, z=seed
RWStructuredBuffer<float4> uOut : register(u0);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= uParams.x || id.y >= uParams.y)
		return;
	uint p = id.y * uParams.x + id.x;
	float4 r;
	r.x = float(id.x ^ id.y) / 255.0;
	r.y = float((id.x + 2u * id.y + uParams.z) & 1023u) / 1023.0;
	r.z = float((7u * id.x + 13u * id.y) & 255u) / 255.0;
	r.w = 1.0;
	uOut[p] = r;
}
)hlsl";

		IDxcBlob* dxil = DxcCompile(SELFTEST_HLSL, L"CSMain", "selftest", false);
		ID3D12RootSignature* rs = nullptr;
		ID3D12PipelineState* pso = nullptr;
		ID3D12Resource* outBuf = nullptr;
		ID3D12Resource* rb = nullptr;

		BoolType ok = dxil != nullptr;

		if (ok)
		{
			// Root sig: root CBV b0 + root UAV u0
			D3D12_ROOT_PARAMETER params[2] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
			params[1].Descriptor.ShaderRegister = 0;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			D3D12_ROOT_SIGNATURE_DESC rsd = {};
			rsd.NumParameters = 2;
			rsd.pParameters = params;
			ID3DBlob* blob = nullptr;
			ID3DBlob* e = nullptr;
			ok = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &e));
			if (ok)
				ok = SUCCEEDED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
			else if (!err.length())
				err = "root_sig_selftest";
			releaseAndReset(e);
			releaseAndReset(blob);
		}
		else if (!err.length())
			err = "dxc_selftest_compile";

		if (ok)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = rs;
			pd.CS.pShaderBytecode = dxil->GetBufferPointer();
			pd.CS.BytecodeLength = dxil->GetBufferSize();
			ok = SUCCEEDED(g_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
			if (!ok && !err.length())
				err = "pso_selftest";
		}

		UINT count = N * N;
		UINT64 bytes = (UINT64)count * 16;
		if (ok)
		{
			outBuf = CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			rb = CreateBuffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
			ok = outBuf && rb;
			if (!ok && !err.length())
				err = "buffers_selftest";
		}

		if (ok)
		{
			// Params por root CBV: usar el anillo CBV comun (slot 0)
			if (!EnsurePipeline())
				ok = false;
		}

		if (ok && BeginCmd())
		{
			memset(g_cbvMapped, 0, CBV_STRIDE);
			uint32_t* p = (uint32_t*)g_cbvMapped;
			p[0] = N; p[1] = N; p[2] = SEED; p[3] = 0;
			g_cmd->SetComputeRootSignature(rs);
			g_cmd->SetPipelineState(pso);
			g_cmd->SetComputeRootConstantBufferView(0, g_cbvBaseVa);
			g_cmd->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());
			g_cmd->Dispatch(N / 8, N / 8, 1);
			D3D12_RESOURCE_BARRIER pre = {};
			pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			pre.Transition.pResource = outBuf;
			pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			pre.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			g_cmd->ResourceBarrier(1, &pre);
			g_cmd->CopyBufferRegion(rb, 0, outBuf, 0, bytes);
			ok = ExecuteAndWait();
		}
		else
			ok = false;

		if (ok)
		{
			uint8_t* mapped = nullptr;
			D3D12_RANGE readAll = { 0, (SIZE_T)bytes };
			if (SUCCEEDED(rb->Map(0, &readAll, (void**)&mapped)))
			{
				const float* px = (const float*)mapped;
				const float eps = 1.0e-5f;
				for (UINT y = 0; y < N && ok; y++)
					for (UINT x = 0; x < N && ok; x++)
					{
						float e0 = (float)(x ^ y) / 255.0f;
						float e1 = (float)((x + 2u * y + SEED) & 1023u) / 1023.0f;
						float e2 = (float)((7u * x + 13u * y) & 255u) / 255.0f;
						const float* g = px + (y * N + x) * 4;
						if (!(fabsf(g[0] - e0) <= eps && fabsf(g[1] - e1) <= eps && fabsf(g[2] - e2) <= eps && fabsf(g[3] - 1.f) <= eps))
						{
							err = "mismatch@" + NumStr((IntType)x) + "," + NumStr((IntType)y);
							ok = false;
						}
					}
				D3D12_RANGE noWrite = { 0, 0 };
				rb->Unmap(0, &noWrite);
			}
			else
			{
				err = "readback_selftest";
				ok = false;
			}
		}

		releaseAndReset(rb);
		releaseAndReset(outBuf);
		releaseAndReset(pso);
		releaseAndReset(rs);
		releaseAndReset(dxil);
		return done(ok);
	}
}

#else // !API_D3D11

// Cinema es un motor D3D11/Windows; en OpenGL (Mac/Linux) el backend DXR no
// existe. Stubs que reportan no-disponible (el caller cae a su propio camino).
namespace CppProject
{
	BoolType CinemaGpu12Available(QString* whyOut) { if (whyOut) *whyOut = "cinema_requires_d3d11"; return false; }
	QString CinemaGpu12BackendInfo() { return QString(); }
	BoolType CinemaGpu12Selftest(QString* errorOut) { if (errorOut) *errorOut = "cinema_requires_d3d11"; return false; }
	BoolType CinemaGpu12EnsureScene() { return false; }
	BoolType CinemaGpu12Begin(IntType, IntType) { return false; }
	BoolType CinemaGpu12Sample(const void*, IntType, IntType, IntType, IntType) { return false; }
	BoolType CinemaGpu12Readback(float*, float*, float*, float*) { return false; }
	void CinemaGpu12ReleaseTargets() {}
}

#endif // API_D3D11
