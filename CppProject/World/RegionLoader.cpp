#include "World.hpp"

#include <QTimer>

namespace CppProject
{
	RegionLoader::RegionLoader()
	{
		QTimer* timer = new QTimer(this);
		timer->setInterval(50);
		timer->connect(timer, &QTimer::timeout, this, &RegionLoader::UpdateLoadRegions);
		timer->start();

		QThread* thread = new QThread;
		StringType::AddQThread(thread);
		QObject::moveToThread(thread);
		thread->start();
	}

	void RegionLoader::UpdateLoadRegions()
	{
		// This slot runs on the loader thread: any exception escaping it is
		// std::terminate (hard app kill, no log). Belt on top of the per-chunk
		// guards — a failing region is marked errored instead of killing the app.
		try
		{
			// Load regions
			for (Region* region : World::regions)
			{
				if (!active) // Set on main thread
					break;
				if (region->loadStatus == Region::LOADING)
				{
					try
					{
						region->Load();
					}
					catch (const QString& err)
					{
						WARNING("Region load failed: " + err);
						region->loadStatus = Region::LOAD_ERROR;
					}
				}
				if (region->unload)
					region->Unload();
			}

			for (Region* region : World::regions)
			{
				if (!active) // Set on main thread
					break;
				if (region->meshStatus == Region::UPDATE_MESH)
				{
					try
					{
						region->UpdateMesh();
					}
					catch (const QString& err)
					{
						WARNING("Region mesh update failed: " + err);
						region->meshStatus = Region::NOMESH;
					}
				}
			}
		}
		catch (const std::exception& e)
		{
			WARNING("Region loader error: " + QString(e.what()));
		}
		catch (...)
		{
			WARNING("Region loader error (unknown)");
		}

		emit UpdateDone();
	}
}