#include "RenderLlmDocument.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace CppProject
{
	namespace
	{
		void SetError(QString* error, const QString& message)
		{
			if (error)
				*error = message;
		}

		bool Fail(QString* error, const QString& message)
		{
			SetError(error, message);
			return false;
		}

		bool IsFiniteNumber(const QJsonValue& value, double minimum, double maximum)
		{
			if (!value.isDouble())
				return false;
			const double number = value.toDouble();
			return std::isfinite(number) && number >= minimum && number <= maximum;
		}

		bool IsInteger(const QJsonValue& value, int minimum, int maximum)
		{
			return IsFiniteNumber(value, minimum, maximum)
				&& std::floor(value.toDouble()) == value.toDouble();
		}

		bool IsId(const QString& value)
		{
			static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{1,80}$"));
			return expression.match(value).hasMatch();
		}

		bool IsColor(const QJsonValue& value)
		{
			static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
			return value.isString() && expression.match(value.toString()).hasMatch();
		}

		bool IsVector3(const QJsonValue& value, double minimum = -1000000.0, double maximum = 1000000.0)
		{
			if (!value.isArray() || value.toArray().size() != 3)
				return false;
			for (const QJsonValue& component : value.toArray())
				if (!IsFiniteNumber(component, minimum, maximum))
					return false;
			return true;
		}

		bool HasOnlyFields(const QJsonObject& object, const QSet<QString>& fields, const QString& context,
			QString* error)
		{
			for (const QString& key : object.keys())
				if (!fields.contains(key))
					return Fail(error, QStringLiteral("Unknown %1 field: %2").arg(context, key));
			return true;
		}

		bool ValidateMaterial(const QString& id, const QJsonValue& value, QString* error)
		{
			if (!IsId(id) || !value.isObject())
				return Fail(error, QStringLiteral("Invalid material entry: %1").arg(id));
			const QJsonObject material = value.toObject();
			const QSet<QString> fields = { QStringLiteral("color"), QStringLiteral("metallic"),
				QStringLiteral("roughness"), QStringLiteral("emission") };
			if (!HasOnlyFields(material, fields, QStringLiteral("material"), error)
				|| material.size() != fields.size()
				|| !IsColor(material.value(QStringLiteral("color")))
				|| !IsFiniteNumber(material.value(QStringLiteral("metallic")), 0.0, 1.0)
				|| !IsFiniteNumber(material.value(QStringLiteral("roughness")), 0.0, 1.0)
				|| !IsFiniteNumber(material.value(QStringLiteral("emission")), 0.0, 1000000.0))
				return Fail(error, QStringLiteral("Invalid material properties: %1").arg(id));
			return true;
		}

		bool ValidateScene(const QJsonObject& scene, const QJsonObject& materials,
			QHash<QString, QString>* objectKinds, QString* error)
		{
			const QSet<QString> sceneFields = { QStringLiteral("objects"), QStringLiteral("world"),
				QStringLiteral("active_camera_id") };
			if (!HasOnlyFields(scene, sceneFields, QStringLiteral("scene"), error)
				|| !scene.value(QStringLiteral("objects")).isArray()
				|| !scene.value(QStringLiteral("world")).isObject())
				return Fail(error, QStringLiteral("Scene requires objects and world."));

			const QJsonArray objects = scene.value(QStringLiteral("objects")).toArray();
			if (objects.size() > 1000)
				return Fail(error, QStringLiteral("Scene exceeds the 1000 object limit."));
			const QSet<QString> kinds = { QStringLiteral("cube"), QStringLiteral("cone"),
				QStringLiteral("cylinder"), QStringLiteral("sphere"), QStringLiteral("plane"),
				QStringLiteral("camera"), QStringLiteral("point"), QStringLiteral("area"), QStringLiteral("sun"), QStringLiteral("group") };
			const QSet<QString> primitives = { QStringLiteral("cube"), QStringLiteral("cone"),
				QStringLiteral("cylinder"), QStringLiteral("sphere"), QStringLiteral("plane") };
			const QSet<QString> lights = { QStringLiteral("point"), QStringLiteral("area"), QStringLiteral("sun") };
			const QSet<QString> objectFields = { QStringLiteral("id"), QStringLiteral("name"),
				QStringLiteral("kind"), QStringLiteral("position"), QStringLiteral("rotation"),
				QStringLiteral("scale"), QStringLiteral("visible"), QStringLiteral("parent_id"),
				QStringLiteral("material_id"), QStringLiteral("camera"), QStringLiteral("light") };

			QHash<QString, QString> parents;
			int sunCount = 0;
			for (const QJsonValue& value : objects)
			{
				if (!value.isObject())
					return Fail(error, QStringLiteral("Scene objects must be objects."));
				const QJsonObject object = value.toObject();
				if (!HasOnlyFields(object, objectFields, QStringLiteral("object"), error))
					return false;
				const QString id = object.value(QStringLiteral("id")).toString();
				const QString name = object.value(QStringLiteral("name")).toString();
				const QString kind = object.value(QStringLiteral("kind")).toString();
				if (!IsId(id) || objectKinds->contains(id))
					return Fail(error, QStringLiteral("Invalid or duplicate object id: %1").arg(id));
				if (name.isEmpty() || name.size() > 120 || !kinds.contains(kind)
					|| !IsVector3(object.value(QStringLiteral("position")))
					|| !IsVector3(object.value(QStringLiteral("rotation")))
					|| !IsVector3(object.value(QStringLiteral("scale")), -10000.0, 10000.0)
					|| !object.value(QStringLiteral("visible")).isBool())
					return Fail(error, QStringLiteral("Invalid object properties: %1").arg(id));
				objectKinds->insert(id, kind);

				if (primitives.contains(kind))
				{
					if (object.contains(QStringLiteral("camera")) || object.contains(QStringLiteral("light")))
						return Fail(error, QStringLiteral("Primitive has incompatible properties: %1").arg(id));
					const QString materialId = object.value(QStringLiteral("material_id")).toString();
					if (!IsId(materialId) || !materials.contains(materialId))
						return Fail(error, QStringLiteral("Unknown material reference on object %1: %2").arg(id, materialId));
				}
				else if (kind == QStringLiteral("group"))
				{
					if (object.contains(QStringLiteral("material_id")) || object.contains(QStringLiteral("camera"))
						|| object.contains(QStringLiteral("light")))
						return Fail(error, QStringLiteral("Group has incompatible render properties: %1").arg(id));
				}
				else if (kind == QStringLiteral("camera"))
				{
					if (object.contains(QStringLiteral("material_id")) || object.contains(QStringLiteral("light")))
						return Fail(error, QStringLiteral("Camera has incompatible properties: %1").arg(id));
					if (!object.value(QStringLiteral("camera")).isObject())
						return Fail(error, QStringLiteral("Camera object requires camera properties: %1").arg(id));
					const QJsonObject camera = object.value(QStringLiteral("camera")).toObject();
					const QSet<QString> fields = { QStringLiteral("fov_y_deg"), QStringLiteral("orthographic"),
						QStringLiteral("ortho_height"), QStringLiteral("dof"), QStringLiteral("focus_distance") };
					if (!HasOnlyFields(camera, fields, QStringLiteral("camera"), error) || camera.size() != fields.size()
						|| !IsFiniteNumber(camera.value(QStringLiteral("fov_y_deg")), 1.0, 179.0)
						|| !camera.value(QStringLiteral("orthographic")).isBool()
						|| !IsFiniteNumber(camera.value(QStringLiteral("ortho_height")), 0.0001, 1000000.0)
						|| !camera.value(QStringLiteral("dof")).isBool()
						|| !IsFiniteNumber(camera.value(QStringLiteral("focus_distance")), 0.0, 1000000.0))
						return Fail(error, QStringLiteral("Invalid camera properties: %1").arg(id));
				}
				else if (lights.contains(kind))
				{
					if (kind == QStringLiteral("sun") && ++sunCount > 1)
						return Fail(error, QStringLiteral("A document may contain at most one sun light."));
					if (object.contains(QStringLiteral("material_id")) || object.contains(QStringLiteral("camera")))
						return Fail(error, QStringLiteral("Light has incompatible properties: %1").arg(id));
					if (!object.value(QStringLiteral("light")).isObject())
						return Fail(error, QStringLiteral("Light object requires light properties: %1").arg(id));
					const QJsonObject light = object.value(QStringLiteral("light")).toObject();
					const QSet<QString> fields = { QStringLiteral("color"), QStringLiteral("power"), QStringLiteral("size") };
					if (!HasOnlyFields(light, fields, QStringLiteral("light"), error) || light.size() != fields.size()
						|| !IsColor(light.value(QStringLiteral("color")))
						|| !IsFiniteNumber(light.value(QStringLiteral("power")), 0.0, 1000000000.0)
						|| !IsFiniteNumber(light.value(QStringLiteral("size")), 0.0, 1000000.0))
						return Fail(error, QStringLiteral("Invalid light properties: %1").arg(id));
				}
			}

			for (const QJsonValue& value : objects)
			{
				const QJsonObject object = value.toObject();
				if (object.contains(QStringLiteral("parent_id")))
				{
					const QString parent = object.value(QStringLiteral("parent_id")).toString();
					if (!objectKinds->contains(parent) || parent == object.value(QStringLiteral("id")).toString())
						return Fail(error, QStringLiteral("Invalid parent reference: %1").arg(parent));
					parents.insert(object.value(QStringLiteral("id")).toString(), parent);
				}
			}
			for (auto it = parents.begin(); it != parents.end(); ++it)
			{
				QSet<QString> visited;
				QString current = it.key();
				while (parents.contains(current))
				{
					if (visited.contains(current))
						return Fail(error, QStringLiteral("Parent cycle includes object: %1").arg(current));
					visited.insert(current);
					current = parents.value(current);
				}
			}
			if (scene.contains(QStringLiteral("active_camera_id")))
			{
				const QString camera = scene.value(QStringLiteral("active_camera_id")).toString();
				if (objectKinds->value(camera) != QStringLiteral("camera"))
					return Fail(error, QStringLiteral("Active camera is not a camera object: %1").arg(camera));
			}

			const QJsonObject world = scene.value(QStringLiteral("world")).toObject();
			const QSet<QString> fields = {"color","strength","ambient_color","sun_color","elevation","rotation","angular_size","fog_enabled","fog_color","fog_distance","fog_height"};
			if (!HasOnlyFields(world, fields, QStringLiteral("world"), error)
				|| !IsColor(world.value(QStringLiteral("color")))
				|| !IsFiniteNumber(world.value(QStringLiteral("strength")), 0.0, 1000000.0))
				return Fail(error, QStringLiteral("Invalid world properties."));
			for(const QString& key : {QString("ambient_color"),QString("sun_color"),QString("fog_color")})
				if(world.contains(key)&&!IsColor(world.value(key)))return Fail(error,"Invalid world color: "+key);
			for(const QString& key : {QString("elevation"),QString("rotation"),QString("angular_size"),QString("fog_distance"),QString("fog_height")})
				if(world.contains(key)&&!IsFiniteNumber(world.value(key),key=="elevation"||key=="rotation"?-1000000.:0.,1000000.))return Fail(error,"Invalid world setting: "+key);
			if(world.contains("fog_enabled")&&!world.value("fog_enabled").isBool())return Fail(error,"Invalid fog state");
			return true;
		}

		bool ValidateTrackValue(const QString& property, const QJsonValue& value,
			const QHash<QString, QString>& objectKinds)
		{
			if (property == QStringLiteral("position") || property == QStringLiteral("rotation")
				|| property == QStringLiteral("scale"))
				return IsVector3(value, -10000.0, 10000.0);
			if (property == QStringLiteral("visible") || property == QStringLiteral("camera.dof"))
				return value.isBool();
			if (property.endsWith(QStringLiteral(".color")))
				return IsColor(value);
			if (property == QStringLiteral("camera.projection"))
				return value.isString() && (value.toString() == QStringLiteral("perspective")
					|| value.toString() == QStringLiteral("orthographic"));
			if (property == QStringLiteral("scene.active_camera_id"))
				return value.isString() && objectKinds.value(value.toString()) == QStringLiteral("camera");
			if (property == QStringLiteral("material.metallic") || property == QStringLiteral("material.roughness"))
				return IsFiniteNumber(value, 0.0, 1.0);
			if (property == QStringLiteral("camera.fov_y_deg"))
				return IsFiniteNumber(value, 1.0, 179.0);
			if (property == QStringLiteral("camera.ortho_height"))
				return IsFiniteNumber(value, 0.0001, 1000000.0);
			if (property == QStringLiteral("render.samples"))
				return IsInteger(value, 1, 65536);
			return IsFiniteNumber(value, 0.0, 1000000000.0);
		}

		bool ValidateAnimation(const QJsonObject& animation, const QHash<QString, QString>& objectKinds,
			QString* error)
		{
			const QSet<QString> fields = { QStringLiteral("schema"), QStringLiteral("fps"),
				QStringLiteral("frame_count"), QStringLiteral("tracks"), QStringLiteral("constraints") };
			if (!HasOnlyFields(animation, fields, QStringLiteral("animation"), error)
				|| animation.size() != fields.size()
				|| !IsInteger(animation.value(QStringLiteral("schema")), 1, 1)
				|| !IsInteger(animation.value(QStringLiteral("fps")), 1, 240)
				|| !IsInteger(animation.value(QStringLiteral("frame_count")), 1, 100000)
				|| !animation.value(QStringLiteral("tracks")).isArray()
				|| !animation.value(QStringLiteral("constraints")).isArray())
				return Fail(error, QStringLiteral("Invalid animation header."));
			if (!animation.value(QStringLiteral("constraints")).toArray().isEmpty())
				return Fail(error, QStringLiteral("Animation constraints are not supported yet."));

			const QJsonArray tracks = animation.value(QStringLiteral("tracks")).toArray();
			if (tracks.size() > 4096)
				return Fail(error, QStringLiteral("Animation exceeds the 4096 track limit."));
			const int frameCount = animation.value(QStringLiteral("frame_count")).toInt();
			const QSet<QString> properties = { QStringLiteral("position"), QStringLiteral("rotation"),
				QStringLiteral("scale"), QStringLiteral("visible"), QStringLiteral("material.color"),
				QStringLiteral("material.metallic"), QStringLiteral("material.roughness"),
				QStringLiteral("material.emission"), QStringLiteral("camera.fov_y_deg"),
				QStringLiteral("camera.ortho_height"), QStringLiteral("camera.focus_distance"),
				QStringLiteral("camera.dof"), QStringLiteral("camera.projection"),
				QStringLiteral("light.color"), QStringLiteral("light.power"), QStringLiteral("light.size"),
				QStringLiteral("world.color"), QStringLiteral("world.strength"),
				QStringLiteral("render.samples"), QStringLiteral("scene.active_camera_id") };
			const QSet<QString> easings = { QStringLiteral("linear"), QStringLiteral("instant"),
				QStringLiteral("bezier"), QStringLiteral("smooth"), QStringLiteral("easeinquad"),
				QStringLiteral("easeoutquad"), QStringLiteral("easeinoutquad"), QStringLiteral("easeincubic"),
				QStringLiteral("easeoutcubic"), QStringLiteral("easeinoutcubic"), QStringLiteral("easeinquart"),
				QStringLiteral("easeoutquart"), QStringLiteral("easeinoutquart"), QStringLiteral("easeinquint"),
				QStringLiteral("easeoutquint"), QStringLiteral("easeinoutquint"), QStringLiteral("easeinsine"),
				QStringLiteral("easeoutsine"), QStringLiteral("easeinoutsine"), QStringLiteral("easeinexpo"),
				QStringLiteral("easeoutexpo"), QStringLiteral("easeinoutexpo"), QStringLiteral("easeincirc"),
				QStringLiteral("easeoutcirc"), QStringLiteral("easeinoutcirc"), QStringLiteral("easeinelastic"),
				QStringLiteral("easeoutelastic"), QStringLiteral("easeinoutelastic"), QStringLiteral("easeinback"),
				QStringLiteral("easeoutback"), QStringLiteral("easeinoutback"), QStringLiteral("easeinbounce"),
				QStringLiteral("easeoutbounce"), QStringLiteral("easeinoutbounce") };
			QSet<QString> trackIds;
			QSet<QString> bindings;
			QHash<QString, QByteArray> keyInterpolation;
			for (const QJsonValue& value : tracks)
			{
				if (!value.isObject())
					return Fail(error, QStringLiteral("Animation tracks must be objects."));
				const QJsonObject track = value.toObject();
				const QSet<QString> trackFields = { QStringLiteral("id"), QStringLiteral("target"),
					QStringLiteral("property"), QStringLiteral("keys") };
				if (!HasOnlyFields(track, trackFields, QStringLiteral("track"), error) || track.size() != trackFields.size())
					return Fail(error, QStringLiteral("Invalid animation track fields."));
				const QString id = track.value(QStringLiteral("id")).toString();
				const QString target = track.value(QStringLiteral("target")).toString();
				const QString property = track.value(QStringLiteral("property")).toString();
				if (!IsId(id) || trackIds.contains(id))
					return Fail(error, QStringLiteral("Invalid or duplicate track id: %1").arg(id));
				if ((!objectKinds.contains(target) && target != QStringLiteral("$world")
					&& target != QStringLiteral("$render") && target != QStringLiteral("$scene"))
					|| !properties.contains(property))
					return Fail(error, QStringLiteral("Invalid track binding: %1.%2").arg(target, property));
				const QString targetKind = objectKinds.value(target);
				const bool objectTransform = objectKinds.contains(target)
					&& (property == QStringLiteral("position") || property == QStringLiteral("rotation")
						|| property == QStringLiteral("visible"));
				const bool primitiveKind = targetKind == QStringLiteral("cube") || targetKind == QStringLiteral("cone")
					|| targetKind == QStringLiteral("cylinder") || targetKind == QStringLiteral("sphere")
					|| targetKind == QStringLiteral("plane");
				const bool primitiveScale = primitiveKind && property == QStringLiteral("scale");
				const bool materialProperty = primitiveKind && property.startsWith(QStringLiteral("material."));
				const bool cameraProperty = targetKind == QStringLiteral("camera")
					&& property.startsWith(QStringLiteral("camera."));
				const bool lightProperty = (targetKind == QStringLiteral("point") || targetKind == QStringLiteral("area")
					|| targetKind == QStringLiteral("sun")) && property.startsWith(QStringLiteral("light."));
				const bool globalProperty = (target == QStringLiteral("$world") && property.startsWith(QStringLiteral("world.")))
					|| (target == QStringLiteral("$render") && property == QStringLiteral("render.samples"))
					|| (target == QStringLiteral("$scene") && property == QStringLiteral("scene.active_camera_id"));
				if (!objectTransform && !primitiveScale && !materialProperty && !cameraProperty && !lightProperty && !globalProperty)
					return Fail(error, QStringLiteral("Track property is incompatible with its target: %1.%2").arg(target, property));
				const QString binding = target + QLatin1Char(':') + property;
				if (bindings.contains(binding))
					return Fail(error, QStringLiteral("Duplicate track binding: %1").arg(binding));
				trackIds.insert(id);
				bindings.insert(binding);
				if (!track.value(QStringLiteral("keys")).isArray() || track.value(QStringLiteral("keys")).toArray().isEmpty()
					|| track.value(QStringLiteral("keys")).toArray().size() > 100000)
					return Fail(error, QStringLiteral("Invalid key list on track: %1").arg(id));
				int previousFrame = -1;
				for (const QJsonValue& keyValue : track.value(QStringLiteral("keys")).toArray())
				{
					if (!keyValue.isObject())
						return Fail(error, QStringLiteral("Track keys must be objects: %1").arg(id));
					const QJsonObject key = keyValue.toObject();
					const QSet<QString> keyFields = { QStringLiteral("frame"), QStringLiteral("value"),
						QStringLiteral("easing"), QStringLiteral("bezier") };
					if (!HasOnlyFields(key, keyFields, QStringLiteral("key"), error)
						|| !key.contains(QStringLiteral("value")) || !key.value(QStringLiteral("easing")).isString())
						return Fail(error, QStringLiteral("Invalid key fields on track: %1").arg(id));
					if (!IsInteger(key.value(QStringLiteral("frame")), 0, frameCount - 1)
						|| key.value(QStringLiteral("frame")).toInt() <= previousFrame)
						return Fail(error, QStringLiteral("Key frames must be integral, increasing, and in range: %1").arg(id));
					previousFrame = key.value(QStringLiteral("frame")).toInt();
					const QString easing = key.value(QStringLiteral("easing")).toString();
					if (!easings.contains(easing) || !ValidateTrackValue(property, key.value(QStringLiteral("value")), objectKinds))
						return Fail(error, QStringLiteral("Invalid key value or easing on track: %1").arg(id));
					if (easing == QStringLiteral("bezier"))
					{
						if (!key.value(QStringLiteral("bezier")).isArray() || key.value(QStringLiteral("bezier")).toArray().size() != 4)
							return Fail(error, QStringLiteral("Bezier key requires four handles: %1").arg(id));
						for (const QJsonValue& handle : key.value(QStringLiteral("bezier")).toArray())
							if (!IsFiniteNumber(handle, -1000000.0, 1000000.0))
								return Fail(error, QStringLiteral("Invalid Bezier handle: %1").arg(id));
					}
					else if (key.contains(QStringLiteral("bezier")))
						return Fail(error, QStringLiteral("Bezier handles require bezier easing: %1").arg(id));
					QJsonObject interpolation { { QStringLiteral("easing"), easing } };
					if (key.contains(QStringLiteral("bezier")))
						interpolation.insert(QStringLiteral("bezier"), key.value(QStringLiteral("bezier")));
					const QByteArray signature = QJsonDocument(interpolation).toJson(QJsonDocument::Compact);
					const QString keySlot = target + QLatin1Char(':') + QString::number(previousFrame);
					if (keyInterpolation.contains(keySlot) && keyInterpolation.value(keySlot) != signature)
						return Fail(error, QStringLiteral("Tracks sharing a target/frame must share interpolation: %1").arg(keySlot));
					keyInterpolation.insert(keySlot, signature);
				}
			}
			return true;
		}

		bool HasRenderLlmExtension(const QString& path)
		{
			return QFileInfo(path).suffix().compare(QStringLiteral("renderllm"), Qt::CaseInsensitive) == 0;
		}

		QJsonValue SortJson(const QJsonValue& value)
		{
			if (value.isArray())
			{
				QJsonArray sorted;
				for (const QJsonValue& item : value.toArray())
					sorted.append(SortJson(item));
				return sorted;
			}
			if (!value.isObject())
				return value;

			const QJsonObject object = value.toObject();
			QStringList keys = object.keys();
			std::sort(keys.begin(), keys.end());
			QJsonObject sorted;
			for (const QString& key : keys)
				sorted.insert(key, SortJson(object.value(key)));
			return sorted;
		}

		bool ContainsCredentialField(const QJsonValue& value, QString* keyFound)
		{
			static const QSet<QString> forbidden = {
				QStringLiteral("api_key"), QStringLiteral("apikey"),
				QStringLiteral("token"), QStringLiteral("access_token"),
				QStringLiteral("refresh_token"), QStringLiteral("secret"),
				QStringLiteral("password"), QStringLiteral("authorization")
			};

			if (value.isArray())
			{
				for (const QJsonValue& item : value.toArray())
					if (ContainsCredentialField(item, keyFound))
						return true;
				return false;
			}
			if (!value.isObject())
				return false;

			const QJsonObject object = value.toObject();
			for (auto it = object.begin(); it != object.end(); ++it)
			{
				if (forbidden.contains(it.key().toLower()))
				{
					if (keyFound)
						*keyFound = it.key();
					return true;
				}
				if (ContainsCredentialField(it.value(), keyFound))
					return true;
			}
			return false;
		}
	}

	bool RenderLlmDocument::Validate(const QJsonObject& candidate, QString* error)
	{
		static const QStringList required = {
			QStringLiteral("version"), QStringLiteral("scene"),
			QStringLiteral("materials"), QStringLiteral("animation"),
			QStringLiteral("render"), QStringLiteral("agent")
		};
		const QSet<QString> allowed(required.begin(), required.end());

		for (const QString& key : required)
		{
			if (!candidate.contains(key))
			{
				SetError(error, QStringLiteral("Missing required top-level field: %1").arg(key));
				return false;
			}
		}
		for (const QString& key : candidate.keys())
		{
			if (!allowed.contains(key))
			{
				SetError(error, QStringLiteral("Unknown top-level field: %1").arg(key));
				return false;
			}
		}

		const QJsonValue version = candidate.value(QStringLiteral("version"));
		if (!version.isDouble() || version.toDouble() != 1.0)
		{
			SetError(error, QStringLiteral("Unsupported render-llm document version."));
			return false;
		}
		for (const QString& key : { QStringLiteral("scene"), QStringLiteral("materials"),
			QStringLiteral("animation"), QStringLiteral("render"), QStringLiteral("agent") })
		{
			if (!candidate.value(key).isObject())
			{
				SetError(error, QStringLiteral("Top-level field must be an object: %1").arg(key));
				return false;
			}
		}

		const QJsonObject materials = candidate.value(QStringLiteral("materials")).toObject();
		if (materials.size() > 1000)
			return Fail(error, QStringLiteral("Document exceeds the 1000 material limit."));
		for (auto it = materials.begin(); it != materials.end(); ++it)
			if (!ValidateMaterial(it.key(), it.value(), error))
				return false;

		QHash<QString, QString> objectKinds;
		if (!ValidateScene(candidate.value(QStringLiteral("scene")).toObject(), materials, &objectKinds, error))
			return false;

		const QJsonObject render = candidate.value(QStringLiteral("render")).toObject();
		const QSet<QString> renderFields = {"renderer","samples","schema","bounces","noise_threshold","denoise","exposure","overscan","tiled"};
		if (!HasOnlyFields(render, renderFields, QStringLiteral("render"), error)
			|| (render.contains(QStringLiteral("schema")) && render.value(QStringLiteral("schema")).toDouble() != 2)
			|| (render.value(QStringLiteral("renderer")).toString() != QStringLiteral("Flame")
				&& render.value(QStringLiteral("renderer")).toString() != QStringLiteral("UltraFlame"))
			|| !IsInteger(render.value(QStringLiteral("samples")), 1, 65536))
			return Fail(error, QStringLiteral("Render settings require Flame or UltraFlame and valid samples."));
		for(const QString& key : {QString("denoise"),QString("tiled")})
			if(render.contains(key)&&!render.value(key).isBool())return Fail(error,"Invalid render switch: "+key);
		for(const QString& key : {QString("noise_threshold"),QString("exposure"),QString("overscan")})
			if(render.contains(key)&&!IsFiniteNumber(render.value(key),0.,key=="noise_threshold"?1.:10000.))return Fail(error,"Invalid render value: "+key);
		if(render.contains("bounces")&&!IsInteger(render.value("bounces"),0,64))return Fail(error,"Invalid bounce count");

		if (!ValidateAnimation(candidate.value(QStringLiteral("animation")).toObject(), objectKinds, error))
			return false;

		QString credentialKey;
		if (ContainsCredentialField(candidate, &credentialKey))
		{
			SetError(error, QStringLiteral("Credentials are not allowed in projects: %1").arg(credentialKey));
			return false;
		}
		return true;
	}

	bool RenderLlmDocument::Parse(const QByteArray& bytes, QString* error)
	{
		if (bytes.size() > MaxDocumentBytes)
		{
			SetError(error, QStringLiteral("Project exceeds the 16 MiB document limit."));
			return false;
		}

		QJsonParseError parseError;
		const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &parseError);
		if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
		{
			SetError(error, QStringLiteral("Invalid render-llm JSON: %1").arg(parseError.errorString()));
			return false;
		}
		const QJsonObject candidate = parsed.object();
		if (!Validate(candidate, error))
			return false;

		root = candidate;
		if (error)
			error->clear();
		return true;
	}

	bool RenderLlmDocument::Load(const QString& path, QString* error)
	{
		if (!HasRenderLlmExtension(path))
		{
			SetError(error, QStringLiteral("Project filename must end in .renderllm."));
			return false;
		}
		const QFileInfo info(path);
		if (!info.exists() || !info.isFile())
		{
			SetError(error, QStringLiteral("Project is not a regular file: %1").arg(path));
			return false;
		}
		if (info.size() > MaxDocumentBytes)
		{
			SetError(error, QStringLiteral("Project exceeds the 16 MiB document limit."));
			return false;
		}

		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			SetError(error, QStringLiteral("Could not open project: %1").arg(file.errorString()));
			return false;
		}
		return Parse(file.readAll(), error);
	}

	bool RenderLlmDocument::Save(const QString& path, QString* error) const
	{
		if (!HasRenderLlmExtension(path))
		{
			SetError(error, QStringLiteral("Project filename must end in .renderllm."));
			return false;
		}
		if (!Validate(root, error))
			return false;

		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly))
		{
			SetError(error, QStringLiteral("Could not open project for writing: %1").arg(file.errorString()));
			return false;
		}
		const QByteArray bytes = CanonicalJson();
		if (file.write(bytes) != bytes.size() || !file.commit())
		{
			SetError(error, QStringLiteral("Could not atomically save project: %1").arg(file.errorString()));
			return false;
		}
		if (error)
			error->clear();
		return true;
	}

	QByteArray RenderLlmDocument::CanonicalJson() const
	{
		return QJsonDocument(SortJson(root).toObject()).toJson(QJsonDocument::Compact) + '\n';
	}
}
