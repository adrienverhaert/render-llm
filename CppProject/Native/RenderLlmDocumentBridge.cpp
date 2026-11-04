#include "RenderLlmDocument.hpp"
#include "RenderLlmSelection.hpp"
#include "RenderLlmAnimation.hpp"
#include "RenderLlmHistory.hpp"
#include "RenderLlmProjectDialog.hpp"

#include "Asset/DataStructure.hpp"
#include "Asset/Object.hpp"
#include "Generated/GmlFunc.hpp"
#include "Generated/Scripts.hpp"
#include "Render/Cinema/CinemaScene.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QVector>
#include <QSet>
#include <QScopedValueRollback>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QCryptographicHash>
#include <QSaveFile>

#include <algorithm>

namespace CppProject
{
	namespace
	{
		QString renderLlmDocumentError;
		QString currentProjectName;
		QByteArray currentProjectHash;
		QByteArray cleanProjectSnapshot;
		QByteArray PersistentProjectState(const QByteArray& bytes) {
			if (bytes.isEmpty()) return {};
			auto root = QJsonDocument::fromJson(bytes).object();if (root.isEmpty()) return {};
			auto scene = root["scene"].toObject();auto objects = scene["objects"].toArray();auto materials = root["materials"].toObject();
			// Observations include evaluated values at the playhead. Authored tracks,
			// not those derived values, determine whether a saved project has changed.
			for (const auto& value : root["animation"].toObject()["tracks"].toArray()) {
				const auto track = value.toObject();if (track["keys"].toArray().isEmpty()) continue;
				const auto target = track["target"].toString(), property = track["property"].toString();
				if (target == "$world" && property.startsWith("world.")) {
					auto world = scene["world"].toObject();world.remove(property.mid(6));scene["world"] = world;continue;
				}
				for (int i = 0; i < objects.size(); ++i) {
					auto object = objects[i].toObject();if (object["id"].toString() != target) continue;
					if (property.startsWith("material.")) {
						const auto id = object["material_id"].toString();auto material = materials[id].toObject();material.remove(property.mid(9));materials[id] = material;
					} else if (property.contains('.')) {
						const auto group = property.section('.',0,0);auto nested = object[group].toObject();nested.remove(property == "camera.projection" ? "orthographic" : property.section('.',1));object[group] = nested;
					} else object.remove(property);
					objects[i] = object;break;
				}
			}
			scene["objects"] = objects;root["scene"] = scene;root["materials"] = materials;
			return QJsonDocument(root).toJson(QJsonDocument::Compact);
		}
		QByteArray ProjectFileHash(const QString& path) {
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly)) return {};
			return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
		}

		struct RuntimeIdentity
		{
			QString saveId;
			QString objectId;
			QString materialId;
			QString kind;
			QHash<QString, QString> trackIds;
			QHash<QString, QSet<IntType>> trackKeys;
			QSet<IntType> importedKeys;
		};

		QHash<IntType, RuntimeIdentity> runtimeIdentities;
		QVector<QByteArray> undoDocuments;
		QVector<QByteArray> redoDocuments;
		IntType documentRevision = 0;
		bool replacingDocument = false;
		struct SunOverrideState
		{
			bool active = false;
			VecType direction;
			VarType color;
			IntType colorFinal = 0;
			VarType strength;
			VarType angle;
		} sunOverride;

		const RuntimeIdentity* RuntimeIdentityFor(IntType timeline)
		{
			const auto it = runtimeIdentities.constFind(timeline);
			if (it == runtimeIdentities.cend()
				|| it->saveId != idVar(timeline, save_id).ToStr().QStr())
				return nullptr;
			return &it.value();
		}

		bool TrackOwnsKey(const RuntimeIdentity& identity, const QString& property, IntType key)
		{
			// New keys made by retained whole-object timeline gestures affect its
			// authored channels. Imported sparse keys belong only to their property.
			return identity.trackKeys.value(property).contains(key) || !identity.importedKeys.contains(key);
		}

		QString PropertyForChannel(IntType channel)
		{
			switch (channel) {
			case e_value_POS_X: case e_value_POS_Y: case e_value_POS_Z: return "position";
			case e_value_ROT_X: case e_value_ROT_Y: case e_value_ROT_Z: return "rotation";
			case e_value_SCA_X: case e_value_SCA_Y: case e_value_SCA_Z: return "scale";
			case e_value_VISIBLE: return "visible";
			case e_value_RGB_MUL: return "material.color";
			case e_value_METALLIC: return "material.metallic";
			case e_value_ROUGHNESS: return "material.roughness";
			case e_value_EMISSIVE: return "material.emission";
			case e_value_CAM_FOV: return "camera.fov_y_deg";
			case e_value_CAM_ORTHO_SCALE: return "camera.ortho_height";
			case e_value_CAM_DOF_DEPTH: return "camera.focus_distance";
			case e_value_CAM_DOF: return "camera.dof";
			case e_value_CAM_ORTHO: return "camera.projection";
			case e_value_LIGHT_COLOR: return "light.color";
			case e_value_LIGHT_STRENGTH: return "light.power";
			case e_value_LIGHT_SIZE: return "light.size";
			case e_value_BG_SKY_COLOR: return "world.color";
			case e_value_BG_SUNLIGHT_STRENGTH: return "world.strength";
			default: return {};
			}
		}

		IntType DocumentLightType(IntType timeline)
		{
			if (const RuntimeIdentity* identity = RuntimeIdentityFor(timeline))
			{
				if (identity->kind == QStringLiteral("area")) return 2;
				if (identity->kind == QStringLiteral("sun")) return 3;
			}
			return idVar(timeline, type).ToInt() == e_tl_type_SPOT_LIGHT ? 1 : 0;
		}

		BoolType SyncDocumentSun()
		{
			sunOverride.active = false;
			const List& timelines = DsList(global::_app->project_timeline_list);
			for (IntType index = 0; index < timelines.vec.size(); ++index)
			{
				const IntType timeline = timelines.Value(index).ToInt();
				const RuntimeIdentity* identity = RuntimeIdentityFor(timeline);
				if (!identity || identity->kind != QStringLiteral("sun"))
					continue;
				sunOverride.active = true;
				sunOverride.direction = global::_app->background_sun_direction;
				sunOverride.color = global::_app->background_sunlight_color;
				sunOverride.colorFinal = global::_app->background_sunlight_color_final;
				sunOverride.strength = global::_app->background_sunlight_strength;
				sunOverride.angle = global::_app->background_sunlight_angle;
				const VarType& values = idVar(timeline, value);
				if (!idArr(timeline, value_inherit).Value(e_value_VISIBLE).ToBool()
					|| idVar(timeline, hide).ToBool())
				{
					global::_app->background_sunlight_strength = 0;
					return true;
				}
				VecType look = point3D_mul_matrix(point3D(0.0001, 1, 0), idVar(timeline, matrix));
				const RealType dx = look.Real(0) - idVar(timeline, world_pos).Value(X_).ToReal();
				const RealType dy = look.Real(1) - idVar(timeline, world_pos).Value(Y_).ToReal();
				const RealType dz = look.Real(2) - idVar(timeline, world_pos).Value(Z_).ToReal();
				const RealType length = std::max<RealType>(0.000001, std::sqrt(dx * dx + dy * dy + dz * dz));
				global::_app->background_sun_direction = vec3(-dx / length, -dy / length, -dz / length);
				const IntType color = values.Value(e_value_LIGHT_COLOR).ToInt();
				global::_app->background_sunlight_color = color;
				global::_app->background_sunlight_color_final = color;
				global::_app->background_sunlight_strength = values.Value(e_value_LIGHT_STRENGTH).ToReal();
				global::_app->background_sunlight_angle = std::max<RealType>(
					0.001, values.Value(e_value_LIGHT_SIZE).ToReal());
				return true;
			}
			return false;
		}

		void RestoreDocumentSun()
		{
			if (!sunOverride.active)
				return;
			global::_app->background_sun_direction = sunOverride.direction;
			global::_app->background_sunlight_color = sunOverride.color;
			global::_app->background_sunlight_color_final = sunOverride.colorFinal;
			global::_app->background_sunlight_strength = sunOverride.strength;
			global::_app->background_sunlight_angle = sunOverride.angle;
			sunOverride.active = false;
		}

		QString HexColor(IntType color)
		{
			return QStringLiteral("#%1%2%3")
				.arg(color_get_red(color), 2, 16, QLatin1Char('0'))
				.arg(color_get_green(color), 2, 16, QLatin1Char('0'))
				.arg(color_get_blue(color), 2, 16, QLatin1Char('0'))
				.toUpper();
		}

		QString TimelineId(IntType timeline)
		{
			if (const RuntimeIdentity* identity = RuntimeIdentityFor(timeline))
				return identity->objectId;
			return QStringLiteral("obj-%1").arg(idVar(timeline, save_id).ToStr().QStr());
		}

		QString MaterialId(IntType timeline)
		{
			if (const RuntimeIdentity* identity = RuntimeIdentityFor(timeline); identity && !identity->materialId.isEmpty())
				return identity->materialId;
			return QStringLiteral("mat-%1").arg(idVar(timeline, save_id).ToStr().QStr());
		}

		QString KindForTimeline(IntType timeline, IntType type)
		{
			if (const RuntimeIdentity* identity = RuntimeIdentityFor(timeline);
				identity && !identity->kind.isEmpty() && identity->objectId != QStringLiteral("$world"))
				return identity->kind;
			switch (type)
			{
				case e_tl_type_CUBE: return QStringLiteral("cube");
				case e_tl_type_FOLDER: return QStringLiteral("group");
				case e_tl_type_CONE: return QStringLiteral("cone");
				case e_tl_type_CYLINDER: return QStringLiteral("cylinder");
				case e_tl_type_SPHERE: return QStringLiteral("sphere");
				case e_tl_type_SURFACE: return QStringLiteral("plane");
				case e_tl_type_CAMERA: return QStringLiteral("camera");
				case e_tl_type_POINT_LIGHT: return QStringLiteral("point");
				default: return QString();
			}
		}

		QJsonArray Vector3(IntType timeline, IntType x, IntType y, IntType z)
		{
			const VarType& values = idVar(timeline, value);
			return { values.Value(x).ToReal(), values.Value(y).ToReal(), values.Value(z).ToReal() };
		}

		enum class TrackValue
		{
			Number,
			Boolean,
			Color,
			Vector,
			Projection
		};

		bool AppendTrack(QJsonArray& tracks, IntType timeline, const QString& property,
			std::initializer_list<IntType> channels, TrackValue encoding, IntType& maxFrame,
			const QString& target = QString())
		{
			const List& keyframes = DsList(idInt(timeline, keyframe_list));
			// A legacy base key is implicit; an explicitly authored one-key track is
			// not. Dropping it here loses new Motion/MCP keys on save and reload.
			const RuntimeIdentity* authored = RuntimeIdentityFor(timeline);
			const bool explicitTrack = authored && authored->trackIds.contains(property);
			if (authored && !explicitTrack) {
				bool addedByTimeline = false;
				for (const auto& key : keyframes.vec) if (!authored->importedKeys.contains(key.value.ToInt())) {addedByTimeline = true;break;}
				if (!addedByTimeline) return true;
			}
			if (keyframes.vec.empty() || (keyframes.vec.size() < 2 && !authored))
				return true;

			QJsonArray keys;
			for (IntType index = 0; index < keyframes.vec.size(); ++index)
			{
				const IntType keyframe = keyframes.Value(index).ToInt();
				if (explicitTrack && !TrackOwnsKey(*authored, property, keyframe)) continue;
				const RealType sourceFrame = idVar(keyframe, position).ToReal();
				const IntType frame = static_cast<IntType>(sourceFrame);
				if (sourceFrame != static_cast<RealType>(frame) || frame < 0)
				{
					renderLlmDocumentError = QStringLiteral("Fractional or negative keyframe is not representable: %1 at %2")
						.arg(idVar(timeline, display_name).ToStr().QStr())
						.arg(sourceFrame, 0, 'g', 12);
					return false;
				}
				maxFrame = std::max(maxFrame, frame);
				const VarType& values = idVar(keyframe, value);
				QJsonValue value;
				const IntType first = *channels.begin();
				switch (encoding)
				{
					case TrackValue::Number:
						value = values.Value(first).ToReal();
						break;
					case TrackValue::Boolean:
						value = values.Value(first).ToBool();
						break;
					case TrackValue::Color:
						value = HexColor(values.Value(first).ToInt());
						break;
					case TrackValue::Projection:
						value = values.Value(first).ToBool() ? QStringLiteral("orthographic") : QStringLiteral("perspective");
						break;
					case TrackValue::Vector:
					{
						auto channel = channels.begin();
						const RealType x = values.Value(*channel++).ToReal();
						const RealType y = values.Value(*channel++).ToReal();
						const RealType z = values.Value(*channel).ToReal();
						value = QJsonArray { x, y, z };
						break;
					}
				}

				const QString easing = values.Value(e_value_TRANSITION).ToStr().QStr();
				QJsonObject key {
					{ QStringLiteral("frame"), frame },
					{ QStringLiteral("value"), value },
					{ QStringLiteral("easing"), easing }
				};
				if (easing == QStringLiteral("bezier"))
				{
					key.insert(QStringLiteral("bezier"), QJsonArray {
						values.Value(e_value_EASE_IN_X).ToReal(),
						values.Value(e_value_EASE_IN_Y).ToReal(),
						values.Value(e_value_EASE_OUT_X).ToReal(),
						values.Value(e_value_EASE_OUT_Y).ToReal()
					});
				}
				keys.append(key);
			}

			if (keys.isEmpty()) return true;
			QString trackId;
			if (const RuntimeIdentity* identity = RuntimeIdentityFor(timeline))
				trackId = identity->trackIds.value(property);
			if (trackId.isEmpty())
			{
				trackId = QStringLiteral("track-%1-%2")
					.arg(idVar(timeline, save_id).ToStr().QStr(), property);
				trackId.replace(QLatin1Char('.'), QLatin1Char('-'));
			}
			tracks.append(QJsonObject {
				{ QStringLiteral("id"), trackId },
				{ QStringLiteral("target"), target.isEmpty() ? TimelineId(timeline) : target },
				{ QStringLiteral("property"), property },
				{ QStringLiteral("keys"), keys }
			});
			return true;
		}

		IntType PrimitiveTemplateType(const QString& kind)
		{
			if (kind == QStringLiteral("cube")) return e_temp_type_CUBE;
			if (kind == QStringLiteral("cone")) return e_temp_type_CONE;
			if (kind == QStringLiteral("cylinder")) return e_temp_type_CYLINDER;
			if (kind == QStringLiteral("sphere")) return e_temp_type_SPHERE;
			if (kind == QStringLiteral("plane")) return e_temp_type_SURFACE;
			return -1;
		}

		IntType CreatePrimitive(const QString& kind)
		{
			const IntType templateType = PrimitiveTemplateType(kind);
			if (templateType < 0)
				return 0;
			obj_template* objectTemplate = new obj_template;
			ScopeAny templateScope(objectTemplate->id);
			temp_event_create(templateScope);
			objectTemplate->type = templateType;
			temp_update_shape(templateScope);
			temp_update_rot_point(templateScope);
			temp_update_display_name(templateScope);
			const IntType timeline = temp_animate(templateScope);
			sortlist_add(global::_app->lib_list, objectTemplate->id);
			objectTemplate->creator = global::_app->id;
			return timeline;
		}

		IntType CreateTimeline(const QString& kind)
		{
			if (kind == QStringLiteral("group"))
				return new_tl(e_tl_type_FOLDER);
			const IntType primitive = PrimitiveTemplateType(kind);
			if (primitive >= 0)
				return CreatePrimitive(kind);
			if (kind == QStringLiteral("camera"))
				return new_tl(e_tl_type_CAMERA);
			if (kind == QStringLiteral("point"))
				return new_tl(e_tl_type_POINT_LIGHT);
			if (kind == QStringLiteral("area") || kind == QStringLiteral("sun"))
				return new_tl(e_tl_type_POINT_LIGHT);
			return 0;
		}

		IntType ColorValue(const QJsonValue& value)
		{
			return hex_to_color(StringType(value.toString().mid(1))).ToInt();
		}

		void SetVector(VarType& values, IntType x, IntType y, IntType z, const QJsonValue& value)
		{
			const QJsonArray vector = value.toArray();
			values[x] = vector.at(0).toDouble();
			values[y] = vector.at(1).toDouble();
			values[z] = vector.at(2).toDouble();
		}

		void SetObjectBase(IntType timeline, const QJsonObject& object, const QJsonObject& materials)
		{
			obj_timeline* runtime = ObjType(obj_timeline, timeline);
			SetVector(runtime->value, e_value_POS_X, e_value_POS_Y, e_value_POS_Z,
				object.value(QStringLiteral("position")));
			SetVector(runtime->value, e_value_ROT_X, e_value_ROT_Y, e_value_ROT_Z,
				object.value(QStringLiteral("rotation")));
			SetVector(runtime->value, e_value_SCA_X, e_value_SCA_Y, e_value_SCA_Z,
				object.value(QStringLiteral("scale")));
			runtime->value[e_value_VISIBLE] = object.value(QStringLiteral("visible")).toBool();
			runtime->hide = false;

			const QString kind = object.value(QStringLiteral("kind")).toString();
			if (PrimitiveTemplateType(kind) >= 0)
			{
				const QJsonObject material = materials.value(object.value(QStringLiteral("material_id")).toString()).toObject();
				runtime->value[e_value_RGB_MUL] = ColorValue(material.value(QStringLiteral("color")));
				runtime->value[e_value_METALLIC] = material.value(QStringLiteral("metallic")).toDouble();
				runtime->value[e_value_ROUGHNESS] = material.value(QStringLiteral("roughness")).toDouble();
				runtime->value[e_value_EMISSIVE] = material.value(QStringLiteral("emission")).toDouble();
			}
			else if (kind == QStringLiteral("camera"))
			{
				const QJsonObject camera = object.value(QStringLiteral("camera")).toObject();
				runtime->value[e_value_CAM_FOV] = camera.value(QStringLiteral("fov_y_deg")).toDouble();
				runtime->value[e_value_CAM_ORTHO] = camera.value(QStringLiteral("orthographic")).toBool();
				runtime->value[e_value_CAM_ORTHO_SCALE] = camera.value(QStringLiteral("ortho_height")).toDouble();
				runtime->value[e_value_CAM_DOF] = camera.value(QStringLiteral("dof")).toBool();
				runtime->value[e_value_CAM_DOF_DEPTH] = camera.value(QStringLiteral("focus_distance")).toDouble();
			}
			else if (kind == QStringLiteral("point") || kind == QStringLiteral("area")
				|| kind == QStringLiteral("sun"))
			{
				const QJsonObject light = object.value(QStringLiteral("light")).toObject();
				runtime->value[e_value_LIGHT_COLOR] = ColorValue(light.value(QStringLiteral("color")));
				runtime->value[e_value_LIGHT_STRENGTH] = light.value(QStringLiteral("power")).toDouble();
				runtime->value[e_value_LIGHT_SIZE] = light.value(QStringLiteral("size")).toDouble();
			}
			for (IntType value = 0; value < e_value_amount; ++value)
				runtime->value_default[value] = runtime->value.Value(value);
		}

		IntType FindOrAddKeyframe(IntType timeline, IntType frame)
		{
			const List& keyframes = DsList(idInt(timeline, keyframe_list));
			for (IntType index = 0; index < keyframes.vec.size(); ++index)
			{
				const IntType keyframe = keyframes.Value(index).ToInt();
				if (ObjType(obj_keyframe, keyframe)->position == frame)
					return keyframe;
			}
			return tl_keyframe_add(ScopeAny(timeline), frame);
		}

		bool SetTrackValue(VarType& values, const QString& property, const QJsonValue& value)
		{
			if (property == QStringLiteral("position"))
				SetVector(values, e_value_POS_X, e_value_POS_Y, e_value_POS_Z, value);
			else if (property == QStringLiteral("rotation"))
				SetVector(values, e_value_ROT_X, e_value_ROT_Y, e_value_ROT_Z, value);
			else if (property == QStringLiteral("scale"))
				SetVector(values, e_value_SCA_X, e_value_SCA_Y, e_value_SCA_Z, value);
			else if (property == QStringLiteral("visible")) values[e_value_VISIBLE] = value.toBool();
			else if (property == QStringLiteral("material.color")) values[e_value_RGB_MUL] = ColorValue(value);
			else if (property == QStringLiteral("material.metallic")) values[e_value_METALLIC] = value.toDouble();
			else if (property == QStringLiteral("material.roughness")) values[e_value_ROUGHNESS] = value.toDouble();
			else if (property == QStringLiteral("material.emission")) values[e_value_EMISSIVE] = value.toDouble();
			else if (property == QStringLiteral("camera.fov_y_deg")) values[e_value_CAM_FOV] = value.toDouble();
			else if (property == QStringLiteral("camera.ortho_height")) values[e_value_CAM_ORTHO_SCALE] = value.toDouble();
			else if (property == QStringLiteral("camera.focus_distance")) values[e_value_CAM_DOF_DEPTH] = value.toDouble();
			else if (property == QStringLiteral("camera.dof")) values[e_value_CAM_DOF] = value.toBool();
			else if (property == QStringLiteral("camera.projection")) values[e_value_CAM_ORTHO] = value.toString() == QStringLiteral("orthographic");
			else if (property == QStringLiteral("light.color")) values[e_value_LIGHT_COLOR] = ColorValue(value);
			else if (property == QStringLiteral("light.power")) values[e_value_LIGHT_STRENGTH] = value.toDouble();
			else if (property == QStringLiteral("light.size")) values[e_value_LIGHT_SIZE] = value.toDouble();
			else if (property == QStringLiteral("world.color")) values[e_value_BG_SKY_COLOR] = ColorValue(value);
			else if (property == QStringLiteral("world.strength")) values[e_value_BG_SUNLIGHT_STRENGTH] = value.toDouble();
			else return false;
			return true;
		}

		QByteArray SnapshotCurrentScene()
		{
			QJsonArray objects;
			QJsonObject materials;
			QJsonArray tracks;
			QStringList unsupported;
			QString activeCamera;
			IntType maxFrame = 0;

			const IntType appId = global::_app->id;
			const List& timelines = DsList(global::_app->project_timeline_list);
			for (IntType index = 0; index < timelines.vec.size(); ++index)
			{
				const IntType timeline = timelines.Value(index).ToInt();
				const IntType type = idVar(timeline, type).ToInt();
				if (type == e_tl_type_BACKGROUND)
				{
					if (!AppendTrack(tracks, timeline, QStringLiteral("world.color"),
						{ e_value_BG_SKY_COLOR }, TrackValue::Color, maxFrame, QStringLiteral("$world"))
						|| !AppendTrack(tracks, timeline, QStringLiteral("world.strength"),
							{ e_value_BG_SUNLIGHT_STRENGTH }, TrackValue::Number, maxFrame, QStringLiteral("$world")))
						return QByteArray();
					continue;
				}

				const QString kind = KindForTimeline(timeline, type);
				if (kind.isEmpty())
				{
					unsupported.append(idVar(timeline, display_name).ToStr().QStr());
					continue;
				}
				const VarType& values = idVar(timeline, value);
				const QString objectId = TimelineId(timeline);
				QJsonObject object {
					{ QStringLiteral("id"), objectId },
					{ QStringLiteral("name"), idVar(timeline, display_name).ToStr().QStr() },
					{ QStringLiteral("kind"), kind },
					{ QStringLiteral("position"), Vector3(timeline, e_value_POS_X, e_value_POS_Y, e_value_POS_Z) },
					{ QStringLiteral("rotation"), Vector3(timeline, e_value_ROT_X, e_value_ROT_Y, e_value_ROT_Z) },
					{ QStringLiteral("scale"), Vector3(timeline, e_value_SCA_X, e_value_SCA_Y, e_value_SCA_Z) },
					{ QStringLiteral("visible"), values.Value(e_value_VISIBLE).ToBool() && !idVar(timeline, hide).ToBool() }
				};

				const IntType parent = idVar(timeline, parent).ToInt();
				if (parent != appId)
					object.insert(QStringLiteral("parent_id"), TimelineId(parent));

				if (PrimitiveTemplateType(kind) >= 0)
				{
					const QString materialId = MaterialId(timeline);
					object.insert(QStringLiteral("material_id"), materialId);
					materials.insert(materialId, QJsonObject {
						{ QStringLiteral("color"), HexColor(values.Value(e_value_RGB_MUL).ToInt()) },
						{ QStringLiteral("metallic"), values.Value(e_value_METALLIC).ToReal() },
						{ QStringLiteral("roughness"), values.Value(e_value_ROUGHNESS).ToReal() },
						{ QStringLiteral("emission"), values.Value(e_value_EMISSIVE).ToReal() }
					});
					if (!AppendTrack(tracks, timeline, QStringLiteral("material.color"), { e_value_RGB_MUL }, TrackValue::Color, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("material.metallic"), { e_value_METALLIC }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("material.roughness"), { e_value_ROUGHNESS }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("material.emission"), { e_value_EMISSIVE }, TrackValue::Number, maxFrame))
						return QByteArray();
				}
				else if (kind == QStringLiteral("camera"))
				{
					object.insert(QStringLiteral("camera"), QJsonObject {
						{ QStringLiteral("fov_y_deg"), values.Value(e_value_CAM_FOV).ToReal() },
						{ QStringLiteral("orthographic"), values.Value(e_value_CAM_ORTHO).ToBool() },
						{ QStringLiteral("ortho_height"), values.Value(e_value_CAM_ORTHO_SCALE).ToReal() },
						{ QStringLiteral("dof"), values.Value(e_value_CAM_DOF).ToBool() },
						{ QStringLiteral("focus_distance"), values.Value(e_value_CAM_DOF_DEPTH).ToReal() }
					});
					if (global::_app->timeline_camera.ToInt() == timeline)
						activeCamera = objectId;
					if (!AppendTrack(tracks, timeline, QStringLiteral("camera.fov_y_deg"), { e_value_CAM_FOV }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("camera.ortho_height"), { e_value_CAM_ORTHO_SCALE }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("camera.focus_distance"), { e_value_CAM_DOF_DEPTH }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("camera.dof"), { e_value_CAM_DOF }, TrackValue::Boolean, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("camera.projection"), { e_value_CAM_ORTHO }, TrackValue::Projection, maxFrame))
						return QByteArray();
				}
				else if (kind == QStringLiteral("point") || kind == QStringLiteral("area")
					|| kind == QStringLiteral("sun"))
				{
					object.insert(QStringLiteral("light"), QJsonObject {
						{ QStringLiteral("color"), HexColor(values.Value(e_value_LIGHT_COLOR).ToInt()) },
						{ QStringLiteral("power"), values.Value(e_value_LIGHT_STRENGTH).ToReal() },
						{ QStringLiteral("size"), values.Value(e_value_LIGHT_SIZE).ToReal() }
					});
					if (!AppendTrack(tracks, timeline, QStringLiteral("light.color"), { e_value_LIGHT_COLOR }, TrackValue::Color, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("light.power"), { e_value_LIGHT_STRENGTH }, TrackValue::Number, maxFrame)
						|| !AppendTrack(tracks, timeline, QStringLiteral("light.size"), { e_value_LIGHT_SIZE }, TrackValue::Number, maxFrame))
						return QByteArray();
				}

				if (!AppendTrack(tracks, timeline, QStringLiteral("position"),
					{ e_value_POS_X, e_value_POS_Y, e_value_POS_Z }, TrackValue::Vector, maxFrame)
					|| !AppendTrack(tracks, timeline, QStringLiteral("rotation"),
						{ e_value_ROT_X, e_value_ROT_Y, e_value_ROT_Z }, TrackValue::Vector, maxFrame)
					|| !AppendTrack(tracks, timeline, QStringLiteral("visible"),
						{ e_value_VISIBLE }, TrackValue::Boolean, maxFrame))
					return QByteArray();
				if (PrimitiveTemplateType(kind) >= 0
					&& !AppendTrack(tracks, timeline, QStringLiteral("scale"),
						{ e_value_SCA_X, e_value_SCA_Y, e_value_SCA_Z }, TrackValue::Vector, maxFrame))
					return QByteArray();
				objects.append(object);
			}

			if (!unsupported.isEmpty())
			{
				std::sort(unsupported.begin(), unsupported.end());
				renderLlmDocumentError = QStringLiteral("Unsupported legacy scene objects: %1")
					.arg(unsupported.join(QStringLiteral(", ")));
				return QByteArray();
			}

			QJsonObject scene {
				{ QStringLiteral("objects"), objects },
				{ QStringLiteral("world"), QJsonObject {
					{"ambient_color",HexColor(global::_app->background_ambient_color.ToInt())},
					{"sun_color",HexColor(global::_app->background_sunlight_color.ToInt())},
					{"elevation",global::_app->background_sky_time.ToReal()},
					{"rotation",global::_app->background_sky_rotation.ToReal()},
					{"angular_size",global::_app->background_sunlight_angle.ToReal()},
					{"fog_enabled",global::_app->background_fog_show.ToReal()>0},
					{"fog_color",HexColor(global::_app->background_fog_color.ToInt())},
					{"fog_distance",global::_app->background_fog_distance.ToReal()},
					{"fog_height",global::_app->background_fog_height.ToReal()},
					{ QStringLiteral("color"), HexColor(global::_app->background_sky_color.ToInt()) },
					{ QStringLiteral("strength"), global::_app->background_sunlight_strength.ToReal() }
				} }
			};
			if (!activeCamera.isEmpty())
				scene.insert(QStringLiteral("active_camera_id"), activeCamera);

			QJsonObject root {
				{ QStringLiteral("version"), 1 },
				{ QStringLiteral("scene"), scene },
				{ QStringLiteral("materials"), materials },
				{ QStringLiteral("animation"), QJsonObject {
					{ QStringLiteral("schema"), 1 },
					{ QStringLiteral("fps"), global::_app->project_tempo.ToReal() },
					{ QStringLiteral("frame_count"), std::max<IntType>({ 1, global::_app->timeline_length.ToInt(), maxFrame + 1 }) },
					{ QStringLiteral("tracks"), tracks },
					{ QStringLiteral("constraints"), QJsonArray() }
				} },
				{ QStringLiteral("render"), QJsonObject {
					{"bounces",global::_app->project_cinema_bounces.ToInt()},
					{"noise_threshold",global::_app->project_cinema_noise.ToReal()},
					{"denoise",global::_app->project_cinema_denoise.ToReal()>0},
					{"exposure",global::_app->project_render_exposure.ToReal()},
					{"overscan",global::_app->project_render_overscan.ToReal()},
					{"tiled",global::_app->project_render_tiled.ToReal()>0},
					{ QStringLiteral("schema"), 2 },
					{ QStringLiteral("renderer"), global::_app->project_render_engine >= 2 ? QStringLiteral("UltraFlame") : QStringLiteral("Flame") },
					{ QStringLiteral("samples"), global::_app->project_render_samples.ToInt() }
				} },
				{ QStringLiteral("agent"), QJsonObject() }
			};

			RenderLlmDocument document;
			if (!document.Parse(QJsonDocument(root).toJson(QJsonDocument::Compact), &renderLlmDocumentError))
				return QByteArray();
			return document.CanonicalJson();
		}

		bool ValidateApplicable(const QJsonObject& root, QString* error)
		{
			const QJsonObject scene = root.value(QStringLiteral("scene")).toObject();
			const QJsonObject animation = root.value(QStringLiteral("animation")).toObject();
			const QJsonArray objects = scene.value(QStringLiteral("objects")).toArray();
			const QJsonArray tracks = animation.value(QStringLiteral("tracks")).toArray();
			for (const QJsonValue& value : tracks)
			{
				const QString target = value.toObject().value(QStringLiteral("target")).toString();
				if (target == QStringLiteral("$render") || target == QStringLiteral("$scene"))
				{
					*error = QStringLiteral("Track target is not representable by the retained timeline: %1").arg(target);
					return false;
				}
			}
			return true;
		}

		bool ApplyRoot(const QJsonObject& root, QString* error)
		{
			QScopedValueRollback<bool> replacement(replacingDocument, true);
			if (!ValidateApplicable(root, error))
				return false;
			const QJsonObject scene = root.value(QStringLiteral("scene")).toObject();
			const QJsonObject materials = root.value(QStringLiteral("materials")).toObject();
			const QJsonObject animation = root.value(QStringLiteral("animation")).toObject();
			const QJsonArray objects = scene.value(QStringLiteral("objects")).toArray();
			const QJsonArray tracks = animation.value(QStringLiteral("tracks")).toArray();

			ScopeAny appScope(global::_app->id);
			// Document transactions rebuild runtime objects, not the user's navigation.
			const double marker = global::_app->timeline_marker.ToReal();
			const auto timelineZoom = global::_app->timeline_zoom;
			const auto cameraFocus = global::_app->cam_work_focus;
			const auto cameraFocusLast = global::_app->cam_work_focus_last;
			const auto cameraFrom = global::_app->cam_work_from;
			const auto cameraXY = global::_app->cam_work_angle_xy;
			const auto cameraZ = global::_app->cam_work_angle_z;
			const auto cameraRoll = global::_app->cam_work_roll;
			const auto cameraZoom = global::_app->cam_work_zoom;
			const auto cameraLookXY = global::_app->cam_work_angle_look_xy;
			const auto cameraLookZ = global::_app->cam_work_angle_look_z;
			const auto cameraLookOffsetXY = global::_app->cam_work_angle_look_off_xy;
			const auto cameraLookOffsetZ = global::_app->cam_work_angle_look_off_z;
			const IntType previousViewCamera = ObjType(obj_view, global::_app->view_main)->camera;
			const QString previousViewCameraId = previousViewCamera >= 0 && instance_exists(previousViewCamera)
				? TimelineId(previousViewCamera) : QString();
			project_reset(appScope);
			// A document replacement can alter geometry and analytic lights without
			// changing the legacy cache key. Force the next retained render through
			// capture/build; CaptureBegin owns safe invalidation of the current scene.
			global::_app->mi3_cinema_scene_gen = StringType();
			runtimeIdentities.clear();
			const auto renderSettings = root.value(QStringLiteral("render")).toObject();
			// Before public engine names split, documents called Cinema "Flame".
			// Preserve those files; only schema 2 Flame explicitly selects NextGen.
			global::_app->project_render_engine = renderSettings.value(QStringLiteral("schema")).toInt() == 2
				&& renderSettings.value(QStringLiteral("renderer")).toString() == QStringLiteral("Flame") ? 1 : 2;
			global::_app->project_render_samples = root.value(QStringLiteral("render")).toObject()
				.value(QStringLiteral("samples")).toInt();
			if(renderSettings.contains("bounces"))global::_app->project_cinema_bounces=renderSettings.value("bounces").toInt();
			if(renderSettings.contains("noise_threshold"))global::_app->project_cinema_noise=renderSettings.value("noise_threshold").toDouble();
			if(renderSettings.contains("denoise"))global::_app->project_cinema_denoise=renderSettings.value("denoise").toBool();
			if(renderSettings.contains("exposure"))global::_app->project_render_exposure=renderSettings.value("exposure").toDouble();
			if(renderSettings.contains("overscan"))global::_app->project_render_overscan=renderSettings.value("overscan").toDouble();
			if(renderSettings.contains("tiled"))global::_app->project_render_tiled=renderSettings.value("tiled").toBool();
			global::_app->project_tempo = animation.value(QStringLiteral("fps")).toInt();
			global::_app->timeline_length = animation.value(QStringLiteral("frame_count")).toInt();
			const QJsonObject world = scene.value(QStringLiteral("world")).toObject();
			if(world.contains("ambient_color"))global::_app->background_ambient_color=ColorValue(world.value("ambient_color"));
			if(world.contains("sun_color"))global::_app->background_sunlight_color=ColorValue(world.value("sun_color"));
			if(world.contains("elevation"))global::_app->background_sky_time=world.value("elevation").toDouble();
			if(world.contains("rotation"))global::_app->background_sky_rotation=world.value("rotation").toDouble();
			if(world.contains("angular_size"))global::_app->background_sunlight_angle=world.value("angular_size").toDouble();
			if(world.contains("fog_enabled"))global::_app->background_fog_show=world.value("fog_enabled").toBool();
			if(world.contains("fog_color"))global::_app->background_fog_color=ColorValue(world.value("fog_color"));
			if(world.contains("fog_distance"))global::_app->background_fog_distance=world.value("fog_distance").toDouble();
			if(world.contains("fog_height"))global::_app->background_fog_height=world.value("fog_height").toDouble();
			global::_app->background_sky_color = ColorValue(world.value(QStringLiteral("color")));
			global::_app->background_sunlight_strength = world.value(QStringLiteral("strength")).toDouble();

			QHash<QString, IntType> timelines;
			for (const QJsonValue& value : objects)
			{
				const QJsonObject object = value.toObject();
				const QString objectId = object.value(QStringLiteral("id")).toString();
				const IntType timeline = CreateTimeline(object.value(QStringLiteral("kind")).toString());
				if (timeline <= 0)
				{
					*error = QStringLiteral("Could not create runtime object: %1").arg(objectId);
					return false;
				}
				mi3_hl_set_name(timeline, StringType(object.value(QStringLiteral("name")).toString()));
				SetObjectBase(timeline, object, materials);
				idBool(timeline, update_matrix) = true;
				timelines.insert(objectId, timeline);
				runtimeIdentities.insert(timeline, RuntimeIdentity {
					idVar(timeline, save_id).ToStr().QStr(), objectId,
					object.value(QStringLiteral("material_id")).toString(),
					object.value(QStringLiteral("kind")).toString(), {}
				});
			}

			for (const QJsonValue& value : objects)
			{
				const QJsonObject object = value.toObject();
				if (!object.contains(QStringLiteral("parent_id")))
					continue;
				const IntType child = timelines.value(object.value(QStringLiteral("id")).toString());
				const IntType parent = timelines.value(object.value(QStringLiteral("parent_id")).toString());
				tl_set_parent(ScopeAny(child), { parent });
			}

			IntType worldTimeline = 0;
			for (const QJsonValue& value : tracks)
				if (value.toObject().value(QStringLiteral("target")).toString() == QStringLiteral("$world"))
				{
					worldTimeline = new_tl(e_tl_type_BACKGROUND);
					if (worldTimeline <= 0)
					{
						*error = QStringLiteral("Could not create the runtime world timeline.");
						return false;
					}
					mi3_hl_set_name(worldTimeline, StringType(QStringLiteral("World")));
					obj_timeline* runtime = ObjType(obj_timeline, worldTimeline);
					runtime->value[e_value_BG_SKY_COLOR] = ColorValue(world.value(QStringLiteral("color")));
					runtime->value[e_value_BG_SUNLIGHT_STRENGTH] = world.value(QStringLiteral("strength")).toDouble();
					for (IntType field = 0; field < e_value_amount; ++field)
						runtime->value_default[field] = runtime->value.Value(field);
					runtimeIdentities.insert(worldTimeline, RuntimeIdentity {
						idVar(worldTimeline, save_id).ToStr().QStr(), QStringLiteral("$world"),
						QString(), QString(), {}
					});
					break;
				}

			for (const QJsonValue& value : tracks)
			{
				const QJsonObject track = value.toObject();
				const QString target = track.value(QStringLiteral("target")).toString();
				const QString property = track.value(QStringLiteral("property")).toString();
				const IntType timeline = target == QStringLiteral("$world") ? worldTimeline : timelines.value(target);
				if (timeline <= 0)
				{
					*error = QStringLiteral("Could not resolve runtime track target: %1").arg(target);
					return false;
				}
				runtimeIdentities[timeline].trackIds.insert(property, track.value(QStringLiteral("id")).toString());
				for (const QJsonValue& keyValue : track.value(QStringLiteral("keys")).toArray())
				{
					const QJsonObject key = keyValue.toObject();
					const IntType keyframe = FindOrAddKeyframe(timeline, key.value(QStringLiteral("frame")).toInt());
					runtimeIdentities[timeline].trackKeys[property].insert(keyframe);
					obj_keyframe* runtimeKey = ObjType(obj_keyframe, keyframe);
					if (!SetTrackValue(runtimeKey->value, property, key.value(QStringLiteral("value"))))
					{
						*error = QStringLiteral("Could not map track property: %1").arg(property);
						return false;
					}
					runtimeKey->value[e_value_TRANSITION] = StringType(key.value(QStringLiteral("easing")).toString());
					if (key.contains(QStringLiteral("bezier")))
					{
						const QJsonArray handles = key.value(QStringLiteral("bezier")).toArray();
						runtimeKey->value[e_value_EASE_IN_X] = handles.at(0).toDouble();
						runtimeKey->value[e_value_EASE_IN_Y] = handles.at(1).toDouble();
						runtimeKey->value[e_value_EASE_OUT_X] = handles.at(2).toDouble();
						runtimeKey->value[e_value_EASE_OUT_Y] = handles.at(3).toDouble();
					}
				}
				idBool(timeline, update_matrix) = true;
			}

			for (auto identity = runtimeIdentities.begin(); identity != runtimeIdentities.end(); ++identity) {
				const List& keys = DsList(idInt(identity.key(), keyframe_list));
				for (const auto& key : keys.vec) identity->importedKeys.insert(key.value.ToInt());
			}
			const QString activeCamera = scene.value(QStringLiteral("active_camera_id")).toString();
			for (auto it = timelines.begin(); it != timelines.end(); ++it)
				ObjType(obj_timeline, it.value())->selected = it.key() == activeCamera;
			if (!activeCamera.isEmpty())
				global::_app->timeline_camera = timelines.value(activeCamera);
			// Applying an Agent edit rebuilds timelines, but must not reset the user's
			// viewport to the distant work camera or retain a destroyed camera handle.
			ObjType(obj_view, global::_app->view_main)->camera = previousViewCamera == -4 || previousViewCamera == -5
				? previousViewCamera : timelines.value(previousViewCameraId, -5);
			global::_app->timeline_marker = std::clamp(marker,0.,double(std::max(0,animation.value("frame_count").toInt()-1)));
			global::_app->timeline_marker_previous = global::_app->timeline_marker;
			global::_app->timeline_zoom = global::_app->timeline_zoom_goal = timelineZoom;
			global::_app->cam_work_focus = cameraFocus;
			global::_app->cam_work_focus_last = cameraFocusLast;
			global::_app->cam_work_from = cameraFrom;
			global::_app->cam_work_angle_xy = global::_app->cam_work_angle_goal_xy = cameraXY;
			global::_app->cam_work_angle_z = global::_app->cam_work_angle_goal_z = cameraZ;
			global::_app->cam_work_roll = cameraRoll;
			global::_app->cam_work_zoom = global::_app->cam_work_zoom_goal = cameraZoom;
			global::_app->cam_work_angle_look_xy = cameraLookXY;
			global::_app->cam_work_angle_look_z = cameraLookZ;
			global::_app->cam_work_angle_look_off_xy = cameraLookOffsetXY;
			global::_app->cam_work_angle_look_off_z = cameraLookOffsetZ;
			tl_update_list(appScope);
			app_update_animate(appScope);
			tl_update_matrix(appScope);
			global::_app->project_changed = true;
			return true;
		}

		void PushRevision(QVector<QByteArray>& stack, const QByteArray& document)
		{
			constexpr int MaxRevisions = 64;
			if (stack.size() == MaxRevisions)
				stack.removeFirst();
			stack.append(document);
		}

		QString BoundedDocumentPath(const QString& requestedName, bool createRoot)
		{
			const QString name = requestedName;
			if (name != name.trimmed() || name.isEmpty() || name.size() > 128
				|| name.startsWith(QLatin1Char('.'))
				|| name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
				|| name.contains(QLatin1Char(':')) || QFileInfo(name).fileName() != name
				|| QFileInfo(name).suffix().compare(QStringLiteral("renderllm"), Qt::CaseInsensitive) != 0)
			{
				renderLlmDocumentError = QStringLiteral(
					"Document name must be a local .renderllm filename without path components.");
				return QString();
			}
			const QString root = QDir(gmlGlobal::working_directory.QStr())
				.filePath(QStringLiteral("Data/renderllm-projects"));
			const QFileInfo rootInfo(root);
			if (rootInfo.exists() && (rootInfo.isSymLink() || !rootInfo.isDir()))
			{
				renderLlmDocumentError = QStringLiteral("Document storage root is not a regular directory.");
				return QString();
			}
			if (createRoot && !QDir().mkpath(root))
			{
				renderLlmDocumentError = QStringLiteral("Could not create the document storage directory.");
				return QString();
			}
			const QString path = QDir(root).filePath(name);
			const QFileInfo target(path);
			if (target.exists() && target.isSymLink())
			{
				renderLlmDocumentError = QStringLiteral("Symbolic-link document targets are not allowed.");
				return QString();
			}
			return path;
		}

		bool ApplyDocument(const QByteArray& bytes, bool recordRevision)
		{
			RenderLlmDocument document;
			if (!document.Parse(bytes, &renderLlmDocumentError))
				return false;
			if (!ValidateApplicable(document.Root(), &renderLlmDocumentError))
				return false;
			const QByteArray backup = SnapshotCurrentScene();
			if (backup.isEmpty())
			{
				renderLlmDocumentError = QStringLiteral("Current scene cannot be checkpointed before apply: %1")
					.arg(renderLlmDocumentError);
				return false;
			}
			QString applyError;
			if (cleanProjectSnapshot.isEmpty()) cleanProjectSnapshot = PersistentProjectState(backup);
			if (!ApplyRoot(document.Root(), &applyError) || SnapshotCurrentScene().isEmpty())
			{
				const QString originalError = applyError.isEmpty() ? renderLlmDocumentError : applyError;
				RenderLlmDocument rollback;
				QString rollbackError;
				if (!rollback.Parse(backup, &rollbackError) || !ApplyRoot(rollback.Root(), &rollbackError))
					renderLlmDocumentError = QStringLiteral("Apply failed (%1); rollback failed (%2).").arg(originalError, rollbackError);
				else
					renderLlmDocumentError = QStringLiteral("Apply failed and was rolled back: %1").arg(originalError);
				return false;
			}
			if (recordRevision)
			{
				PushRevision(undoDocuments, backup);
				redoDocuments.clear();
			}
			++documentRevision;
			renderLlmDocumentError.clear();
			return true;
		}

		bool RestoreRevision(QVector<QByteArray>& source, QVector<QByteArray>& destination)
		{
			if (source.isEmpty())
			{
				renderLlmDocumentError = QStringLiteral("No document revision is available.");
				return false;
			}
			const QByteArray current = SnapshotCurrentScene();
			if (current.isEmpty())
				return false;
			while (!source.isEmpty() && source.last() == current) source.removeLast();
			if (source.isEmpty()) return false;
			const QByteArray target = source.last();
			if (!ApplyDocument(target, false))
			{
				renderLlmDocumentError = QStringLiteral("Could not restore document revision: %1")
					.arg(renderLlmDocumentError);
				return false;
			}
			PushRevision(destination, current);
			source.removeLast();
			renderLlmDocumentError.clear();
			return true;
		}
	}

	StringType renderllm_document_canonicalize(StringType source)
	{
		RenderLlmDocument document;
		if (!document.Parse(source.ToUtf8(), &renderLlmDocumentError))
			return StringType();
		renderLlmDocumentError.clear();
		return StringType(document.CanonicalJson());
	}

	StringType renderllm_document_error()
	{
		return StringType(renderLlmDocumentError);
	}

	void RenderLlmCheckpointNativeEdit()
	{
		if (replacingDocument || runtimeIdentities.isEmpty() || !global::_app) return;
		const QByteArray current = SnapshotCurrentScene();
		if (current.isEmpty()) return;
		if (undoDocuments.isEmpty() || undoDocuments.last() != current) PushRevision(undoDocuments, current);
		redoDocuments.clear();
		++documentRevision;
	}

	bool RenderLlmEvaluateChannel(IntType timeline, IntType channel)
	{
		static thread_local bool evaluating = false;
		if (evaluating) return false;
		const RuntimeIdentity* identity = RuntimeIdentityFor(timeline);
		if (!identity) return false;
		const QString property = PropertyForChannel(channel);
		if (!identity->trackIds.contains(property)) return false;
		auto* runtime = ObjType(obj_timeline, timeline);
		List& list = DsList(runtime->keyframe_list);
		auto allKeys = std::move(list.vec);
		for (const auto& key : allKeys)
			if (TrackOwnsKey(*identity, property, key.value.ToInt())) list.vec.push_back(key);
		if (list.vec.size() == allKeys.size()) { list.vec = std::move(allKeys); return false; }
		if (list.vec.empty()) {
			list.vec = std::move(allKeys);
			const auto value = runtime->value_default.Value(channel);
			if (runtime->value.Value(channel) != value) {runtime->value[channel] = value;runtime->update_matrix = true;}
			return true;
		}

		// Only the neighbor selection changes. Use the original progress, easing,
		// Catmull-Rom, color/bool and interpolation functions without reimplementing
		// their math. Restore the whole-object selection context before returning.
		const auto oldCurrent = runtime->keyframe_current, oldNext = runtime->keyframe_next;
		const auto oldCurrentValues = runtime->keyframe_current_values, oldNextValues = runtime->keyframe_next_values;
		const auto oldPreviousValues = runtime->keyframe_prev_values_cr, oldNext2Values = runtime->keyframe_next2_values_cr;
		const auto oldProgress = runtime->keyframe_progress;
		const auto oldEase = runtime->keyframe_progress_ease, oldTransition = runtime->keyframe_transition;
		const auto oldAnimate = runtime->keyframe_animate, oldSmooth = runtime->keyframe_smooth;
		const IntType metadata[] = {e_value_TRANSITION,e_value_EASE_IN_X,e_value_EASE_IN_Y,e_value_EASE_OUT_X,e_value_EASE_OUT_Y};
		QVector<VarType> oldMetadata;for (auto field : metadata) oldMetadata.append(runtime->value.Value(field));
		ScopeAny scope(timeline);
		evaluating = true;
		runtime->keyframe_current = null_;runtime->keyframe_next = null_;
		runtime->keyframe_current_values = null_;runtime->keyframe_next_values = null_;
		IntType currentIndex = -1;
		for (IntType index = 0; index < list.vec.size(); ++index) {
			runtime->keyframe_next = list.Value(index).ToInt();
			if (ObjType(obj_keyframe, runtime->keyframe_next)->position > global::_app->timeline_marker) break;
			runtime->keyframe_current = runtime->keyframe_next;currentIndex = index;
		}
		runtime->keyframe_progress = tl_update_values_progress(scope, global::_app->timeline_marker);
		runtime->keyframe_animate = runtime->keyframe_current > 0 && runtime->keyframe_next > 0 && runtime->keyframe_current != runtime->keyframe_next;
		if (runtime->keyframe_current != null_) runtime->keyframe_current_values = ObjType(obj_keyframe, runtime->keyframe_current)->value;
		if (runtime->keyframe_next != null_) runtime->keyframe_next_values = ObjType(obj_keyframe, runtime->keyframe_next)->value;
		runtime->keyframe_progress_ease = 0;
		for (auto field : metadata) tl_update_values_ease(scope, field);
		runtime->keyframe_transition = runtime->value.Value(e_value_TRANSITION);runtime->keyframe_smooth = false;
		if (runtime->keyframe_animate && runtime->keyframe_transition == "smooth") {
			runtime->keyframe_smooth = true;
			runtime->keyframe_prev_values_cr = currentIndex > 0 ? ObjType(obj_keyframe, list.Value(currentIndex-1))->value : runtime->keyframe_current_values;
			runtime->keyframe_next2_values_cr = currentIndex+2 < list.vec.size() ? ObjType(obj_keyframe, list.Value(currentIndex+2))->value : runtime->keyframe_next_values;
		}
		if (runtime->keyframe_transition == "bezier")
			runtime->keyframe_progress_ease = ease_bezier_curve(ArrType::From({0,0}), ArrType::From({runtime->value.Value(e_value_EASE_IN_X),runtime->value.Value(e_value_EASE_IN_Y)}), ArrType::From({runtime->value.Value(e_value_EASE_OUT_X),runtime->value.Value(e_value_EASE_OUT_Y)}), ArrType::From({1,1}), runtime->keyframe_progress);
		else runtime->keyframe_progress_ease = ease(runtime->keyframe_transition, runtime->keyframe_progress);
		tl_update_values_ease(scope, channel);
		for (int index = 0; index < 5; ++index) runtime->value[metadata[index]] = oldMetadata[index];
		runtime->keyframe_current = oldCurrent;runtime->keyframe_next = oldNext;
		runtime->keyframe_current_values = oldCurrentValues;runtime->keyframe_next_values = oldNextValues;
		runtime->keyframe_prev_values_cr = oldPreviousValues;runtime->keyframe_next2_values_cr = oldNext2Values;
		runtime->keyframe_progress = oldProgress;runtime->keyframe_progress_ease = oldEase;runtime->keyframe_transition = oldTransition;
		runtime->keyframe_animate = oldAnimate;runtime->keyframe_smooth = oldSmooth;
		list.vec = std::move(allKeys);evaluating = false;
		return true;
	}

	QString RenderLlmSelectedObject()
	{
		if (!global::_app || global::tl_edit_amount < 1) return {};
		const IntType selected = global::tl_edit;
		return instance_exists(selected) ? TimelineId(selected) : QString();
	}

	bool RenderLlmSelectObject(const QString& objectId)
	{
		if (!global::_app) return false;
		const List& list = DsList(global::_app->project_timeline_list);
		for (IntType index = 0; index < list.vec.size(); ++index) {
			const IntType timeline = list.Value(index).ToInt();
			if (TimelineId(timeline) != objectId) continue;
			tl_select_single(ScopeAny(timeline));
			// Refresh selection-dependent timeline state without reopening legacy tabs.
			app_update_tl_edit_select(ScopeAny(global::_app->id));
			return true;
		}
		return false;
	}

	StringType renderllm_document_snapshot()
	{
		const QByteArray snapshot = SnapshotCurrentScene();
		if (snapshot.isEmpty())
			return StringType();
		renderLlmDocumentError.clear();
		return StringType(snapshot);
	}

	BoolType renderllm_document_apply(StringType source)
	{
		return ApplyDocument(source.ToUtf8(), true);
	}

	BoolType renderllm_document_undo()
	{
		return RestoreRevision(undoDocuments, redoDocuments);
	}

	BoolType renderllm_document_redo()
	{
		return RestoreRevision(redoDocuments, undoDocuments);
	}

	IntType renderllm_document_revision()
	{
		return documentRevision;
	}

	BoolType renderllm_document_open(StringType name)
	{
		const QString path = BoundedDocumentPath(name.QStr(), false);
		if (path.isEmpty())
			return false;
		RenderLlmDocument document;
		if (!document.Load(path, &renderLlmDocumentError)
			|| !ApplyDocument(document.CanonicalJson(), true))
			return false;
		currentProjectName = name.QStr();
		currentProjectHash = ProjectFileHash(path);
		renderllm_mark_project_clean();
		return true;
	}

	BoolType renderllm_document_save(StringType name)
	{
		const QString path = BoundedDocumentPath(name.QStr(), true);
		if (path.isEmpty())
			return false;
		RenderLlmDocument document;
		const QByteArray snapshot = SnapshotCurrentScene();
		if (snapshot.isEmpty() || !document.Parse(snapshot, &renderLlmDocumentError)
			|| !document.Save(path, &renderLlmDocumentError))
			return false;
		renderLlmDocumentError.clear();
		currentProjectName = name.QStr();
		currentProjectHash = ProjectFileHash(path);
		renderllm_mark_project_clean();
		return true;
	}

	QString renderllm_current_project_name() { return currentProjectName; }
	void renderllm_reset_project_file() {
		if (!replacingDocument) { currentProjectName.clear(); currentProjectHash.clear(); cleanProjectSnapshot.clear(); }
	}
	void renderllm_mark_project_clean() {
		cleanProjectSnapshot = PersistentProjectState(SnapshotCurrentScene());
		if (global::_app && !cleanProjectSnapshot.isEmpty()) global::_app->project_changed = false;
	}
	bool renderllm_project_has_changes() {
		if (!global::_app) return false;
		const auto current = PersistentProjectState(SnapshotCurrentScene());
		if (current.isEmpty()) return true; // Do not discard a scene that cannot be serialized.
		if (current != cleanProjectSnapshot && qEnvironmentVariable("RENDER_LLM_TEST_MOTION") == "dirty-playback") {
			QSaveFile before(QDir::current().filePath("Data/dirty-clean.json")), after(QDir::current().filePath("Data/dirty-current.json"));
			if(before.open(QIODevice::WriteOnly)){before.write(cleanProjectSnapshot);before.commit();}
			if(after.open(QIODevice::WriteOnly)){after.write(current);after.commit();}
		}
		return cleanProjectSnapshot.isEmpty() ? global::_app->project_changed > 0 : current != cleanProjectSnapshot;
	}
	bool renderllm_new_project() {
		auto root = QJsonDocument::fromJson(SnapshotCurrentScene()).object();
		if (root.isEmpty()) return false;
		root["scene"] = QJsonObject{{"objects",QJsonArray{}},{"world",QJsonObject{{"color","#202024"},{"strength",1}}}};
		root["materials"] = QJsonObject{};root["agent"] = QJsonObject{};
		root["animation"] = QJsonObject{{"schema",1},{"fps",24},{"frame_count",96},{"tracks",QJsonArray{}},{"constraints",QJsonArray{}}};
		if (!ApplyDocument(QJsonDocument(root).toJson(), false)) return false;
		undoDocuments.clear();redoDocuments.clear();renderllm_reset_project_file();renderllm_mark_project_clean();
		return true;
	}
	bool renderllm_save_current_project() {
		if (currentProjectName.isEmpty()) {renderLlmDocumentError = "Choose a project name first.";return false;}
		const QString path = BoundedDocumentPath(currentProjectName, false);
		if (path.isEmpty()) return false;
		if (currentProjectHash.isEmpty() || ProjectFileHash(path) != currentProjectHash) {
			renderLlmDocumentError = "The project file changed outside this editor. Use Projects to save a separate copy or reopen it.";
			return false;
		}
		return renderllm_document_save(StringType(currentProjectName));
	}

	IntType renderllm_document_light_type(IntType timeline)
	{
		return DocumentLightType(timeline);
	}

	IntType renderllm_document_light_add(IntType timeline,
		RealType x, RealType y, RealType z, RealType dirX, RealType dirY, RealType dirZ,
		RealType range, IntType color, RealType strength, RealType size, RealType fadeSize,
		RealType spotRadius, RealType spotSharpness, BoolType isSpot)
	{
		return CinemaSceneLightAdd(x, y, z, dirX, dirY, dirZ, range, color, strength,
			size, fadeSize, spotRadius, spotSharpness, isSpot, DocumentLightType(timeline));
	}

	BoolType renderllm_document_sync_sun()
	{
		return SyncDocumentSun();
	}

	void renderllm_document_restore_sun()
	{
		RestoreDocumentSun();
	}
}
