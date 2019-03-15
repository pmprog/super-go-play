#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_task_wdt.h"
#include "esp_spiffs.h"
#include "driver/rtc_io.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#include "../components/gnuboy/loader.h"
#include "../components/gnuboy/hw.h"
#include "../components/gnuboy/lcd.h"
#include "../components/gnuboy/fb.h"
#include "../components/gnuboy/cpu.h"
#include "../components/gnuboy/mem.h"
#include "../components/gnuboy/sound.h"
#include "../components/gnuboy/pcm.h"
#include "../components/gnuboy/regs.h"
#include "../components/gnuboy/rtc.h"
#include "../components/gnuboy/gnuboy.h"

#include <string.h>

#include "hourglass_empty_black_48dp.h"
#include "menutiles.h"

#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_sdcard.h"


extern int debug_trace;

struct fb fb;
struct pcm pcm;


uint16_t* displayBuffer[2]; //= { fb0, fb0 }; //[160 * 144];
uint8_t currentBuffer;

uint16_t* framebuffer;
int frame = 0;
uint elapsedTime = 0;

int32_t* audioBuffer[2];
volatile uint8_t currentAudioBuffer = 0;
volatile uint16_t currentAudioSampleCount;
volatile int16_t* currentAudioBufferPtr;

odroid_battery_state battery_state;

const char* StateFileName = "/storage/gnuboy.sav";

#define GAMEBOY_WIDTH (160)
#define GAMEBOY_HEIGHT (144)

#define AUDIO_SAMPLE_RATE (32000)

const char* SD_BASE_PATH = "/sd";

// --- MAIN
QueueHandle_t vidQueue;
QueueHandle_t audioQueue;

float Volume = 1.0f;

int pcm_submit()
{
    odroid_audio_submit(currentAudioBufferPtr, currentAudioSampleCount >> 1);

    return 1;
}


int BatteryPercent = 100;


void run_to_vblank()
{
  /* FRAME BEGIN */

  /* FIXME: djudging by the time specified this was intended
  to emulate through vblank phase which is handled at the
  end of the loop. */
  cpu_emulate(2280);

  /* FIXME: R_LY >= 0; comparsion to zero can also be removed
  altogether, R_LY is always 0 at this point */
  while (R_LY > 0 && R_LY < 144)
  {
    /* Step through visible line scanning phase */
    emu_step();
  }

  /* VBLANK BEGIN */

  //vid_end();
  if ((frame % 2) == 0)
  {
      xQueueSend(vidQueue, &framebuffer, portMAX_DELAY);

      // swap buffers
      currentBuffer = currentBuffer ? 0 : 1;
      framebuffer = displayBuffer[currentBuffer];

      fb.ptr = framebuffer;
  }

  rtc_tick();

  sound_mix();

  //if (pcm.pos > 100)
  {
        currentAudioBufferPtr = audioBuffer[currentAudioBuffer];
        currentAudioSampleCount = pcm.pos;

        void* tempPtr = 0x1234;
        xQueueSend(audioQueue, &tempPtr, portMAX_DELAY);

        // Swap buffers
        currentAudioBuffer = currentAudioBuffer ? 0 : 1;
        pcm.buf = audioBuffer[currentAudioBuffer];
        pcm.pos = 0;
  }

  if (!(R_LCDC & 0x80)) {
    /* LCDC operation stopped */
    /* FIXME: djudging by the time specified, this is
    intended to emulate through visible line scanning
    phase, even though we are already at vblank here */
    cpu_emulate(32832);
  }

  while (R_LY > 0) {
    /* Step through vblank phase */
    emu_step();
  }
}

volatile bool videoTaskIsRunning = false;
bool scaling_enabled = true;
bool previous_scale_enabled = true;

void videoTask(void *arg)
{
  esp_err_t ret;

  videoTaskIsRunning = true;

  uint16_t* param;
  while(1)
  {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if (param == 1)
            break;

        if (previous_scale_enabled != scaling_enabled)
        {
            // Clear display
            ili9341_write_frame_gb(NULL, true);
            previous_scale_enabled = scaling_enabled;
        }

        ili9341_write_frame_gb(param, scaling_enabled);
        odroid_input_battery_level_read(&battery_state);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }


    // Draw hourglass
    odroid_display_lock();

    odroid_display_show_hourglass();

    odroid_display_unlock();


    videoTaskIsRunning = false;
    vTaskDelete(NULL);

    while (1) {}
}


volatile bool AudioTaskIsRunning = false;
void audioTask(void* arg)
{
  // sound
  uint16_t* param;

  AudioTaskIsRunning = true;
  while(1)
  {
    xQueuePeek(audioQueue, &param, portMAX_DELAY);

    if (param == 0)
    {
        // TODO: determine if this is still needed
        abort();
    }
    else if (param == 1)
    {
        break;
    }
    else
    {
        pcm_submit();
    }

    xQueueReceive(audioQueue, &param, portMAX_DELAY);
  }

  printf("audioTask: exiting.\n");
  odroid_audio_terminate();

  AudioTaskIsRunning = false;
  vTaskDelete(NULL);

  while (1) {}
}


static void SaveState()
{
    // Save sram
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char* romPath = odroid_settings_RomFilePath_get();
    if (romPath)
    {
        char* fileName = odroid_util_GetFileName(romPath);
        if (!fileName) abort();

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileName);
        if (!pathName) abort();

        FILE* f = fopen(pathName, "w");
        if (f == NULL)
        {
            printf("%s: fopen save failed\n", __func__);
            abort();
        }

        savestate(f);
        fclose(f);

        printf("%s: savestate OK.\n", __func__);

        free(pathName);
        free(fileName);
        free(romPath);
    }
    else
    {
        FILE* f = fopen(StateFileName, "w");
        if (f == NULL)
        {
            printf("SaveState: fopen save failed\n");
        }
        else
        {
            savestate(f);
            fclose(f);

            printf("SaveState: savestate OK.\n");
        }
    }


    odroid_system_led_set(0);
    odroid_input_battery_monitor_enabled_set(1);
}

static void LoadState(const char* cartName)
{
    char* romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        char* fileName = odroid_util_GetFileName(romName);
        if (!fileName) abort();

        char* pathName = odroid_sdcard_create_savefile_path(SD_BASE_PATH, fileName);
        if (!pathName) abort();

        FILE* f = fopen(pathName, "r");
        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            loadstate(f);
            fclose(f);

            vram_dirty();
            pal_dirty();
            sound_dirty();
            mem_updatemap();

            printf("LoadState: loadstate OK.\n");
        }

        free(pathName);
        free(fileName);
        free(romName);
    }
    else
    {
        FILE* f = fopen(StateFileName, "r");
        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            loadstate(f);
            fclose(f);

            vram_dirty();
            pal_dirty();
            sound_dirty();
            mem_updatemap();

            printf("LoadState: loadstate OK.\n");
        }
    }

	pal_set(odroid_settings_GBPalette_get());
    Volume = odroid_settings_Volume_get();
}

static void PowerDown()
{
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");

    xQueueSend(audioQueue, &param, portMAX_DELAY);
    while (AudioTaskIsRunning) {}


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) {}


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();

    // LCD
    printf("PowerDown: Powerdown LCD panel.\n");
    ili9341_poweroff();

    odroid_system_sleep();


    // Should never reach here
    abort();
}

static void DoMenuHome()
{
    esp_err_t err;
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");

    xQueueSend(audioQueue, &param, portMAX_DELAY);
    while (AudioTaskIsRunning) {}


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) {}


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();


    // Set menu application
    odroid_system_application_set(0);


    // Reset
    esp_restart();
}
static void DoMenuHomeNoSave()
{
    esp_err_t err;
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");

    xQueueSend(audioQueue, &param, portMAX_DELAY);
    while (AudioTaskIsRunning) {}


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) {}


    // Set menu application
    odroid_system_application_set(0);


    // Reset
    esp_restart();
}

void app_menu_drawtile(int x, int y, uint8_t tile, uint16_t fc, uint16_t bc )
{
    for(int chy = 0; chy < 8; chy++)
    {
        uint8_t t = menutiles[tile][chy];

        for(int chx = 0; chx < 8; chx++)
        {
            uint8_t c = (t >> (7 - chx)) & 0x01;
            framebuffer[ ((y + chy) * 160) + (x + chx) ] = (c == 0 ? bc : fc);
        }
    }
}

int menu_selected_index = 0;
int menu_palette_format = 0;

void app_menu_redrawmenu()
{
    // Render the gameboy display
    printf("menu:startrender\n");
    int currentpalette = pal_get();
    uint16_t currentcolours[4] = { 
        pal_getcolour(currentpalette, 0, menu_palette_format),
        pal_getcolour(currentpalette, 1, menu_palette_format),
        pal_getcolour(currentpalette, 2, menu_palette_format),
        pal_getcolour(currentpalette, 3, menu_palette_format) };

    for(int y = 0; y < 8; y++)
    {
        for(int x = 0; x < 10; x++ )
        {
            uint16_t fc = currentcolours[menufcmap[y][x]];
            uint16_t bc = currentcolours[menubcmap[y][x]];
            uint8_t tl = menumap[y][x];
            if( x == 2 && menu_selected_index == y - 1 )
            {
                tl = 9;
            }
            if( x == 0 && y == 0 )
            {
                tl = menu_palette_format;
            }
            app_menu_drawtile( 40 + (x * 8), 40 + (y * 8), tl, fc, bc );
        }
    }
    printf("menu:vidQueue\n");
    xQueueSend(vidQueue, &framebuffer, portMAX_DELAY);
}

void app_menu_loop()
{
    menu_selected_index = 5;
    app_menu_redrawmenu();

    odroid_gamepad_state lastJoysticState;
    odroid_input_gamepad_read(&lastJoysticState);
    while (true)
    {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        // Leave menu 
        if (!lastJoysticState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
        {
            return;
        }

        if (!lastJoysticState.values[ODROID_INPUT_VOLUME] && joystick.values[ODROID_INPUT_VOLUME])
        {
            odroid_audio_volume_mute();
            printf("main: Volume=%d\n", odroid_audio_volume_get());
        }

        if (!lastJoysticState.values[ODROID_INPUT_UP] && joystick.values[ODROID_INPUT_UP])
        {
            menu_selected_index = (menu_selected_index + 4) % 5;
            app_menu_redrawmenu();
        }
        if (!lastJoysticState.values[ODROID_INPUT_DOWN] && joystick.values[ODROID_INPUT_DOWN])
        {
            menu_selected_index = (menu_selected_index + 1) % 5;
            app_menu_redrawmenu();
        }

        if (!lastJoysticState.values[ODROID_INPUT_SELECT] && joystick.values[ODROID_INPUT_SELECT])
        {
            menu_palette_format = (menu_palette_format + 1) % 3;
            app_menu_redrawmenu();
        }

        if ((!lastJoysticState.values[ODROID_INPUT_A] && joystick.values[ODROID_INPUT_A]) || (!lastJoysticState.values[ODROID_INPUT_RIGHT] && joystick.values[ODROID_INPUT_RIGHT]))
        {
            switch( menu_selected_index )
            {
                case 0:
                    odroid_audio_volume_increase();
                    printf("main: Volume=%d\n", odroid_audio_volume_get());
                    break;
                case 1:
                    pal_next();
                    odroid_settings_GBPalette_set(pal_get());
                    app_menu_redrawmenu();
                    break;
                case 2:
                    LoadState(rom.name);
                    break;
                case 3:
                    SaveState();
                    break;
                case 4:
                    gpio_set_level(GPIO_NUM_2, 1);
                    DoMenuHomeNoSave();
                    gpio_set_level(GPIO_NUM_2, 0);
                    break;
                case 5:
                    return;
            }
        }
        if ((!lastJoysticState.values[ODROID_INPUT_B] && joystick.values[ODROID_INPUT_B]) || (!lastJoysticState.values[ODROID_INPUT_LEFT] && joystick.values[ODROID_INPUT_LEFT]))
        {
            switch( menu_selected_index )
            {
                case 0:
                    odroid_audio_volume_decrease();
                    printf("main: Volume=%d\n", odroid_audio_volume_get());
                    break;
                case 1:
                    pal_previous();
                    odroid_settings_GBPalette_set(pal_get());
                    app_menu_redrawmenu();
                    break;
                case 2:
                    LoadState(rom.name);
                    break;
                case 3:
                    SaveState();
                    break;
                case 4:
                    gpio_set_level(GPIO_NUM_2, 1);
                    DoMenuHomeNoSave();
                    gpio_set_level(GPIO_NUM_2, 0);
                    break;
                case 5:
                    return;
            }
        }

        printf("menu:joystick\n");
        lastJoysticState = joystick;
    }
}


void app_main(void)
{
    printf("gnuboy (%s-%s).\n", COMPILEDATE, GITREV);

    nvs_flash_init();

    odroid_system_init();

    odroid_input_gamepad_init();



    // Boot state overrides
    bool forceConsoleReset = false;

    switch (esp_sleep_get_wakeup_cause())
    {
        case ESP_SLEEP_WAKEUP_EXT0:
        {
            printf("app_main: ESP_SLEEP_WAKEUP_EXT0 deep sleep wake\n");
            break;
        }

        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_TIMER:
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
        case ESP_SLEEP_WAKEUP_ULP:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        {
            printf("app_main: Non deep sleep startup\n");

            odroid_gamepad_state bootState = odroid_input_read_raw();

            if (bootState.values[ODROID_INPUT_MENU])
            {
                // Force return to factory app to recover from
                // ROM loading crashes

                // Set menu application
                odroid_system_application_set(0);

                // Reset
                esp_restart();
            }

            if (bootState.values[ODROID_INPUT_START])
            {
                // Reset emulator if button held at startup to
                // override save state
                forceConsoleReset = true;
            }

            break;
        }
        default:
            printf("app_main: Not a deep sleep reset\n");
            break;
    }

    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART)
    {
        forceConsoleReset = true;
        odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);
    }


    // Display
    ili9341_prepare();
    ili9341_init();
    //odroid_display_show_splash();

    // Load ROM
    loader_init(NULL);

    // Clear display
    ili9341_write_frame_gb(NULL, true);

    // Audio hardware
    odroid_audio_init(odroid_settings_AudioSink_get(), AUDIO_SAMPLE_RATE);

    // Allocate display buffers
    displayBuffer[0] = heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    displayBuffer[1] = heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    if (displayBuffer[0] == 0 || displayBuffer[1] == 0)
        abort();

    framebuffer = displayBuffer[0];

    for (int i = 0; i < 2; ++i)
    {
        memset(displayBuffer[i], 0, 160 * 144 * 2);
    }

    printf("app_main: displayBuffer[0]=%p, [1]=%p\n", displayBuffer[0], displayBuffer[1]);

    // blue led
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 0);

    //  Charge
    odroid_input_battery_level_init();

    // video
    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
    audioQueue = xQueueCreate(1, sizeof(uint16_t*));

    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(&audioTask, "audioTask", 2048, NULL, 5, NULL, 1); //768


    //debug_trace = 1;

    emu_reset();

    //&rtc.carry, &rtc.stop,
    rtc.d = 1;
    rtc.h = 1;
    rtc.m = 1;
    rtc.s = 1;
    rtc.t = 1;

    // vid_begin
    memset(&fb, 0, sizeof(fb));
    fb.w = 160;
  	fb.h = 144;
  	fb.pelsize = 2;
  	fb.pitch = fb.w * fb.pelsize;
  	fb.indexed = 0;
  	fb.ptr = framebuffer;
  	fb.enabled = 1;
  	fb.dirty = 0;


    // Note: Magic number obtained by adjusting until audio buffer overflows stop.
    const int audioBufferLength = AUDIO_SAMPLE_RATE / 10 + 1;
    //printf("CHECKPOINT AUDIO: HEAP:0x%x - allocating 0x%x\n", esp_get_free_heap_size(), audioBufferLength * sizeof(int16_t) * 2 * 2);
    const int AUDIO_BUFFER_SIZE = audioBufferLength * sizeof(int16_t) * 2;

    // pcm.len = count of 16bit samples (x2 for stereo)
    memset(&pcm, 0, sizeof(pcm));
    pcm.hz = AUDIO_SAMPLE_RATE;
  	pcm.stereo = 1;
  	pcm.len = /*pcm.hz / 2*/ audioBufferLength;
  	pcm.buf = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
  	pcm.pos = 0;

    audioBuffer[0] = pcm.buf;
    audioBuffer[1] = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);

    if (audioBuffer[0] == 0 || audioBuffer[1] == 0)
        abort();


    sound_reset();


    lcd_begin();


    // Load state
    LoadState(rom.name);


    uint startTime;
    uint stopTime;
    uint totalElapsedTime = 0;
    uint actualFrameCount = 0;
    odroid_gamepad_state lastJoysticState;

    ushort menuButtonFrameCount = 0;
    bool ignoreMenuButton = lastJoysticState.values[ODROID_INPUT_MENU];

    // Reset if button held at startup
    if (forceConsoleReset)
    {
        emu_reset();
    }


    scaling_enabled = odroid_settings_ScaleDisabled_get(ODROID_SCALE_DISABLE_GB) ? false : true;

    odroid_input_gamepad_read(&lastJoysticState);

    while (true)
    {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        // Leave menu 
        if (!lastJoysticState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
        {
            app_menu_loop();
        }

        /*
        if (ignoreMenuButton)
        {
            ignoreMenuButton = lastJoysticState.values[ODROID_INPUT_MENU];
        }

        if (!ignoreMenuButton && lastJoysticState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
        {
            ++menuButtonFrameCount;
        }
        else
        {
            menuButtonFrameCount = 0;
        }

        //if (!lastJoysticState.Menu && joystick.Menu)
        if (menuButtonFrameCount > 60 * 1)
        {
            DoMenuHomeNoSave();

            gpio_set_level(GPIO_NUM_2, 0);
        }

        if (!ignoreMenuButton && lastJoysticState.values[ODROID_INPUT_MENU] && !joystick.values[ODROID_INPUT_MENU])
        {
            // Save state
            gpio_set_level(GPIO_NUM_2, 1);

            DoMenuHome();

            gpio_set_level(GPIO_NUM_2, 0);
        }
        */


        pad_set(PAD_UP, joystick.values[ODROID_INPUT_UP]);
        pad_set(PAD_RIGHT, joystick.values[ODROID_INPUT_RIGHT]);
        pad_set(PAD_DOWN, joystick.values[ODROID_INPUT_DOWN]);
        pad_set(PAD_LEFT, joystick.values[ODROID_INPUT_LEFT]);

        pad_set(PAD_SELECT, joystick.values[ODROID_INPUT_SELECT]);
        pad_set(PAD_START, joystick.values[ODROID_INPUT_START]);

        pad_set(PAD_A, joystick.values[ODROID_INPUT_A]);
        pad_set(PAD_B, joystick.values[ODROID_INPUT_B]);


        startTime = xthal_get_ccount();
        run_to_vblank();
        stopTime = xthal_get_ccount();


        lastJoysticState = joystick;


        if (stopTime > startTime)
          elapsedTime = (stopTime - startTime);
        else
          elapsedTime = ((uint64_t)stopTime + (uint64_t)0xffffffff) - (startTime);

        totalElapsedTime += elapsedTime;
        ++frame;
        ++actualFrameCount;

        if (actualFrameCount == 60)
        {
          float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f); // 240000000.0f; // (240Mhz)
          float fps = actualFrameCount / seconds;

          printf("HEAP:0x%x, FPS:%f, BATTERY:%d [%d]\n", esp_get_free_heap_size(), fps, battery_state.millivolts, battery_state.percentage);

          actualFrameCount = 0;
          totalElapsedTime = 0;
        }
    }
}
