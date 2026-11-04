#include "Native/RenderLlmDocument.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace
{
	int failures = 0;

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	QByteArray ValidDocument()
	{
		return R"({
  "scene": {"objects": [], "world": {"color": "#000000", "strength": 1}},
  "version": 1,
  "render": {"renderer": "Flame", "samples": 24},
  "materials": {},
  "agent": {"conversation_ids": []},
  "animation": {"schema": 1, "fps": 24, "frame_count": 96, "tracks": [], "constraints": []}
})";
	}
}

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	CppProject::RenderLlmDocument document;
	QString error;

	Check(document.Parse(ValidDocument(), &error), "valid document parses");
	const QByteArray expected =
		"{\"agent\":{\"conversation_ids\":[]},\"animation\":{\"constraints\":[],\"fps\":24,\"frame_count\":96,\"schema\":1,\"tracks\":[]},"
		"\"materials\":{},\"render\":{\"renderer\":\"Flame\",\"samples\":24},\"scene\":{\"objects\":[],\"world\":{\"color\":\"#000000\",\"strength\":1}},\"version\":1}\n";
	Check(document.CanonicalJson() == expected, "canonical JSON is stable and key-sorted");

	CppProject::RenderLlmDocument reparsed;
	Check(reparsed.Parse(document.CanonicalJson(), &error), "canonical JSON reparses");
	Check(reparsed.CanonicalJson() == expected, "canonical JSON is idempotent");

	QByteArray missing = ValidDocument();
	missing.replace("\"agent\": {\"conversation_ids\": []},", "");
	Check(!reparsed.Parse(missing, &error) && error.contains("agent"), "missing section is rejected");

	QByteArray unknown = ValidDocument();
	unknown.replace("\"version\": 1,", "\"version\": 1, \"legacy\": {},");
	Check(!reparsed.Parse(unknown, &error) && error.contains("legacy"), "unknown top-level section is rejected");

	QByteArray credentials = ValidDocument();
	credentials.replace("\"conversation_ids\": []", "\"api_key\": \"must-not-persist\"");
	Check(!reparsed.Parse(credentials, &error) && error.contains("Credentials"), "credential fields are rejected recursively");

	QByteArray wrongRenderer = ValidDocument();
	wrongRenderer.replace("\"Flame\"", "\"Cinema\"");
	Check(!reparsed.Parse(wrongRenderer, &error) && error.contains("Flame"), "non-Flame renderer is rejected");
	QByteArray ultraRenderer = ValidDocument();
	ultraRenderer.replace("\"Flame\"", "\"UltraFlame\"");
	Check(reparsed.Parse(ultraRenderer, &error), "UltraFlame renderer is accepted and preserved");
	QByteArray explicitEngine = ValidDocument();
	explicitEngine.replace("\"render\": {", "\"render\": {\"schema\":2,");
	Check(reparsed.Parse(explicitEngine, &error), "explicit public-engine schema is accepted");
	explicitEngine.replace("\"schema\":2,", "\"schema\":3,");
	Check(!reparsed.Parse(explicitEngine, &error), "unknown engine schema is rejected");
	QByteArray controls = ValidDocument();
	controls.replace("\"strength\": 1", "\"strength\":1,\"ambient_color\":\"#112233\",\"fog_enabled\":false,\"fog_distance\":8000");
	controls.replace("\"samples\": 24", "\"samples\":24,\"bounces\":4,\"noise_threshold\":0.03,\"denoise\":true,\"exposure\":1.25,\"overscan\":0,\"tiled\":true");
	Check(reparsed.Parse(controls, &error), "studio control fields parse");
	controls.replace("\"fog_enabled\":false", "\"fog_enabled\":\"false\"");
	Check(!reparsed.Parse(controls, &error), "fog switch requires a boolean");
	controls.replace("\"fog_enabled\":\"false\"", "\"fog_enabled\":false");
	controls.replace("\"noise_threshold\":0.03", "\"noise_threshold\":-1");
	Check(!reparsed.Parse(controls, &error), "negative noise threshold is rejected");

	QByteArray badReference = ValidDocument();
	badReference.replace("\"objects\": []", R"("objects": [{"id":"cube","name":"Cube","kind":"cube","position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":true,"material_id":"missing"}])");
	Check(!reparsed.Parse(badReference, &error) && error.contains("material reference"), "dangling material reference is rejected");

	QByteArray parentCycle = ValidDocument();
	parentCycle.replace("\"materials\": {}", R"("materials": {"mat":{"color":"#FFFFFF","metallic":0,"roughness":1,"emission":0}})");
	parentCycle.replace("\"objects\": []", R"("objects": [{"id":"a","name":"A","kind":"cube","position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":true,"material_id":"mat","parent_id":"b"},{"id":"b","name":"B","kind":"cube","position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":true,"material_id":"mat","parent_id":"a"}])");
	Check(!reparsed.Parse(parentCycle, &error) && error.contains("cycle"), "parent cycles are rejected");

	QByteArray invalidTrack = ValidDocument();
	QByteArray groups = ValidDocument();
	groups.replace("\"objects\": []", R"("objects": [{"id":"root","name":"Assembly","kind":"group","position":[1,2,3],"rotation":[0,0,30],"scale":[2,2,2],"visible":true},{"id":"child","name":"Nested","kind":"group","position":[0,1,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":false,"parent_id":"root"}])");
	groups.replace("\"tracks\": []", R"("tracks": [{"id":"move","target":"root","property":"position","keys":[{"frame":0,"value":[1,2,3],"easing":"linear"},{"frame":24,"value":[4,5,6],"easing":"linear"}]}])");
	const bool groupsParsed = reparsed.Parse(groups, &error);
	Check(groupsParsed, "nested transform-only animated groups parse");
	const QByteArray groupCanonical = reparsed.CanonicalJson();
	CppProject::RenderLlmDocument groupRoundTrip;
	Check(groupsParsed && groupRoundTrip.Parse(groupCanonical, &error) && groupRoundTrip.CanonicalJson() == groupCanonical,
		"group hierarchy and animation round-trip canonically");
	QByteArray groupCycle = groups;
	groupCycle.replace("\"name\":\"Assembly\"", "\"parent_id\":\"child\",\"name\":\"Assembly\"");
	Check(!reparsed.Parse(groupCycle, &error) && error.contains("cycle"), "nested group cycles are rejected");
	QByteArray groupMaterial = groups;
	groupMaterial.replace("\"name\":\"Assembly\"", "\"material_id\":\"unused\",\"name\":\"Assembly\"");
	Check(!reparsed.Parse(groupMaterial, &error), "groups reject renderable material properties");
	QByteArray groupMaterialTrack = groups;
	groupMaterialTrack.replace("\"property\":\"position\"", "\"property\":\"material.color\"");
	Check(!reparsed.Parse(groupMaterialTrack, &error), "groups reject material animation");
	invalidTrack.replace("\"tracks\": []", R"("tracks": [{"id":"track-1","target":"$world","property":"world.strength","keys":[{"frame":4,"value":1,"easing":"linear"},{"frame":4,"value":2,"easing":"linear"}]}])");
	Check(!reparsed.Parse(invalidTrack, &error) && error.contains("increasing"), "non-increasing key frames are rejected");

	QByteArray invalidBezier = ValidDocument();
	invalidBezier.replace("\"tracks\": []", R"("tracks": [{"id":"track-1","target":"$world","property":"world.strength","keys":[{"frame":0,"value":1,"easing":"bezier","bezier":[0,1,2]}]}])");
	Check(!reparsed.Parse(invalidBezier, &error) && error.contains("four handles"), "invalid Bezier handles are rejected");

	QByteArray incompatibleTrack = ValidDocument();
	incompatibleTrack.replace("\"tracks\": []", R"("tracks": [{"id":"track-1","target":"$world","property":"position","keys":[{"frame":0,"value":[0,0,0],"easing":"linear"}]}])");
	Check(!reparsed.Parse(incompatibleTrack, &error) && error.contains("incompatible"), "track target/property mismatch is rejected");

	QByteArray conflictingInterpolation = ValidDocument();
	conflictingInterpolation.replace("\"tracks\": []", R"("tracks": [{"id":"track-1","target":"$world","property":"world.strength","keys":[{"frame":0,"value":1,"easing":"linear"}]},{"id":"track-2","target":"$world","property":"world.color","keys":[{"frame":0,"value":"#FFFFFF","easing":"smooth"}]}])");
	Check(!reparsed.Parse(conflictingInterpolation, &error) && error.contains("share interpolation"), "shared target/frame interpolation conflicts are rejected");

	QByteArray duplicateSun = ValidDocument();
	duplicateSun.replace("\"objects\": []", R"("objects": [{"id":"sun-1","name":"Sun 1","kind":"sun","position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":true,"light":{"color":"#FFFFFF","power":1,"size":0.526}},{"id":"sun-2","name":"Sun 2","kind":"sun","position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],"visible":true,"light":{"color":"#FFFFFF","power":1,"size":0.526}}])");
	Check(!reparsed.Parse(duplicateSun, &error) && error.contains("at most one sun"), "multiple global sun lights are rejected");

	QByteArray oversized(CppProject::RenderLlmDocument::MaxDocumentBytes + 1, ' ');
	Check(!reparsed.Parse(oversized, &error) && error.contains("16 MiB"), "oversized document is rejected before parsing");

	QTemporaryDir temporary;
	Check(temporary.isValid(), "temporary directory is available");
	const QString projectPath = QDir(temporary.path()).filePath(QStringLiteral("scene.renderllm"));
	Check(document.Save(projectPath, &error), "atomic .renderllm save succeeds");
	CppProject::RenderLlmDocument loaded;
	Check(loaded.Load(projectPath, &error), "saved .renderllm file loads");
	Check(loaded.CanonicalJson() == expected, "saved bytes round-trip exactly");
	Check(!document.Save(QDir(temporary.path()).filePath(QStringLiteral("scene.json")), &error),
		"non-.renderllm save is rejected");

	if (failures == 0)
		std::cout << "RenderLlmDocumentTests: PASS\n";
	return failures == 0 ? 0 : 1;
}
