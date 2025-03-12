/* [TEST: OK]
* OpenAL HRTF Example with Keyboard HRTF Switching
*
* Copyright (c) 2015 by Chris Robinson <chris.kcat@gmail.com>
* Modified to add keyboard HRTF switching functionality
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*/

/* This file contains an example for selecting an HRTF with keyboard controls. */

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sndfile.h"

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"

#include "common/alhelpers.h"

#include "win_main_utf8.h"

#ifndef M_PI
#define M_PI                         (3.14159265358979323846)
#endif

static LPALCGETSTRINGISOFT alcGetStringiSOFT;
static LPALCRESETDEVICESOFT alcResetDeviceSOFT;

#ifdef _WIN32
#include <windows.h>
#endif

void EnableANSI() {
#ifdef _WIN32
	// 設定 Console 輸出碼頁為 UTF-8
	SetConsoleOutputCP(CP_UTF8);
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD dwMode = 0;
	if (GetConsoleMode(hOut, &dwMode)) {
		dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode(hOut, dwMode);
	}
#endif
}

// 添加鼠標控制相關頭文件
#ifdef _WIN32
#include <windows.h>
#include <conio.h>  // 用於 Windows 下的鍵盤輸入
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// 聲源位置結構
typedef struct {
	float x;
	float y;
	float z;
	float scale_factor; // 鼠標移動距離到聲源移動距離的比例因子
} SourcePosition;

// 鼠標位置結構
typedef struct {
	int x;
	int y;
} MousePosition;

// 非阻塞鍵盤輸入初始化 (Linux)
#ifndef _WIN32
static struct termios orig_termios;

void reset_terminal_mode()
{
	tcsetattr(0, TCSANOW, &orig_termios);
}

void set_conio_terminal_mode()
{
	struct termios new_termios;

	/* take two copies - one for now, one for later */
	tcgetattr(0, &orig_termios);
	memcpy(&new_termios, &orig_termios, sizeof(new_termios));

	/* register cleanup handler, and set the new terminal mode */
	atexit(reset_terminal_mode);
	cfmakeraw(&new_termios);
	new_termios.c_cc[VMIN] = 0;
	new_termios.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &new_termios);
}

int kbhit()
{
	int ch = getchar();

	if (ch != EOF) {
		ungetc(ch, stdin);
		return 1;
	}

	return 0;
}

int getch()
{
	int r;
	unsigned char c;
	if ((r = read(0, &c, 1)) < 0) {
		return r;
	}
	else {
		return c;
	}
}
#endif

// 初始化鍵盤處理
void init_keyboard()
{
#ifndef _WIN32
	// Linux 設置非阻塞終端模式
	set_conio_terminal_mode();
	int flags = fcntl(0, F_GETFL, 0);
	fcntl(0, F_SETFL, flags | O_NONBLOCK);
#endif
}

// 獲取鼠標位置
MousePosition GetMousePosition() {
	MousePosition pos = { 0, 0 };

#ifdef _WIN32
	POINT p;
	if (GetCursorPos(&p)) {
		pos.x = p.x;
		pos.y = p.y;
	}
#else
	Display *display = XOpenDisplay(NULL);
	if (display) {
		Window root = DefaultRootWindow(display);
		Window root_return, child_return;
		int root_x, root_y, win_x, win_y;
		unsigned int mask_return;

		XQueryPointer(display, root, &root_return, &child_return,
			&root_x, &root_y, &win_x, &win_y, &mask_return);

		pos.x = root_x;
		pos.y = root_y;

		XCloseDisplay(display);
	}
#endif

	return pos;
}

// 螢幕尺寸結構
typedef struct {
	int width;
	int height;
} ScreenSize;

// 獲取螢幕尺寸
ScreenSize GetScreenSize() {
	ScreenSize size = { 0, 0 };

#ifdef _WIN32
	size.width = GetSystemMetrics(SM_CXSCREEN);
	size.height = GetSystemMetrics(SM_CYSCREEN);
#else
	Display *display = XOpenDisplay(NULL);
	if (display) {
		Screen *screen = DefaultScreenOfDisplay(display);
		size.width = WidthOfScreen(screen);
		size.height = HeightOfScreen(screen);
		XCloseDisplay(display);
	}
#endif

	return size;
}

// 切換 HRTF
ALboolean switchHRTF(ALCdevice *device, ALCint current_hrtf_index, ALCint direction, ALCint num_hrtf) {
	// 計算新的 HRTF 索引
	ALCint new_index = (current_hrtf_index + direction) % num_hrtf;
	// 處理負數索引
	if (new_index < 0) new_index = num_hrtf - 1;

	ALCint attr[5];
	int i = 0;

	// 設置 HRTF 屬性
	attr[i++] = ALC_HRTF_SOFT;
	attr[i++] = ALC_TRUE;
	attr[i++] = ALC_HRTF_ID_SOFT;
	attr[i++] = new_index;
	attr[i] = 0;

	// 重設裝置
	if (!alcResetDeviceSOFT(device, attr)) {
		printf("Failed to reset device: %s\n",
			alcGetString(device, alcGetError(device)));
		return AL_FALSE;
	}

	// 檢查 HRTF 是否啟用
	ALCint hrtf_state;
	alcGetIntegerv(device, ALC_HRTF_SOFT, 1, &hrtf_state);
	if (!hrtf_state) {
		printf("HRTF not enabled after switching!\n");
		return AL_FALSE;
	}

	// 顯示當前使用的 HRTF
	//const ALchar *name = alcGetString(device, ALC_HRTF_SPECIFIER_SOFT);
	//printf("\nHRTF switched to %d: %s\n", new_index, name);

	return AL_TRUE;
}

// 檢查鍵盤輸入
int checkKeypress() {
#ifdef _WIN32
	if (_kbhit()) {
		return _getch();
	}
#else
	if (kbhit()) {
		return getch();
	}
#endif
	return 0;
}

/* LoadBuffer loads the named audio file into an OpenAL buffer object, and
 * returns the new buffer ID.
 */
static ALuint LoadSound(const char *filename)
{
	ALenum err, format;
	ALuint buffer;
	SNDFILE *sndfile;
	SF_INFO sfinfo;
	short *membuf;
	sf_count_t num_frames;
	ALsizei num_bytes;

	/* Open the audio file and check that it's usable. */
	sndfile = sf_open(filename, SFM_READ, &sfinfo);
	if (!sndfile)
	{
		fprintf(stderr, "Could not open audio in %s: %s\n", filename, sf_strerror(sndfile));
		return 0;
	}
	if (sfinfo.frames < 1 || sfinfo.frames >(sf_count_t)(INT_MAX / sizeof(short)) / sfinfo.channels)
	{
		fprintf(stderr, "Bad sample count in %s (%" PRId64 ")\n", filename, sfinfo.frames);
		sf_close(sndfile);
		return 0;
	}

	/* Get the sound format, and figure out the OpenAL format */
	format = AL_NONE;
	if (sfinfo.channels == 1)
		format = AL_FORMAT_MONO16;
	else if (sfinfo.channels == 2)
		format = AL_FORMAT_STEREO16;
	else if (sfinfo.channels == 3)
	{
		if (sf_command(sndfile, SFC_WAVEX_GET_AMBISONIC, NULL, 0) == SF_AMBISONIC_B_FORMAT)
			format = AL_FORMAT_BFORMAT2D_16;
	}
	else if (sfinfo.channels == 4)
	{
		if (sf_command(sndfile, SFC_WAVEX_GET_AMBISONIC, NULL, 0) == SF_AMBISONIC_B_FORMAT)
			format = AL_FORMAT_BFORMAT3D_16;
	}
	if (!format)
	{
		fprintf(stderr, "Unsupported channel count: %d\n", sfinfo.channels);
		sf_close(sndfile);
		return 0;
	}

	/* Decode the whole audio file to a buffer. */
	membuf = malloc((size_t)(sfinfo.frames * sfinfo.channels) * sizeof(short));

	num_frames = sf_readf_short(sndfile, membuf, sfinfo.frames);
	if (num_frames < 1)
	{
		free(membuf);
		sf_close(sndfile);
		fprintf(stderr, "Failed to read samples in %s (%" PRId64 ")\n", filename, num_frames);
		return 0;
	}
	num_bytes = (ALsizei)(num_frames * sfinfo.channels) * (ALsizei)sizeof(short);

	/* Buffer the audio data into a new buffer object, then free the data and
	 * close the file.
	 */
	buffer = 0;
	alGenBuffers(1, &buffer);
	alBufferData(buffer, format, membuf, num_bytes, sfinfo.samplerate);

	free(membuf);
	sf_close(sndfile);

	/* Check if an error occurred, and clean up if so. */
	err = alGetError();
	if (err != AL_NO_ERROR)
	{
		fprintf(stderr, "OpenAL Error: %s\n", alGetString(err));
		if (buffer && alIsBuffer(buffer))
			alDeleteBuffers(1, &buffer);
		return 0;
	}

	return buffer;
}

int main(int argc, char **argv)
{
	ALCdevice *device;
	ALCcontext *context;
	ALboolean has_angle_ext;
	ALuint source, buffer;
	const char *soundname;
	const char *hrtfname;
	ALCint hrtf_state;
	ALCint num_hrtf;
	ALdouble azimuth_radian, elevation_radian, distance_3d, distance_hor;
	ALenum state;

	EnableANSI();

	/* Print out usage if no arguments were specified */
	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s [-device <name>] [-hrtf <name>] <soundfile>\n", argv[0]);
		return 1;
	}

	/* Initialize OpenAL, and check for HRTF support. */
	argv++; argc--;
	if (InitAL(&argv, &argc) != 0)
		return 1;

	context = alcGetCurrentContext();
	device = alcGetContextsDevice(context);
	if (!alcIsExtensionPresent(device, "ALC_SOFT_HRTF"))
	{
		fprintf(stderr, "Error: ALC_SOFT_HRTF not supported\n");
		CloseAL();
		return 1;
	}

	/* Define a macro to help load the function pointers. */
#define LOAD_PROC(d, T, x)  ((x) = FUNCTION_CAST(T, alcGetProcAddress((d), #x)))
	LOAD_PROC(device, LPALCGETSTRINGISOFT, alcGetStringiSOFT);
	LOAD_PROC(device, LPALCRESETDEVICESOFT, alcResetDeviceSOFT);
#undef LOAD_PROC

	/* Check for the AL_EXT_STEREO_ANGLES extension to be able to also rotate
	 * stereo sources.
	 */
	has_angle_ext = alIsExtensionPresent("AL_EXT_STEREO_ANGLES");
	printf("AL_EXT_STEREO_ANGLES %sfound\n", has_angle_ext ? "" : "not ");

	/* Check for user-preferred HRTF */
	if (strcmp(argv[0], "-hrtf") == 0)
	{
		hrtfname = argv[1];
		soundname = argv[2];
	}
	else
	{
		hrtfname = NULL;
		soundname = argv[0];
	}

	/* Enumerate available HRTFs, and reset the device using one. */
	alcGetIntegerv(device, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &num_hrtf);
	if (!num_hrtf)
		printf("No HRTFs found\n");
	else
	{
		ALCint attr[5];
		ALCint index = -1;
		ALCint i;

		printf("Available HRTFs:\n");
		for (i = 0; i < num_hrtf; i++)
		{
			const ALCchar *name = alcGetStringiSOFT(device, ALC_HRTF_SPECIFIER_SOFT, i);
			printf("    %d: %s\n", i, name);

			/* Check if this is the HRTF the user requested. */
			if (hrtfname && strcmp(name, hrtfname) == 0)
				index = i;
			else
				index = 0;
		}

		i = 0;
		attr[i++] = ALC_HRTF_SOFT;
		attr[i++] = ALC_TRUE;
		if (index == -1)
		{
			if (hrtfname)
				printf("HRTF \"%s\" not found\n", hrtfname);
			printf("Using default HRTF... ");
		}
		else
		{
			printf("Selecting HRTF %d... ", index);
			attr[i++] = ALC_HRTF_ID_SOFT;
			attr[i++] = index;
		}
		attr[i] = 0;

		if (!alcResetDeviceSOFT(device, attr))
			printf("Failed to reset device: %s\n", alcGetString(device, alcGetError(device)));
	}

	/* Check if HRTF is enabled, and show which is being used. */
	alcGetIntegerv(device, ALC_HRTF_SOFT, 1, &hrtf_state);
	if (!hrtf_state)
		printf("HRTF not enabled!\n");
	else
	{
		const ALchar *name = alcGetString(device, ALC_HRTF_SPECIFIER_SOFT);
		printf("HRTF enabled, using %s\n", name);
	}
	fflush(stdout);

	/* Load the sound into a buffer. */
	buffer = LoadSound(soundname);
	if (!buffer)
	{
		CloseAL();
		return 1;
	}

	/* Create the source to play the sound with. */
	source = 0;
	alGenSources(1, &source);
	alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSourcei(source, AL_BUFFER, (ALint)buffer);
	alSourcei(source, AL_LOOPING, AL_TRUE); // 設置循環播放
	assert(alGetError() == AL_NO_ERROR && "Failed to setup sound source");

	// 獲取螢幕尺寸
	ScreenSize screen = GetScreenSize();
	int center_x = screen.width / 2;
	int center_y = screen.height / 2;
	MousePosition center_pos = { center_x, center_y };
	MousePosition current_mouse_pos;

	// 找出螢幕短邊的長度（像素）
	int short_edge = (screen.width < screen.height) ? screen.width : screen.height;
	int pixel_to_short_edge = short_edge / 2; // 從中心到短邊的距離

	// 設定比例因子：從中心到短邊的距離應當映射為10.0個OpenAL單位
	float distance_on_short_edge = 8.0f; // 定義從中心到短邊的聲源距離
	float scale_factor = distance_on_short_edge / (float)pixel_to_short_edge;

	// 初始化聲源位置
	SourcePosition source_pos = { 0.0f, 0.0f, 0.0f, scale_factor };

	printf("螢幕尺寸: %d x %d pixels, 中心位置: (%d, %d)\n", screen.width, screen.height, center_x, center_y);
	printf("短邊長度: %d pixels, 離心距離: %d pixels\n", short_edge, pixel_to_short_edge);
	printf("距離乘數: %.5f (%.1f units at short edge)\n", scale_factor, distance_on_short_edge);

	// 初始化鍵盤處理
	init_keyboard();

	// 記錄當前使用的 HRTF 索引
	ALCint current_hrtf_index = 0;
	// 如果有指定 HRTF，獲取其索引
	if (hrtf_state) {
		const ALchar *current_name = alcGetString(device, ALC_HRTF_SPECIFIER_SOFT);
		for (ALCint i = 0; i < num_hrtf; i++) {
			const ALchar *name = alcGetStringiSOFT(device, ALC_HRTF_SPECIFIER_SOFT, i);
			if (strcmp(name, current_name) == 0) {
				current_hrtf_index = i;
				break;
			}
		}
	}

	printf("\n\n//===================================================================================//\n");
	printf("  功能說明:\n");
	printf("           透過滑鼠/鍵盤, 控制mono音檔在3D空間中的位置\n");
	printf("  操作方法:\n");
	printf("           前後左右 : 移動滑鼠(螢幕中心=人的位置, 游標上下左右=聲源前後左右)\n");
	printf("           垂直高度 : 鍵盤↑↓\n");
	printf("           HRTF切換 : 鍵盤←→\n");
	printf("           離    開 : ESC\n");
	printf("//===================================================================================//\n\n");

	/* Play the sound until it finishes. */
	alSourcePlay(source);
	ALboolean running = AL_TRUE;
	do {
		al_nssleep(10000000);	// delay 10 ms = 10000000 ns

		alcSuspendContext(context);

		// 獲取當前鼠標位置
		current_mouse_pos = GetMousePosition();

		// 計算相對於中心點的偏移
		int dx = current_mouse_pos.x - center_pos.x;
		int dy = current_mouse_pos.y - center_pos.y;

		// 計算聲源位置（使用我們的比例因子）
		source_pos.x = dx * scale_factor;
		source_pos.z = dy * scale_factor;

		// 計算水平角（方位角）和距離
		azimuth_radian = atan2(source_pos.x, -source_pos.z); // 弧度
		double azimuth_degree = azimuth_radian * 180.0f / M_PI;    // 轉換為度數
		if (azimuth_degree > 180.0f)
			azimuth_degree = -(180.0f - azimuth_degree);
		// 計算仰角（Y 軸與水平距離形成的角度）
		distance_hor = sqrt(source_pos.x * source_pos.x + source_pos.z * source_pos.z);
		elevation_radian = atan2(source_pos.y, distance_hor);
		double elevation_degree = elevation_radian * 180.0 / M_PI;
		if (elevation_degree > 180.0f)
			elevation_degree = -(180.0f - elevation_degree);
		// 計算聲源到原點的距離
		distance_3d = sqrt(source_pos.x * source_pos.x + source_pos.y * source_pos.y + source_pos.z * source_pos.z);

		// 處理鍵盤輸入
		int key = checkKeypress();
		if (key) {
			// Windows 方向鍵產生兩個字節，第一個是 224，第二個是實際的鍵碼
			if (key == 224) {
				key = checkKeypress();
				if (key == 75) {  // 左方向鍵
					// 切換到前一個 HRTF
					if (switchHRTF(device, current_hrtf_index, -1, num_hrtf)) {
						current_hrtf_index = (current_hrtf_index - 1 + num_hrtf) % num_hrtf;
					}
				}
				else if (key == 77) {  // 右方向鍵
					// 切換到下一個 HRTF
					if (switchHRTF(device, current_hrtf_index, 1, num_hrtf)) {
						current_hrtf_index = (current_hrtf_index + 1) % num_hrtf;
					}
				}
				else if (key == 72) {  // 上方向鍵
					source_pos.y += 0.2f;  // 調高聲音位置，可根據需要調整步進
				}
				else if (key == 80) {  // 下方向鍵
					source_pos.y -= 0.2f;
				}
			}

			// ESC 鍵結束程序 (Windows 中 ESC 鍵碼是 27)
			if (key == 27) {
				running = AL_FALSE;
			}
		}

		// 應用新的聲源位置
		alSource3f(source, AL_POSITION, source_pos.x, source_pos.y, source_pos.z);

		// 顯示當前位置、方位角和距離
		printf("\r滑鼠位移: (%d, %d) 聲源座標: (%.1f, %.1f, %.1f) 水平角: %.1f° 仰角: %.1f° 距離: %.1f HRTF: %d/%d\033[K",
			dx, dy, source_pos.x, source_pos.y, source_pos.z, azimuth_degree, elevation_degree, distance_3d, current_hrtf_index, num_hrtf - 1);
		fflush(stdout);

		alcProcessContext(context);

		alGetSourcei(source, AL_SOURCE_STATE, &state);
	} while (running && alGetError() == AL_NO_ERROR && state == AL_PLAYING);

	/* All done. Delete resources, and close down OpenAL. */
	alDeleteSources(1, &source);
	alDeleteBuffers(1, &buffer);

#ifndef _WIN32
	// 恢復終端設置
	reset_terminal_mode();
#endif

	CloseAL();

	return 0;
}