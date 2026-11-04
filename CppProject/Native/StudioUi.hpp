#pragma once
#include "Common.hpp"
#include <QString>
#include <QStringList>
#include <functional>
namespace CppProject::StudioUi {
// Presentation only: retained action handlers continue to own state and undo.
void Install();
void Workspace();
void Surface(double x, double y, double w, double h, IntType color, double alpha = 1, double radius = 7);
void Field(double x, double y, double w, double h, bool focused, bool error = false);
void Toggle(double x, double y, bool on, double alpha = 1);
void ColorDialog(IntType script, IntType color);
void PickColor(IntType color, std::function<void(IntType)> accepted);
void AgentSettings();
void AddObject();
void Look();
void Objects();
void Motion();
void ReviewTiming();
int Message(const QString& title, const QString& text, const QStringList& buttons, int safeChoice);
bool HasModal();
QString StyleSheet();
}
