#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/errno_private.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <grvl/Division.h>
#include <grvl/JSEngine.h>
#include <grvl/Label.h>
#include <grvl/ListView.h>
#include <grvl/Manager.h>
#include <grvl/Misc.h>
#include <grvl/ScrollPanel.h>

LOG_MODULE_REGISTER(grvl, CONFIG_APP_LOG_LEVEL);

namespace fs = std::filesystem;

#define DISPLAY DT_CHOSEN(zephyr_display)

static constexpr auto WIDTH = DT_PROP(DISPLAY, width);
static constexpr auto HEIGHT = DT_PROP(DISPLAY, height);

static constexpr auto GRVL_THRD_STACK_SIZE = KB(8);
static constexpr auto GRVL_THRD_PRIORITY = 1;

static constexpr auto TARGET_FRAMERATE = 30;

#if defined(CONFIG_BOARD_STM32H747I_DISCO)
static constexpr auto BPP =
	DISPLAY_BITS_PER_PIXEL(DT_PROP(DT_CHOSEN(zephyr_display), pixel_format)) / 8;
#endif // CONFIG_BOARD_STM32H747I_DISCO

#if defined(CONFIG_BOARD_NATIVE_SIM)
static constexpr auto BPP = DISPLAY_BITS_PER_PIXEL(PIXEL_FORMAT_ARGB_8888) / 8;
#endif // CONFIG_BOARD_NATIVE_SIM

const static struct device *display_dev = DEVICE_DT_GET(DISPLAY);
static struct display_buffer_descriptor disp_buf;

static uintptr_t framebuffer;

static grvl::Manager *Manager;

static struct {
	bool pressed;
	int32_t x;
	int32_t y;
} mouse_pointer;

K_TIMER_DEFINE(refresh_rate_timer, nullptr, nullptr);

#if defined(CONFIG_BOARD_STM32H747I_DISCO)
#include <zephyr/fs/ext2.h>
#include <zephyr/fs/fs.h>
#include <zephyr/linker/devicetree_regions.h>
#include <stm32h747xx.h>

#define BUF_DEF(label, size)                                                                       \
	static uint8_t sdram_##label[size] Z_GENERIC_SECTION(                                      \
		LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(sdram2)))

static constexpr auto FB_MAX = WIDTH * HEIGHT * BPP * 2;
BUF_DEF(grvl_fb, FB_MAX);

static constexpr auto SDCARD_DEV = "SD";
static constexpr auto ROMFS_PATH = "/romfs";

// clang-format off
static struct fs_mount_t mp = {
	.type = FS_EXT2,
	.mnt_point = ROMFS_PATH,
	.storage_dev = const_cast<char *>(SDCARD_DEV),
	.flags = FS_MOUNT_FLAG_NO_FORMAT | FS_MOUNT_FLAG_READ_ONLY
};
// clang-format on

static int setup_romfs()
{
	int rc;
	if ((rc = fs_mount(&mp))) {
		LOG_ERR("Failed to mount sdcard (err: %d)", rc);
	}
	return rc;
}

static fs::path get_romfs_path()
{
	return fs::path(ROMFS_PATH);
}

static DMA2D_HandleTypeDef hal_dma2d;
static constexpr auto COLOR_MODE = (BPP == 2)   ? DMA2D_INPUT_RGB565
				   : (BPP == 3) ? DMA2D_INPUT_RGB888
						: DMA2D_INPUT_ARGB8888;

static void dma2d_init()
{
	__HAL_RCC_DMA2D_CLK_ENABLE();
}

static void grvl_stm32_dma_operation(uintptr_t fg_mem, uintptr_t bg_mem, uintptr_t out_mem,
				     uint32_t width, uint32_t height, uint32_t fg_off,
				     uint32_t bg_off, uint32_t out_off, uint32_t fg_fmt,
				     uint32_t bg_fmt, uint32_t out_fmt, uint32_t fnt_alpha)
{
	int rc;

	hal_dma2d.Instance = DMA2D;
	hal_dma2d.XferCpltCallback = nullptr;

	hal_dma2d.Init = {
		.Mode = bg_mem ? DMA2D_M2M_BLEND : DMA2D_M2M_PFC,
		.ColorMode = out_fmt,
		.OutputOffset = out_off,
	};

	hal_dma2d.LayerCfg[0] = {
		.InputOffset = bg_off,
		.InputColorMode = bg_fmt,
		.AlphaMode = DMA2D_NO_MODIF_ALPHA,
		.InputAlpha = 0xFF,
	};

	hal_dma2d.LayerCfg[1] = {
		.InputOffset = fg_off,
		.InputColorMode = fg_fmt,
		.AlphaMode = DMA2D_NO_MODIF_ALPHA,
		.InputAlpha = fnt_alpha,
	};

	/* DMA2D Initialization */
	if ((rc = HAL_DMA2D_Init(&hal_dma2d)) != HAL_OK) {
		LOG_ERR("Failed to initialize DMA transfer (err %d)",
			HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_ConfigLayer(&hal_dma2d, 0)) != HAL_OK) {
		LOG_ERR("Failed to configure DMA layer (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_ConfigLayer(&hal_dma2d, 1)) != HAL_OK) {
		LOG_ERR("Failed to configure DMA layer (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_BlendingStart(&hal_dma2d, fg_mem, bg_mem, out_mem, width, height)) !=
	    HAL_OK) {
		LOG_ERR("Failed to start DMA transfer (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_PollForTransfer(&hal_dma2d, 100)) != HAL_OK) {
		LOG_ERR("DMA transfer timeout (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}
}

static void grvl_stm32_dma_fill(uintptr_t out_mem, uint32_t width, uint32_t height, uint32_t off,
				uint32_t col, uint32_t fmt)
{
	int rc;

	hal_dma2d.Instance = DMA2D;
	hal_dma2d.XferCpltCallback = nullptr;

	hal_dma2d.Init = {
		.Mode = DMA2D_R2M,
		.ColorMode = fmt,
		.OutputOffset = off,
	};

	if ((rc = HAL_DMA2D_Init(&hal_dma2d)) != HAL_OK) {
		LOG_ERR("Failed to initialize DMA transfer (err %d)",
			HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_Start(&hal_dma2d, col, out_mem, width, height)) != HAL_OK) {
		LOG_ERR("Failed to start DMA transfer (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}

	if ((rc = HAL_DMA2D_PollForTransfer(&hal_dma2d, 50)) != HAL_OK) {
		LOG_ERR("DMA transfer timeout (err %d)", HAL_DMA2D_GetError(&hal_dma2d));
		return;
	}
}

static int board_init()
{
	dma2d_init();
	return setup_romfs();
}

#endif // CONFIG_BOARD_STM32H747I_DISCO

#if defined(CONFIG_BOARD_NATIVE_SIM)
static fs::path get_romfs_path()
{
	char *envval = std::getenv("ROMFS_PATH");
	if (envval) {
		return fs::absolute(fs::path(envval));
	}
	return fs::absolute("romfs");
}

static int board_init()
{
	return 0;
}
#endif // CONFIG_BOARD_NATIVE_SIM

static void grvl_input_callback(input_event *evt, void *user_data)
{
	switch (evt->code) {
	case INPUT_BTN_TOUCH:
		mouse_pointer.pressed = evt->value;
		break;
	case INPUT_ABS_X:
		mouse_pointer.x = evt->value;
		break;
	case INPUT_ABS_Y:
		mouse_pointer.y = evt->value;
		break;
	default:
		LOG_WRN_ONCE("Unknown input %d", evt->code);
		break;
	}
}

INPUT_CALLBACK_DEFINE(nullptr, grvl_input_callback, nullptr);

static void grvl_set_layer_pointer(uintptr_t addr)
{
	LOG_DBG("grvl_set_layer_pointer");
	framebuffer = addr;
}

static uint64_t grvl_get_timestamp()
{
	return k_uptime_get();
}

static void *duk_malloc(void *data, size_t size)
{
	LOG_DBG("duk_malloc (size: %d)", size);
	return malloc(size);
}

static void *duk_realloc(void *data, void *ptr, size_t size)
{
	LOG_DBG("duk_realloc");
	return realloc(ptr, size);
}

static void duk_free(void *data, void *ptr)
{
	LOG_DBG("duk_free");
	free(ptr);
}

static grvl::gui_callbacks_t grvl_callbacks = {
#if defined(CONFIG_BOARD_STM32H747I_DISCO)
	.dma_operation = grvl_stm32_dma_operation,
	.dma_operation_clt = nullptr,
	.dma_fill = grvl_stm32_dma_fill,
#endif

	.set_layer_pointer = grvl_set_layer_pointer,

#if defined(CONFIG_DEBUG)
	.gui_printf =
		[](const char *text, va_list argList) {
			vprintf(text, argList);
			printf("\n");
		},
#endif

	.get_timestamp = grvl_get_timestamp,

	.duk_alloc_func = duk_malloc,
	.duk_realloc_func = duk_realloc,
	.duk_free_func = duk_free,
};

static int hw_init()
{
	struct display_capabilities caps;

	LOG_INF("HW init");

	if (board_init()) {
		return -ENODEV;
	};

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display %s initialization failed", display_dev->name);
		return -ENODEV;
	}

	LOG_DBG("Display %s", display_dev->name);

	display_get_capabilities(display_dev, &caps);
	if (caps.x_resolution != WIDTH || caps.y_resolution != HEIGHT) {
		LOG_ERR("Mismatched display size");
		return -EINVAL;
	}

	disp_buf.buf_size = caps.x_resolution * caps.y_resolution *
			    (DISPLAY_BITS_PER_PIXEL(caps.current_pixel_format) / 8);
	disp_buf.pitch = caps.x_resolution;
	disp_buf.width = caps.x_resolution;
	disp_buf.height = caps.y_resolution;

	LOG_DBG("Display %s: width: %d, height: %d, bpp: %d, bufsize: %d", display_dev->name,
		disp_buf.width, disp_buf.height, BPP, disp_buf.buf_size);

	display_blanking_off(display_dev);

	return 0;
}

static void load_fonts(fs::path &rpath, grvl::Manager &manager)
{
	auto font = [&](const char *path) {
		return new grvl::GrvlBakedFont((rpath / path).string().c_str());
	};

	LOG_DBG("Loading fonts");

	manager.AddFontToFontContainer("mona10", font("fonts/mona10.gbf"));
	manager.AddFontToFontContainer("mona12", font("fonts/mona12.gbf"));
	manager.AddFontToFontContainer("mona14", font("fonts/mona14.gbf"));
	manager.AddFontToFontContainer("mona16", font("fonts/mona16.gbf"));
}

static void load_images(fs::path &rpath, grvl::Manager &manager)
{
	auto imageContent = [&](const char *path) {
		return new grvl::ImageContent(grvl::ImageContent::FromPNG((rpath / path).c_str()));
	};

	LOG_DBG("Loading images");
	manager.AddImageContentToContainer("dots", imageContent("images/dots.png"));
	manager.AddImageContentToContainer("light_gray_left_vector",
					   imageContent("images/light_gray_left_vector.png"));
	manager.AddImageContentToContainer("light_gray_right_vector",
					   imageContent("images/light_gray_right_vector.png"));
	manager.AddImageContentToContainer("signal", imageContent("images/signal.png"));
	manager.AddImageContentToContainer("wifi", imageContent("images/wifi.png"));
	manager.AddImageContentToContainer("battery", imageContent("images/battery.png"));
}

static int grvl_init()
{
	LOG_INF("Grvl init");

	fs::path romfs_path = get_romfs_path();

	grvl::grvl::Init(&grvl_callbacks);
	grvl::JSEngine::SetSourceCodeWorkingDirectory(romfs_path.string());

#if defined(CONFIG_BOARD_STM32H747I_DISCO)
	grvl::Manager::Initialize(WIDTH, HEIGHT, BPP, false, sdram_grvl_fb);
#else
	grvl::Manager::Initialize(WIDTH, HEIGHT, BPP, false);
#endif

	Manager = &grvl::Manager::GetInstance();

	load_fonts(romfs_path, *Manager);
	load_images(romfs_path, *Manager);

	Manager->BuildFromXML((romfs_path / "gui.xml").string().c_str());

	Manager->InitializationFinished();
	Manager->SetActiveScreen("home", 0);

	grvl::JSEngine::MakeJavaScriptFunctionCall("InitializeCalendar");

	return 0;
}

static int grvl_loop()
{
	LOG_INF("Main loop");

	for (;;) {
		k_timer_start(&refresh_rate_timer,
			      K_MSEC(DIV_ROUND_CLOSEST(1000, TARGET_FRAMERATE)), K_NO_WAIT);

		grvl::JSEngine::MakeJavaScriptFunctionCall("UpdateCurrentTime");
		grvl::JSEngine::MakeJavaScriptFunctionCall("UpdatePositionOfCurrentTimeLine");

		Manager->ProcessTouchPoint(mouse_pointer.pressed, mouse_pointer.x, mouse_pointer.y);

		Manager->MainLoopIteration();

		display_write(display_dev, 0, 0, &disp_buf, reinterpret_cast<void *>(framebuffer));

		k_timer_status_sync(&refresh_rate_timer);
	}

	LOG_ERR("Where did you come from");
	k_sleep(K_FOREVER);

	return 0;
}

static void grvl_thread(void *a1, void *a2, void *a3)
{
	int err;
	if ((err = hw_init())) {
		LOG_ERR("Failed to initialize hardware");
		return;
	}

	if ((err = grvl_init())) {
		LOG_ERR("Failed to initialize grvl");
		return;
	}

	grvl_loop();
}

K_THREAD_DEFINE(grvl_tid, GRVL_THRD_STACK_SIZE, grvl_thread, nullptr, nullptr, nullptr,
		GRVL_THRD_PRIORITY, 0, 0);
