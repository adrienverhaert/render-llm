# Windows build

The supported target is Windows x64. Build the committed C++ sources; GameMaker and
the GML generator are not required. Linux and macOS are not verified native targets.

Install Visual Studio Build Tools with the C++ workload and Windows SDK, CMake,
Node.js 22+, and Qt 5.15.2 for MSVC 2019 x64. A newer compatible MSVC compiler can
consume that Qt package. The Windows Visual C++ runtime must be installed for the
shared Qt libraries. Python is needed only if using `aqtinstall` to obtain Qt.

## Dependencies

Install Qt from its official distribution, or in a separate Python environment:

```powershell
python -m venv build/qt-installer
build/qt-installer/Scripts/python.exe -m pip install aqtinstall==3.3.0
build/qt-installer/Scripts/python.exe -m aqt install-qt windows desktop 5.15.2 win64_msvc2019_64 --archives qtbase -O build/qt
```

Download the native headers and FreeType source:

```powershell
./tools/fetch-native-sources.ps1
```

The script verifies all four archives against `tools/native-sources.json`, retains
their source and notices under `build/dependencies`, and rejects an incomplete
existing destination rather than deleting it. `tar` must be available on PATH.
Precompiled media/audio/zip libraries remain in `CppProject/External/Win64`.

## Diagnostic build

```powershell
./tools/build-native.ps1 -BuildPath build/native `
  -QtPrefix build/qt/5.15.2/msvc2019_64 `
  -DependencyRoot build/dependencies -AllowGpuFallback

ctest --test-dir build/native -C Release --output-on-failure
```

The build compiles FreeType explicitly, chooses the installed Visual Studio CMake
generator, stages the required Qt DLLs and platform plugin, and builds the agent
from its npm lockfile. It overlays immutable resources without replacing user
projects or agent state. Use a fresh build directory for packaging.

The diagnostic option permits a D3D11/fallback package. It does not prove DXR or
OIDN execution. The native executable is `build/native/Release/render-llm.exe`.

## Verified GPU runtime

For a GPU candidate, omit `-AllowGpuFallback` and supply `-OidnBin` and `-DxcBin`
together. `tools/native-runtime.json` pins every runtime DLL by SHA-256; staging
rejects a mismatch. The pinned OIDN 2.3.3 and DXC 1.8.2502.8 files come from public
vendor releases. Download and verify both archives and all seven runtime DLLs:

```powershell
./tools/fetch-gpu-runtime.ps1
./tools/build-native.ps1 -BuildPath build/native-gpu `
  -QtPrefix build/qt/5.15.2/msvc2019_64 -DependencyRoot build/dependencies `
  -OidnBin build/gpu-runtime/verified -DxcBin build/gpu-runtime/verified
```

Run a real render and inspect its actual backend and denoiser. A selected option
or successful build is not proof of the GPU path.

## Local tests and packaging

```powershell
./tools/test-still-export.ps1 -Runtime build/native/Release
./tools/package-local-test.ps1 -Runtime build/native/Release
```

The archive tool is for local candidates, not public releases. It copies only
allowlisted resources, verifies the staged agent, extracts the ZIP and checks each
file's hash. It requires the pinned GPU runtime. Run the extracted executable
separately; archive integrity does not prove startup or rendering.

Native chat needs the user's own supported Codex CLI/Desktop installation and
login. Node is not bundled. No model call is made by the free regression suite.
See [STATUS.md](STATUS.md) and [LICENSING.md](../LICENSING.md) before distributing.
