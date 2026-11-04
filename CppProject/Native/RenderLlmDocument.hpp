#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace CppProject
{
	class RenderLlmDocument
	{
	public:
		static constexpr qint64 MaxDocumentBytes = 16 * 1024 * 1024;

		bool Parse(const QByteArray& bytes, QString* error = nullptr);
		bool Load(const QString& path, QString* error = nullptr);
		bool Save(const QString& path, QString* error = nullptr) const;

		QByteArray CanonicalJson() const;
		const QJsonObject& Root() const { return root; }

	private:
		static bool Validate(const QJsonObject& candidate, QString* error);
		QJsonObject root;
	};
}
