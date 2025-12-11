#include "hardware/pwm.h"
#include "addons/buzzerspeaker.h"
#include "songs.h"
#include "drivermanager.h"
#include "storagemanager.h"
#include "usbdriver.h"
#include "math.h"
#include "helper.h"
#include "config.pb.h"
#include "gamepad.h"

bool BuzzerSpeakerAddon::available() {
    const BuzzerOptions& options = Storage::getInstance().getAddonOptions().buzzerOptions;
	return options.enabled && isValidPin(options.pin);
}

void BuzzerSpeakerAddon::setup() {
	const BuzzerOptions& options = Storage::getInstance().getAddonOptions().buzzerOptions;
	buzzerPin = options.pin;
	gpio_set_function(buzzerPin, GPIO_FUNC_PWM);
	buzzerPinSlice = pwm_gpio_to_slice_num (buzzerPin);
	buzzerPinChannel = pwm_gpio_to_channel (buzzerPin);

    // enable pin is optional so not required to toggle addon
    if (isValidPin(options.pin)) {
        isSpeakerOn = true;
        buzzerEnablePin = options.enablePin;
        gpio_init(buzzerEnablePin);
        gpio_set_dir(buzzerEnablePin, GPIO_OUT);
        gpio_put(buzzerEnablePin, isSpeakerOn);
    }

	buzzerVolume = options.volume;
	introPlayed = false;
}

void BuzzerSpeakerAddon::process() {
    // 1. 处理开机音乐逻辑
    if (!introPlayed) {
        playIntro();
    }

    // 2. 处理歌曲播放
    processBuzzer();

    // 3. --- 按键音逻辑 (区分按键频率) ---
    if (currentSong == NULL) {
        // 获取手柄指针
        Gamepad * gamepad = Storage::getInstance().GetGamepad();
        
        // 获取当前按下的所有键位 (位掩码)
        uint32_t currentButtons = gamepad->state.buttons | gamepad->state.dpad;

        static uint32_t lastButtons = 0;
        static uint32_t beepOffTime = 0;

        // 计算出“新按下”的那些键 (上升沿检测)
        uint32_t changedButtons = currentButtons & ~lastButtons;

        // 如果有新键按下
        if (changedButtons != 0) {
            uint32_t freq = 0; // 默认频率

            // --- 音调分配逻辑 (优先级从上到下) ---
            // 如果同时按下多个，优先响上面的频率
            
            // 方向键 (低音)
            if (changedButtons & GAMEPAD_MASK_UP)    freq = 262; // C4 (Do)
            else if (changedButtons & GAMEPAD_MASK_DOWN)  freq = 294; // D4 (Re)
            else if (changedButtons & GAMEPAD_MASK_LEFT)  freq = 330; // E4 (Mi)
            else if (changedButtons & GAMEPAD_MASK_RIGHT) freq = 349; // F4 (Fa)
            
            // 基础四键 (中音) - 对应街霸的轻/中拳脚
            else if (changedButtons & GAMEPAD_MASK_B1)    freq = 392; // G4 (So) - Cross/A
            else if (changedButtons & GAMEPAD_MASK_B2)    freq = 440; // A4 (La) - Circle/B
            else if (changedButtons & GAMEPAD_MASK_B3)    freq = 494; // B4 (Ti) - Square/X
            else if (changedButtons & GAMEPAD_MASK_B4)    freq = 523; // C5 (Do) - Triangle/Y

            // 肩键 (高音) - 对应街霸的重拳脚
            else if (changedButtons & GAMEPAD_MASK_L1)    freq = 587; // D5 (Re)
            else if (changedButtons & GAMEPAD_MASK_R1)    freq = 659; // E5 (Mi)
            else if (changedButtons & GAMEPAD_MASK_L2)    freq = 698; // F5 (Fa)
            else if (changedButtons & GAMEPAD_MASK_R2)    freq = 784; // G5 (So)

            // 功能键 (特殊高音)
            else if (changedButtons & GAMEPAD_MASK_S1)    freq = 880; // A5 (Select/Back)
            else if (changedButtons & GAMEPAD_MASK_S2)    freq = 988; // B5 (Start/Options)
            else if (changedButtons & GAMEPAD_MASK_A1)    freq = 1046; // C6 (Home/Guide)
            else if (changedButtons & GAMEPAD_MASK_A2)    freq = 1175; // D6 (Capture/Touch)
            else if (changedButtons & GAMEPAD_MASK_L3)    freq = 1318; // E6
            else if (changedButtons & GAMEPAD_MASK_R3)    freq = 1397; // F6

            // 如果匹配到了频率，播放声音
            if (freq > 0) {
                // 播放特定频率
                pwmSetFreqDuty(buzzerPinSlice, buzzerPinChannel, freq, 0.03 * ((float) buzzerVolume));
                pwm_set_enabled(buzzerPinSlice, true);
                
                // 设置30ms后关闭 (你可以改长一点，比如 50ms)
                beepOffTime = getMillis() + 30; 
            }
        }

        // 更新按键状态缓存
        lastButtons = currentButtons;

        // 关闭逻辑
        if (beepOffTime > 0 && getMillis() > beepOffTime) {
            pwm_set_enabled(buzzerPinSlice, false);
            beepOffTime = 0;
        }
    }
}

void BuzzerSpeakerAddon::playIntro() {
	if (getMillis() < 1000) {
		return;
	}

	bool isConfigMode = DriverManager::getInstance().isConfigMode();

	if (!get_usb_mounted() || isConfigMode) {
		play(&configModeSong);
	} else {
		play(&introSong);
	}
	introPlayed = true;
}

void BuzzerSpeakerAddon::processBuzzer() {
	if (currentSong == NULL) {
		return;
	}

	uint32_t currentTimeSong = getMillis() - startedSongMils;
	uint32_t totalTimeSong = currentSong->song.size() * currentSong->toneDuration;
	uint16_t currentTonePosition = floor((currentTimeSong * currentSong->song.size()) / totalTimeSong);
	Tone currentTone = currentSong->song[currentTonePosition];

	if (currentTonePosition >= currentSong->song.size()) {
		stop();
		return;
	}

	if (currentTone == PAUSE) {
		pwm_set_enabled (buzzerPinSlice, false);
		return;
	}

	pwmSetFreqDuty(buzzerPinSlice, buzzerPinChannel, currentTone, 0.03 * ((float) buzzerVolume));
	pwm_set_enabled (buzzerPinSlice, true);
}

void BuzzerSpeakerAddon::play(Song *song) {
	startedSongMils = getMillis();
	currentSong = song;
}

void BuzzerSpeakerAddon::stop() {
	pwm_set_enabled (buzzerPinSlice, false);
	currentSong = NULL;
}

uint32_t BuzzerSpeakerAddon::pwmSetFreqDuty(uint slice, uint channel, uint32_t frequency, float duty) {
	uint32_t clock = 125000000;
	uint32_t divider16 = clock / frequency / 4096 +
							(clock % (frequency * 4096) != 0);
	if (divider16 / 16 == 0)
	divider16 = 16;
	uint32_t wrap = clock * 16 / divider16 / frequency - 1;
	pwm_set_clkdiv_int_frac(slice, divider16/16,
										divider16 & 0xF);
	pwm_set_wrap(slice, wrap);
	pwm_set_chan_level(slice, channel, wrap * duty / 100);
	return wrap;
}

