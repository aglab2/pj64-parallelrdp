#ifndef CONFIG_H
#define CONFIG_H

#define KEY_FULLSCREEN 0
#define KEY_UPSCALING 1
#define KEY_SCREEN_WIDTH 2
#define KEY_SCREEN_HEIGHT 3
#define KEY_SSREADBACKS 4 
#define KEY_SSDITHER 5
#define KEY_DEINTERLACE 6
#define KEY_INTEGER 7
#define KEY_OVERSCANCROP 8
#define KEY_AA 9
#define KEY_DIVOT 10
#define KEY_GAMMADITHER 11 
#define KEY_VIBILERP 12
#define KEY_VIDITHER 13 
#define KEY_NATIVETEXTLOD 14
#define KEY_NATIVETEXTRECT 15
#define KEY_VSYNC 16
#define KEY_DOWNSCALING 17
#define KEY_WIDESCREEN 18
#define KEY_SYNCHRONOUS 19
#define NUM_CONFIGVARS 20

struct settingkey_t
{
	char name[255];
	int val;
};

#ifdef __cplusplus
extern "C" {
#endif

	extern struct settingkey_t settings[NUM_CONFIGVARS];

	extern void config_init(void);
	extern void config_save(void);
	extern void config_load(void);

#ifdef __cplusplus
}
#endif


#endif // CONFIG_H
