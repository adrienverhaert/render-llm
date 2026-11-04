#include "AppHandler.hpp"
#include "Native/StudioUi.hpp"
#include "AppWindow.hpp"

#include "Asset/Font.hpp"
#include "Asset/Shader.hpp"
#include "Asset/Sound.hpp"
#include "Asset/Surface.hpp"
#include "ErrorDialog.hpp"
#include "Generated/Scripts.hpp"
#include "Library/ServeLib.hpp"
#include "Native/RenderLlmBatchDialog.hpp"
#include "Native/RenderLlmProjectDialog.hpp"
#include "Native/StartupTrace.hpp"
#include "Render/GLWidget.hpp"
#include "Render/GraphicsApiHandler.hpp"
#include "Render/PrimitiveRenderer.hpp"
#include "Render/TexturePage.hpp"
#include "Render/VertexBufferRenderer.hpp"

#include <QDesktopWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QStyle>
#include <QTimer>
#include <QStandardPaths>

#ifdef OS_WINDOWS
#define USE_GPU 1
typedef unsigned long DWORD;
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = USE_GPU;
extern "C" __declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = USE_GPU;
#endif

int main(int argc, char* argv[])
{
	// MI3 modo SERVE: PRIMERISIMO, antes de QApplication. Con --serve arranca
	// el hilo lector de stdin para que el pool pueda mandar trabajos mientras
	// el motor todavia esta cargando assets (8-10 s). Sin --serve no hace
	// absolutamente nada: ni hilo, ni tocar stdin, ni coste.
	CppProject::Mi3ServeInit(argc, argv);

	return (new CppProject::AppHandler(argc, argv))->Run();
}

namespace CppProject
{
	AppHandler* AppHandler::handler = nullptr;
	Random::ThreadData Random::threadData[OPENMP_MAX_THREADS];
	IntType Printer::indent = 0;

	AppHandler::AppHandler(int argc, char* argv[])
	{
		StartupTrace::Mark("app-handler.enter");
		try
		{
			// Disable DPI scaling
			QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);

			// Create graphics API handler
			new GraphicsApiHandler;
			StartupTrace::Mark("graphics-handler.ready");

			// Create application
			handler = this;
			new QApplication(argc, argv);
			StartupTrace::Mark("qapplication.ready");
			QCoreApplication::setApplicationName(PROJECT_NAME);

			// Initialize string table
			StringType::AddQThread(QThread::currentThread());
			StringType::AddGMLStrings();
			StartupTrace::Mark("strings.ready");

			// Set paths
			QDir().mkpath(user_directory_get());
			QDir().mkpath(projects_directory_get());
			file_delete_lib(log_file);

		#if RELEASE_MODE
			gmlGlobal::working_directory = QCoreApplication::applicationDirPath() + "/";
		#else
			gmlGlobal::working_directory = QDir::currentPath() + "/";
			DEBUG("Debug mode enabled");
		#endif
			DEBUG("render-llm native host; retained engine version " + mineimator_version + (mineimator_version_extra.IsEmpty() ? "" : " " + mineimator_version_extra) + " (" + mineimator_version_date + ")");
		#if OS_WINDOWS
			BOOL is64Bit;
			IsWow64Process((HANDLE)qApp->applicationPid(), &is64Bit);
			DEBUG("Platform: Windows " + QString(is64Bit ? "x64" : "x86"));
		#ifdef _WIN64
			DEBUG("Executable: 64-bit");
		#else
			DEBUG("Executable: 32-bit");
		#endif
		#elif OS_MAC
			DEBUG("Platform: MacOS");
		#else
			DEBUG("Platform: Linux");
		#endif
			DEBUG("OS: " + os_get());
			DEBUG("working_directory: " + gmlGlobal::working_directory);
			DEBUG("user_directory: " + user_directory_get());
			DEBUG("DPI: " + NumStr(qApp->desktop()->logicalDpiX()));
			omp_set_num_threads(std::min(omp_get_max_threads(), OPENMP_MAX_THREADS));
			DEBUG("OpenMP max threads: " + NumStr(omp_get_max_threads()));

			// Create temporary folder
		#if OS_WINDOWS
			gmlGlobal::game_save_id = QStandardPaths::standardLocations(QStandardPaths::AppDataLocation)[0] + "/";
		#else
			gmlGlobal::game_save_id = QDir::tempPath() + "/" + StringType(PROJECT_NAME) + "_tmp/";
		#endif
			if (QDir(gmlGlobal::game_save_id).exists())
				DEBUG("Found temporary folder " + gmlGlobal::game_save_id);
			else if (QDir().mkdir(gmlGlobal::game_save_id))
				DEBUG("Created temporary folder " + gmlGlobal::game_save_id);
			else
				FATAL("Could not create temporary folder " + gmlGlobal::game_save_id);


			// Set globals
			gmlGlobal::room_speed = 60;
			gmlGlobal::delta_time = 1.0;
			gmlGlobal::GM_runtime_version = "NA";
			gmlGlobal::async_load = ds_map_create();
			fpsLastUpdate = QDateTime::currentDateTime();
			startTime = QDateTime::currentDateTime().toMSecsSinceEpoch();
			randomize();

			// Map cursors
			cursorMap[cr_none] = Qt::BlankCursor;
			cursorMap[cr_default] = Qt::ArrowCursor;
			cursorMap[cr_beam] = Qt::IBeamCursor;
			cursorMap[cr_drag] = Qt::ClosedHandCursor;
			cursorMap[cr_size_nesw] = Qt::SizeBDiagCursor;
			cursorMap[cr_size_ns] = Qt::SizeVerCursor;
			cursorMap[cr_size_nwse] = Qt::SizeFDiagCursor;
			cursorMap[cr_size_we] = Qt::SizeHorCursor;
			cursorMap[cr_size_all] = Qt::SizeAllCursor;
			cursorMap[cr_handpoint] = Qt::PointingHandCursor;

			// Map Qt/GML key codes
			keyMap[Qt::Key_Alt] = vk_alt;
			keyMap[Qt::Key_Control] = vk_control;
			keyMap[Qt::Key_Shift] = vk_shift;
			keyMap[Qt::Key_Backspace] = vk_backspace;
			keyMap[Qt::Key_Delete] = vk_delete;
			keyMap[Qt::Key_Down] = vk_down;
			keyMap[Qt::Key_End] = vk_end;
			keyMap[Qt::Key_Return] = keyMap[Qt::Key_Enter] = vk_enter;
			keyMap[Qt::Key_Escape] = vk_escape;
			keyMap[Qt::Key_F1] = vk_f1;
			keyMap[Qt::Key_F2] = vk_f2;
			keyMap[Qt::Key_F3] = vk_f3;
			keyMap[Qt::Key_F4] = vk_f4;
			keyMap[Qt::Key_F5] = vk_f5;
			keyMap[Qt::Key_F6] = vk_f6;
			keyMap[Qt::Key_F7] = vk_f7;
			keyMap[Qt::Key_F8] = vk_f8;
			keyMap[Qt::Key_F9] = vk_f9;
			keyMap[Qt::Key_F10] = vk_f10;
			keyMap[Qt::Key_F11] = vk_f11;
			keyMap[Qt::Key_F12] = vk_f12;
			keyMap[Qt::Key_Home] = vk_home;
			keyMap[Qt::Key_Insert] = vk_insert;
			keyMap[Qt::Key_Left] = vk_left;
			keyMap[Qt::Key_multiply] = vk_multiply;
			keyMap[Qt::Key_PageDown] = vk_pagedown;
			keyMap[Qt::Key_PageUp] = vk_pageup;
			keyMap[Qt::Key_Pause] = vk_pause;
			keyMap[Qt::Key_Print] = vk_printscreen;
			keyMap[Qt::Key_Right] = vk_right;
			keyMap[Qt::Key_Space] = vk_space;
			keyMap[Qt::Key_Tab] = vk_tab;
			keyMap[Qt::Key_Up] = vk_up;

		#if API_D3D11
			// Initialize Direct3D graphics
			StartupTrace::Mark("d3d11.begin");
			GFX->Init();
			StartupTrace::Mark("d3d11.ready");
		#endif

			// Initialize audio
			try
			{
				StartupTrace::Mark("openal.begin");
				DEBUG("Initialize OpenAL");
				openALDevice = alcOpenDevice(nullptr);
				if (!openALDevice)
					throw QString("alcOpenDevice failed");

				openALContext = alcCreateContext(openALDevice, nullptr);
				if (!openALContext)
					throw QString("alcCreateContext failed");

				if (!alcMakeContextCurrent(openALContext))
					throw QString("alcMakeContextCurrent failed");

				DEBUG("Audio is supported");
				audioSupported = true;
				StartupTrace::Mark("openal.ready");
			}
			catch (const QString& err)
			{
				DEBUG("Audio not supported: " + err);
				audioSupported = false;
			}

			// Create main window
			StartupTrace::Mark("main-window.begin");
			AddWindow();
			StartupTrace::Mark("main-window.ready");

		}
		catch (const QString& ex)
		{
			// MI3 headless: sin dialogo de error (visible + modal); el FATAL ya
			// quedo en el log via Printer::Fatal — solo salir con codigo 1.
			if (!AppWindow::HeadlessMode())
				new ErrorDialog(ex);
			qApp->exit(1);
		}
	}

	AppHandler::~AppHandler()
	{
		StringType::qThreadActive = false;

		if (audioSupported)
		{
			alcMakeContextCurrent(nullptr);
			alcDestroyContext(openALContext);
			alcCloseDevice(openALDevice);
		}
	}

	int AppHandler::Run()
	{
		StartupTrace::Mark("event-loop.begin");
		return qApp->exec();
	}

	void AppHandler::LoadResources()
	{
		if (prRenderer)
			return;
		StartupTrace::Mark("resources.begin");

		DEBUG("Load resources");

		// Create renderers
		prRenderer = new PrimitiveRenderer;
		vbRenderer = new VertexBufferRenderer;

		// First texture page
		new TexturePage;

		// Load sprites and shaders into memory
		Shader::Init();
		Asset::Load();
		StudioUi::Install();

		// GPU settings
		gpu_set_blendenable(true);
		gpu_set_blendmode(bm_normal);
		gpu_set_ztestenable(false);
		gpu_set_zwriteenable(false);
		gpu_set_cullmode(cull_counterclockwise);

		DEBUG("Resources loaded");

		// Start application loop
		stepTimer.start(10, this);
		StartupTrace::Mark("resources.ready");

		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_UI"))
			QTimer::singleShot(500, this, &AppHandler::TryCaptureUi);
	}

	void AppHandler::TryCaptureUi()
	{
		const bool editorReady = global::_app && global::load_assets_stage == "exit"
			&& global::_app->window_state == "";
		if (!editorReady)
		{
			if (++uiCapturePolls <= 120)
				QTimer::singleShot(500, this, &AppHandler::TryCaptureUi);
			else
				StartupTrace::Mark("ui-capture timed-out waiting for editor");
			return;
		}
		if (uiCapturePolls >= 0)
		{
			uiCapturePolls = -1;
			bool delayOk = false;
			const int requestedDelay = qEnvironmentVariableIntValue("RENDER_LLM_CAPTURE_SETTLE_MS", &delayOk);
			const int settleDelay = delayOk ? qBound(1000, requestedDelay, 30000) : 3000;
			QTimer::singleShot(settleDelay, this, &AppHandler::TryCaptureUi);
			return;
		}

		bool widthOk = false;
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ERROR_UI"))
		{
			ErrorDialog diagnostic("Diagnostic fixture: no project operation was performed.");
			return;
		}
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_SCENE") &&
			!renderllm_document_open(StringType(qEnvironmentVariable("RENDER_LLM_CAPTURE_SCENE"))))
		{
			StartupTrace::Mark("ui-capture.scene-failed " + renderllm_document_error().QStr());
			qApp->exit(2); return;
		}
		bool heightOk = false;
		const int requestedWidth = qEnvironmentVariableIntValue("RENDER_LLM_CAPTURE_WIDTH", &widthOk);
		const int requestedHeight = qEnvironmentVariableIntValue("RENDER_LLM_CAPTURE_HEIGHT", &heightOk);
		mainWindow->resize(widthOk ? qBound(960, requestedWidth, 3840) : 1280,
			heightOk ? qBound(640, requestedHeight, 2160) : 720);
		mainWindow->ShowNormal();
		mainWindow->raise();
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_AGENT_UI"))
		{
			ds_list_clear(global::mi3_as_log_list);
			global::mi3_as_log_chars = 0;
			global::mi3_as_log_trimmed = false;
			global::mi3_as_notice = "";
			global::mi3_as_busy_kind = "";
			global::mi3_as_busy_label = "";
			tab_show(ScopeAny(global::_app->id), global::_app->assistant);
			StartupTrace::Mark("ui-capture.agent-open");
		}
		if (qEnvironmentVariableIsSet("RENDER_LLM_TEST_CONNECT_AGENT")) action_assistant_launch(ScopeAny(global::_app->id));
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_VIDEO_UI"))
		{
			action_toolbar_export_movie(ScopeAny(global::_app->id));
			StartupTrace::Mark("ui-capture.video-open");
		}
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_RENDER_UI"))
		{
			action_toolbar_export_image(ScopeAny(global::_app->id));
			StartupTrace::Mark("ui-capture.render-open");
		}
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_BATCH_UI"))
		{
			renderllm_open_batch_dialog();
			StartupTrace::Mark("ui-capture.batch-open");
		}
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_COLOR_UI"))
			StudioUi::ColorDialog(qEnvironmentVariableIsSet("RENDER_LLM_TEST_COLOR")?ID_action_background_sky_color:ID_action_background_ambient_color, 0xededed);
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_LOOK_UI")) StudioUi::Look();
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_OBJECTS_UI")) StudioUi::Objects();
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_MOTION_UI")) StudioUi::Motion();
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ALERT_UI"))
			StudioUi::Message("Replace this project?", "A saved project already uses this name. Replacing it cannot be undone.", {"Replace project", "Keep existing"}, 1);
		if (qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_PROJECT_UI"))
		{
			renderllm_open_project_dialog();
			StartupTrace::Mark("ui-capture.project-open");
		}
		const bool captureFlameSettings = qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_FLAME_SETTINGS_UI");
		const bool captureEnvironment = qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ENVIRONMENT_UI");
		if (captureFlameSettings || captureEnvironment)
		{
			const IntType properties = global::_app->properties;
			tab_show(ScopeAny(global::_app->id), properties);
			const IntType panel = ObjType(obj_tab, properties)->panel;
			for (IntType index = 0; index < ObjType(obj_panel, panel)->tab_list_amount; ++index)
			{
				if (ObjType(obj_panel, panel)->tab_list.Value(index) == properties)
				{
					ObjType(obj_panel, panel)->tab_selected = index;
					break;
				}
			}
			ObjType(obj_category, ObjType(obj_tab, properties)->project)->show = false;
			ObjType(obj_category, ObjType(obj_tab, properties)->render)->show = captureFlameSettings;
			ObjType(obj_category, ObjType(obj_tab, properties)->library)->show = false;
			ObjType(obj_category, ObjType(obj_tab, properties)->background)->show = captureEnvironment;
			ObjType(obj_category, ObjType(obj_tab, properties)->resources)->show = false;
			StartupTrace::Mark(captureFlameSettings
				? "ui-capture.flame-settings-open" : "ui-capture.environment-open");
		}
		const bool captureAddObject = qEnvironmentVariableIsSet("RENDER_LLM_CAPTURE_ADD_UI");
		if (captureAddObject)
		{
			StudioUi::AddObject();
			StartupTrace::Mark("ui-capture.add-object-open");
		}
		QTimer::singleShot(qEnvironmentVariableIsSet("RENDER_LLM_TEST_CONNECT_AGENT") ? 7000 : captureAddObject ? 2000 : 1000, [this]()
			{
				const QString path = QDir::current().filePath(QStringLiteral("Data/renderllm-ui.png"));
				GFX->SubmitBatch();
				const QImage capture = mainWindow->GetSurface()->ToImage();
				QSaveFile output(path);
				const bool encoded = !capture.isNull() && output.open(QIODevice::WriteOnly)
					&& capture.save(&output, "PNG");
				const bool saved = encoded && output.commit();
				StartupTrace::Mark(QStringLiteral("ui-capture saved=%1 width=%2 height=%3 title=%4")
					.arg(saved ? 1 : 0).arg(capture.width()).arg(capture.height())
					.arg(mainWindow->windowTitle()));
				if (!qEnvironmentVariableIsSet("RENDER_LLM_RUN_EXPORT_UI") && !qEnvironmentVariableIsSet("RENDER_LLM_TEST_EXPORT_SETTINGS") && !qEnvironmentVariableIsSet("RENDER_LLM_RUN_BATCH_UI") && !qEnvironmentVariableIsSet("RENDER_LLM_TEST_PROJECT_SAVE")) qApp->exit(saved && (!qEnvironmentVariableIsSet("RENDER_LLM_TEST_CONNECT_AGENT") || global::mi3_as_conn=="ready") ? 0 : 1);
			});
	}

	AppWindow* AppHandler::AddWindow(QRect rect, IntType id, AppWindow* from)
	{
		// Create main window
		AppWindow* win = new AppWindow(id);
		mouseWindow = win;
		windows.append(win);

		// Set as main
		if (!mainWindow)
			mainWindow = win;

		if (AppWindow::HeadlessMode())
		{
			// MI3 headless: nada de minimizar/posicionar en pantalla — la
			// ventana nacio camuflada (off-screen, alpha 0, sin activacion)
			// en el constructor; solo hay que mostrarla para que pinte.
			win->ShowNormal();
		}

		else if (rect == QRect()) // Show minimized if no rect given
		{
			win->showMinimized();
			StartupTrace::Mark(QStringLiteral("main-window.show-minimized handle=%1 visible=%2 minimized=%3")
				.arg(qulonglong(win->winId())).arg(win->isVisible() ? 1 : 0).arg(win->isMinimized() ? 1 : 0));
		}

		else if (from) // Show relative to a window
		{
			win->setGeometry(from->x() + rect.x(), from->y() + rect.y() + QApplication::style()->pixelMetric(QStyle::PM_TitleBarHeight) + 4, rect.width(), rect.height());
			win->ShowNormal();
		}
		else
		{
			win->setGeometry(rect);
			win->ShowNormal();
		}

		win->UpdateSize();
		return win;
	}

	void AppHandler::timerEvent(QTimerEvent* event)
	{
		stepTimer.stop();

		// Debug
		if (keyboard_check_pressed(vk_f6) && dev_mode)
		{
			move_all_to_texture_page();
			TexturePage::Debug();
		}

		// Loop through opened windows
		for (AppWindow* win : windows)
		{
			if (win->closing)
				continue;

			currentWindow = win;

			// Set mouse to widget window
			if (mouseWindow == win)
			{
				if (AppWin->mouseLocked)
				{
					QPoint winPos = AppWin->mouseLockWinPos + (AppWin->mousePos - AppWin->mouseLastPos) / scale;
					gmlGlobal::mouse_x = winPos.x();
					gmlGlobal::mouse_y = winPos.y();
				}
				else
				{
					gmlGlobal::mouse_x = win->mousePos.x() / scale;
					gmlGlobal::mouse_y = win->mousePos.y() / scale;
				}
			}
			else
				gmlGlobal::mouse_x = gmlGlobal::mouse_y = -1;
			
			if (!GFX->StartOffScreenRender())
				continue;
			GFX->surface = win->GetSurface();
			GFX->surface->BeginUse(win->size());

			GFX->ClearDepth();
			GFX->shader = PR->GetShader();
			GFX->shader->BeginUse();
			
			// Run application
			try
			{
				// Start
				if (!global::_app)
				{
					new app;
					app_event_create(global::_app->id);
				}
				if (!productDefaultsApplied && global::load_assets_stage == "exit"
					&& global::_app->window_state == "")
				{
					// The retained shortcut strip is useful in the legacy authoring UI, but it
					// competes with render-llm's compact timeline and duplicates discoverable
					// controls. Apply this once, after persisted settings have loaded, so the
					// user can still opt into it later through the compatibility setting.
					global::_app->setting_show_shortcuts_bar = false;
					if (!AppWindow::HeadlessMode()) StudioUi::Workspace();
					renderllm_mark_project_clean();
					productDefaultsApplied = true;
					StartupTrace::Mark("product-defaults shortcuts-bar=0");
				}

				app_event_step(global::_app->id);
				app_event_draw(global::_app->id);
			}
			catch (AppEndRequest)
			{
				DEBUG("App end requested");
			
				GFX->shader->EndUse();
				if (GFX->surface)
					GFX->surface->EndUse();

				// MI3 headless/serve: la ventana va camuflada como Qt::Tool y Qt
				// NO cuenta las tool windows para "se cerro la ultima ventana"
				// (WA_QuitOnClose sale en false en las tool), asi que tras
				// game_end() el bucle de eventos seguia vivo con el proceso entero
				// detras. En el modo de un disparo no se notaba porque el arnes
				// siempre remata por PID; en un worker de larga vida un proceso que
				// no muere solo es un huerfano garantizado -- y v2 ya se quemo con
				// huerfanos. Se pide la salida ANTES del close(), porque closeEvent
				// hace "delete App" y a la vuelta este objeto ya no existe.
				if (AppWindow::HeadlessMode())
					qApp->exit(0);

				mainWindow->closing = true;
				mainWindow->close();
				return;
			}
			catch (const QString& ex)
			{
				// MI3 headless: sin dialogo de error (visible + modal); el
				// FATAL ya quedo en el log — salir con codigo 1 para que el
				// runner detecte el crash en vez de colgarse hasta el timeout.
				if (!AppWindow::HeadlessMode())
					new ErrorDialog(ex);
				qApp->exit(1);
			}

			GFX->SubmitBatch();
			GFX->shader->EndUse();
			if (GFX->surface)
				GFX->surface->EndUse();

			win->mouseWheel = 0;
			win->mouseLastPos = win->mousePos;
			if (win->mouseUnlock)
				win->mouseLocked = win->mouseUnlock = false;

			win->Present();
			win->UpdateSize();
		}

		VB->EndFrame();
		currentWindow = mainWindow;
		blocked = false;

		// Reset pressed & release key states
		for (IntType key : keyStateMap.keys())
			keyStateMap[key].pressed = keyStateMap[key].released = false;
		for (IntType key : keyWinStateMap.keys())
			keyWinStateMap[key].pressed = keyWinStateMap[key].released = false;
		keyStateMap[vk_nokey].pressed = keyStateMap[vk_nokey].released = true;

		// Add new window(s)
		if (global::_app->window_state == "")
		{
			for (const AddedWindow& addWin : addedWindows)
			{
				AppWindow* win = AddWindow(addWin.rect, addWin.id, addWin.from);
				if (addWin.maximize)
					win->Maximize();
			}
			addedWindows.clear();
		}

		// Update fps every second
		if (fpsLastUpdate.msecsTo(QDateTime::currentDateTime()) > 1000)
		{
			// Calculate delta_time as duration of previous step in microseconds
			gmlGlobal::delta_time = fpsTimer.ElapsedMs() * 1000.0;

			// Calculate fps using duration of previous step minus step timer delay
			double elapsedMs = fpsTimer.ElapsedMs() - 1000.0 / gmlGlobal::room_speed;
			if (elapsedMs > 0.0)
				gmlGlobal::fps_real = 1.0 / (elapsedMs / 1000.0);

			gmlGlobal::fps = std::clamp(gmlGlobal::fps_real, IntType(0), gmlGlobal::room_speed);
			fpsLastUpdate = QDateTime::currentDateTime();

			// Clean unused data
			VecType::CleanHeapData();
			Font::CleanUnused();
			Mesh<>::CleanBuffers();
			StringType::GetCurrentQThreadData()->mainTable.Clean();

			// Reload shader when debugging
			Shader::CheckReload();
		}

		// Delete finished sounds
		SoundInstance::CleanSounds();

		// Schedule next step
		fpsTimer.Reset();
		stepTimer.start(1000.0 / gmlGlobal::room_speed, this);
	}

	IntType AppHandler::GetMsec() const
	{
		return QDateTime::currentDateTime().toMSecsSinceEpoch() - startTime;
	}

	int AppHandler::ExecDialog(QDialog* dialog)
	{
		// MI3 headless: JAMAS mostrar un dialogo modal (robaria el foco, se
		// veria en pantalla y colgaria la corrida esperando un click que no
		// llegara). Se loguea y se responde "rechazado" (0): show_question
		// devuelve No y los QFileDialog quedan cancelados, un default
		// determinista y seguro para la automatizacion.
		if (AppWindow::HeadlessMode())
		{
			DEBUG("MI3 headless: dialogo suprimido (auto-rechazo): "
				+ dialog->windowTitle());
			return 0;
		}

		GFX->SubmitBatch();

		// Clear keys
		for (IntType key : keyStateMap.keys())
			keyStateMap[key] = {};
		for (IntType key : keyWinStateMap.keys())
			keyWinStateMap[key] = {};

		blocked = true;
		int res = dialog->exec();

		GFX->StartOffScreenRender();

		return res;
	}

	void AppHandler::SetKeyDown(QKeyEvent* event, BoolType down)
	{
		QVector<IntType> keys = {};
		switch (event->key())
		{
			case Qt::Key_Alt: keys = { vk_alt, vk_ralt, vk_lalt }; break;
			case Qt::Key_Control: keys = { vk_control, vk_rcontrol, vk_lcontrol }; break;
			case Qt::Key_Shift: keys = { vk_shift, vk_rshift, vk_lshift }; break;
			default:
			{
				if (keyMap.contains(event->key())) // Mapped key
					keys = { keyMap.value(event->key()) };
				else if (event->key() > 255 || event->modifiers() & Qt::ShiftModifier) // Unicode key pressed or shift
					keys = { event->nativeVirtualKey() };
				else
					keys = { event->key() };
				break;
			}
		}

		// Update key states
		for (IntType key : keys)
			keyStateMap[key].SetDown(down);

		// Set last key
		if (keys.length() && down)
			gmlGlobal::keyboard_lastkey = keys[0];

		// Update vk_nokey/vk_anykey down state
		keyStateMap[vk_nokey].down = true;
		keyStateMap[vk_anykey].down = false;

		QHashIterator<IntType, KeyState> it(keyStateMap);
		while (it.hasNext())
		{
			it.next();
			if (it.key() == vk_nokey || !it.value().down)
				continue;
			
			keyStateMap[vk_nokey].down = false;
			keyStateMap[vk_anykey].down = true;
			break;
		}

	#if DEBUG_MODE && 0
		QString keyStr = "";
		for (IntType key : keys)
			keyStr += NumStr(key) + " ";
		DEBUG(QString(down ? "KeyPress" : "KeyRelease") + " key=" + NumStr(event->key()) + ", nativeVirtualKey=" + NumStr(event->nativeVirtualKey()) + ", nativeScanCode=" + NumStr(event->nativeScanCode()) + " => " + keyStr);
	#endif
	}

	void AppHandler::SetWinKeyDown(IntType keyCode, BoolType down)
	{
	#if OS_WINDOWS
		IntType key = 0;
		switch (keyCode)
		{
			case 312: key = vk_ralt; break;
			case 56: key = vk_lalt; break;
			case 285: key = vk_rcontrol; break;
			case 29: key = vk_lcontrol; break;
			case 54: key = vk_rshift; break;
			case 42: key = vk_lshift; break;
		}

		if (key)
		{
			keyWinStateMap[key].SetDown(down);

			// Set last key
			if (down)
				gmlGlobal::keyboard_lastkey = key;
		}
	#endif
	}

	void AppHandler::KeyState::SetDown(BoolType down)
	{
		if (down && !this->down)
			pressed = true;
		if (!down && this->down)
			released = true;
		this->down = down;

		if (pressed)
		{
			App->keyStateMap[vk_nokey].pressed = false;
			App->keyStateMap[vk_anykey].pressed = true;
		}

		if (released)
		{
			App->keyStateMap[vk_nokey].released = false;
			App->keyStateMap[vk_anykey].released = true;
		}
	}

	void AppHandler::HttpProgress(const HttpRequest& request, IntType received, IntType total)
	{
		GFX->StartOffScreenRender();
		Map& asyncMap = DsMap(gmlGlobal::async_load);
		asyncMap["id"] = request.id;
		asyncMap["status"] = 1;
		asyncMap["sizeDownloaded"] = received;
		asyncMap["contentLength"] = total;
		app_event_http(ScopeAny(global::_app->id));
	}

	void AppHandler::HttpResponse(const HttpRequest& request)
	{
		GFX->StartOffScreenRender();
		Map& asyncMap = DsMap(gmlGlobal::async_load);
		asyncMap["id"] = request.id;
		asyncMap["http_status"] = request.data->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (request.data->error())
		{
			asyncMap["status"] = -1;
			WARNING("Error fetching " + request.url + ":\n" + request.data->errorString());
		}
		else
		{
			asyncMap["status"] = 0;
			asyncMap["result"] = QString(request.data->readAll());
		}

		app_event_http(ScopeAny(global::_app->id));
	}
}
