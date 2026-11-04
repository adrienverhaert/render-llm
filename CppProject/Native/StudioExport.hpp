#pragma once
namespace CppProject::StudioUi {
void Export(bool video);
// Refuse editor teardown until the export host has drained its owned renderer.
bool StopExportBeforeExit();
}
