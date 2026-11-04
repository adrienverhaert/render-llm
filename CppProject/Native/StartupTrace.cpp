#include "StartupTrace.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace CppProject::StartupTrace
{
	void Mark(const QString& stage)
	{
		if (!qEnvironmentVariableIsSet("RENDER_LLM_STARTUP_TRACE"))
			return;

		QFile file(QDir::current().filePath(QStringLiteral("Data/renderllm-startup.log")));
		if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
			return;

		QTextStream stream(&file);
		stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
		       << ' ' << stage << '\n';
	}

	void Mark(const char* stage)
	{
		Mark(QString::fromLatin1(stage));
	}
}
