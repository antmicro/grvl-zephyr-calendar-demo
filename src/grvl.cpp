#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/errno_private.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <grvl/platform/ZephyrApp.h>

#include <grvl/grvl.h>
#include <grvl/JSEngine.h>
#include <grvl/Manager.h>
#include <grvl/Misc.h>

#include <grvl/component/Label.h>
#include <grvl/container/Division.h>
#include <grvl/container/ListView.h>
#include <grvl/container/ScrollPanel.h>

LOG_MODULE_REGISTER(calendar, CONFIG_APP_LOG_LEVEL);

namespace fs = std::filesystem;

static constexpr auto GRVL_THRD_STACK_SIZE = KB(10);
static constexpr auto GRVL_THRD_PRIORITY = 1;
static constexpr auto TARGET_FRAMERATE = 30;

K_TIMER_DEFINE(refresh_rate_timer, nullptr, nullptr);

#if defined(CONFIG_BOARD_STM32H747I_DISCO)
#include <zephyr/fs/ext2.h>
#include <zephyr/fs/fs.h>

static constexpr auto ROMFS_PATH = "/romfs";

static int setup_romfs()
{
	int rc;
	static struct fs_mount_t mount;

	// this struct needs to stay static - it must not go out of scope
	// while we are still using the filesystem.
	mount.type = FS_EXT2;
	mount.mnt_point = ROMFS_PATH;
	mount.storage_dev = (void*) "SD";
	mount.flags = FS_MOUNT_FLAG_NO_FORMAT | FS_MOUNT_FLAG_READ_ONLY;

	if (rc = fs_mount(&mount)) {
		LOG_ERR("Failed to mount sdcard (err: %d)", rc);
	}
	return rc;
}

static fs::path get_romfs_path()
{
	return fs::path(ROMFS_PATH);
}
#endif

#if defined(CONFIG_BOARD_NATIVE_SIM)
static int setup_romfs()
{
    return 0;
}

static fs::path get_romfs_path()
{
	char *envval = std::getenv("ROMFS_PATH");
	if (envval) {
		return fs::absolute(fs::path(envval));
	}
	return fs::absolute("romfs");
}

#endif

static void load_fonts(fs::path &rpath, grvl::Manager &manager)
{
	auto font = [&](const char *path) {
		return new grvl::GrvlBakedFont((rpath / path).string().c_str());
	};

	LOG_DBG("Loading fonts");

	auto mona10 = font("fonts/mona10.gbf");

	manager.AddFontToFontContainer("normal", mona10);
	manager.AddFontToFontContainer("mona10", mona10);
	manager.AddFontToFontContainer("mona12", font("fonts/mona12.gbf"));
	manager.AddFontToFontContainer("mona14", font("fonts/mona14.gbf"));
	manager.AddFontToFontContainer("mona16", font("fonts/mona16.gbf"));
}

static void load_images(fs::path &rpath, grvl::Manager &manager)
{
	auto image = [&](const char *path) {
		return new grvl::ImageContent((rpath / path).c_str(), grvl::Format::AL44);
	};

	LOG_DBG("Loading images");
	manager.AddImageContentToContainer("dots", image("images/dots.png"));
	manager.AddImageContentToContainer("light_gray_left_vector", image("images/light_gray_left_vector.png"));
	manager.AddImageContentToContainer("light_gray_right_vector", image("images/light_gray_right_vector.png"));
	manager.AddImageContentToContainer("signal", image("images/signal.png"));
	manager.AddImageContentToContainer("wifi", image("images/wifi.png"));
	manager.AddImageContentToContainer("battery", image("images/battery.png"));
}

static void grvl_thread(void *a1, void *a2, void *a3)
{
	const device* display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	setup_romfs();

	grvl::ZephyrApp app {display};
	grvl::Application::Init(&app);

	auto mgr = &grvl::Manager::GetInstance();

	// configure application
	fs::path romfs_path = get_romfs_path();

	grvl::JSEngine::SetSourceCodeWorkingDirectory(romfs_path.string());

	load_fonts(romfs_path, *mgr);
	load_images(romfs_path, *mgr);

	mgr->BuildFromXML((romfs_path / "gui.xml").string().c_str());
	mgr->InitializationFinished();
	mgr->SetActiveScreen("home", 0);

	grvl::JSEngine::MakeJavaScriptFunctionCall("InitializeCalendar");

	LOG_INF("Ready to draw");

	while (true) {
		k_timer_start(&refresh_rate_timer, K_MSEC(DIV_ROUND_CLOSEST(1000, TARGET_FRAMERATE)), K_NO_WAIT);

		grvl::JSEngine::MakeJavaScriptFunctionCall("UpdateCurrentTime");
		grvl::JSEngine::MakeJavaScriptFunctionCall("UpdatePositionOfCurrentTimeLine");

		app.Render();
		app.Swap();
		app.Poll();

		k_timer_status_sync(&refresh_rate_timer);
	}

	LOG_ERR("How did we get here?");
	k_sleep(K_FOREVER);
}

K_THREAD_DEFINE(grvl_tid, GRVL_THRD_STACK_SIZE, grvl_thread, nullptr, nullptr, nullptr, GRVL_THRD_PRIORITY, 0, 0);
