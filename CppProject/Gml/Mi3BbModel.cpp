// Mi3BbModel.cpp — MI3: native .bbmodel -> .mimodel/.miobject converter.
//
// Faithful C++ port of tools/bb2mi core.mjs (mimodel_plus.js v1.0.0), the
// golden-verified Blockbench->Mine-imator converter: per-face UV, multiple
// textures and resolution mismatches are re-baked into a box-UV atlas;
// multi-axis rotations use an exact matrix decomposition in the engine's
// Y·X·Z euler order. bakeHeadDisplay() (hat placement) is also ported.
//
// Exposed to GML (declared via CppGen/gml.json) as:
//   mi3_bbmodel_convert(bbmodel_path, out_dir, head_attach) -> string
// returning "ok|<miobject>|<mimodel>|<stats>|<warning count>|<warnings>"
// or "err|<CODE>|<message>". The output .mimodel JSON is written with a
// plain-notation number formatter (the engine's JSON parser mis-evaluates
// scientific notation) and ASCII-sanitized names.

#include "Generated/Scripts.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QMap>

#include <cmath>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>

static const double MI3_PI = 3.14159265358979323846;

namespace CppProject
{
namespace Mi3Bb
{
	// ---------------------------------------------------------------- helpers

	static const char* FACE_DIRS[6] = { "north", "east", "south", "west", "up", "down" };
	static const int MAX_TREE_DEPTH = 500;

	struct Vec3 { double x = 0, y = 0, z = 0; };

	static double z0(double n) { return n == 0.0 ? 0.0 : n; }
	static Vec3 zv(Vec3 v) { return { z0(v.x), z0(v.y), z0(v.z) }; }
	static Vec3 T(Vec3 v) { return zv({ -v.x, v.y, -v.z }); }
	static Vec3 add3(Vec3 a, Vec3 b) { return zv({ a.x + b.x, a.y + b.y, a.z + b.z }); }
	static Vec3 sub3(Vec3 a, Vec3 b) { return zv({ a.x - b.x, a.y - b.y, a.z - b.z }); }
	static Vec3 neg3(Vec3 a) { return zv({ -a.x, -a.y, -a.z }); }
	static bool isZero3(Vec3 v) { return std::abs(v.x) <= 1e-9 && std::abs(v.y) <= 1e-9 && std::abs(v.z) <= 1e-9; }
	static void minMax3(Vec3 a, Vec3 b, Vec3& mn, Vec3& mx)
	{
		mn = { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
		mx = { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
	}

	static Vec3 jsonVec3(const QJsonValue& v, Vec3 def = {})
	{
		if (!v.isArray()) return def;
		QJsonArray a = v.toArray();
		if (a.size() < 3) return def;
		return { a[0].toDouble(), a[1].toDouble(), a[2].toDouble() };
	}
	static QJsonArray vecArr(Vec3 v) { return QJsonArray { v.x, v.y, v.z }; }

	// Exact euler conversion: BB 'ZYX' euler -> engine Y·X·Z euler through the
	// 180°-yaw conjugated rotation matrix (see core.mjs for the derivation).
	typedef double Mat3[3][3];
	static void matRx(double a, Mat3 M) { double c = cos(a), s = sin(a); double t[3][3] = { {1,0,0},{0,c,-s},{0,s,c} }; memcpy(M, t, sizeof(t)); }
	static void matRy(double a, Mat3 M) { double c = cos(a), s = sin(a); double t[3][3] = { {c,0,s},{0,1,0},{-s,0,c} }; memcpy(M, t, sizeof(t)); }
	static void matRz(double a, Mat3 M) { double c = cos(a), s = sin(a); double t[3][3] = { {c,-s,0},{s,c,0},{0,0,1} }; memcpy(M, t, sizeof(t)); }
	static void matMul(const Mat3 A, const Mat3 B, Mat3 M)
	{
		Mat3 R;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				R[r][c] = A[r][0] * B[0][c] + A[r][1] * B[1][c] + A[r][2] * B[2][c];
		memcpy(M, R, sizeof(R));
	}
	static double cleanAngle(double v)
	{
		double r = std::round(v);
		if (std::abs(v - r) < 1e-7) return r == 0.0 ? 0.0 : r;
		return std::round(v * 1e6) / 1e6;
	}
	static Vec3 eulerBBtoMI(Vec3 rotBB)
	{
		const double d2r = MI3_PI / 180.0;
		Mat3 Rz, Ry, Rx, Tmp, M;
		matRz(-rotBB.z * d2r, Rz);
		matRy(rotBB.y * d2r, Ry);
		matRx(-rotBB.x * d2r, Rx);
		matMul(Ry, Rx, Tmp);
		matMul(Rz, Tmp, M);
		double mx, my, mz;
		double sx = -M[1][2];
		if (std::abs(sx) > 0.9999999)
		{
			mx = sx > 0 ? MI3_PI / 2 : -MI3_PI / 2;
			mz = 0;
			my = atan2(sx * M[0][1], M[0][0]);
		}
		else
		{
			mx = asin(std::max(-1.0, std::min(1.0, sx)));
			my = atan2(M[0][2], M[2][2]);
			mz = atan2(M[1][0], M[1][1]);
		}
		const double r2d = 180.0 / MI3_PI;
		return { cleanAngle(mx * r2d), cleanAngle(my * r2d), cleanAngle(mz * r2d) };
	}

	static bool hasBadNumber(const QJsonValue& v)
	{
		if (v.isDouble()) { double d = v.toDouble(); return !std::isfinite(d) || std::abs(d) > 1e15; }
		if (v.isArray()) { for (const QJsonValue& e : v.toArray()) if (hasBadNumber(e)) return true; }
		return false;
	}

	static QString asciiName(const QJsonValue& raw, const QString& fallback)
	{
		QString s = raw.toString();
		QString out;
		for (QChar ch : s)
		{
			ushort u = ch.unicode();
			out += (u >= 0x20 && u <= 0x7E) ? ch : QChar('_');
		}
		out = out.trimmed();
		if (out.isEmpty()) out = fallback;
		return out.left(100);
	}

	static QString uniqueName(const QString& base, QSet<QString>& used)
	{
		QString name = base;
		int n = 2;
		while (used.contains(name)) name = base + "_" + QString::number(n++);
		used.insert(name);
		return name;
	}

	static QString safeFileBase(const QString& nameIn)
	{
		QString s = nameIn.trimmed();
		QString out;
		for (QChar ch : s)
		{
			if (ch.isLetterOrNumber() && ch.unicode() < 128) out += ch;
			else if (ch == ' ' || ch == '_' || ch == '.' || ch == '-') out += ch;
			else out += '_';
		}
		out.replace(QRegularExpression("\\s+"), "_");
		while (out.startsWith('_') || out.startsWith('.')) out.remove(0, 1);
		while (out.endsWith('_') || out.endsWith('.')) out.chop(1);
		if (out.isEmpty()) out = "model";
		return out.left(60);
	}

	// ------------------------------------------------------------ JSON writer
	// Engine-parser-safe: plain decimal notation only (never scientific),
	// numbers rounded to 6 decimals, -0 normalized.

	static double roundJsonSafe(double n)
	{
		if (!std::isfinite(n) || std::abs(n) > 1e15) return 0;
		double v = std::round(n * 1e6) / 1e6;
		if (v == 0.0) v = 0.0;
		return v;
	}

	static QString numToJson(double vIn)
	{
		double v = roundJsonSafe(vIn);
		if (v == std::floor(v) && std::abs(v) < 1e15)
			return QString::number((qlonglong)v);
		QString s = QString::number(v, 'f', 6);
		while (s.endsWith('0')) s.chop(1);
		if (s.endsWith('.')) s.chop(1);
		return s;
	}

	static QString strToJson(const QString& s)
	{
		QString out = "\"";
		for (QChar ch : s)
		{
			ushort u = ch.unicode();
			if (ch == '"') out += "\\\"";
			else if (ch == '\\') out += "\\\\";
			else if (u < 0x20)
				out += QString("\\u%1").arg(u, 4, 16, QChar('0'));
			else
				out += ch;
		}
		return out + "\"";
	}

	static void writeJsonValue(const QJsonValue& v, QString& out, int indent)
	{
		QString pad(indent, '\t');
		QString padIn(indent + 1, '\t');
		if (v.isObject())
		{
			QJsonObject o = v.toObject();
			if (o.isEmpty()) { out += "{}"; return; }
			out += "{\n";
			QStringList keys = o.keys();
			for (int i = 0; i < keys.size(); i++)
			{
				out += padIn + strToJson(keys[i]) + ": ";
				writeJsonValue(o.value(keys[i]), out, indent + 1);
				if (i < keys.size() - 1) out += ",";
				out += "\n";
			}
			out += pad + "}";
		}
		else if (v.isArray())
		{
			QJsonArray a = v.toArray();
			if (a.isEmpty()) { out += "[]"; return; }
			// Compact numeric arrays (vectors) on one line
			bool allNum = true;
			for (const QJsonValue& e : a) if (!e.isDouble()) { allNum = false; break; }
			if (allNum && a.size() <= 4)
			{
				out += "[";
				for (int i = 0; i < a.size(); i++)
				{
					out += numToJson(a[i].toDouble());
					if (i < a.size() - 1) out += ", ";
				}
				out += "]";
				return;
			}
			out += "[\n";
			for (int i = 0; i < a.size(); i++)
			{
				out += padIn;
				writeJsonValue(a[i], out, indent + 1);
				if (i < a.size() - 1) out += ",";
				out += "\n";
			}
			out += pad + "]";
		}
		else if (v.isBool()) out += v.toBool() ? "true" : "false";
		else if (v.isDouble()) out += numToJson(v.toDouble());
		else if (v.isString()) out += strToJson(v.toString());
		else out += "null";
	}

	static QString writeJson(const QJsonObject& root)
	{
		QString out;
		writeJsonValue(root, out, 0);
		out += "\n";
		return out;
	}

	// -------------------------------------------------------------- warnings

	struct Warnings
	{
		QStringList codes;
		QStringList messages;
		void raw(const QString& code, const QString& message)
		{
			if (codes.contains(code)) return;
			codes.append(code);
			messages.append(message);
		}
	};

	struct ConvError
	{
		bool failed = false;
		QString code, message;
	};

	// ------------------------------------------------------- bbmodel access

	struct Image
	{
		QImage img;      // Format_RGBA8888 when valid
		bool valid = false;
		int width = 0, height = 0;
	};

	static bool isCubeEl(const QJsonObject& el)
	{
		return el.value("type").toString("cube") == "cube" || el.value("type").isUndefined();
	}

	static bool cubeBoxUv(const QJsonObject& el, const QJsonObject& meta)
	{
		if (el.contains("box_uv")) return el.value("box_uv").toBool();
		if (meta.contains("box_uv")) return meta.value("box_uv").toBool();
		return true;
	}

	struct Dims { int w = 0, h = 0, d = 0; };

	// AUTHORED spans (BB texture-pixel units) that drive the box-UV layout.
	// When bakeHeadDisplay has rescaled the geometry it stashes the pre-bake
	// spans in mi3_spans_prebake: the ORIGINAL spans keep driving the UV
	// layout so the art stays 1:1 with the source texture, and buildShape
	// reconciles the scaled geometry through the shape's "scale" field.
	static void cubeSpans(const QJsonObject& el, double out[3])
	{
		QJsonArray pre = el.value("mi3_spans_prebake").toArray();
		if (pre.size() >= 3)
		{
			out[0] = std::abs(pre[0].toDouble());
			out[1] = std::abs(pre[1].toDouble());
			out[2] = std::abs(pre[2].toDouble());
			return;
		}
		Vec3 from = jsonVec3(el.value("from"));
		Vec3 to = jsonVec3(el.value("to"));
		out[0] = std::abs(to.x - from.x);
		out[1] = std::abs(to.y - from.y);
		out[2] = std::abs(to.z - from.z);
	}

	// Integer dims ceil'd to whole pixels; degenerate axes stay 0. The 1e-9
	// slack keeps spans like 8.000000000000001 (fractional endpoints, exact
	// integer size) at 8.
	static int spanPx(double v) { return std::max(0, (int)std::ceil(v - 1e-9)); }
	static Dims cubeDims(const QJsonObject& el)
	{
		double s[3];
		cubeSpans(el, s);
		Dims d;
		d.w = spanPx(s[0]);
		d.h = spanPx(s[1]);
		d.d = spanPx(s[2]);
		return d;
	}

	struct Slot { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
	struct Slots { Slot up, down, east, north, west, south; };
	static Slots boxUvSlots(int w, int h, int d)
	{
		Slots s;
		s.up = { d, 0, d + w, d };
		s.down = { d + w, 0, d + 2 * w, d };
		s.east = { 0, d, d, d + h };
		s.north = { d, d, d + w, d + h };
		s.west = { d + w, d, 2 * d + w, d + h };
		s.south = { 2 * d + w, d, 2 * d + 2 * w, d + h };
		return s;
	}
	static Slot slotFor(const Slots& s, const QString& dir)
	{
		if (dir == "up") return s.up;
		if (dir == "down") return s.down;
		if (dir == "east") return s.east;
		if (dir == "north") return s.north;
		if (dir == "west") return s.west;
		return s.south;
	}
	static void footprintSize(int w, int h, int d, int& fw, int& fh) { fw = 2 * d + 2 * w; fh = d + h; }

	// Blockbench-canonical synthesized faces for box_uv cubes without faces[]
	static QJsonObject synthesizeBoxFaces(const QJsonObject& el, int textureRef)
	{
		Dims dm = cubeDims(el);
		QJsonArray offA = el.value("uv_offset").toArray();
		double offX = offA.size() > 0 ? offA[0].toDouble() : 0;
		double offY = offA.size() > 1 ? offA[1].toDouble() : 0;
		Slots uvslots = boxUvSlots(dm.w, dm.h, dm.d); // "slots" is a Qt macro keyword
		bool mirror = el.value("mirror_uv").toBool();
		QJsonObject faces;
		for (const char* dirC : FACE_DIRS)
		{
			QString dir = dirC;
			QString src = dir;
			if (mirror && dir == "east") src = "west";
			if (mirror && dir == "west") src = "east";
			Slot s = slotFor(uvslots, src);
			double uv[4] = { offX + s.x0, offY + s.y0, offX + s.x1, offY + s.y1 };
			if (dir == "up") { std::swap(uv[0], uv[2]); std::swap(uv[1], uv[3]); }
			else if (dir == "down") std::swap(uv[0], uv[2]);
			if (mirror) std::swap(uv[0], uv[2]);
			QJsonObject face;
			face["uv"] = QJsonArray { uv[0], uv[1], uv[2], uv[3] };
			face["texture"] = textureRef;
			face["rotation"] = 0;
			faces[dir] = face;
		}
		return faces;
	}

	static void destToFaceLerp(const QString& dir, double u, double v, double& lu, double& lv)
	{
		if (dir == "up") { lu = 1 - u; lv = 1 - v; }
		else if (dir == "down") { lu = 1 - u; lv = v; }
		else { lu = u; lv = v; }
	}

	// texture ref (index | id | uuid | null) -> index or -1
	struct TexResolver
	{
		QMap<QString, int> byAlias;
		int count = 0;
		int resolve(const QJsonValue& ref) const
		{
			if (ref.isNull() || ref.isUndefined() || (ref.isBool() && !ref.toBool())) return -1;
			if (ref.isDouble())
			{
				int i = (int)ref.toDouble();
				return (i >= 0 && i < count) ? i : -1;
			}
			QString s = ref.isString() ? ref.toString() : QString();
			if (byAlias.contains(s)) return byAlias.value(s);
			bool okNum = false;
			int asNum = s.toInt(&okNum);
			if (okNum && asNum >= 0 && asNum < count) return asNum;
			return -1;
		}
	};
	static TexResolver makeTextureResolver(const QJsonArray& textures)
	{
		TexResolver r;
		r.count = textures.size();
		for (int i = 0; i < textures.size(); i++)
		{
			QJsonObject t = textures[i].toObject();
			if (t.contains("id") && !r.byAlias.contains(t.value("id").toVariant().toString()))
				r.byAlias.insert(t.value("id").toVariant().toString(), i);
			if (t.contains("uuid") && !r.byAlias.contains(t.value("uuid").toString()))
				r.byAlias.insert(t.value("uuid").toString(), i);
		}
		return r;
	}

	// --------------------------------------------------- uv space + sampling

	struct CubeInfo
	{
		QJsonObject el;
		QJsonObject faces;
		int w = 0, h = 0, d = 0;
		int fpw = 1, fph = 1;
	};

	struct UvSpace { double width = 0, height = 0; bool valid = false; };

	static QMap<int, UvSpace> chooseUvSpaces(const QJsonObject& bb, const std::vector<CubeInfo>& infos,
		const QJsonArray& textures, const std::vector<Image>& images, const TexResolver& resolver)
	{
		QMap<int, std::vector<std::array<double, 4>>> rectsByIndex;
		for (const CubeInfo& info : infos)
		{
			for (const char* dirC : FACE_DIRS)
			{
				QJsonObject face = info.faces.value(dirC).toObject();
				if (face.isEmpty() || (face.contains("enabled") && !face.value("enabled").toBool(true))) continue;
				if (!face.value("uv").isArray()) continue;
				int idx = resolver.resolve(face.value("texture"));
				if (idx < 0) continue;
				QJsonArray uv = face.value("uv").toArray();
				if (uv.size() < 4) continue;
				std::array<double, 4> rect = { uv[0].toDouble(), uv[1].toDouble(), uv[2].toDouble(), uv[3].toDouble() };
				rectsByIndex[idx].push_back(rect);
			}
		}

		const double EPS = 0.51;
		auto inBoundsCount = [&](const std::vector<std::array<double, 4>>& rects, double w, double h) {
			int n = 0;
			for (const std::array<double, 4>& uv : rects)
			{
				double xmin = std::min(uv[0], uv[2]), xmax = std::max(uv[0], uv[2]);
				double ymin = std::min(uv[1], uv[3]), ymax = std::max(uv[1], uv[3]);
				if (xmin >= -EPS && xmax <= w + EPS && ymin >= -EPS && ymax <= h + EPS) n++;
			}
			return n;
		};

		QJsonObject res = bb.value("resolution").toObject();
		QMap<int, UvSpace> spaceByIndex;
		for (auto it = rectsByIndex.begin(); it != rectsByIndex.end(); ++it)
		{
			int idx = it.key();
			const auto& rects = it.value();
			QJsonObject tex = textures[idx].toObject();
			const Image* img = (idx < (int)images.size()) ? &images[idx] : nullptr;

			std::vector<UvSpace> candidates;
			double tw = tex.value("uv_width").toDouble(), th = tex.value("uv_height").toDouble();
			if (tw > 0 && th > 0) candidates.push_back({ tw, th, true });
			double rw = res.value("width").toDouble(), rh = res.value("height").toDouble();
			if (rw > 0 && rh > 0) candidates.push_back({ rw, rh, true });
			if (img && img->width > 0 && img->height > 0) candidates.push_back({ (double)img->width, (double)img->height, true });
			if (candidates.empty()) { spaceByIndex[idx] = UvSpace(); continue; }

			UvSpace chosen; chosen.valid = false;
			UvSpace best = candidates[0];
			int bestScore = -1;
			for (const UvSpace& c : candidates)
			{
				int score = inBoundsCount(rects, c.width, c.height);
				if (!chosen.valid && score >= (int)rects.size() * 0.9) chosen = c;
				if (score > bestScore) { best = c; bestScore = score; }
			}
			spaceByIndex[idx] = chosen.valid ? chosen : best;
		}
		return spaceByIndex;
	}

	static void sampleSourceFaceUv(double destU, double destV, const QJsonObject& face, double& outU, double& outV)
	{
		double lx = destU, ly = destV;
		double rotD = face.value("rotation").toDouble();
		int rotation = ((int)std::llround(rotD) % 360 + 360) % 360;
		int steps = rotation / 90;
		for (int i = 0; i < steps; i++)
		{
			double nlx = ly, nly = 1 - lx;
			lx = nlx; ly = nly;
		}
		QJsonArray uv = face.value("uv").toArray();
		double u0 = uv[0].toDouble(), v0 = uv[1].toDouble(), u1 = uv[2].toDouble(), v1 = uv[3].toDouble();
		outU = u0 + (u1 - u0) * lx;
		outV = v0 + (v1 - v0) * ly;
	}

	static void sampleNearest(const Image& img, double x, double y, uchar px[4])
	{
		int ix = (int)std::floor(x), iy = (int)std::floor(y);
		px[0] = px[1] = px[2] = px[3] = 0;
		if (ix < 0 || iy < 0 || ix >= img.width || iy >= img.height) return;
		const uchar* line = img.img.constScanLine(iy);
		const uchar* p = line + ix * 4;
		px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; px[3] = p[3];
	}

	// ------------------------------------------- one-sided thin panel checks

	struct FacePair { int axis; const char* a; const char* b; bool flipU; };
	static const FacePair FACE_PAIRS[3] = {
		{ 0, "east", "west", true },
		{ 1, "up", "down", false },
		{ 2, "north", "south", true },
	};

	static void cubeSizes(const QJsonObject& el, double out[3])
	{
		Vec3 from = jsonVec3(el.value("from"));
		Vec3 to = jsonVec3(el.value("to"));
		out[0] = std::abs(to.x - from.x);
		out[1] = std::abs(to.y - from.y);
		out[2] = std::abs(to.z - from.z);
	}
	static double cubeInflate(const QJsonObject& el)
	{
		double v = el.value("inflate").toDouble();
		return std::isfinite(v) ? v : 0;
	}

	static double faceRectOpacity(const QJsonObject& face, const Image* img, double uvW, double uvH)
	{
		if (!img || !img->valid || uvW <= 0 || uvH <= 0) return -1;
		QJsonArray uv = face.value("uv").toArray();
		if (uv.size() < 4) return -1;
		double scaleX = img->width / uvW;
		double scaleY = img->height / uvH;
		if (std::abs(scaleX - scaleY) > 1e-9) scaleY = scaleX;
		const int N = 12;
		int opaque = 0, total = 0;
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
			{
				double u = uv[0].toDouble() + (uv[2].toDouble() - uv[0].toDouble()) * ((i + 0.5) / N);
				double v = uv[1].toDouble() + (uv[3].toDouble() - uv[1].toDouble()) * ((j + 0.5) / N);
				uchar px[4];
				sampleNearest(*img, u * scaleX, v * scaleY, px);
				total++;
				if (px[3] > 8) opaque++;
			}
		return total ? (double)opaque / total : -1;
	}

	static bool detectOneSidedPanels(const QJsonObject& bb, const std::vector<QJsonObject>& cubes,
		const QJsonArray& textures, const std::vector<Image>& images, const TexResolver& resolver)
	{
		QJsonObject res = bb.value("resolution").toObject();
		for (const QJsonObject& el : cubes)
		{
			double sizes[3];
			cubeSizes(el, sizes);
			double infl = cubeInflate(el);
			QJsonObject faces = el.value("faces").toObject();
			for (const FacePair& pair : FACE_PAIRS)
			{
				if (sizes[pair.axis] + 2 * infl > 2.001) continue;
				QJsonObject fa = faces.value(pair.a).toObject();
				QJsonObject fb = faces.value(pair.b).toObject();
				if (fa.isEmpty() || fb.isEmpty()) continue;
				auto measure = [&](const QJsonObject& face) -> double {
					int idx = resolver.resolve(face.value("texture"));
					if (idx < 0) return 0;
					QJsonObject tex = textures[idx].toObject();
					const Image* img = (idx < (int)images.size()) ? &images[idx] : nullptr;
					double uvW = 0, uvH = 0;
					if (tex.value("uv_width").toDouble() > 0 && tex.value("uv_height").toDouble() > 0)
					{ uvW = tex.value("uv_width").toDouble(); uvH = tex.value("uv_height").toDouble(); }
					else if (res.value("width").toDouble() > 0)
					{ uvW = res.value("width").toDouble(); uvH = res.value("height").toDouble(); }
					else if (img) { uvW = img->width; uvH = img->height; }
					return faceRectOpacity(face, img, uvW, uvH);
				};
				double covA = measure(fa);
				double covB = measure(fb);
				if (covA < 0 || covB < 0) continue;
				if ((covA >= 0.3 && covB <= 0.05) || (covB >= 0.3 && covA <= 0.05)) return true;
			}
		}
		return false;
	}

	// ------------------------------------------------------- shelf packing

	struct Placement { int x = 0, y = 0; };

	static int nextPowerOfTwo(int n) { int p = 1; while (p < n) p *= 2; return p; }

	static bool tryShelfPack(const std::vector<std::array<int, 3>>& sorted, int side, std::vector<Placement>& out)
	{
		out.assign(sorted.size(), Placement());
		int shelfY = 0, shelfHeight = 0, cursorX = 0;
		for (const auto& fp : sorted)
		{
			int w = fp[0], h = fp[1], index = fp[2];
			if (w > side || h > side) return false;
			if (cursorX + w > side) { shelfY += shelfHeight; cursorX = 0; shelfHeight = 0; }
			if (shelfY + h > side) return false;
			out[index] = { cursorX, shelfY };
			cursorX += w;
			shelfHeight = std::max(shelfHeight, h);
		}
		return true;
	}

	static bool packFootprints(const std::vector<std::pair<int, int>>& footprints, int gutter,
		std::vector<Placement>& placements, int& sizeOut)
	{
		int g = std::min(16, std::max(0, gutter));
		std::vector<std::array<int, 3>> sorted;
		long long totalArea = 0;
		for (int i = 0; i < (int)footprints.size(); i++)
		{
			int w = footprints[i].first + 2 * g, h = footprints[i].second + 2 * g;
			sorted.push_back({ w, h, i });
			totalArea += (long long)w * h;
		}
		std::sort(sorted.begin(), sorted.end(), [](const std::array<int, 3>& a, const std::array<int, 3>& b) {
			return a[1] != b[1] ? a[1] > b[1] : a[0] > b[0];
		});
		int side = nextPowerOfTwo(std::max(1, (int)std::ceil(std::sqrt((double)totalArea))));
		for (int attempt = 0; attempt < 24; attempt++)
		{
			std::vector<Placement> raw;
			if (tryShelfPack(sorted, side, raw))
			{
				placements.resize(footprints.size());
				for (int i = 0; i < (int)footprints.size(); i++)
					placements[i] = { raw[i].x + g, raw[i].y + g };
				sizeOut = side;
				return true;
			}
			side *= 2;
		}
		return false;
	}

	// ------------------------------------------------------------- rebake

	struct RebakeResult
	{
		QImage image;
		QMap<QString, QPoint> uvOffsetByUuid;
		int size = 0;
		int facesAttempted = 0, facesSampled = 0;
		int untexturedCubes = 0, backsideFilled = 0;
		bool flipbook = false;
		bool ok = false;
	};

	static RebakeResult rebakeAtlas(const QJsonObject& bb, const std::vector<QJsonObject>& cubes,
		const QJsonArray& textures, const std::vector<Image>& images, const TexResolver& resolver, int gutter)
	{
		RebakeResult out;

		int primaryIndex = -1;
		for (const QJsonObject& el : cubes)
		{
			QJsonObject faces = el.value("faces").toObject();
			for (const char* dirC : FACE_DIRS)
			{
				QJsonObject f = faces.value(dirC).toObject();
				if (f.isEmpty()) continue;
				int idx = resolver.resolve(f.value("texture"));
				if (idx >= 0) { primaryIndex = idx; break; }
			}
			if (primaryIndex >= 0) break;
		}
		if (primaryIndex < 0 && textures.size()) primaryIndex = 0;

		std::vector<CubeInfo> infos;
		for (const QJsonObject& el : cubes)
		{
			CubeInfo info;
			info.el = el;
			Dims dm = cubeDims(el);
			info.w = dm.w; info.h = dm.h; info.d = dm.d;
			int fw, fh;
			footprintSize(dm.w, dm.h, dm.d, fw, fh);
			info.fpw = std::max(fw, 1);
			info.fph = std::max(fh, 1);
			QJsonObject faces = el.value("faces").toObject();
			bool hasAnyFace = false;
			for (const char* dirC : FACE_DIRS)
			{
				QJsonObject f = faces.value(dirC).toObject();
				if (!f.isEmpty() && f.value("uv").isArray()) { hasAnyFace = true; break; }
			}
			info.faces = hasAnyFace ? faces : synthesizeBoxFaces(el, primaryIndex);
			infos.push_back(info);
		}

		std::vector<std::pair<int, int>> footprints;
		for (const CubeInfo& c : infos) footprints.push_back({ c.fpw, c.fph });
		std::vector<Placement> placements;
		int size = 0;
		if (!packFootprints(footprints, gutter, placements, size)) return out;

		QMap<int, UvSpace> uvSpaceByIndex = chooseUvSpaces(bb, infos, textures, images, resolver);

		std::vector<uchar> atlas((size_t)size * size * 4, 0);
		auto setPixel = [&](int x, int y, const uchar px[4]) {
			size_t i = ((size_t)y * size + x) * 4;
			atlas[i] = px[0]; atlas[i + 1] = px[1]; atlas[i + 2] = px[2]; atlas[i + 3] = px[3];
		};
		auto slotOpaqueFraction = [&](const Placement& origin, const Slot& slot) -> double {
			int w = slot.x1 - slot.x0, h = slot.y1 - slot.y0;
			if (w <= 0 || h <= 0) return -1;
			int opaque = 0;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					if (atlas[(((size_t)origin.y + slot.y0 + y) * size + (origin.x + slot.x0 + x)) * 4 + 3] > 8) opaque++;
			return (double)opaque / (w * h);
		};

		for (int ci = 0; ci < (int)infos.size(); ci++)
		{
			const CubeInfo& info = infos[ci];
			const Placement& origin = placements[ci];
			Slots uvslots = boxUvSlots(info.w, info.h, info.d);

			bool cubeHadTexturedFace = false, cubeHadRealSlot = false;
			for (const char* dirC : FACE_DIRS)
			{
				QString dir = dirC;
				Slot slot = slotFor(uvslots, dir);
				int slotW = slot.x1 - slot.x0, slotH = slot.y1 - slot.y0;
				if (slotW <= 0 || slotH <= 0) continue;
				cubeHadRealSlot = true;

				QJsonObject face = info.faces.value(dir).toObject();
				int texIndex = face.isEmpty() ? -1 : resolver.resolve(face.value("texture"));
				bool faceEnabled = !face.isEmpty() && face.value("enabled").toBool(true) && texIndex >= 0 && face.value("uv").isArray();
				const Image* img = (faceEnabled && texIndex < (int)images.size()) ? &images[texIndex] : nullptr;
				UvSpace uvSpace = faceEnabled ? uvSpaceByIndex.value(texIndex) : UvSpace();
				bool canSample = img && img->valid && uvSpace.width > 0 && uvSpace.height > 0;
				if (faceEnabled)
				{
					out.facesAttempted++;
					cubeHadTexturedFace = true;
					if (canSample) out.facesSampled++;
				}

				double scaleX = 0, scaleY = 0;
				if (canSample)
				{
					scaleX = img->width / uvSpace.width;
					scaleY = img->height / uvSpace.height;
					if (std::abs(scaleX - scaleY) > 1e-9) { scaleY = scaleX; out.flipbook = true; }
				}

				for (int y = 0; y < slotH; y++)
					for (int x = 0; x < slotW; x++)
					{
						uchar px[4] = { 0, 0, 0, 0 };
						if (canSample)
						{
							double destU, destV;
							destToFaceLerp(dir, (x + 0.5) / slotW, (y + 0.5) / slotH, destU, destV);
							double su, sv;
							sampleSourceFaceUv(destU, destV, face, su, sv);
							sampleNearest(*img, su * scaleX, sv * scaleY, px);
						}
						setPixel(origin.x + slot.x0 + x, origin.y + slot.y0 + y, px);
					}
			}
			if (cubeHadRealSlot && !cubeHadTexturedFace) out.untexturedCubes++;

			Vec3 fromEl = jsonVec3(info.el.value("from"));
			Vec3 toEl = jsonVec3(info.el.value("to"));
			bool cubeIsInverted = toEl.x < fromEl.x || toEl.y < fromEl.y || toEl.z < fromEl.z;

			// Inverted (inside-out) cubes: Blockbench builds a true MIRROR — every
			// face quad moves to the OPPOSITE side of the box, taking its art along.
			// The engine's invert flag only flips winding in place, so each wall
			// would sample its own side's art. Swap the opposing slot pairs (with
			// the same U-mirror the backside fill uses) so every visible interior
			// wall shows exactly the art Blockbench shows. (Verified with a
			// color-per-face probe cube against web.blockbench.net.)
			if (cubeIsInverted)
			{
				for (const FacePair& pair : FACE_PAIRS)
				{
					Slot slotA = slotFor(uvslots, pair.a), slotB = slotFor(uvslots, pair.b);
					int w = slotA.x1 - slotA.x0, h = slotA.y1 - slotA.y0;
					if (w <= 0 || h <= 0) continue;
					if (slotB.x1 - slotB.x0 != w || slotB.y1 - slotB.y0 != h) continue;
					for (int y = 0; y < h; y++)
						for (int x = 0; x < w; x++)
						{
							int sx = pair.flipU ? (w - 1 - x) : x;
							size_t ai = (((size_t)origin.y + slotA.y0 + y) * size + (origin.x + slotA.x0 + sx)) * 4;
							size_t bi = (((size_t)origin.y + slotB.y0 + y) * size + (origin.x + slotB.x0 + x)) * 4;
							for (int c = 0; c < 4; c++) std::swap(atlas[ai + c], atlas[bi + c]);
						}
				}
			}

			// backside fill for one-sided thin panels.
			// NEVER for: (a) front-side formats (java_block/java_item render with
			// backface culling in Blockbench, so a transparent face is an artist's
			// intentional hidden wall — outline packs rely on this), or (b)
			// inverted cubes (transparent interior walls are intentional).
			QString fmtId = bb.value("meta").toObject().value("model_format").toString();
			bool frontSideFormat = (fmtId == "java_block" || fmtId == "java_item");
			if (frontSideFormat || cubeIsInverted) continue;
			double sizesEl[3];
			cubeSizes(info.el, sizesEl);
			double inflEl = cubeInflate(info.el);
			for (const FacePair& pair : FACE_PAIRS)
			{
				if (sizesEl[pair.axis] + 2 * inflEl > 2.001) continue;
				Slot slotA = slotFor(uvslots, pair.a), slotB = slotFor(uvslots, pair.b);
				double covA = slotOpaqueFraction(origin, slotA);
				double covB = slotOpaqueFraction(origin, slotB);
				if (covA < 0 || covB < 0) continue;
				Slot src, dst;
				bool doFill = false;
				if (covA >= 0.3 && covB <= 0.05) { src = slotA; dst = slotB; doFill = true; }
				else if (covB >= 0.3 && covA <= 0.05) { src = slotB; dst = slotA; doFill = true; }
				if (!doFill) continue;
				int w = src.x1 - src.x0, h = src.y1 - src.y0;
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
					{
						int sx = pair.flipU ? (w - 1 - x) : x;
						size_t si = (((size_t)origin.y + src.y0 + y) * size + (origin.x + src.x0 + sx)) * 4;
						size_t di = (((size_t)origin.y + dst.y0 + y) * size + (origin.x + dst.x0 + x)) * 4;
						atlas[di] = atlas[si]; atlas[di + 1] = atlas[si + 1];
						atlas[di + 2] = atlas[si + 2]; atlas[di + 3] = atlas[si + 3];
					}
				out.backsideFilled++;
			}
		}

		for (int i = 0; i < (int)infos.size(); i++)
			out.uvOffsetByUuid.insert(infos[i].el.value("uuid").toString(), QPoint(placements[i].x, placements[i].y));

		out.image = QImage(atlas.data(), size, size, size * 4, QImage::Format_RGBA8888).copy();
		out.size = size;
		out.ok = true;
		return out;
	}

	// ---------------------------------------------------------- shape build

	static bool isPanelShape(const QJsonObject& shape)
	{
		if (!shape.value("from").isArray() || !shape.value("to").isArray()) return false;
		Vec3 from = jsonVec3(shape.value("from"));
		Vec3 to = jsonVec3(shape.value("to"));
		double dims[3] = { std::abs(to.x - from.x), std::abs(to.y - from.y), std::abs(to.z - from.z) };
		std::sort(dims, dims + 3);
		return dims[0] <= 2 && 2 * dims[0] <= dims[1];
	}

	static QJsonObject buildShape(const QJsonObject& el, Vec3 sumAncestorPosMi, const QPoint* uvOverride,
		Warnings& warnings, bool& flatThickened)
	{
		Vec3 from = jsonVec3(el.value("from"));
		Vec3 to = jsonVec3(el.value("to"));

		double inflate = cubeInflate(el);
		QJsonArray stretchA = el.value("stretch").toArray();
		double stretch[3] = { 1, 1, 1 };
		if (stretchA.size() >= 3) { stretch[0] = stretchA[0].toDouble(1); stretch[1] = stretchA[1].toDouble(1); stretch[2] = stretchA[2].toDouble(1); }
		bool nonUnitStretch = !(stretch[0] == 1 && stretch[1] == 1 && stretch[2] == 1);
		if (nonUnitStretch)
		{
			warnings.raw("STRETCH_APPROXIMATED", "non-uniform stretch was approximated by resizing the box geometry.");
			Vec3 center = { (from.x + to.x) / 2, (from.y + to.y) / 2, (from.z + to.z) / 2 };
			from = { center.x + (from.x - center.x) * stretch[0], center.y + (from.y - center.y) * stretch[1], center.z + (from.z - center.z) * stretch[2] };
			to = { center.x + (to.x - center.x) * stretch[0], center.y + (to.y - center.y) * stretch[1], center.z + (to.z - center.z) * stretch[2] };
		}

		// Blockbench-effective box: BB pushes (from -= inflate, to += inflate) in
		// the ORIGINAL (possibly inverted) from/to frame. With a negative inflate
		// an axis can flip through zero — a flat fin becomes an INVERTED
		// 2*|inflate| shell (outline packs rely on this for rims around flat
		// parts). Derive the invert flag and the emitted geometry from the
		// EFFECTIVE box, never the raw one.
		Vec3 effFrom = { from.x - inflate, from.y - inflate, from.z - inflate };
		Vec3 effTo = { to.x + inflate, to.y + inflate, to.z + inflate };
		double effDim[3] = { effTo.x - effFrom.x, effTo.y - effFrom.y, effTo.z - effFrom.z };
		int negAxes = (effDim[0] < -1e-9 ? 1 : 0) + (effDim[1] < -1e-9 ? 1 : 0) + (effDim[2] < -1e-9 ? 1 : 0);
		double rawSpan[3] = { std::abs(to.x - from.x), std::abs(to.y - from.y), std::abs(to.z - from.z) };
		double delta[3] = { std::abs(effDim[0]) - rawSpan[0], std::abs(effDim[1]) - rawSpan[1], std::abs(effDim[2]) - rawSpan[2] };
		double deltaMin = std::min({ delta[0], delta[1], delta[2] });
		double deltaMax = std::max({ delta[0], delta[1], delta[2] });
		// Uniform delta → expressible as the engine's scalar inflate on the raw
		// box (exact UV layout). Mixed (an axis flipped through zero) → bake the
		// effective box into from/to; the scale machinery below keeps the UV
		// spans authored-exact (sub-texel drift only on the flipped axis).
		bool bakeEff = (deltaMax - deltaMin) > 1e-6;
		double inflateOut = bakeEff ? 0.0 : deltaMin / 2.0;
		if (bakeEff) { from = effFrom; to = effTo; }

		Vec3 bbMin, bbMax;
		minMax3(from, to, bbMin, bbMax);

		// The engine derives EVERY box-UV span and offset from |to - from| in
		// texture pixels, so the emitted spans must equal the integer dims the
		// UV layout (atlas slots / authored art) was built for. Fractional
		// true spans (head-bake scaling, fractional authoring, stretch) move
		// into the shape's "scale" field instead: the engine computes geometry
		// as from/to x scale but keeps reading UVs from the raw spans. T() is
		// a pure per-axis sign map, so BB axis i maps to MI axis i and the
		// per-axis factors carry over 1:1.
		Dims dm = cubeDims(el);
		double spans[3] = { std::abs(to.x - from.x), std::abs(to.y - from.y), std::abs(to.z - from.z) };
		double k[3] = {
			(dm.w > 0 && spans[0] > 1e-12) ? spans[0] / dm.w : 1.0,
			(dm.h > 0 && spans[1] > 1e-12) ? spans[1] / dm.h : 1.0,
			(dm.d > 0 && spans[2] > 1e-12) ? spans[2] / dm.d : 1.0,
		};
		bool hasScale = std::abs(k[0] - 1) > 1e-9 || std::abs(k[1] - 1) > 1e-9 || std::abs(k[2] - 1) > 1e-9;

		double depth = dm.d;
		double uvOffX, uvOffY;
		if (uvOverride) { uvOffX = uvOverride->x(); uvOffY = uvOverride->y(); }
		else
		{
			QJsonArray offA = el.value("uv_offset").toArray();
			uvOffX = offA.size() > 0 ? offA[0].toDouble() : 0;
			uvOffY = offA.size() > 1 ? offA[1].toDouble() : 0;
		}

		Vec3 rotation = jsonVec3(el.value("rotation"));
		bool hasOwnRotation = !isZero3(rotation);

		QJsonObject shape;
		shape["type"] = "block";
		shape["uv"] = QJsonArray { uvOffX + depth, uvOffY + depth };

		if (!hasOwnRotation)
		{
			Vec3 tMin = T(bbMin), tMax = T(bbMax), mi1, mi2;
			minMax3(tMin, tMax, mi1, mi2);
			shape["from"] = vecArr(mi1);
			shape["to"] = vecArr(mi2);
			shape["position"] = vecArr(neg3(sumAncestorPosMi));
		}
		else
		{
			Vec3 origin = jsonVec3(el.value("origin"));
			Vec3 pMi = T(origin);
			Vec3 tMin = sub3(T(bbMin), pMi);
			Vec3 tMax = sub3(T(bbMax), pMi);
			Vec3 mi1, mi2;
			minMax3(tMin, tMax, mi1, mi2);
			shape["from"] = vecArr(mi1);
			shape["to"] = vecArr(mi2);
			shape["position"] = vecArr(sub3(pMi, sumAncestorPosMi));
			shape["rotation"] = vecArr(eulerBBtoMI(rotation));
		}

		if (hasScale)
		{
			Vec3 sf = jsonVec3(shape.value("from"));
			Vec3 st = jsonVec3(shape.value("to"));
			shape["from"] = vecArr(zv({ sf.x / k[0], sf.y / k[1], sf.z / k[2] }));
			shape["to"] = vecArr(zv({ st.x / k[0], st.y / k[1], st.z / k[2] }));
			shape["scale"] = QJsonArray { k[0], k[1], k[2] };
		}

		if (negAxes % 2 == 1) shape["invert"] = true;

		double minEff = std::min({ std::abs(to.x - from.x), std::abs(to.y - from.y), std::abs(to.z - from.z) }) + 2 * inflateOut;
		if (minEff < 0.001)
		{
			inflateOut += (0.02 - minEff) / 2;
			flatThickened = true;
		}
		// The engine computes geometry as (from - inflate) x scale, i.e.
		// inflate is in pre-scale units — divide so the effective (world)
		// inflate stays what was intended (mean k: exact for the uniform
		// head-bake case, negligible approximation otherwise).
		if (hasScale && inflateOut != 0)
			inflateOut /= (k[0] + k[1] + k[2]) / 3.0;
		if (inflateOut != 0) shape["inflate"] = inflateOut;
		if (!uvOverride && el.value("mirror_uv").toBool()) shape["texture_mirror"] = true;

		return shape;
	}

	// -------------------------------------------------------- outliner walk

	static QJsonObject groupDataFor(const QJsonObject& bb, const QJsonObject& node)
	{
		QString uuid = node.value("uuid").toString();
		if (uuid.isEmpty()) return QJsonObject();
		for (const QJsonValue& gv : bb.value("groups").toArray())
		{
			QJsonObject g = gv.toObject();
			if (g.value("uuid").toString() == uuid) return g;
		}
		if (node.value("name").isString() && node.value("origin").isArray()) return node;
		return QJsonObject();
	}

	// ------------------------------------------------- miobject templates

	static const char* BODYPART_TEMPLATE_JSON = R"JSON({
		"type": "bodypart", "color_tag": "null", "hide": false, "lock": false, "ghost": false,
		"depth": 0, "default_values": {}, "lock_bend": true, "tree_extend": false,
		"inherit": { "position": true, "rotation": true, "scale": true, "alpha": true, "color": true,
			"texture": true, "surface": true, "subsurface": true, "visibility": true,
			"bend": false, "rot_point": true, "glow_color": true, "select": false, "pose": false },
		"scale_resize": false, "rot_point_custom": false, "rot_point": [0, 0, 0],
		"backfaces": false, "texture_blur": false, "texture_filtering": false,
		"shadows": true, "ssao": true, "glow": false, "glow_texture": true, "only_render_glow": false,
		"glint_mode": 0, "glint_scale": 1, "glint_speed": 1, "glint_strength": 1, "glint_tex": "default",
		"fog": true, "hq_hiding": false, "lq_hiding": false, "blend_mode": "normal", "alpha_mode": 2
	})JSON";

	static const char* MODEL_TEMPLATE_JSON = R"JSON({
		"type": "model", "color_tag": "null", "hide": false, "lock": false, "ghost": false,
		"depth": 0, "default_values": {}, "lock_bend": true, "tree_extend": false,
		"inherit": { "position": true, "rotation": true, "scale": true, "alpha": false, "color": false,
			"texture": false, "surface": true, "subsurface": true, "visibility": true,
			"bend": false, "rot_point": false, "glow_color": true, "select": false, "pose": false },
		"scale_resize": true, "rot_point_custom": false, "rot_point": [0, 0, 0],
		"backfaces": false, "texture_blur": false, "texture_filtering": false,
		"shadows": true, "ssao": true, "glow": false, "glow_texture": true, "only_render_glow": false,
		"glint_mode": 0, "glint_scale": 1, "glint_speed": 1, "glint_strength": 1, "glint_tex": "default",
		"fog": true, "hq_hiding": false, "lq_hiding": false, "blend_mode": "normal", "alpha_mode": 2
	})JSON";

	static QString nextId(QSet<QString>& seen)
	{
		static const char* ALPHA = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		QString id;
		do
		{
			id.clear();
			for (int i = 0; i < 16; i++)
				id += ALPHA[QRandomGenerator::global()->bounded(36)];
		} while (seen.contains(id));
		seen.insert(id);
		return id;
	}

	static QJsonObject buildMiObject(const QJsonObject& mimodel, const QString& modelFilename, const QString& skinFilename)
	{
		QJsonObject BODYPART = QJsonDocument::fromJson(BODYPART_TEMPLATE_JSON).object();
		QJsonObject MODEL = QJsonDocument::fromJson(MODEL_TEMPLATE_JSON).object();

		QSet<QString> seen;
		QString modelResId = nextId(seen);
		QString skinResId = nextId(seen);
		QString templateId = nextId(seen);
		QString modelTimelineId = nextId(seen);

		QJsonArray resources;
		{
			QJsonObject r1; r1["id"] = modelResId; r1["type"] = "model"; r1["filename"] = modelFilename; r1["material_format"] = 2;
			QJsonObject r2; r2["id"] = skinResId; r2["type"] = "skin"; r2["filename"] = skinFilename; r2["player_skin"] = false; r2["material_format"] = 2;
			resources.append(r1); resources.append(r2);
		}
		QJsonArray templates;
		{
			QJsonObject t; t["id"] = templateId; t["type"] = "model"; t["name"] = "";
			t["model"] = modelResId; t["model_tex"] = "null"; t["model_tex_material"] = "null"; t["model_tex_normal"] = "null";
			templates.append(t);
		}

		QJsonArray bodypartTimelines;
		std::function<QString(const QJsonObject&, const QString&, int, int)> walk =
			[&](const QJsonObject& part, const QString& parentTimelineId, int siblingIndex, int depth) -> QString
		{
			QString id = nextId(seen);
			QJsonArray childParts = part.value("parts").toArray();
			for (int i = 0; i < childParts.size(); i++)
				walk(childParts[i].toObject(), id, i, depth + 1);
			QJsonObject tl = BODYPART;
			tl["id"] = id;
			tl["name"] = "";
			tl["temp"] = templateId;
			tl["model_part_name"] = part.value("name").toString();
			tl["part_of"] = modelTimelineId;
			tl["keyframes"] = QJsonObject();
			tl["parent"] = parentTimelineId;
			tl["parent_tree_index"] = siblingIndex;
			tl["tree_extend"] = childParts.size() > 0;
			// Mirror the part's computed backfaces (inverted-hull parts need culling).
			tl["backfaces"] = part.value("backfaces").toBool(true);
			bodypartTimelines.append(tl);
			return id;
		};

		QJsonArray parts = mimodel.value("parts").toArray();
		for (int i = 0; i < parts.size(); i++)
			walk(parts[i].toObject(), modelTimelineId, i, 0);

		QJsonArray partIds;
		for (const QJsonValue& tv : bodypartTimelines)
			partIds.append(tv.toObject().value("id").toString());

		QJsonObject modelTimeline = MODEL;
		modelTimeline["id"] = modelTimelineId;
		modelTimeline["name"] = "";
		modelTimeline["temp"] = templateId;
		modelTimeline["backfaces"] = true; // Blockbench semantics: double-sided
		modelTimeline["parts"] = partIds;
		{
			QJsonObject kf0; kf0["TEXTURE_OBJ"] = skinResId;
			QJsonObject kfs; kfs["0"] = kf0;
			modelTimeline["keyframes"] = kfs;
		}
		modelTimeline["parent"] = "root";
		modelTimeline["parent_tree_index"] = 0;

		QJsonArray timelines = bodypartTimelines;
		timelines.append(modelTimeline);

		QJsonObject obj;
		obj["format"] = 34;
		obj["created_in"] = "2.0.2";
		obj["templates"] = templates;
		obj["timelines"] = timelines;
		obj["resources"] = resources;
		return obj;
	}

	// ---------------------------------------------------- bake head display

	static bool bakeHeadDisplay(QJsonObject& bb, Warnings& warnings)
	{
		QJsonObject display = bb.value("display").toObject();
		QJsonObject dh = display.value("head").toObject();
		if (dh.isEmpty()) return false;

		Vec3 t = jsonVec3(dh.value("translation"));
		Vec3 s = jsonVec3(dh.value("scale"), { 1, 1, 1 });
		Vec3 r = jsonVec3(dh.value("rotation"));

		double f[3] = { 0.625 * s.x, 0.625 * s.y, 0.625 * s.z };
		double add[3] = { 0.625 * t.x - f[0] * 8, 0.625 * t.y - f[1] * 8 + 4, 0.625 * t.z - f[2] * 8 };
		auto tp = [&](Vec3 p) -> Vec3 { return { f[0] * p.x + add[0], f[1] * p.y + add[1], f[2] * p.z + add[2] }; };

		QJsonArray elements = bb.value("elements").toArray();
		for (int i = 0; i < elements.size(); i++)
		{
			QJsonObject el = elements[i].toObject();
			// Stash the AUTHORED spans before rescaling: they keep driving the
			// box-UV layout (cubeSpans/cubeDims), so the art stays 1:1 with the
			// source texture and the scale factor moves into the shape's
			// "scale" field. Baking the factor into from/to instead produced
			// fractional UV spans that misread the texture (squashed geometry,
			// floating faces).
			if (el.value("from").isArray() && el.value("to").isArray())
			{
				Vec3 f0 = jsonVec3(el.value("from"));
				Vec3 t0 = jsonVec3(el.value("to"));
				el["mi3_spans_prebake"] = QJsonArray {
					std::abs(t0.x - f0.x), std::abs(t0.y - f0.y), std::abs(t0.z - f0.z) };
			}
			if (el.value("from").isArray()) el["from"] = vecArr(tp(jsonVec3(el.value("from"))));
			if (el.value("to").isArray()) el["to"] = vecArr(tp(jsonVec3(el.value("to"))));
			if (el.value("origin").isArray()) el["origin"] = vecArr(tp(jsonVec3(el.value("origin"))));
			if (el.value("inflate").isDouble()) el["inflate"] = el.value("inflate").toDouble() * f[0];
			elements[i] = el;
		}
		bb["elements"] = elements;

		QJsonArray groups = bb.value("groups").toArray();
		for (int i = 0; i < groups.size(); i++)
		{
			QJsonObject g = groups[i].toObject();
			if (g.value("origin").isArray()) g["origin"] = vecArr(tp(jsonVec3(g.value("origin"))));
			groups[i] = g;
		}
		bb["groups"] = groups;

		std::function<QJsonArray(const QJsonArray&)> walkOutliner = [&](const QJsonArray& nodes) -> QJsonArray
		{
			QJsonArray out;
			for (const QJsonValue& nv : nodes)
			{
				if (nv.isObject())
				{
					QJsonObject n = nv.toObject();
					if (n.value("origin").isArray()) n["origin"] = vecArr(tp(jsonVec3(n.value("origin"))));
					if (n.value("children").isArray()) n["children"] = walkOutliner(n.value("children").toArray());
					out.append(n);
				}
				else out.append(nv);
			}
			return out;
		};
		bb["outliner"] = walkOutliner(bb.value("outliner").toArray());

		if (!isZero3(r))
			warnings.raw("HEAD_DISPLAY_ROTATION_IGNORED", "display.head has a rotation that was not baked; the hat may need a matching manual rotation.");
		warnings.raw("HEAD_ATTACH_BAKED", "display.head placement baked in: attach the model to a head bone with zero offsets.");
		return true;
	}

	// ---------------------------------------------------------- conversion

	struct ConvResult
	{
		bool ok = false;
		QString errCode, errMessage;
		QJsonObject mimodel;
		QImage texture;
		Warnings warnings;
		int cubes = 0, parts = 0, shapes = 0, atlasSize = 0;
		bool rebaked = false;
	};

	static int countPartsDeep(const QJsonArray& parts)
	{
		int n = 0;
		for (const QJsonValue& pv : parts)
		{
			n += 1;
			n += countPartsDeep(pv.toObject().value("parts").toArray());
		}
		return n;
	}

	static ConvResult convertScene(const QJsonObject& bb, const std::vector<Image>& images, const QString& modelNameOpt)
	{
		ConvResult res;
		Warnings& warnings = res.warnings;

		// ---- structural validation
		if (!bb.value("elements").isArray()) { res.errCode = "MISSING_ELEMENTS"; res.errMessage = "the file has no valid element list."; return res; }
		if (!bb.value("outliner").isArray()) { res.errCode = "MISSING_OUTLINER"; res.errMessage = "the file has no valid outliner (hierarchy) data."; return res; }

		QJsonArray elements = bb.value("elements").toArray();
		QJsonObject meta = bb.value("meta").toObject();

		// Minecraft Java block/item models place every cube in ABSOLUTE space
		// with per-element rotation; groups are organizational pivots only and
		// their rotation is NOT part of the mesh (Blockbench bakes any group
		// rotation into the child cubes' from/to + rotation on save). Applying
		// a group's rotation on top would double-transform every cube -- they
		// swing far off their baked positions and the model scatters. So for
		// these formats we keep the group hierarchy (names/positions cancel
		// out to the cubes' raw coords) but DROP the group rotations.
		QString modelFormat = meta.value("model_format").toString();
		if (modelFormat.isEmpty()) modelFormat = bb.value("model_format").toString();
		bool javaElementSpace = (modelFormat == "java_block" || modelFormat == "java_item");

		int meshCount = 0, exportableCubes = 0;
		for (const QJsonValue& ev : elements)
		{
			QJsonObject el = ev.toObject();
			if (el.isEmpty() || (el.contains("export") && !el.value("export").toBool(true))) continue;
			QString type = el.value("type").toString();
			if (type == "mesh") meshCount++;
			else if (type == "texture_mesh") meshCount++;
			else if (isCubeEl(el)) exportableCubes++;
		}
		if (meshCount && exportableCubes == 0)
		{
			res.errCode = "ONLY_MESHES";
			res.errMessage = "this model is built entirely from free-form meshes. Mine-imator only supports box geometry - convert the meshes to cubes in Blockbench first.";
			return res;
		}
		if (meshCount)
			warnings.raw("MESHES_SKIPPED", QString::number(meshCount) + " free-form mesh element(s) were skipped (unsupported).");

		// bad numbers
		for (const QJsonValue& ev : elements)
		{
			QJsonObject el = ev.toObject();
			if (!isCubeEl(el)) continue;
			if (hasBadNumber(el.value("from")) || hasBadNumber(el.value("to")) || hasBadNumber(el.value("origin")) ||
				hasBadNumber(el.value("rotation")) || hasBadNumber(el.value("inflate")) ||
				hasBadNumber(el.value("uv_offset")) || hasBadNumber(el.value("stretch")))
			{
				res.errCode = "INVALID_GEOMETRY_VALUE";
				res.errMessage = "the model contains invalid or extreme numeric values.";
				return res;
			}
		}

		if (bb.value("animations").toArray().size())
			warnings.raw("ANIMATIONS_IGNORED", "Blockbench animations are not converted (only the rest pose is exported).");

		// ---- inclusion set (includeHidden = true, like the plugin default)
		QMap<QString, QJsonObject> elementsByUuid;
		for (const QJsonValue& ev : elements)
		{
			QJsonObject el = ev.toObject();
			if (el.contains("uuid")) elementsByUuid.insert(el.value("uuid").toString(), el);
		}

		std::vector<QJsonObject> included;
		QSet<QString> includedUuids;
		bool tooDeep = false;
		std::function<void(const QJsonArray&, int)> collectWalk = [&](const QJsonArray& nodes, int depth)
		{
			if (tooDeep) return;
			if (depth > MAX_TREE_DEPTH) { tooDeep = true; return; }
			for (const QJsonValue& nv : nodes)
			{
				if (nv.isString())
				{
					QJsonObject el = elementsByUuid.value(nv.toString());
					if (el.isEmpty() || !isCubeEl(el)) continue;
					if (el.contains("export") && !el.value("export").toBool(true)) continue;
					QString uuid = el.value("uuid").toString();
					if (!includedUuids.contains(uuid)) { includedUuids.insert(uuid); included.push_back(el); }
				}
				else if (nv.isObject())
				{
					QJsonObject node = nv.toObject();
					QJsonObject gd = groupDataFor(bb, node);
					if (!gd.isEmpty() && gd.contains("export") && !gd.value("export").toBool(true)) continue;
					collectWalk(node.value("children").toArray(), depth + 1);
					if (tooDeep) return;
				}
			}
		};
		collectWalk(bb.value("outliner").toArray(), 0);
		if (tooDeep) { res.errCode = "TREE_TOO_DEEP"; res.errMessage = "the model hierarchy is too deep to process."; return res; }
		if (included.empty())
		{
			res.errCode = "EMPTY_MODEL";
			res.errMessage = "the model contains no convertible geometry (0 exportable cubes).";
			return res;
		}

		QJsonArray textures = bb.value("textures").toArray();
		TexResolver resolver = makeTextureResolver(textures);

		// ---- rebake decision
		QStringList reasons;
		{
			bool anyPerFace = false;
			for (const QJsonObject& el : included)
				if (!cubeBoxUv(el, meta)) { anyPerFace = true; break; }
			if (anyPerFace) reasons << "per-face UV";

			// Fractional AUTHORED cube dims: the engine reads box UVs at
			// integer spans (cubeDims), so art authored on a fractional layout
			// is re-rasterized onto the integer layout; buildShape then maps
			// the fractional geometry onto it via the shape "scale" field.
			// Spans that are only fractional because of a head-display bake
			// don't count: cubeSpans returns the pre-bake layout.
			for (const QJsonObject& el : included)
			{
				double sp[3];
				cubeSpans(el, sp);
				if (std::abs(sp[0] - std::round(sp[0])) > 1e-6 ||
					std::abs(sp[1] - std::round(sp[1])) > 1e-6 ||
					std::abs(sp[2] - std::round(sp[2])) > 1e-6)
				{
					reasons << "fractional cube dimensions";
					break;
				}
			}

			QSet<int> used;
			bool unresolvedRef = false;
			for (const QJsonObject& el : included)
			{
				QJsonObject faces = el.value("faces").toObject();
				for (const char* dirC : FACE_DIRS)
				{
					QJsonObject f = faces.value(dirC).toObject();
					if (f.isEmpty()) continue;
					QJsonValue tref = f.value("texture");
					if (tref.isNull() || tref.isUndefined() || (tref.isBool() && !tref.toBool())) continue;
					int idx = resolver.resolve(tref);
					if (idx < 0) unresolvedRef = true;
					else used.insert(idx);
				}
			}
			if (used.size() > 1) reasons << "multiple textures";

			int effIndex = used.size() ? *used.begin() : (textures.size() ? 0 : -1);
			if (effIndex >= 0 && used.size() <= 1)
			{
				const Image* img = (effIndex < (int)images.size()) ? &images[effIndex] : nullptr;
				QJsonObject tex = textures[effIndex].toObject();
				double uvW = 0, uvH = 0;
				if (tex.value("uv_width").toDouble() > 0 && tex.value("uv_height").toDouble() > 0)
				{ uvW = tex.value("uv_width").toDouble(); uvH = tex.value("uv_height").toDouble(); }
				else if (bb.value("resolution").toObject().value("width").toDouble() > 0)
				{
					uvW = bb.value("resolution").toObject().value("width").toDouble();
					uvH = bb.value("resolution").toObject().value("height").toDouble();
				}
				if (img && img->width > 0 && uvW > 0 && uvH > 0 && (img->width != (int)uvW || img->height != (int)uvH))
					reasons << "texture resolution mismatch";

				double boundW = uvW > 0 ? uvW : (img ? img->width : 0);
				double boundH = uvH > 0 ? uvH : (img ? img->height : 0);
				if (boundW > 0 && boundH > 0)
				{
					for (const QJsonObject& el : included)
					{
						if (!cubeBoxUv(el, meta)) continue;
						QJsonArray offA = el.value("uv_offset").toArray();
						double ox = offA.size() > 0 ? offA[0].toDouble() : 0;
						double oy = offA.size() > 1 ? offA[1].toDouble() : 0;
						Dims dm = cubeDims(el);
						int fw, fh;
						footprintSize(dm.w, dm.h, dm.d, fw, fh);
						if (ox < 0 || oy < 0 || ox + fw > boundW || oy + fh > boundH)
						{
							reasons << "box UV outside texture bounds";
							break;
						}
					}
				}
			}
			Q_UNUSED(unresolvedRef);

			if (detectOneSidedPanels(bb, included, textures, images, resolver))
				reasons << "one-sided thin panels";
		}
		bool doRebake = !reasons.isEmpty();

		int textureSizePx = 0;
		QMap<QString, QPoint> uvOffsetByUuid;
		bool haveUvOverride = false;

		if (doRebake)
		{
			bool anyPixels = false;
			for (const Image& img : images) if (img.valid) { anyPixels = true; break; }
			if (!anyPixels && textures.size() > 0)
			{
				res.errCode = "REBAKE_NO_PIXELS";
				res.errMessage = "this model needs its textures re-baked (" + reasons.join(", ") + "), but no texture pixels could be decoded. Save the .bbmodel with embedded textures.";
				return res;
			}
			RebakeResult baked = rebakeAtlas(bb, included, textures, images, resolver, 1);
			if (!baked.ok) { res.errCode = "ATLAS_PACK_FAILED"; res.errMessage = "internal error: atlas packing failed."; return res; }
			if (textures.isEmpty())
				warnings.raw("TEXTURE_GENERATED", "the model has no textures; the generated atlas is blank.");
			if (baked.facesAttempted > 0 && baked.facesSampled == 0)
				warnings.raw("TEXTURE_REBAKE_EMPTY", "no textured face could be sampled; the model may appear untextured.");
			else if (baked.facesSampled < baked.facesAttempted)
				warnings.raw("TEXTURE_REBAKE_PARTIAL", QString::number(baked.facesAttempted - baked.facesSampled) + " face(s) could not be sampled and will appear transparent.");
			if (baked.flipbook)
				warnings.raw("ANIMATED_TEXTURE", "animated (flipbook) texture(s) detected; only the first frame was baked.");
			if (baked.untexturedCubes)
				warnings.raw("CUBES_UNTEXTURED", QString::number(baked.untexturedCubes) + " cube(s) reference no valid texture and were baked transparent.");
			if (baked.backsideFilled)
				warnings.raw("PANEL_BACKSIDE_FILLED", QString::number(baked.backsideFilled) + " one-sided thin panel(s) got their backside filled so they render from both sides.");
			warnings.raw("TEXTURE_REBAKED", QString("textures were re-baked into a single %1x%1 box-UV atlas (%2).").arg(baked.size).arg(reasons.join(", ")));

			res.texture = baked.image;
			textureSizePx = baked.size;
			uvOffsetByUuid = baked.uvOffsetByUuid;
			haveUvOverride = true;
			res.atlasSize = baked.size;
			res.rebaked = true;
		}
		else
		{
			int effIndex = -1;
			for (const QJsonObject& el : included)
			{
				QJsonObject faces = el.value("faces").toObject();
				for (const char* dirC : FACE_DIRS)
				{
					QJsonObject f = faces.value(dirC).toObject();
					if (f.isEmpty()) continue;
					QJsonValue tref = f.value("texture");
					if (tref.isNull() || tref.isUndefined() || (tref.isBool() && !tref.toBool())) continue;
					int idx = resolver.resolve(tref);
					if (idx >= 0) { effIndex = idx; break; }
				}
				if (effIndex >= 0) break;
			}
			if (effIndex < 0) effIndex = 0;

			if (effIndex >= textures.size())
			{
				warnings.raw("TEXTURE_GENERATED", "the model has no texture; a plain white 16x16 texture was generated.");
				QImage white(16, 16, QImage::Format_RGBA8888);
				white.fill(QColor(255, 255, 255, 255));
				res.texture = white;
				textureSizePx = 16;
			}
			else
			{
				const Image* img = (effIndex < (int)images.size() && images[effIndex].valid) ? &images[effIndex] : nullptr;
				if (!img)
				{
					res.errCode = "TEXTURE_UNAVAILABLE";
					res.errMessage = "no image data could be read for the model texture. Save the .bbmodel with embedded textures.";
					return res;
				}
				int w = img->width, h = img->height;
				if (w != h)
				{
					int size = std::max(w, h);
					QImage canvas(size, size, QImage::Format_RGBA8888);
					canvas.fill(QColor(0, 0, 0, 0));
					for (int y = 0; y < h; y++)
						memcpy(canvas.scanLine(y), img->img.constScanLine(y), (size_t)w * 4);
					warnings.raw("TEXTURE_PADDED_SQUARE", QString("the texture was not square (%1x%2); it was padded to %3x%3.").arg(w).arg(h).arg(size));
					res.texture = canvas;
					textureSizePx = size;
				}
				else
				{
					res.texture = img->img;
					textureSizePx = w;
				}
			}
		}

		// ---- geometry
		QSet<QString> usedPartNames;
		bool flatThickened = false;

		std::function<QJsonObject(const QJsonObject&, Vec3, Vec3, int)> convertGroupNode =
			[&](const QJsonObject& node, Vec3 parentOriginBB, Vec3 sumAncestorPosMi, int depth) -> QJsonObject
		{
			if (depth > MAX_TREE_DEPTH) return QJsonObject();
			QJsonObject g = groupDataFor(bb, node);
			if (g.isEmpty()) return QJsonObject();
			if (g.contains("export") && !g.value("export").toBool(true)) return QJsonObject();

			QString name = uniqueName(asciiName(g.value("name"), "part"), usedPartNames);
			Vec3 origin = jsonVec3(g.value("origin"));
			Vec3 posMi = T(sub3(origin, parentOriginBB));

			QJsonObject part;
			part["name"] = name;
			part["position"] = vecArr(posMi);
			Vec3 rotation = jsonVec3(g.value("rotation"));
			if (!isZero3(rotation) && !javaElementSpace) part["rotation"] = vecArr(eulerBBtoMI(rotation));
			else if (!isZero3(rotation) && javaElementSpace) warnings.raw("JAVA_GROUP_ROTATION_IGNORED", "Java model: group/bone rotations are baked into the cubes by Blockbench, so group rotations were ignored to keep the geometry from double-rotating.");

			Vec3 childSum = add3(sumAncestorPosMi, posMi);
			QJsonArray shapes;
			QJsonArray childParts;
			for (const QJsonValue& cv : node.value("children").toArray())
			{
				if (cv.isString())
				{
					QJsonObject el = elementsByUuid.value(cv.toString());
					if (el.isEmpty() || !isCubeEl(el) || !includedUuids.contains(el.value("uuid").toString())) continue;
					QPoint uvOv;
					bool hasOv = haveUvOverride && uvOffsetByUuid.contains(el.value("uuid").toString());
					if (hasOv) uvOv = uvOffsetByUuid.value(el.value("uuid").toString());
					shapes.append(buildShape(el, childSum, hasOv ? &uvOv : nullptr, warnings, flatThickened));
				}
				else if (cv.isObject())
				{
					QJsonObject sub = convertGroupNode(cv.toObject(), origin, childSum, depth + 1);
					if (!sub.isEmpty()) childParts.append(sub);
				}
			}
			if (shapes.size()) part["shapes"] = shapes;
			// Backfaces policy: double-sided by default (thin shapes stay
			// visible from behind) EXCEPT parts holding inverted-hull outline
			// cubes. The inverted-hull technique REQUIRES backface culling:
			// the shell's near faces must be culled so only the silhouette rim
			// shows (Blockbench culls too). With culling off, the inward-normal
			// shell draws on top of the model as a dark copy covering it all.
			bool hasInvertedShape = false;
			for (const QJsonValue& sv : shapes)
				if (sv.toObject().value("invert").toBool(false)) { hasInvertedShape = true; break; }
			part["backfaces"] = !hasInvertedShape;
			if (childParts.size()) part["parts"] = childParts;
			return part;
		};

		QJsonArray rootParts;
		for (const QJsonValue& nv : bb.value("outliner").toArray())
		{
			if (nv.isString())
			{
				QJsonObject el = elementsByUuid.value(nv.toString());
				if (el.isEmpty() || !isCubeEl(el) || !includedUuids.contains(el.value("uuid").toString())) continue;
				QString name = uniqueName(asciiName(el.value("name"), "cube"), usedPartNames);
				QPoint uvOv;
				bool hasOv = haveUvOverride && uvOffsetByUuid.contains(el.value("uuid").toString());
				if (hasOv) uvOv = uvOffsetByUuid.value(el.value("uuid").toString());
				QJsonObject shape = buildShape(el, { 0, 0, 0 }, hasOv ? &uvOv : nullptr, warnings, flatThickened);
				QJsonObject loosePart;
				loosePart["name"] = name;
				loosePart["position"] = vecArr({ 0, 0, 0 });
				loosePart["shapes"] = QJsonArray { shape };
				// Double-sided EXCEPT inverted-hull cubes (those need culling; see above).
				loosePart["backfaces"] = !shape.value("invert").toBool(false);
				rootParts.append(loosePart);
			}
			else if (nv.isObject())
			{
				QJsonObject part = convertGroupNode(nv.toObject(), { 0, 0, 0 }, { 0, 0, 0 }, 0);
				if (!part.isEmpty()) rootParts.append(part);
			}
		}
		if (flatThickened)
			warnings.raw("FLAT_CUBES_THICKENED", "flat (zero-thickness) cube(s) were given 0.02px of thickness (the engine drops faces of perfectly flat boxes).");

		int totalShapes = 0;
		std::function<void(const QJsonArray&)> countShapes = [&](const QJsonArray& parts)
		{
			for (const QJsonValue& pv : parts)
			{
				QJsonObject p = pv.toObject();
				totalShapes += p.value("shapes").toArray().size();
				countShapes(p.value("parts").toArray());
			}
		};
		countShapes(rootParts);
		if (totalShapes == 0)
		{
			res.errCode = "EMPTY_MODEL";
			res.errMessage = "the model produced no convertible geometry (everything was skipped or hidden).";
			return res;
		}

		QString modelName = asciiName(QJsonValue(modelNameOpt.isEmpty() ? bb.value("name").toString() : modelNameOpt), "model");
		QString fileBase = safeFileBase(modelName);

		QJsonObject mimodel;
		mimodel["name"] = modelName;
		mimodel["texture"] = fileBase + ".png";
		mimodel["texture_size"] = QJsonArray { textureSizePx, textureSizePx };
		mimodel["parts"] = rootParts;

		res.mimodel = mimodel;
		res.cubes = (int)included.size();
		res.parts = countPartsDeep(rootParts);
		res.shapes = totalShapes;
		res.ok = true;
		return res;
	}

	// ------------------------------------------------- texture extraction

	static std::vector<Image> extractImages(const QJsonObject& bb, const QString& bbDir)
	{
		std::vector<Image> images;
		for (const QJsonValue& tv : bb.value("textures").toArray())
		{
			QJsonObject t = tv.toObject();
			Image im;
			QString source = t.value("source").toString();
			QByteArray data;
			if (source.startsWith("data:image"))
			{
				int comma = source.indexOf(',');
				if (comma > 0)
					data = QByteArray::fromBase64(source.mid(comma + 1).toLatin1());
			}
			if (data.isEmpty())
			{
				// fall back to the texture's file path (absolute or relative)
				QString path = t.value("path").toString();
				QString rel = t.value("relative_path").toString();
				QString use;
				if (!path.isEmpty() && QFile::exists(path)) use = path;
				else if (!rel.isEmpty() && QFile::exists(QDir(bbDir).filePath(rel))) use = QDir(bbDir).filePath(rel);
				else if (!t.value("name").toString().isEmpty() && QFile::exists(QDir(bbDir).filePath(t.value("name").toString())))
					use = QDir(bbDir).filePath(t.value("name").toString());
				if (!use.isEmpty())
				{
					QFile f(use);
					if (f.open(QFile::ReadOnly)) data = f.readAll();
				}
			}
			if (!data.isEmpty())
			{
				QImage img = QImage::fromData(data);
				if (!img.isNull())
				{
					im.img = img.convertToFormat(QImage::Format_RGBA8888);
					im.width = im.img.width();
					im.height = im.img.height();
					im.valid = true;
				}
			}
			images.push_back(im);
		}
		return images;
	}
} // namespace Mi3Bb

	// ------------------------------------------------------------ GML entry
	// mi3_bbmodel_convert(bbmodel_path, out_dir, head_attach) -> string
	//   "ok|<miobject path>|<mimodel path>|<stats>|<n warnings>|<warnings ;>"
	//   "err|<CODE>|<message>"

	StringType mi3_bbmodel_convert(StringType bbPathS, StringType outDirS, IntType headAttach)
	{
		using namespace Mi3Bb;

		QString bbPath = bbPathS;
		QString outDir = outDirS;

		QFile f(bbPath);
		if (!f.open(QFile::ReadOnly))
			return QString("err|FILE_NOT_FOUND|could not open the file.");

		QJsonParseError perr;
		QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
		f.close();
		if (doc.isNull() || !doc.isObject())
			return QString("err|INVALID_JSON|the file is not a valid Blockbench model (") + perr.errorString() + ").";

		QJsonObject bb = doc.object();
		Warnings headWarnings;
		// headAttach: 0 = off, 1 = force (warn when absent), 2 = auto (bake if present)
		if (headAttach == 1)
		{
			if (!bakeHeadDisplay(bb, headWarnings))
				headWarnings.raw("NO_DISPLAY_HEAD", "no Display > Head values found; converted without head placement.");
		}
		else if (headAttach == 2)
			bakeHeadDisplay(bb, headWarnings);

		QFileInfo bbInfo(bbPath);
		std::vector<Image> images = extractImages(bb, bbInfo.absolutePath());

		QString baseName = bbInfo.completeBaseName();
		// Deliberate divergence from the JS reference: unnamed projects fall back
		// to the source FILENAME (predictable outputs) instead of meta.model_format.
		ConvResult conv = convertScene(bb, images, bb.value("name").toString(baseName));
		if (!conv.ok)
			return QString("err|") + conv.errCode + "|" + conv.errMessage;

		for (int i = 0; i < headWarnings.codes.size(); i++)
			conv.warnings.raw(headWarnings.codes[i], headWarnings.messages[i]);

		// ---- write files (unique base name inside outDir)
		QDir dir(outDir);
		if (!dir.exists()) dir.mkpath(".");
		QString fileBase = safeFileBase(conv.mimodel.value("name").toString());
		QString base = fileBase;
		int n = 2;
		while (QFile::exists(dir.filePath(base + ".mimodel")) || QFile::exists(dir.filePath(base + ".miobject")))
			base = fileBase + "_" + QString::number(n++);

		QString texName = base + ".png";
		QJsonObject mimodel = conv.mimodel;
		mimodel["texture"] = texName;

		QString mimodelPath = dir.filePath(base + ".mimodel");
		QString miobjectPath = dir.filePath(base + ".miobject");
		QString texPath = dir.filePath(texName);

		{
			QFile mf(mimodelPath);
			if (!mf.open(QFile::WriteOnly | QFile::Truncate))
				return QString("err|WRITE_FAILED|could not write ") + mimodelPath;
			mf.write(writeJson(mimodel).toUtf8());
		}
		if (!conv.texture.save(texPath, "PNG"))
			return QString("err|WRITE_FAILED|could not write ") + texPath;
		{
			QJsonObject miobject = buildMiObject(mimodel, base + ".mimodel", texName);
			QFile of(miobjectPath);
			if (!of.open(QFile::WriteOnly | QFile::Truncate))
				return QString("err|WRITE_FAILED|could not write ") + miobjectPath;
			of.write(writeJson(miobject).toUtf8());
		}

		QString stats = QString("%1 cubes -> %2 parts / %3 shapes").arg(conv.cubes).arg(conv.parts).arg(conv.shapes);
		if (conv.rebaked) stats += QString(", atlas %1x%1").arg(conv.atlasSize);

		QString warnJoin;
		for (int i = 0; i < conv.warnings.messages.size(); i++)
		{
			if (i) warnJoin += " ; ";
			warnJoin += "[" + conv.warnings.codes[i] + "] " + conv.warnings.messages[i];
		}
		warnJoin.replace('|', '/');

		return QString("ok|") + miobjectPath + "|" + mimodelPath + "|" + stats + "|" +
			QString::number(conv.warnings.messages.size()) + "|" + warnJoin;
	}
}
