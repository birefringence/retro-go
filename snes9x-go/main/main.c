#include <rg_system.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#include "snes9x.h"
#include "soundux.h"
#include "memmap.h"
#include "apu.h"
#include "display.h"
#include "gfx.h"
#include "cpuexec.h"
#include "srtc.h"

#include "keymap.h"

#define AUDIO_SAMPLE_RATE (22050)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60)

static rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH];

static rg_video_update_t updates[2];
static rg_video_update_t *currentUpdate = &updates[0];
static rg_app_t *app;

static bool apu_enabled = true;
static bool lowpass_filter = false;
static int frameskip = 3;

bool overclock_cycles = false;
int one_c = 4, slow_one_c = 5, two_c = 6;
bool reduce_sprite_flicker = false;

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_KEYMAP = "keymap";
// --- MAIN

static void update_keymap(int id)
{
	// keymap_id = id % KEYMAPS_COUNT;

	// memcpy(&keymap, &KEYMAPS[keymap_id], sizeof(keymap));

	// S9xUnmapAllControls();

	// for (int i = 0; i < keymap.size; i++)
	// {
	// 	S9xMapButtonT(i, keymap.keys[i].action);
	// }
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_display_save_frame(filename, currentUpdate, width, height);
}

static bool save_state_handler(const char *filename)
{
    return 0;
}

static bool load_state_handler(const char *filename)
{
    return 1;
}

static bool reset_handler(bool hard)
{
    return true;
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        apu_enabled = !apu_enabled;
        Settings.APUEnabled = apu_enabled;
    }

    sprintf(option->value, "%s", apu_enabled ? "On " : "Off");

    return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) frameskip--;
    if (event == RG_DIALOG_NEXT) frameskip++;

    frameskip = RG_MAX(frameskip, 1);

    sprintf(option->value, "%d", frameskip);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        lowpass_filter = !lowpass_filter;

    sprintf(option->value, "%s", lowpass_filter ? "On" : "Off");

    return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
	if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
	{
		if (event == RG_DIALOG_PREV && --keymap_id < 0) keymap_id = KEYMAPS_COUNT - 1;
		if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1) keymap_id = 0;
		update_keymap(keymap_id);
		rg_settings_set_number(NS_APP, SETTING_KEYMAP, keymap_id);
		return RG_DIALOG_CLOSE;
	}
	else if (event == RG_DIALOG_ENTER || event == RG_DIALOG_ALT)
	{
		return RG_DIALOG_DISMISS;
	}
	return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
	{
		rg_gui_option_t options[16 + 4] = {};
		char values[16][16];
		char profile[32];
		bool dismissed = false;

		while (!dismissed)
		{
			rg_gui_option_t *option = options;

			option->label = "Profile";
			option->value = strcat(strcat(strcpy(profile, "< "), keymap.name), " >");
			option->flags = RG_DIALOG_FLAG_NORMAL;
			option->update_cb = &change_keymap_cb;
			option++;

			option->flags = RG_DIALOG_FLAG_NORMAL;
			option++;

			for (int i = 0; i < keymap.size; i++)
			{
				// keys[i].key_id contains a bitmask, convert to bit number
				int key_id = log2(keymap.keys[i].key_id);

				// For now we don't display the D-PAD because it doesn't fit on large font
				if (key_id < 4)
					continue;

				const char *key = KEYNAMES[key_id];
				const char *mod = (keymap.keys[i].mod1) ? "MENU + " : "";
				option->label = keymap.keys[i].action;
				option->value = strcat(strcpy(values[i], mod), key);
				option->flags = RG_DIALOG_FLAG_NORMAL;
				option->update_cb = &change_keymap_cb;
				option++;
			}

			option->flags = RG_DIALOG_FLAG_LAST;
			option++;

			dismissed = rg_gui_dialog("Controls", options, 0) == -1;
			rg_display_clear(C_BLACK);
		}
	}

	strcpy(option->value, keymap.name);

    return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.Screen = (uint8_t *)malloc(GFX.Pitch * (SNES_HEIGHT_EXTENDED + 2)) + SNES_HEIGHT_EXTENDED;
    GFX.SubScreen = (uint8_t *)malloc(GFX.Pitch * (SNES_HEIGHT_EXTENDED + 2)) + SNES_HEIGHT_EXTENDED;
    GFX.ZBuffer = (uint8_t *)malloc((GFX.Pitch >> 1) * (SNES_HEIGHT_EXTENDED + 2)) + SNES_HEIGHT_EXTENDED;
    GFX.SubZBuffer = (uint8_t *)malloc((GFX.Pitch >> 1) * (SNES_HEIGHT_EXTENDED + 2)) + SNES_HEIGHT_EXTENDED;
    GFX.Delta = (GFX.SubScreen - GFX.Screen) >> 1;
    updates[0].buffer = GFX.Screen;
    return true;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joypad = 0;
    if (joystick & RG_KEY_MENU)
        rg_gui_game_menu();
    if (joystick & RG_KEY_OPTION)
        rg_gui_options_menu();
    if (joystick & RG_KEY_UP)       joypad |= SNES_UP_MASK;
    if (joystick & RG_KEY_DOWN)     joypad |= SNES_DOWN_MASK;
    if (joystick & RG_KEY_LEFT)     joypad |= SNES_LEFT_MASK;
    if (joystick & RG_KEY_RIGHT)    joypad |= SNES_RIGHT_MASK;
    if (joystick & RG_KEY_START)    joypad |= SNES_START_MASK;
    if (joystick & RG_KEY_SELECT)   joypad |= SNES_A_MASK;
    if (joystick & RG_KEY_A)        joypad |= SNES_B_MASK;
    if (joystick & RG_KEY_B)        joypad |= SNES_Y_MASK;
    // if (joystick & RG_KEY_START)    joypad |= SNES_X_MASK;
    // if (joystick & RG_KEY_SELECT)   joypad |= SNES_Y_MASK;
    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

#ifdef USE_BLARGG_APU
static void S9xAudioCallback(void)
{
    S9xFinalizeSamples();
    size_t available_samples = S9xGetSampleCount();
    S9xMixSamples(&mixbuffer, available_samples);
    rg_audio_submit(mixbuffer, available_samples >> 1);
}
#endif

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
    };
	const rg_gui_option_t options[] = {
		{2, "APU enable", (char*)"", 1, &apu_toggle_cb},
		{2, "LP Filter", (char*)"", 1, &lowpass_filter_cb},
		{2, "Frameskip", (char*)"", 1, &frameskip_cb},
		{2, "Controls", (char*)"", 1, &menu_keymap_cb},
		RG_DIALOG_CHOICE_LAST
	};
    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, options);

    memset(&Settings, 0, sizeof(Settings));
    Settings.JoystickEnabled = false;
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.ApplyCheats = false;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.InterpolatedSound = true;
#ifdef USE_BLARGG_APU
    Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
#endif

    if (!S9xInitDisplay())
        RG_PANIC("Display init failed!");

    if (!S9xInitMemory())
        RG_PANIC("Memory init failed!");

    if (!S9xInitAPU())
        RG_PANIC("APU init failed!");

    if (!S9xInitSound(0, 0))
        RG_PANIC("Sound init failed!");

    if (!S9xInitGFX())
        RG_PANIC("Graphics init failed!");

    if (!LoadROM(app->romPath))
        RG_PANIC("ROM loading failed!");

#ifdef USE_BLARGG_APU
    S9xSetSamplesAvailableCallback(S9xAudioCallback);
#else
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
#endif

    rg_display_set_source_format(SNES_WIDTH, SNES_HEIGHT, 0, 0, GFX.Pitch, RG_PIXEL_565_LE);

    printf("%s\n", Memory.ROMName);

    int frames = 0;

    while (true)
    {
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & RG_KEY_MENU)
            rg_gui_game_menu();
        if (joystick & RG_KEY_OPTION)
            rg_gui_options_menu();

        int64_t startTime = rg_system_timer();

        IPPU.RenderThisFrame = (frames++ % frameskip) == 0;
        if (!apu_enabled) // don't touch APUEnabled if not needed
            Settings.APUEnabled = false;

        S9xMainLoop();

#ifndef USE_BLARGG_APU
    if (apu_enabled)
    {
        if (lowpass_filter)
            S9xMixSamplesLowPass(&mixbuffer, AUDIO_BUFFER_LENGTH << 1, (60 * 65536) / 100);
        else
            S9xMixSamples(&mixbuffer, AUDIO_BUFFER_LENGTH << 1);
    }
#endif

        int elapsed = rg_system_timer() - startTime;

        if (IPPU.RenderThisFrame)
            rg_display_queue_update(&updates[0], NULL);

#ifndef USE_BLARGG_APU
    if (apu_enabled)
        rg_audio_submit(mixbuffer, AUDIO_BUFFER_LENGTH);
#endif

        rg_system_tick(elapsed);
    }
}
