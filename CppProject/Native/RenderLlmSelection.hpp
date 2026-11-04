#pragma once
#include <QString>
namespace CppProject {
// Presentation selection only: no document mutation or new history transaction.
QString RenderLlmSelectedObject();
bool RenderLlmSelectObject(const QString& objectId);
}
