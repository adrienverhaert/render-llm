#pragma once
namespace CppProject {
// Checkpoint a retained native edit before it mutates the scene. Document
// replacement suppresses this hook; UI/MCP/HTTP then share the same undo stack.
void RenderLlmCheckpointNativeEdit();
}
