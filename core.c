#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "core.h"
#include "libpicofe/input.h"
#include "main.h"
#include "options.h"
#include "overrides.h"
#include "plat.h"
#include "unzip.h"

#define PATH_SEPARATOR_CHAR '/'

struct core_cbs current_core;

double sample_rate;
double frame_rate;
double aspect_ratio;
unsigned audio_buffer_size_override;
int state_slot;

static char config_dir[MAX_PATH];
static char save_dir[MAX_PATH];
static char system_dir[MAX_PATH];
static char temp_rom[MAX_PATH];
static struct retro_disk_control_ext_callback disk_control_ext;

static uint32_t buttons = 0;

#define MAX_EXTENSIONS 24

static void core_handle_zip(const char *path, struct retro_system_info *info, struct retro_game_info *game_info, FILE** file) {
	const char *extensions[MAX_EXTENSIONS] = {0};
	char *extensionstr = strdup(info->valid_extensions);
	char *ext = NULL;
	char *saveptr = NULL;
	int index = 0;
	bool haszip = false;
	FILE *dest = NULL;

	if (info->valid_extensions && has_suffix_i(path, ".zip")) {
		while((ext = strtok_r(index == 0 ? extensionstr : NULL, "|", &saveptr))) {
			if (!strcmp(ext, "zip")) {
				haszip = true;
				break;
			}
			extensions[index++] = ext;
			if (index > MAX_EXTENSIONS - 1) break;
		}

		if (!haszip) {
			if (!unzip_tmp(*file, extensions, temp_rom, MAX_PATH)) {
				game_info->path = temp_rom;
				dest = fopen(temp_rom, "r");
				if (dest) {
					fclose(*file);
					*file = dest;
				}
			}
		}
	}
	free(extensionstr);
}

static int core_load_game_info(const char *path, struct retro_game_info *game_info) {
	struct retro_system_info info = {0};
	FILE *file = fopen(path, "rb");
	int ret = -1;

	if (!file) {
		PA_ERROR("Couldn't load content: %s\n", strerror(errno));
		goto finish;
	}

	PA_INFO("Loading %s\n", path);
	game_info->path = path;

	current_core.retro_get_system_info(&info);

	core_handle_zip(path, &info, game_info, &file);

	fseek(file, 0, SEEK_END);
	game_info->size = ftell(file);
	rewind(file);

	if (!info.need_fullpath) {
		void *game_data = malloc(game_info->size);

		if (!game_data) {
			PA_ERROR("Couldn't allocate memory for content\n");
			goto finish;
		}

		if (fread(game_data, 1, game_info->size, file) != game_info->size) {
			PA_ERROR("Couldn't read file: %s\n", strerror(errno));
			goto finish;
		}

		game_info->data = game_data;
	}

	ret = 0;
finish:
	if (file)
		fclose(file);

	return ret;
}

static void core_free_game_info(struct retro_game_info *game_info) {
	if (game_info->data) {
		free((void *)game_info->data);
		game_info->data = NULL;
		game_info->size = 0;
	}
}

static void gamepak_related_name(char *buf, size_t len, const char *new_extension)
{
	char filename[MAX_PATH];
	char *dot;

	strncpy(filename, basename(content_path), sizeof(filename));
	filename[sizeof(filename) - 1] = 0;

	dot = strrchr(filename, '.');
	if (dot)
		*dot = 0;

	snprintf(buf, len, "%s%s%s", save_dir, filename, new_extension);
}

void config_file_name(char *buf, size_t len, int is_game)
{
	if (is_game && content_path) {
		gamepak_related_name(buf, len, ".cfg");
	} else {
		snprintf(buf, len, "%s%s", config_dir, "picoarch.cfg");
	}
}

void save_relative_path(char *buf, size_t len, const char *basename) {
	snprintf(buf, len, "%s%s", save_dir, basename);
}

void sram_write(void) {
	char filename[MAX_PATH];
	FILE *sram_file = NULL;
	void *sram;

	size_t sram_size = current_core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) {
		return;
	}

	gamepak_related_name(filename, MAX_PATH, ".sav");

	sram_file = fopen(filename, "w");
	if (!sram_file) {
		PA_ERROR("Error opening SRAM file: %s\n", strerror(errno));
		return;
	}

	sram = current_core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || sram_size != fwrite(sram, 1, sram_size, sram_file)) {
		PA_ERROR("Error writing SRAM data to file\n");
	}

	fclose(sram_file);

	sync();
}

void sram_read(void) {
	char filename[MAX_PATH];
	FILE *sram_file = NULL;
	void *sram;

	size_t sram_size = current_core.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) {
		return;
	}

	gamepak_related_name(filename, MAX_PATH, ".sav");

	sram_file = fopen(filename, "r");
	if (!sram_file) {
		return;
	}

	sram = current_core.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		PA_ERROR("Error reading SRAM data\n");
	}

	fclose(sram_file);
}

bool state_allowed(void) {
	return current_core.retro_serialize_size() > 0;
}

void state_file_name(char *name, size_t size, int slot) {
	char extension[5] = {0};

	snprintf(extension, 5, ".st%d", slot);
	gamepak_related_name(name, MAX_PATH, extension);
}

int state_read(void) {
	char filename[MAX_PATH];
	FILE *state_file = NULL;
	void *state = NULL;
	int ret = -1;

	size_t state_size = current_core.retro_serialize_size();
	if (!state_size) {
		return 0;
	}

	state = calloc(1, state_size);
	if (!state) {
		PA_ERROR("Couldn't allocate memory for state\n");
		goto error;
	}

	state_file_name(filename, MAX_PATH, state_slot);

	state_file = fopen(filename, "r");
	if (!state_file) {
		PA_ERROR("Error opening state file: %s\n", strerror(errno));
		goto error;
	}

	if (state_size != fread(state, 1, state_size, state_file)) {
		PA_ERROR("Error reading state data from file\n");
		goto error;
	}

	if (!current_core.retro_unserialize(state, state_size)) {
		PA_ERROR("Error restoring save state\n", strerror(errno));
		goto error;
	}

	ret = 0;
error:
	if (state)
		free(state);
	if (state_file)
		fclose(state_file);
	return ret;
}

int state_write(void) {
	char filename[MAX_PATH];
	FILE *state_file = NULL;
	void *state = NULL;
	int ret = -1;

	size_t state_size = current_core.retro_serialize_size();
	if (!state_size) {
		return false;
	}

	state = calloc(1, state_size);
	if (!state) {
		PA_ERROR("Couldn't allocate memory for state\n");
		goto error;
	}

	state_file_name(filename, MAX_PATH, state_slot);

	state_file = fopen(filename, "w");
	if (!state_file) {
		PA_ERROR("Error opening state file: %s\n", strerror(errno));
		goto error;
	}

	if (!current_core.retro_serialize(state, state_size)) {
		PA_ERROR("Error creating save state\n", strerror(errno));
		goto error;
	}

	if (state_size != fwrite(state, 1, state_size, state_file)) {
		PA_ERROR("Error writing state data to file\n");
		goto error;
	}

	plat_dump_screen(filename);

	ret = 0;
error:
	if (state)
		free(state);
	if (state_file)
		fclose(state_file);

	sync();
	return ret;
}

unsigned disc_get_count(void) {
	if (disk_control_ext.get_num_images)
		return disk_control_ext.get_num_images();

	return 0;
}

unsigned disc_get_index(void) {
	if (disk_control_ext.get_image_index)
		return disk_control_ext.get_image_index();

	return 0;
}

bool disc_switch_index(unsigned index) {
	bool ret = false;
	if (!disk_control_ext.set_eject_state || !disk_control_ext.set_image_index)
		return false;

	disk_control_ext.set_eject_state(true);
	ret = disk_control_ext.set_image_index(index);
	disk_control_ext.set_eject_state(false);

	return ret;
}

bool disc_replace_index(unsigned index, const char *content_path) {
	bool ret = false;
	struct retro_game_info info = {0};
	if (!disk_control_ext.replace_image_index)
		return false;

	if (core_load_game_info(content_path, &info)) {
		goto finish;
	}

	ret = disk_control_ext.replace_image_index(index, &info);

finish:
	core_free_game_info(&info);
	return ret;
}

static void set_directories(const char *core_name) {
	const char *home = getenv("HOME");
	char *dst = save_dir;
	int len = MAX_PATH;
#ifndef MINUI
	char cwd[MAX_PATH];
#endif
	if (home != NULL) {
		snprintf(dst, len, "%s/.picoarch-%s/", home, core_name);
		mkdir(dst, 0755);
	}

	strncpy(config_dir, save_dir, MAX_PATH-1);

#ifdef MINUI
	strncpy(system_dir, save_dir, MAX_PATH-1);
#else
	if (getcwd(cwd, MAX_PATH)) {
		snprintf(system_dir, MAX_PATH, "%s/system", cwd);
		mkdir(system_dir, 0755);
	} else {
		PA_FATAL("Can't find system directory");
	}
#endif
}

static bool pa_environment(unsigned cmd, void *data) {
	switch(cmd) {
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: { /* 3 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: { /* 6 */
		/* Just warn for now. TODO: Display on the screen */
		const struct retro_message *message = (const struct retro_message*)data;
		PA_WARN(message->msg);
		break;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: { /* 9 */
		const char **out = (const char **)data;
		if (out)
			*out = system_dir;
		break;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { /* 10 */
		const enum retro_pixel_format *format = (enum retro_pixel_format *)data;

		if (*format != RETRO_PIXEL_FORMAT_RGB565) {
			/* 565 is only supported format */
			return false;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: { /* 13 */
		const struct retro_disk_control_callback *var =
			(const struct retro_disk_control_callback *)data;

		if (var) {
			memset(&disk_control_ext, 0, sizeof(struct retro_disk_control_ext_callback));
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_callback));
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: { /* 15 */
		struct retro_variable *var = (struct retro_variable *)data;
		if (var && var->key) {
			var->value = options_get_value(var->key);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_VARIABLES: { /* 16 */
		const struct retro_variable *vars = (const struct retro_variable *)data;
		options_free();
		if (vars) {
			options_init_variables(vars);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: { /* 17 */
		bool *out = (bool *)data;
		if (out)
			*out = options_changed();
		break;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: { /* 27 */
		struct retro_log_callback *log_cb = (struct retro_log_callback *)data;
		if (log_cb)
			log_cb->log = pa_log;
		break;
	}
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: { /* 31 */
		const char **out = (const char **)data;
		if (out)
			*out = save_dir;
		break;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: { /* 52 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
		unsigned *out = (unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: { /* 53 */
		options_free();
		if (data) {
			options_init(*(const struct retro_core_option_definition **)data);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: { /* 54 */
		const struct retro_core_options_intl *options = (const struct retro_core_options_intl *)data;
		if (options && options->us) {
			options_free();
			options_init(options->us);
			load_config();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: { /* 55 */
		const struct retro_core_option_display *display =
			(const struct retro_core_option_display *)data;

		if (display)
			options_set_visible(display->key, display->visible);
		break;
	}
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: { /* 57 */
		unsigned *out =	(unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: { /* 58 */
		const struct retro_disk_control_ext_callback *var =
			(const struct retro_disk_control_ext_callback *)data;

		if (var) {
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_ext_callback));
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: { /* 62 */
		const struct retro_audio_buffer_status_callback *cb =
			(const struct retro_audio_buffer_status_callback *)data;
		if (cb) {
			current_core.retro_audio_buffer_status = cb->callback;
		} else {
			current_core.retro_audio_buffer_status = NULL;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: { /* 63 */
		const unsigned *latency_ms = (const unsigned *)data;
		if (latency_ms) {
			unsigned frames = *latency_ms * frame_rate / 1000;
			if (frames > audio_buffer_size && frames < 30)
				audio_buffer_size_override = frames;
			else
				PA_WARN("Audio buffer change out of range (%d), ignored\n", frames);
		}
		break;
	}
	default:
		PA_DEBUG("Unsupported environment cmd: %u\n", cmd);
		return false;
	}

	return true;
}

static void pa_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (data && !should_quit) {
		pa_track_render();
		plat_video_process(data, width, height, pitch);
	}
}

static void pa_audio_sample(int16_t left, int16_t right) {
	const struct audio_frame frame = { .left = left, .right = right };
	if (!should_quit && enable_audio)
		plat_sound_write(&frame, 1);
}

static size_t pa_audio_sample_batch(const int16_t *data, size_t frames) {
	if (!should_quit && enable_audio)
		plat_sound_write((const struct audio_frame *)data, frames);
	return frames;
}

static void pa_input_poll(void) {
	int actions[IN_BINDTYPE_COUNT] = { 0, };
	unsigned int emu_act;
	int which = EACTION_NONE;

	in_update(actions);
	emu_act = actions[IN_BINDTYPE_EMU];
	if (emu_act) {
		for (; !(emu_act & 1); emu_act >>= 1, which++)
			;
		emu_act = which;
	}
	handle_emu_action(which);

	buttons = actions[IN_BINDTYPE_PLAYER12];
}

static int16_t pa_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return buttons;

		return (buttons >> id) & 1;
	}

	return 0;
}

int core_load(const char *corefile) {
	void (*set_environment)(retro_environment_t) = NULL;
	void (*set_video_refresh)(retro_video_refresh_t) = NULL;
	void (*set_audio_sample)(retro_audio_sample_t) = NULL;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
	void (*set_input_poll)(retro_input_poll_t) = NULL;
	void (*set_input_state)(retro_input_state_t) = NULL;

	PA_INFO("Loading core %s\n", corefile);

	memset(&current_core, 0, sizeof(current_core));
	current_core.handle = dlopen(corefile, RTLD_LAZY);

	if (!current_core.handle) {
		PA_ERROR("Couldn't load core: %s\n", dlerror());
		return -1;
	}

	set_directories(core_name);
	set_overrides(core_name);

	current_core.retro_init = dlsym(current_core.handle, "retro_init");
	current_core.retro_deinit = dlsym(current_core.handle, "retro_deinit");
	current_core.retro_get_system_info = dlsym(current_core.handle, "retro_get_system_info");
	current_core.retro_get_system_av_info = dlsym(current_core.handle, "retro_get_system_av_info");
	current_core.retro_set_controller_port_device = dlsym(current_core.handle, "retro_set_controller_port_device");
	current_core.retro_reset = dlsym(current_core.handle, "retro_reset");
	current_core.retro_run = dlsym(current_core.handle, "retro_run");
	current_core.retro_serialize_size = dlsym(current_core.handle, "retro_serialize_size");
	current_core.retro_serialize = dlsym(current_core.handle, "retro_serialize");
	current_core.retro_unserialize = dlsym(current_core.handle, "retro_unserialize");
	current_core.retro_cheat_reset = dlsym(current_core.handle, "retro_cheat_reset");
	current_core.retro_cheat_set = dlsym(current_core.handle, "retro_cheat_set");
	current_core.retro_load_game = dlsym(current_core.handle, "retro_load_game");
	current_core.retro_load_game_special = dlsym(current_core.handle, "retro_load_game_special");
	current_core.retro_unload_game = dlsym(current_core.handle, "retro_unload_game");
	current_core.retro_get_region = dlsym(current_core.handle, "retro_get_region");
	current_core.retro_get_memory_data = dlsym(current_core.handle, "retro_get_memory_data");
	current_core.retro_get_memory_size = dlsym(current_core.handle, "retro_get_memory_size");

	set_environment = dlsym(current_core.handle, "retro_set_environment");
	set_video_refresh = dlsym(current_core.handle, "retro_set_video_refresh");
	set_audio_sample = dlsym(current_core.handle, "retro_set_audio_sample");
	set_audio_sample_batch = dlsym(current_core.handle, "retro_set_audio_sample_batch");
	set_input_poll = dlsym(current_core.handle, "retro_set_input_poll");
	set_input_state = dlsym(current_core.handle, "retro_set_input_state");

	set_environment(pa_environment);
	set_video_refresh(pa_video_refresh);
	set_audio_sample(pa_audio_sample);
	set_audio_sample_batch(pa_audio_sample_batch);
	set_input_poll(pa_input_poll);
	set_input_state(pa_input_state);

	current_core.retro_init();
	current_core.initialized = true;
	PA_INFO("Finished loading core\n");
	return 0;
}

int core_load_content(const char *path) {
	struct retro_game_info game_info = {0};
	struct retro_system_av_info av_info = {0};
	int ret = -1;

	if (core_load_game_info(path, &game_info)) {
		goto finish;
	}

	if (!current_core.retro_load_game(&game_info)) {
		PA_ERROR("Couldn't load content\n");
		goto finish;
	}

	sram_read();

	current_core.retro_get_system_av_info(&av_info);

	PA_INFO("Screen: %dx%d\n", av_info.geometry.base_width, av_info.geometry.base_height);
	PA_INFO("Audio sample rate: %f\n", av_info.timing.sample_rate);
	PA_INFO("Frame rate: %f\n", av_info.timing.fps);
	PA_INFO("Reported aspect ratio: %f\n", av_info.geometry.aspect_ratio);

	sample_rate = av_info.timing.sample_rate;
	frame_rate = av_info.timing.fps;
	aspect_ratio = av_info.geometry.aspect_ratio;

#ifdef MMENU
	gamepak_related_name(save_template_path, MAX_PATH, ".st%i");
#endif

	ret = 0;
finish:
	core_free_game_info(&game_info);
	return ret;
}

void core_unload(void) {
	PA_INFO("Unloading core...\n");

	if (current_core.initialized) {
		current_core.retro_deinit();
		current_core.initialized = false;
	}

	if (temp_rom[0]) {
		remove(temp_rom);
		temp_rom[0] = '\0';
	}
	options_free();

	if (current_core.handle) {
		dlclose(current_core.handle);
		current_core.handle = NULL;
	}
}
