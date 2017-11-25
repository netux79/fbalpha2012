#include "libretro.h"
#include "burner.h"

#ifdef USE_OLD_MAPPING
	#include "bind_map.h"
#endif

#include <vector>
#include <string>

#include "cd/cd_interface.h"

#define FBA_VERSION "v0.2.97.29" // Sept 16, 2013 (SVN)

#if defined(CPS1_ONLY)
#define CORE_OPTION_NAME "fbalpha2012_cps1"
#elif defined(CPS2_ONLY)
#define CORE_OPTION_NAME "fbalpha2012_cps2"
#elif defined(CPS3_ONLY)
#define CORE_OPTION_NAME "fbalpha2012_cps3"
#elif defined(NEOGEO_ONLY)
#define CORE_OPTION_NAME "fbalpha2012_neogeo"
#else
#define CORE_OPTION_NAME "fbalpha2012"
#endif

#if defined(_XBOX) || defined(_WIN32)
   char slash = '\\';
#else
   char slash = '/';
#endif

static void log_dummy(enum retro_log_level level, const char *fmt, ...) { }
static const char *print_label(unsigned i);

static void set_environment();
static bool apply_dipswitch_from_variables();

static void set_input_descriptors();

static void evaluate_neogeo_bios_mode(const char* drvname);

static retro_environment_t environ_cb;
static retro_log_printf_t log_cb = log_dummy;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;

#define BPRINTF_BUFFER_SIZE 512
char bprintf_buf[BPRINTF_BUFFER_SIZE];
static INT32 __cdecl libretro_bprintf(INT32 nStatus, TCHAR* szFormat, ...)
{
   va_list vp;
   va_start(vp, szFormat);
   int rc = vsnprintf(bprintf_buf, BPRINTF_BUFFER_SIZE, szFormat, vp);
   va_end(vp);

   if (rc >= 0)
   {
      retro_log_level retro_log = RETRO_LOG_DEBUG;
      if (nStatus == PRINT_UI)
         retro_log = RETRO_LOG_INFO;
      else if (nStatus == PRINT_IMPORTANT)
         retro_log = RETRO_LOG_WARN;
      else if (nStatus == PRINT_ERROR)
         retro_log = RETRO_LOG_ERROR;
         
      log_cb(retro_log, bprintf_buf);
   }
   
   return rc;
}

INT32 (__cdecl *bprintf) (INT32 nStatus, TCHAR* szFormat, ...) = libretro_bprintf;

// FBARL ---

extern UINT8 NeoSystem;
bool is_neogeo_game = false;
bool allow_neogeo_mode = true;

enum neo_geo_modes
{
   /* MVS */
   NEO_GEO_MODE_MVS = 0,

   /* AES */
   NEO_GEO_MODE_AES = 1,

   /* UNIBIOS */
   NEO_GEO_MODE_UNIBIOS = 2,

   /* DIPSWITCH */
   NEO_GEO_MODE_DIPSWITCH = 3,
};

static uint8_t keybinds[0x5000][2];

#define RETRO_DEVICE_ID_JOYPAD_EMPTY 255
static UINT8 diag_input_hold_frame_delay = 0;
static int   diag_input_combo_start_frame = 0;
static bool  diag_combo_activated = false;
static bool  one_diag_input_pressed = false;
static bool  all_diag_input_pressed = true;

static UINT8 *diag_input;
static UINT8 diag_input_start[] =       {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_start_a_b[] =   {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_start_l_r[] =   {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select[] =      {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select_a_b[] =  {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select_l_r[] =  {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_EMPTY };

static unsigned int BurnDrvGetIndexByName(const char* name);

static neo_geo_modes g_opt_neo_geo_mode = NEO_GEO_MODE_MVS;
static bool gamepad_controls_p1 = true;
static bool gamepad_controls_p2 = true;
static bool newgen_controls_p1 = false;
static bool newgen_controls_p2 = false;
static bool remap_lr_p1 = false;
static bool remap_lr_p2 = false;
static bool core_aspect_par = false;

extern INT32 EnableHiscores;

#define STAT_NOFIND  0
#define STAT_OK      1
#define STAT_CRC     2
#define STAT_SMALL   3
#define STAT_LARGE   4

#define cpsx 1
#define neogeo 2

static int descriptor_id = 0;

struct ROMFIND
{
	unsigned int nState;
	int nArchive;
	int nPos;
   BurnRomInfo ri;
};

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

#define AUDIO_SAMPLERATE 32000
#define AUDIO_SEGMENT_LENGTH 534 // <-- Hardcoded value that corresponds well to 32kHz audio.

static uint32_t *g_fba_frame;
static int16_t g_audio_buf[AUDIO_SEGMENT_LENGTH * 2];

// libretro globals

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

static const struct retro_variable var_empty = { NULL, NULL };

static const struct retro_variable var_fba_aspect = { CORE_OPTION_NAME "_aspect", "Core-provided aspect ratio; DAR|PAR" };
static const struct retro_variable var_fba_cpu_speed_adjust = { CORE_OPTION_NAME "_cpu_speed_adjust", "CPU overclock; 100|110|120|130|140|150|160|170|180|190|200" };
static const struct retro_variable var_fba_diagnostic_input = { CORE_OPTION_NAME "_diagnostic_input", "Diagnostic Input; None|Hold Start|Start + A + B|Hold Start + A + B|Start + L + R|Hold Start + L + R|Hold Select|Select + A + B|Hold Select + A + B|Select + L + R|Hold Select + L + R" };
static const struct retro_variable var_fba_hiscores = { CORE_OPTION_NAME "_hiscores", "Hiscores; enabled|disabled" };

// Neo Geo core options
static const struct retro_variable var_fba_neogeo_mode = { CORE_OPTION_NAME "_neogeo_mode", "Neo Geo mode; MVS|AES|UNIBIOS|DIPSWITCH" };

// Mapping core options
static const struct retro_variable var_fba_controls_p1 = { CORE_OPTION_NAME "_controls_p1", "P1 control scheme; gamepad|arcade" };
static const struct retro_variable var_fba_controls_p2 = { CORE_OPTION_NAME "_controls_p2", "P2 control scheme; gamepad|arcade" };
static const struct retro_variable var_fba_neogeo_controls_p1 = { CORE_OPTION_NAME "_neogeo_controls_p1", "Neo Geo P1 gamepad scheme; classic|newgen" };
static const struct retro_variable var_fba_neogeo_controls_p2 = { CORE_OPTION_NAME "_neogeo_controls_p2", "Neo Geo P2 gamepad scheme; classic|newgen" };
static const struct retro_variable var_fba_lr_controls_p1 = { CORE_OPTION_NAME "_lr_controls_p1", "L/R P1 gamepad scheme; normal|remap to R1/R2" };
static const struct retro_variable var_fba_lr_controls_p2 = { CORE_OPTION_NAME "_lr_controls_p2", "L/R P2 gamepad scheme; normal|remap to R1/R2" };

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   set_environment();
}

struct RomBiosInfo {
	char* filename;
	uint32_t crc;
	uint8_t NeoSystem;
	char* friendly_name;
	uint8_t priority;
};

static struct RomBiosInfo mvs_bioses[] = {
   {"asia-s3.rom",       0x91b64be3, 0x00, "MVS Asia/Europe ver. 6 (1 slot)",  1 },
   {"sp-s2.sp1",         0x9036d879, 0x01, "MVS Asia/Europe ver. 5 (1 slot)",  2 },
   {"sp-s.sp1",          0xc7f2fa45, 0x02, "MVS Asia/Europe ver. 3 (4 slot)",  3 },
   {"sp-u2.sp1",         0xe72943de, 0x03, "MVS USA ver. 5 (2 slot)"        ,  4 },
   {"sp-e.sp1",          0x2723a5b5, 0x04, "MVS USA ver. 5 (6 slot)"        ,  5 },
   {"vs-bios.rom",       0xf0e8f27d, 0x05, "MVS Japan ver. 6 (? slot)"      ,  6 },
   {"sp-j2.sp1",         0xacede59C, 0x06, "MVS Japan ver. 5 (? slot)"      ,  7 },
   {"sp1.jipan.1024",    0x9fb0abe4, 0x07, "MVS Japan ver. 3 (4 slot)"      ,  8 },
   {"sp-45.sp1",         0x03cc9f6a, 0x08, "NEO-MVH MV1C"                   ,  9 },
   {"japan-j3.bin",      0xdff6d41f, 0x09, "MVS Japan (J3)"                 , 10 },
   {"sp-1v1_3db8c.bin",  0x162f0ebe, 0x0d, "Deck ver. 6 (Git Ver 1.3)"      , 11 },
   {NULL, 0, 0, NULL, 0 }
};

static struct RomBiosInfo aes_bioses[] = {
   {"neo-epo.bin",       0xd27a71f1, 0x0b, "AES Asia"                       ,  1 },
   {"neo-po.bin",        0x16d0c132, 0x0a, "AES Japan"                      ,  2 },
   {"neodebug.bin",      0x698ebb7d, 0x0c, "Development Kit"                ,  3 },
   {NULL, 0, 0, NULL, 0 }
};

static struct RomBiosInfo uni_bioses[] = {
   {"uni-bios_3_0.rom",  0xa97c89a9, 0x0e, "Universe BIOS ver. 3.0"         ,  1 },
   {"uni-bios_2_3.rom",  0x27664eb5, 0x0f, "Universe BIOS ver. 2.3"         ,  2 },
   {"uni-bios_2_3o.rom", 0x601720ae, 0x10, "Universe BIOS ver. 2.3 (alt)"   ,  3 },
   {"uni-bios_2_2.rom",  0x2d50996a, 0x11, "Universe BIOS ver. 2.2"         ,  4 },
   {"uni-bios_2_1.rom",  0x8dabf76b, 0x12, "Universe BIOS ver. 2.1"         ,  5 },
   {"uni-bios_2_0.rom",  0x0c12c2ad, 0x13, "Universe BIOS ver. 2.0"         ,  6 },
   {"uni-bios_1_3.rom",  0xb24b44a0, 0x14, "Universe BIOS ver. 1.3"         ,  7 },
   {"uni-bios_1_2.rom",  0x4fa698e9, 0x15, "Universe BIOS ver. 1.2"         ,  8 },
   {"uni-bios_1_2o.rom", 0xe19d3ce9, 0x16, "Universe BIOS ver. 1.2 (alt)"   ,  9 },
   {"uni-bios_1_1.rom",  0x5dda0d84, 0x17, "Universe BIOS ver. 1.1"         , 10 },
   {"uni-bios_1_0.rom",  0x0ce453a0, 0x18, "Universe BIOS ver. 1.0"         , 11 },
   {NULL, 0, 0, NULL, 0 }
};

static struct RomBiosInfo unknown_bioses[] = {
   {"neopen.sp1",        0xcb915e76, 0x1a, "NeoOpen BIOS v0.1 beta"         ,  1 },
   {NULL, 0, 0, NULL, 0 }
};

static RomBiosInfo *available_mvs_bios = NULL;
static RomBiosInfo *available_aes_bios = NULL;
static RomBiosInfo *available_uni_bios = NULL;

#if !(defined(CPS1_ONLY) || defined(CPS2_ONLY) || defined(CPS3_ONLY) || defined(GEKKO))
void set_neo_system_bios()
{
   if (g_opt_neo_geo_mode == NEO_GEO_MODE_DIPSWITCH)
   {
      // Nothing to do in DIPSWITCH mode because the NeoSystem variable is changed by the DIP Switch core option
      log_cb(RETRO_LOG_INFO, "DIPSWITCH Neo Geo Mode selected => NeoSystem: 0x%02x.\n", NeoSystem);
   }
   else if (g_opt_neo_geo_mode == NEO_GEO_MODE_MVS)
   {
      NeoSystem &= ~(UINT8)0x1f;
      if (available_mvs_bios)
      {
         NeoSystem |= available_mvs_bios->NeoSystem;
         log_cb(RETRO_LOG_INFO, "MVS Neo Geo Mode selected => Set NeoSystem: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_mvs_bios->filename, available_mvs_bios->crc, available_mvs_bios->friendly_name);
      }
      else
      {
         // fallback to another bios type if we didn't find the bios selected by the user
         available_mvs_bios = (available_aes_bios) ? available_aes_bios : available_uni_bios;
         if (available_mvs_bios)
         {
            NeoSystem |= available_mvs_bios->NeoSystem;
            log_cb(RETRO_LOG_WARN, "MVS Neo Geo Mode selected but MVS bios not available => fall back to another: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_mvs_bios->filename, available_mvs_bios->crc, available_mvs_bios->friendly_name);
         }
      }
   }
   else if (g_opt_neo_geo_mode == NEO_GEO_MODE_AES)
   {
      NeoSystem &= ~(UINT8)0x1f;
      if (available_aes_bios)
      {
         NeoSystem |= available_aes_bios->NeoSystem;
         log_cb(RETRO_LOG_INFO, "AES Neo Geo Mode selected => Set NeoSystem: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_aes_bios->filename, available_aes_bios->crc, available_aes_bios->friendly_name);
      }
      else
      {
         // fallback to another bios type if we didn't find the bios selected by the user
         available_aes_bios = (available_mvs_bios) ? available_mvs_bios : available_uni_bios;
         if (available_aes_bios)
         {
            NeoSystem |= available_aes_bios->NeoSystem;
            log_cb(RETRO_LOG_WARN, "AES Neo Geo Mode selected but AES bios not available => fall back to another: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_aes_bios->filename, available_aes_bios->crc, available_aes_bios->friendly_name);
         }
      }      
   }
   else if (g_opt_neo_geo_mode == NEO_GEO_MODE_UNIBIOS)
   {
      NeoSystem &= ~(UINT8)0x1f;
      if (available_uni_bios)
      {
         NeoSystem |= available_uni_bios->NeoSystem;
         log_cb(RETRO_LOG_INFO, "UNIBIOS Neo Geo Mode selected => Set NeoSystem: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_uni_bios->filename, available_uni_bios->crc, available_uni_bios->friendly_name);
      }
      else
      {
         // fallback to another bios type if we didn't find the bios selected by the user
         available_uni_bios = (available_mvs_bios) ? available_mvs_bios : available_aes_bios;
         if (available_uni_bios)
         {
            NeoSystem |= available_uni_bios->NeoSystem;
            log_cb(RETRO_LOG_WARN, "UNIBIOS Neo Geo Mode selected but UNIBIOS not available => fall back to another: 0x%02x (%s [0x%08x] (%s)).\n", NeoSystem, available_uni_bios->filename, available_uni_bios->crc, available_uni_bios->friendly_name);
         }
      }
   }
}
#endif /* #if !(defined(CPS1_ONLY) || defined(CPS2_ONLY) || defined(CPS3_ONLY)) */

char g_rom_dir[1024];
char g_save_dir[1024];
char g_system_dir[1024];
static bool driver_inited;


void retro_get_system_info(struct retro_system_info *info)
{
#ifndef TARGET
#define TARGET ""
#endif
   info->library_name = "FB Alpha 2012" TARGET;
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = FBA_VERSION GIT_VERSION;
   info->need_fullpath = true;
   info->block_extract = true;
   info->valid_extensions = "iso|zip|7z";
}

/////
static void InputTick();
static void InputMake();
static bool init_input();
static void check_variables();

void wav_exit() { }

// FBA stubs
unsigned ArcadeJoystick;

int bDrvOkay;
int bRunPause;
bool bAlwaysProcessKeyboardInput;

bool bDoIpsPatch;
void IpsApplyPatches(UINT8 *, char *) {}

TCHAR szAppHiscorePath[MAX_PATH];
TCHAR szAppSamplesPath[MAX_PATH];
TCHAR szAppBurnVer[16];

CDEmuStatusValue CDEmuStatus;

const char* isowavLBAToMSF(const int LBA) { return ""; }
int isowavMSFToLBA(const char* address) { return 0; }
TCHAR* GetIsoPath() { return NULL; }
INT32 CDEmuInit() { return 0; }
INT32 CDEmuExit() { return 0; }
INT32 CDEmuStop() { return 0; }
INT32 CDEmuPlay(UINT8 M, UINT8 S, UINT8 F) { return 0; }
INT32 CDEmuLoadSector(INT32 LBA, char* pBuffer) { return 0; }
UINT8* CDEmuReadTOC(INT32 track) { return 0; }
UINT8* CDEmuReadQChannel() { return 0; }
INT32 CDEmuGetSoundBuffer(INT16* buffer, INT32 samples) { return 0; }

// Replace the char c_find by the char c_replace in the destination c string
char* str_char_replace(char* destination, char c_find, char c_replace)
{
   for (unsigned str_idx = 0; str_idx < strlen(destination); str_idx++)
   {
      if (destination[str_idx] == c_find)
         destination[str_idx] = c_replace;
   }

   return destination;
}

std::vector<retro_input_descriptor> normal_input_descriptors;

static struct GameInp *pgi_reset;
static struct GameInp *pgi_diag;

struct dipswitch_core_option_value
{
   struct GameInp *pgi;
   BurnDIPInfo bdi;
   char friendly_name[100];
};

struct dipswitch_core_option
{
   char option_name[100];
   char friendly_name[100];

   std::string values_str;
   std::vector<dipswitch_core_option_value> values;
};

static int nDIPOffset;

static std::vector<dipswitch_core_option> dipswitch_core_options;

static void InpDIPSWGetOffset (void)
{
	BurnDIPInfo bdi;
	nDIPOffset = 0;

	for(int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags == 0xF0)
		{
			nDIPOffset = bdi.nInput;
            log_cb(RETRO_LOG_INFO, "DIP switches offset: %d.\n", bdi.nInput);
			break;
		}
	}
}

void InpDIPSWResetDIPs (void)
{
	int i = 0;
	BurnDIPInfo bdi;
	struct GameInp * pgi = NULL;

	InpDIPSWGetOffset();

	while (BurnDrvGetDIPInfo(&bdi, i) == 0)
	{
		if (bdi.nFlags == 0xFF)
		{
			pgi = GameInp + bdi.nInput + nDIPOffset;
			if (pgi)
				pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);	
		}
		i++;
	}
}

static int InpDIPSWInit(void)
{
   log_cb(RETRO_LOG_INFO, "Initialize DIP switches.\n");

   dipswitch_core_options.clear();

   BurnDIPInfo bdi;
   struct GameInp *pgi;

   const char * drvname = BurnDrvGetTextA(DRV_NAME);
   
   if (!drvname)
      return 0;

   for (int i = 0, j = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      /* 0xFE is the beginning label for a DIP switch entry */
      /* 0xFD are region DIP switches */
      if ((bdi.nFlags == 0xFE || bdi.nFlags == 0xFD) && bdi.nSetting > 0)
      {
         dipswitch_core_options.push_back(dipswitch_core_option());
         dipswitch_core_option *dip_option = &dipswitch_core_options.back();

         // Clean the dipswitch name to creation the core option name (removing space and equal characters)
         char option_name[100];

         // Some dipswitch has no name...
         if (bdi.szText)
         {
            strcpy(option_name, bdi.szText);
         }
         else // ... so, to not hang, we will generate a name based on the position of the dip (DIPSWITCH 1, DIPSWITCH 2...)
         {
            sprintf(option_name, "DIPSWITCH %d", (char) dipswitch_core_options.size());
            log_cb(RETRO_LOG_WARN, "Error in %sDIPList : The DIPSWITCH '%d' has no name. '%s' name has been generated\n", drvname, dipswitch_core_options.size(), option_name);
         }

         strncpy(dip_option->friendly_name, option_name, sizeof(dip_option->friendly_name));

         str_char_replace(option_name, ' ', '_');
         str_char_replace(option_name, '=', '_');

         snprintf(dip_option->option_name, sizeof(dip_option->option_name), CORE_OPTION_NAME "_dipswitch_%s_%s", drvname, option_name);

         // Search for duplicate name, and add number to make them unique in the core-options file
         for (int dup_idx = 0, dup_nbr = 1; dup_idx < dipswitch_core_options.size() - 1; dup_idx++) // - 1 to exclude the current one
         {
            if (strcmp(dip_option->option_name, dipswitch_core_options[dup_idx].option_name) == 0)
            {
               dup_nbr++;
               snprintf(dip_option->option_name, sizeof(dip_option->option_name), CORE_OPTION_NAME "_dipswitch_%s_%s_%d", drvname, option_name, dup_nbr);
            }
         }

         // Reserve space for the default value
         dip_option->values.reserve(bdi.nSetting + 1); // + 1 for default value
         dip_option->values.assign(bdi.nSetting + 1, dipswitch_core_option_value());

         int values_count = 0;
         bool skip_unusable_option = false;
         for (int k = 0; values_count < bdi.nSetting; k++)
         {
            BurnDIPInfo bdi_value;
            if (BurnDrvGetDIPInfo(&bdi_value, k + i + 1) != 0)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': End of the struct was reached too early\n", drvname, dip_option->friendly_name);
               break;
            }

            if (bdi_value.nFlags == 0xFE || bdi_value.nFlags == 0xFD)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': Start of next DIPSWITCH is too early\n", drvname, dip_option->friendly_name);
               break;
            }

            struct GameInp *pgi_value = GameInp + bdi_value.nInput + nDIPOffset;

            // When the pVal of one value is NULL => the DIP switch is unusable. So it will be skipped by removing it from the list
            if (pgi_value->Input.pVal == 0)
            {
               skip_unusable_option = true;
               break;
            }

            // Filter away NULL entries
            if (bdi_value.nFlags == 0)
            {
               log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': the line '%d' is useless\n", drvname, dip_option->friendly_name, k + 1);
               continue;
            }

            dipswitch_core_option_value *dip_value = &dip_option->values[values_count + 1]; // + 1 to skip the default value

            BurnDrvGetDIPInfo(&(dip_value->bdi), k + i + 1);
            dip_value->pgi = pgi_value;
            strncpy(dip_value->friendly_name, dip_value->bdi.szText, sizeof(dip_value->friendly_name));

            bool is_default_value = (dip_value->pgi->Input.Constant.nConst & dip_value->bdi.nMask) == (dip_value->bdi.nSetting);

            if (is_default_value)
            {
               dipswitch_core_option_value *default_dip_value = &dip_option->values[0];

               default_dip_value->bdi = dip_value->bdi;
               default_dip_value->pgi = dip_value->pgi;

               snprintf(default_dip_value->friendly_name, sizeof(default_dip_value->friendly_name), "%s %s", "(Default)", default_dip_value->bdi.szText);
            }

            values_count++;
         }
         
         if (bdi.nSetting > values_count)
         {
            // Truncate the list at the values_count found to not have empty values
            dip_option->values.resize(values_count + 1); // +1 for default value
            log_cb(RETRO_LOG_WARN, "Error in %sDIPList for DIPSWITCH '%s': '%d' values were intended and only '%d' were found\n", drvname, dip_option->friendly_name, bdi.nSetting, values_count);
         }

         // Skip the unusable option by removing it from the list
         if (skip_unusable_option)
         {
            dipswitch_core_options.pop_back();
            continue;
         }

         pgi = GameInp + bdi.nInput + nDIPOffset;

         // Create the string values for the core option
         dip_option->values_str.assign(dip_option->friendly_name);
         dip_option->values_str.append("; ");

         log_cb(RETRO_LOG_INFO, "'%s' (%d)\n", dip_option->friendly_name, dip_option->values.size() - 1); // -1 to exclude the Default from the DIP Switch count
         for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
         {
            dipswitch_core_option_value *dip_value = &(dip_option->values[dip_value_idx]);

            dip_option->values_str.append(dip_option->values[dip_value_idx].friendly_name);
            if (dip_value_idx != dip_option->values.size() - 1)
               dip_option->values_str.append("|");

            log_cb(RETRO_LOG_INFO, "   '%s'\n", dip_option->values[dip_value_idx].friendly_name);
         }         
         //dip_option->values_str.shrink_to_fit(); // C++ 11 feature

         j++;
      }
   }

   evaluate_neogeo_bios_mode(drvname);

   set_environment();
   apply_dipswitch_from_variables();

   return 0;
}

static void evaluate_neogeo_bios_mode(const char* drvname)
{
   if (!is_neogeo_game)
      return;

   bool is_neogeo_needs_specific_bios = false;

   // search the BIOS dipswitch
   for (int dip_idx = 0; dip_idx < dipswitch_core_options.size(); dip_idx++)
   {
      if (strcasecmp(dipswitch_core_options[dip_idx].friendly_name, "BIOS") == 0)
      {
         if (dipswitch_core_options[dip_idx].values.size() > 0)
         {
            // values[0] is the default value of the dipswitch
            // if the default is different than 0, this means that a different Bios is needed
            if (dipswitch_core_options[dip_idx].values[0].bdi.nSetting != 0x00)
            {
               is_neogeo_needs_specific_bios = true;
               break;
            }
         }
      }      
   }

   if (is_neogeo_needs_specific_bios)
   {
      // disable the NeoGeo mode core option
      allow_neogeo_mode = false;

      // set the NeoGeo mode to DIPSWITCH to rely on the Default Bios Dipswitch
      g_opt_neo_geo_mode = NEO_GEO_MODE_DIPSWITCH;
   }   
}

static void set_environment()
{
   std::vector<const retro_variable*> vars_systems;

   // Add the Global core options
   vars_systems.push_back(&var_fba_aspect);
   vars_systems.push_back(&var_fba_cpu_speed_adjust);
   vars_systems.push_back(&var_fba_controls_p1);
   vars_systems.push_back(&var_fba_controls_p2);
   vars_systems.push_back(&var_fba_hiscores);

   // Add the remap L/R to R1/R2 options
   vars_systems.push_back(&var_fba_lr_controls_p1);
   vars_systems.push_back(&var_fba_lr_controls_p2);

   if (pgi_diag)
   {
      vars_systems.push_back(&var_fba_diagnostic_input);
   }

   if (is_neogeo_game)
   {
      // Add the Neo Geo core options
      if (allow_neogeo_mode)
         vars_systems.push_back(&var_fba_neogeo_mode);

      vars_systems.push_back(&var_fba_neogeo_controls_p1);
      vars_systems.push_back(&var_fba_neogeo_controls_p2);
   }

   int nbr_vars = vars_systems.size();
   int nbr_dips = dipswitch_core_options.size();

   log_cb(RETRO_LOG_INFO, "set_environment: SYSTEM: %d, DIPSWITCH: %d\n", nbr_vars, nbr_dips);

   struct retro_variable vars[nbr_vars + nbr_dips + 1]; // + 1 for the empty ending retro_variable
   
   int idx_var = 0;

   // Add the System core options
   for (int i = 0; i < nbr_vars; i++, idx_var++)
   {
      vars[idx_var] = *vars_systems[i];
      log_cb(RETRO_LOG_INFO, "retro_variable (SYSTEM)    { '%s', '%s' }\n", vars[idx_var].key, vars[idx_var].value);
   }

   // Add the DIP switches core options
   for (int dip_idx = 0; dip_idx < nbr_dips; dip_idx++, idx_var++)
   {
      vars[idx_var].key = dipswitch_core_options[dip_idx].option_name;
      vars[idx_var].value = dipswitch_core_options[dip_idx].values_str.c_str();
      log_cb(RETRO_LOG_INFO, "retro_variable (DIPSWITCH) { '%s', '%s' }\n", vars[idx_var].key, vars[idx_var].value);
   }

   vars[idx_var] = var_empty;
   
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

// Update DIP switches value  depending of the choice the user made in core options
static bool apply_dipswitch_from_variables()
{
   bool dip_changed = false;
   
   log_cb(RETRO_LOG_INFO, "Apply DIP switches value from core options.\n");
   struct retro_variable var = {0};
   
   for (int dip_idx = 0; dip_idx < dipswitch_core_options.size(); dip_idx++)
   {
      dipswitch_core_option *dip_option = &dipswitch_core_options[dip_idx];

      var.key = dip_option->option_name;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) == false)
         continue;

      for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
      {
         dipswitch_core_option_value *dip_value = &(dip_option->values[dip_value_idx]);

         if (strcasecmp(var.value, dip_value->friendly_name) != 0)
            continue;

         int old_nConst = dip_value->pgi->Input.Constant.nConst;

         dip_value->pgi->Input.Constant.nConst = (dip_value->pgi->Input.Constant.nConst & ~dip_value->bdi.nMask) | (dip_value->bdi.nSetting & dip_value->bdi.nMask);
         dip_value->pgi->Input.nVal = dip_value->pgi->Input.Constant.nConst;
         if (dip_value->pgi->Input.pVal)
            *(dip_value->pgi->Input.pVal) = dip_value->pgi->Input.nVal;

         if (dip_value->pgi->Input.Constant.nConst == old_nConst)
         {
            log_cb(RETRO_LOG_INFO, "DIP switch at PTR: [%-10d] [0x%02x] -> [0x%02x] - No change - '%s' '%s' [0x%02x]\n",
               dip_value->pgi->Input.pVal, old_nConst, dip_value->pgi->Input.Constant.nConst, dip_option->friendly_name, dip_value->friendly_name, dip_value->bdi.nSetting);
         }
         else
         {
            dip_changed = true;
            log_cb(RETRO_LOG_INFO, "DIP switch at PTR: [%-10d] [0x%02x] -> [0x%02x] - Changed   - '%s' '%s' [0x%02x]\n",
               dip_value->pgi->Input.pVal, old_nConst, dip_value->pgi->Input.Constant.nConst, dip_option->friendly_name, dip_value->friendly_name, dip_value->bdi.nSetting);
         }
      }
   }

#if !(defined(CPS1_ONLY) || defined(CPS2_ONLY) || defined(CPS3_ONLY) || defined(GEKKO))
   // Override the NeoGeo bios DIP Switch by the main one (for the moment)
   if (is_neogeo_game)
      set_neo_system_bios();
#endif

   return dip_changed;
}

int InputSetCooperativeLevel(const bool bExclusive, const bool bForeGround) { return 0; }

void Reinitialise(void) {
    // Update the geometry, some games (sfiii2) and systems (megadrive) need it.
    struct retro_system_av_info av_info;
    retro_get_system_av_info(&av_info);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

static void ForceFrameStep()
{
   nBurnLayer = 0xff;
   pBurnSoundOut = g_audio_buf;
   nBurnSoundRate = AUDIO_SAMPLERATE;
   //nBurnSoundLen = AUDIO_SEGMENT_LENGTH;
   nCurrentFrame++;

   BurnDrvFrame();
}

// Non-idiomatic (OutString should be to the left to match strcpy())
// Seems broken to not check nOutSize.
char* TCHARToANSI(const TCHAR* pszInString, char* pszOutString, int /*nOutSize*/)
{
   if (pszOutString)
   {
      strcpy(pszOutString, pszInString);
      return pszOutString;
   }

   return (char*)pszInString;
}

int QuoteRead(char **, char **, char*) { return 1; }
char *LabelCheck(char *, char *) { return 0; }
const int nConfigMinVersion = 0x020921;

// addition to support loading of roms without crc check
static int find_rom_by_name(char *name, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
	for (i = 0; i < elems; i++)
   {
      if(!strcmp(list[i].szName, name)) 
         return i; 
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Not found: %s (name = %s)\n", list[i].szName, name);
#endif

	return -1;
}

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
	  {
         return i;
	  }
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Not found: 0x%X (crc: 0x%X)\n", list[i].nCrc, crc);
#endif
   
   return -1;
}

static RomBiosInfo* find_bios_info(char *szName, uint32_t crc, struct RomBiosInfo* bioses)
{
   for (int i = 0; bioses[i].filename != NULL; i++)
   {
      if (strcmp(bioses[i].filename, szName) == 0 || bioses[i].crc == crc)
      {
         return &bioses[i];
      }
   }

#if 0
   log_cb(RETRO_LOG_ERROR, "Bios not found: %s (crc: 0x%08x)\n", szName, crc);
#endif

   return NULL;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      for (unsigned i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static int archive_load_rom(uint8_t *dest, int *wrote, int i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}

// This code is very confusing. The original code is even more confusing :(
static bool open_archive()
{
	memset(g_find_list, 0, sizeof(g_find_list));

	// FBA wants some roms ... Figure out how many.
	g_rom_count = 0;
	while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
		g_rom_count++;

	g_find_list_path.clear();
	
	// Check if we have said archives.
	// Check if archives are found. These are relative to g_rom_dir.
	char *rom_name;
	for (unsigned index = 0; index < 32; index++)
	{
		if (BurnDrvGetZipName(&rom_name, index))
			continue;

		log_cb(RETRO_LOG_INFO, "[FBA] Archive: %s\n", rom_name);

		char path[1024];
#if defined(_XBOX) || defined(_WIN32)
		snprintf(path, sizeof(path), "%s\\%s", g_rom_dir, rom_name);
#else
		snprintf(path, sizeof(path), "%s/%s", g_rom_dir, rom_name);
#endif

		if (ZipOpen(path) != 0)
			log_cb(RETRO_LOG_ERROR, "[FBA] Failed to find archive: %s, let's continue with other archives...\n", path);
		else
			g_find_list_path.push_back(path);

		ZipClose();
	}

	for (unsigned z = 0; z < g_find_list_path.size(); z++)
	{
		if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "[FBA] Failed to open archive %s\n", g_find_list_path[z].c_str());
			return false;
		}

        log_cb(RETRO_LOG_INFO, "[FBA] Parsing archive %s.\n", g_find_list_path[z].c_str());

		ZipEntry *list = NULL;
		int count;
		ZipGetList(&list, &count);

		// Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
		for (unsigned i = 0; i < g_rom_count; i++)
		{
			if (g_find_list[i].nState == STAT_OK)
				continue;

			if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
			{
				g_find_list[i].nState = STAT_OK;
				continue;
			}

            int index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);

            BurnDrvGetRomName(&rom_name, i, 0);

            bool bad_crc = false;

            if (index < 0)
            {
               index = find_rom_by_name(rom_name, list, count);
               bad_crc = true;
            }

			// USE UNI-BIOS...
			if (index < 0)
			{
				log_cb(RETRO_LOG_WARN, "[FBA] Searching ROM at index %d with CRC 0x%08x and name %s => Not Found\n", i, g_find_list[i].ri.nCrc, rom_name);
               continue;              
            }

            if (bad_crc)
               log_cb(RETRO_LOG_WARN, "[FBA] Using ROM at index %d with wrong CRC and name %s\n", i, rom_name);

#if 0
            log_cb(RETRO_LOG_INFO, "[FBA] Searching ROM at index %d with CRC 0x%08x and name %s => Found\n", i, g_find_list[i].ri.nCrc, rom_name);
#endif                          
            // Search for the best bios available by category
            if (is_neogeo_game)
            {
               RomBiosInfo *bios;

               // MVS BIOS
               bios = find_bios_info(list[index].szName, list[index].nCrc, mvs_bioses);
               if (bios)
               {
                  if (!available_mvs_bios || (available_mvs_bios && bios->priority < available_mvs_bios->priority))
                     available_mvs_bios = bios;
               }

               // AES BIOS
               bios = find_bios_info(list[index].szName, list[index].nCrc, aes_bioses);
               if (bios)
               {
                  if (!available_aes_bios || (available_aes_bios && bios->priority < available_aes_bios->priority))
                     available_aes_bios = bios;
               }

               // Universe BIOS
               bios = find_bios_info(list[index].szName, list[index].nCrc, uni_bioses);
               if (bios)
               {
                  if (!available_uni_bios || (available_uni_bios && bios->priority < available_uni_bios->priority))
                     available_uni_bios = bios;
               }
            }

			// Yay, we found it!
			g_find_list[i].nArchive = z;
			g_find_list[i].nPos = index;
			g_find_list[i].nState = STAT_OK;

			if (list[index].nLen < g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_SMALL;
			else if (list[index].nLen > g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_LARGE;
		}

		free_archive_list(list, count);
		ZipClose();
	}

    bool is_neogeo_bios_available = false;
    if (is_neogeo_game)
    {
       if (!available_mvs_bios && !available_aes_bios && !available_uni_bios)
       {
          log_cb(RETRO_LOG_WARN, "[FBA] NeoGeo BIOS missing ...\n");
       }

#if !(defined(CPS1_ONLY) || defined(CPS2_ONLY) || defined(CPS3_ONLY) || defined(GEKKO))
       set_neo_system_bios();
#endif

       // if we have a least one type of bios, we will be able to skip the asia-s3.rom non optional bios
       if (available_mvs_bios || available_aes_bios || available_uni_bios)
       {
          is_neogeo_bios_available = true;
       }
    }

	// Going over every rom to see if they are properly loaded before we continue ...
	for (unsigned i = 0; i < g_rom_count; i++)
	{
		if (g_find_list[i].nState != STAT_OK)
		{
			if(!(g_find_list[i].ri.nType & BRF_OPT))
            {
				// make the asia-s3.rom [0x91B64BE3] (mvs_bioses[0]) optional if we have another bios available
				if (is_neogeo_game && g_find_list[i].ri.nCrc == mvs_bioses[0].crc && is_neogeo_bios_available)
					continue;

				log_cb(RETRO_LOG_ERROR, "[FBA] ROM at index %d with CRC 0x%08x is required ...\n", i, g_find_list[i].ri.nCrc);
				return false;
			}
		}
	}

	BurnExtLoadRom = archive_load_rom;
	return true;
}

void retro_init()
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = log_dummy;

	BurnLibInit();
}

void retro_deinit()
{
   char output[128];

   if (driver_inited)
   {
      snprintf (output, sizeof(output), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
      BurnStateSave(output, 0);
      BurnDrvExit();
   }
   driver_inited = false;
   BurnLibExit();
   if (g_fba_frame)
      free(g_fba_frame);
}

void retro_reset()
{
#if !(defined(CPS1_ONLY) || defined(CPS2_ONLY) || defined(CPS3_ONLY) || defined(GEKKO))
   // restore the NeoSystem because it was changed during the gameplay
   if (is_neogeo_game)
      set_neo_system_bios();
#endif

   if (pgi_reset)
   {
      pgi_reset->Input.nVal = 1;
      *(pgi_reset->Input.pVal) = pgi_reset->Input.nVal;
   }

   ForceFrameStep();
}

static void check_variables(void)
{
   struct retro_variable var = {0};

   var.key = var_fba_cpu_speed_adjust.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "110") == 0)
         nBurnCPUSpeedAdjust = 0x0110;
      else if (strcmp(var.value, "120") == 0)
         nBurnCPUSpeedAdjust = 0x0120;
      else if (strcmp(var.value, "130") == 0)
         nBurnCPUSpeedAdjust = 0x0130;
      else if (strcmp(var.value, "140") == 0)
         nBurnCPUSpeedAdjust = 0x0140;
      else if (strcmp(var.value, "150") == 0)
         nBurnCPUSpeedAdjust = 0x0150;
      else if (strcmp(var.value, "160") == 0)
         nBurnCPUSpeedAdjust = 0x0160;
      else if (strcmp(var.value, "170") == 0)
         nBurnCPUSpeedAdjust = 0x0170;
      else if (strcmp(var.value, "180") == 0)
         nBurnCPUSpeedAdjust = 0x0180;
      else if (strcmp(var.value, "190") == 0)
         nBurnCPUSpeedAdjust = 0x0190;
      else if (strcmp(var.value, "200") == 0)
         nBurnCPUSpeedAdjust = 0x0200;
      else
         nBurnCPUSpeedAdjust = 0x0100;
   }

   var.key = var_fba_controls_p1.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "gamepad") == 0)
         gamepad_controls_p1 = true;
      else
         gamepad_controls_p1 = false;
   }

   var.key = var_fba_controls_p2.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "gamepad") == 0)
         gamepad_controls_p2 = true;
      else
         gamepad_controls_p2 = false;
   }

   var.key = var_fba_aspect.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "PAR") == 0)
         core_aspect_par = true;
	  else
         core_aspect_par = false;
   }

   var.key = var_fba_lr_controls_p1.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "remap to R1/R2") == 0)
         remap_lr_p1 = true;
      else
         remap_lr_p1 = false;
   }

   var.key = var_fba_lr_controls_p2.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "remap to R1/R2") == 0)
         remap_lr_p2 = true;
      else
         remap_lr_p2 = false;
   }

   if (pgi_diag)
   {
      var.key = var_fba_diagnostic_input.key;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      {
         diag_input = NULL;
         diag_input_hold_frame_delay = 0;
         if (strcmp(var.value, "Hold Start") == 0)
         {
            diag_input = diag_input_start;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Start + A + B") == 0)
         {
            diag_input = diag_input_start_a_b;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Start + A + B") == 0)
         {
            diag_input = diag_input_start_a_b;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Start + L + R") == 0)
         {
            diag_input = diag_input_start_l_r;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Start + L + R") == 0)
         {
            diag_input = diag_input_start_l_r;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Hold Select") == 0)
         {
            diag_input = diag_input_select;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Select + A + B") == 0)
         {
            diag_input = diag_input_select_a_b;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Select + A + B") == 0)
         {
            diag_input = diag_input_select_a_b;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Select + L + R") == 0)
         {
            diag_input = diag_input_select_l_r;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Select + L + R") == 0)
         {
            diag_input = diag_input_select_l_r;
            diag_input_hold_frame_delay = 60;
         }
      }
   }

   if (is_neogeo_game)
   {
      var.key = var_fba_neogeo_controls_p1.key;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      {
         if (strcmp(var.value, "newgen") == 0)
            newgen_controls_p1 = true;
         else
            newgen_controls_p1 = false;
      }
      
      var.key = var_fba_neogeo_controls_p2.key;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      {
         if (strcmp(var.value, "newgen") == 0)
            newgen_controls_p2 = true;
         else
            newgen_controls_p2 = false;
      }

      if (allow_neogeo_mode)
      {
         var.key = var_fba_neogeo_mode.key;
         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
         {
            if (strcmp(var.value, "MVS") == 0)
               g_opt_neo_geo_mode = NEO_GEO_MODE_MVS;
            else if (strcmp(var.value, "AES") == 0)
               g_opt_neo_geo_mode = NEO_GEO_MODE_AES;
            else if (strcmp(var.value, "UNIBIOS") == 0)
               g_opt_neo_geo_mode = NEO_GEO_MODE_UNIBIOS;
            else if (strcmp(var.value, "DIPSWITCH") == 0)
               g_opt_neo_geo_mode = NEO_GEO_MODE_DIPSWITCH;
         }
      }
   }
   
   var.key = var_fba_hiscores.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "enabled") == 0)
         EnableHiscores = true;
      else
         EnableHiscores = false;
   }
}

void retro_run()
{
   int width, height;
   BurnDrvGetFullSize(&width, &height);
   pBurnDraw = (uint8_t*)g_fba_frame;

   InputMake();

   ForceFrameStep();

   unsigned drv_flags = BurnDrvGetFlags();
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);

   nBurnPitch = width * pitch_size;
   video_cb(g_fba_frame, width, height, nBurnPitch);
   audio_batch_cb(g_audio_buf, nBurnSoundLen);

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      bool old_gamepad_controls_p1 = gamepad_controls_p1;
      bool old_gamepad_controls_p2 = gamepad_controls_p2;
      bool old_newgen_controls_p1 = newgen_controls_p1;
      bool old_newgen_controls_p2 = newgen_controls_p2;
      bool old_remap_lr_p1 = remap_lr_p1;
      bool old_remap_lr_p2 = remap_lr_p2;
      bool old_core_aspect_par = core_aspect_par;
      neo_geo_modes old_g_opt_neo_geo_mode = g_opt_neo_geo_mode;

      check_variables();

      apply_dipswitch_from_variables();

      bool reinit_input_performed = false;
      // reinitialise input if user changed the control scheme
      if (old_gamepad_controls_p1 != gamepad_controls_p1 ||
          old_gamepad_controls_p2 != gamepad_controls_p2 ||
          old_newgen_controls_p1 != newgen_controls_p1 ||
          old_newgen_controls_p2 != newgen_controls_p2 ||
          old_remap_lr_p1 != remap_lr_p1 ||
          old_remap_lr_p2 != remap_lr_p2)
      {
         init_input();
         reinit_input_performed = true;
      }

      if (!reinit_input_performed) // if the reinit_input_performed is true, the 2 following methods was already called in the init_input one
      {
         // Re-assign all the input_descriptors to retroarch
         set_input_descriptors();
      }

      // adjust aspect ratio if the needed
      if (old_core_aspect_par != core_aspect_par)
      {
         struct retro_system_av_info av_info;
         retro_get_system_av_info(&av_info);
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
      }

      // reset the game if the user changed the bios
      if (old_g_opt_neo_geo_mode != g_opt_neo_geo_mode)
      {
         retro_reset();
      }
   }
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_size;

static int burn_write_state_cb(BurnArea *pba)
{
   memcpy(write_state_ptr, pba->Data, pba->nLen);
   write_state_ptr += pba->nLen;
   return 0;
}

static int burn_read_state_cb(BurnArea *pba)
{
   memcpy(pba->Data, read_state_ptr, pba->nLen);
   read_state_ptr += pba->nLen;
   return 0;
}

static int burn_dummy_state_cb(BurnArea *pba)
{
   state_size += pba->nLen;
   return 0;
}

size_t retro_serialize_size()
{
   if (state_size)
      return state_size;

   BurnAcb = burn_dummy_state_cb;
   state_size = 0;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);
   return state_size;
}

bool retro_serialize(void *data, size_t size)
{
   if (size != state_size)
      return false;

   BurnAcb = burn_write_state_cb;
   write_state_ptr = (uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != state_size)
      return false;
   BurnAcb = burn_read_state_cb;
   read_state_ptr = (const uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, 0);

   return true;
}

void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   int width, height;
   BurnDrvGetFullSize(&width, &height);
   int maximum = width > height ? width : height;
   struct retro_game_geometry geom = { (unsigned)width, (unsigned)height, (unsigned)maximum, (unsigned)maximum };
   
   int game_aspect_x, game_aspect_y;
   BurnDrvGetAspect(&game_aspect_x, &game_aspect_y);

   if (game_aspect_x != 0 && game_aspect_y != 0 && !core_aspect_par)
   {
      geom.aspect_ratio = (float)game_aspect_x / (float)game_aspect_y;
      log_cb(RETRO_LOG_INFO, "retro_get_system_av_info: base_width: %d, base_height: %d, max_width: %d, max_height: %d, aspect_ratio: (%d/%d) = %f (core_aspect_par: %d)\n", geom.base_width, geom.base_height, geom.max_width, geom.max_height, game_aspect_x, game_aspect_y, geom.aspect_ratio, core_aspect_par);
   }
   else
   {
      log_cb(RETRO_LOG_INFO, "retro_get_system_av_info: base_width: %d, base_height: %d, max_width: %d, max_height: %d, aspect_ratio: %f\n", geom.base_width, geom.base_height, geom.max_width, geom.max_height, geom.aspect_ratio);
   }

#ifdef FBACORES_CPS
   struct retro_system_timing timing = { 59.629403, 59.629403 * AUDIO_SEGMENT_LENGTH };
#else
   struct retro_system_timing timing = { (nBurnFPS / 100.0), (nBurnFPS / 100.0) * AUDIO_SEGMENT_LENGTH };
#endif

   info->geometry = geom;
   info->timing   = timing;
}

int VidRecalcPal()
{
   return BurnRecalcPal();
}

bool analog_controls_enabled = false;

static bool fba_init(unsigned driver, const char *game_zip_name)
{
   nBurnDrvActive = driver;

   if (!open_archive()) {
      log_cb(RETRO_LOG_ERROR, "[FBA] Cannot find driver.\n");
      return false;
   }

   nBurnBpp = 2;
   nFMInterpolation = 3;
   nInterpolation = 1;

   analog_controls_enabled = init_input();

   InpDIPSWInit();

   BurnDrvInit();

   char input[128];
   snprintf (input, sizeof(input), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
   BurnStateLoad(input, 0, NULL);

   int width, height;
   BurnDrvGetFullSize(&width, &height);
   unsigned drv_flags = BurnDrvGetFlags();
   if (!(drv_flags & BDF_GAME_WORKING)) {
      log_cb(RETRO_LOG_ERROR, "[FBA] Game %s is not marked as working\n", game_zip_name);
      return false;
   }
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
   nBurnPitch = width * pitch_size;

   unsigned rotation;
   switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
   {
      case BDF_ORIENTATION_VERTICAL:
         rotation = 1;
         break;

      case BDF_ORIENTATION_FLIPPED:
         rotation = 2;
         break;

      case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
         rotation = 3;
         break;

      default:
         rotation = 0;
   }
/*
   if(
         (strcmp("gunbird2", game_zip_name) == 0) ||
         (strcmp("s1945ii", game_zip_name) == 0) ||
         (strcmp("s1945iii", game_zip_name) == 0) ||
         (strcmp("dragnblz", game_zip_name) == 0) ||
         (strcmp("gnbarich", game_zip_name) == 0) ||
         (strcmp("mjgtaste", game_zip_name) == 0) ||
         (strcmp("tgm2", game_zip_name) == 0) ||
         (strcmp("tgm2p", game_zip_name) == 0) ||
         (strcmp("soldivid", game_zip_name) == 0) ||
         (strcmp("daraku", game_zip_name) == 0) ||
         (strcmp("sbomber", game_zip_name) == 0) ||
         (strcmp("sbombera", game_zip_name) == 0) 

         )
   {
      nBurnBpp = 4;
   }
*/
   log_cb(RETRO_LOG_INFO, "Game: %s\n", game_zip_name);

   environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

   VidRecalcPal();

#ifdef FRONTEND_SUPPORTS_RGB565
   if(nBurnBpp == 4)
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
         log_cb(RETRO_LOG_INFO, "Frontend supports XRGB888 - will use that instead of XRGB1555.\n");
   }
   else
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) 
         log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
   }
#endif

   return true;
}

#if defined(FRONTEND_SUPPORTS_RGB565)
static unsigned int HighCol16(int r, int g, int b, int  /* i */)
{
   return (((r << 8) & 0xf800) | ((g << 3) & 0x07e0) | ((b >> 3) & 0x001f));
}
#else
static unsigned int HighCol15(int r, int g, int b, int  /* i */)
{
   return (((r << 7) & 0x7c00) | ((g << 2) & 0x03e0) | ((b >> 3) & 0x001f));
}
#endif


static void init_video()
{
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

bool retro_load_game(const struct retro_game_info *info)
{
   char basename[128];

   if (!info)
      return false;

   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   const char *dir = NULL;
   // If save directory is defined use it, ...
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", g_save_dir);
   }
   else
   {
      // ... otherwise use rom directory
      strncpy(g_save_dir, g_rom_dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_ERROR, "Save dir not defined => use roms dir %s\n", g_save_dir);
   }

   // If system directory is defined use it, ...
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_INFO, "Setting system dir to %s\n", g_system_dir);
   }
   else
   {
      // ... otherwise use rom directory
      strncpy(g_system_dir, g_rom_dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_ERROR, "System dir not defined => use roms dir %s\n", g_system_dir);
   }

   unsigned i = BurnDrvGetIndexByName(basename);
   if (i < nBurnDrvCount)
   {
      INT32 width, height;

      const char * boardrom = BurnDrvGetTextA(DRV_BOARDROM);
      is_neogeo_game = (boardrom && strcmp(boardrom, "neogeo") == 0);

      set_environment();
      check_variables();

      pBurnSoundOut = g_audio_buf;
      nBurnSoundRate = AUDIO_SAMPLERATE;
      nBurnSoundLen = AUDIO_SEGMENT_LENGTH;

      if (!fba_init(i, basename))
         goto error;

      driver_inited = true;

      BurnDrvGetFullSize(&width, &height);

      g_fba_frame = (uint32_t*)malloc(width * height * sizeof(uint32_t));

      return true;
   }

error:
   log_cb(RETRO_LOG_ERROR, "[FBA] Cannot load this game.\n");
   return false;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game(void) {}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned) { return 0; }
size_t retro_get_memory_size(unsigned) { return 0; }

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned, unsigned) {}

static const char *print_label(unsigned i)
{
   switch(i)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return "RetroPad B Button";
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return "RetroPad Y Button";
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return "RetroPad Select Button";
      case RETRO_DEVICE_ID_JOYPAD_START:
         return "RetroPad Start Button";
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return "RetroPad D-Pad Up";
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return "RetroPad D-Pad Down";
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return "RetroPad D-Pad Left";
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return "RetroPad D-Pad Right";
      case RETRO_DEVICE_ID_JOYPAD_A:
         return "RetroPad A Button";
      case RETRO_DEVICE_ID_JOYPAD_X:
         return "RetroPad X Button";
      case RETRO_DEVICE_ID_JOYPAD_L:
         return "RetroPad L Button";
      case RETRO_DEVICE_ID_JOYPAD_R:
         return "RetroPad R Button";
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return "RetroPad L2 Button";
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return "RetroPad R2 Button";
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return "RetroPad L3 Button";
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return "RetroPad R3 Button";
      case RETRO_DEVICE_ID_JOYPAD_EMPTY:
         return "None";
      default:
         return "No known label";
   }
}

#ifndef USE_OLD_MAPPING
static bool init_input(void)
{
   // Define nMaxPlayers early; GameInpInit() needs it (normally defined in DoLibInit()).
   nMaxPlayers = BurnDrvGetMaxPlayers();
   GameInpInit();
   GameInpDefault();

   // Handle twinstick games (issue libretro/fbalpha#102)
   bool twinstick_game[5] = { false, false, false, false, false };
   bool up_is_mapped[5] = { false, false, false, false, false };
   bool down_is_mapped[5] = { false, false, false, false, false };
   bool left_is_mapped[5] = { false, false, false, false, false };
   bool right_is_mapped[5] = { false, false, false, false, false };

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   // Needed for Neo Geo button mappings (and other drivers in future)
   const char * parentrom  = BurnDrvGetTextA(DRV_PARENT);
   const char * boardrom   = BurnDrvGetTextA(DRV_BOARDROM);
   const char * drvname    = BurnDrvGetTextA(DRV_NAME);
   const char * systemname = BurnDrvGetTextA(DRV_SYSTEM);
   INT32	genre		= BurnDrvGetGenreFlags();
   INT32	hardware	= BurnDrvGetHardwareCode();

   log_cb(RETRO_LOG_INFO, "drvname: %s\n", drvname);
   if(parentrom)
      log_cb(RETRO_LOG_INFO, "parentrom: %s\n", parentrom);
   if(boardrom)
      log_cb(RETRO_LOG_INFO, "boardrom: %s\n", boardrom);
   if (systemname)
      log_cb(RETRO_LOG_INFO, "systemname: %s\n", systemname);
   log_cb(RETRO_LOG_INFO, "genre: %d\n", genre);
   log_cb(RETRO_LOG_INFO, "hardware: %d\n", hardware);

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++)
      keybinds[i][0] = 0xff;

   pgi = GameInp;
   pgi_reset = NULL;
   pgi_diag = NULL;

   normal_input_descriptors.clear();

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;

      bool bPlayerInInfo = (toupper(bii.szInfo[0]) == 'P' && bii.szInfo[1] >= '1' && bii.szInfo[1] <= '4'); // Because some of the older drivers don't use the standard input naming.
      bool bPlayerInName = (bii.szName[0] == 'P' && bii.szName[1] >= '1' && bii.szName[1] <= '4');

      if (bPlayerInInfo || bPlayerInName) {
         INT32 nPlayer = -1;

         if (bPlayerInName)
            nPlayer = bii.szName[1] - '1';
         if (bPlayerInInfo && nPlayer == -1)
            nPlayer = bii.szInfo[1] - '1';

         char* szi = bii.szInfo + 3;

         if (strncmp("select", szi, 6) == 0) {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            value_found = true;
         }
         if (strncmp("coin", szi, 4) == 0) {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            value_found = true;
         }
         if (strncmp("start", szi, 5) == 0) {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_START;
            value_found = true;
         }
         if (strncmp("up", szi, 2) == 0) {
            if( up_is_mapped[nPlayer] ) {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
               twinstick_game[nPlayer] = true;
            } else {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_UP;
               up_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("down", szi, 4) == 0) {
            if( down_is_mapped[nPlayer] ) {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_B;
               twinstick_game[nPlayer] = true;
            } else {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
               down_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("left", szi, 4) == 0) {
            if( left_is_mapped[nPlayer] ) {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_Y;
               twinstick_game[nPlayer] = true;
            } else {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
               left_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("right", szi, 5) == 0) {
            if( right_is_mapped[nPlayer] ) {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
               twinstick_game[nPlayer] = true;
            } else {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
               right_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }

         if (strncmp("fire ", szi, 5) == 0) {
            char *szb = szi + 5;
            INT32 nButton = strtol(szb, NULL, 0);
            if (twinstick_game[nPlayer]) {
               switch (nButton) {
                  case 1:
                     keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_R;
                     value_found = true;
                     break;
                  case 2:
                     keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L;
                     value_found = true;
                     break;
               }
			}
            else if (nFireButtons <= 4) {
               if (is_neogeo_game) {
                  switch (nButton) {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((newgen_controls_p1 && nPlayer == 0) || (newgen_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_B) : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((newgen_controls_p1 && nPlayer == 0) || (newgen_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A) : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((newgen_controls_p1 && nPlayer == 0) || (newgen_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y) : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((newgen_controls_p1 && nPlayer == 0) || (newgen_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_X) : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                  }
               } else {
                  switch (nButton) {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                  }
               }
            } else {
               if (bStreetFighterLayout) {
                  switch (nButton) {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R : RETRO_DEVICE_ID_JOYPAD_L) : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                     case 5:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_L);
                        value_found = true;
                        break;
                     case 6:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_R) : RETRO_DEVICE_ID_JOYPAD_R);
                        value_found = true;
                        break;
                  }
               } else {
                  switch (nButton) {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_R) : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                     case 5:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_L);
                        value_found = true;
                        break;
                     case 6:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R : RETRO_DEVICE_ID_JOYPAD_L) : RETRO_DEVICE_ID_JOYPAD_R);
                        value_found = true;
                        break;
                     case 7:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_R2) : RETRO_DEVICE_ID_JOYPAD_R2);
                        value_found = true;
                        break;
                     case 8:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_L : RETRO_DEVICE_ID_JOYPAD_L2) : RETRO_DEVICE_ID_JOYPAD_L2);
                        value_found = true;
                        break;
                  }
               }
            }
         }

         if (!value_found)
            continue;

         if(nPlayer >= 0)
            keybinds[pgi->Input.Switch.nCode][1] = nPlayer;

         UINT32 port = (UINT32)nPlayer;
         UINT32 device = RETRO_DEVICE_JOYPAD;
         UINT32 index = 0;
         UINT32 id = keybinds[pgi->Input.Switch.nCode][0];

         // "P1 XXX" - try to exclude the "P1 " from the szName
         INT32 offset_player_x = 0;
         if (strlen(bii.szName) > 3 && bii.szName[0] == 'P' && bii.szName[2] == ' ')
            offset_player_x = 3;

         char* description = bii.szName + offset_player_x;
         
         normal_input_descriptors.push_back((retro_input_descriptor){ port, device, index, id, description });

         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - assigned to key [%-25s] on port %2d.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode, print_label(keybinds[pgi->Input.Switch.nCode][0]), port);
      }

      // Store the pgi that controls the reset input
      if (strcmp(bii.szInfo, "reset") == 0)
      {
         value_found = true;
         pgi_reset = pgi;
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }

      // Store the pgi that controls the diagnostic input
      if (strcmp(bii.szInfo, "diag") == 0)
      {
         value_found = true;
         pgi_diag = pgi;
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - controlled by core option.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }

      if (!value_found && bii.nType != BIT_DIPSWITCH)
      {
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - WARNING! Button unaccounted.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }
   }

   // Update core option for diagnostic input
   set_environment();
   // Read the user core option values
   check_variables();

   // The list of normal and macro input_descriptors are filled, we can assign all the input_descriptors to retroarch
   set_input_descriptors();

   return has_analog;
}
#else
static bool init_input(void)
{
   // Define nMaxPlayers early; GameInpInit() needs it (normally defined in DoLibInit()).
   nMaxPlayers = BurnDrvGetMaxPlayers();
   GameInpInit();
   GameInpDefault();

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   // Needed for Neo Geo button mappings (and other drivers in future)
   const char * parentrom  = BurnDrvGetTextA(DRV_PARENT);
   const char * boardrom   = BurnDrvGetTextA(DRV_BOARDROM);
   const char * drvname    = BurnDrvGetTextA(DRV_NAME);
   const char * systemname = BurnDrvGetTextA(DRV_SYSTEM);
   INT32        genre      = BurnDrvGetGenreFlags();
   INT32        hardware   = BurnDrvGetHardwareCode();

   log_cb(RETRO_LOG_INFO, "drvname: %s\n", drvname);
   if (parentrom)
      log_cb(RETRO_LOG_INFO, "parentrom: %s\n", parentrom);
   if (boardrom)
      log_cb(RETRO_LOG_INFO, "boardrom: %s\n", boardrom);
   if (systemname)
      log_cb(RETRO_LOG_INFO, "systemname: %s\n", systemname);
   log_cb(RETRO_LOG_INFO, "genre: %d\n", genre);
   log_cb(RETRO_LOG_INFO, "hardware: %d\n", hardware);
   log_cb(RETRO_LOG_INFO, "max players: %d\n", nMaxPlayers);
   log_cb(RETRO_LOG_INFO, "has_analog: %d\n", has_analog);

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++)
      keybinds[i][0] = 0xff;

   key_map bind_map[BIND_MAP_COUNT];
   unsigned counter = init_bind_map(bind_map, gamepad_controls, newgen_controls_p1, newgen_controls_p2, remap_lr_p1, remap_lr_p2);

   bool is_avsp =   (parentrom && strcmp(parentrom, "avsp") == 0   || strcmp(drvname, "avsp") == 0);
   bool is_armwar = (parentrom && strcmp(parentrom, "armwar") == 0 || strcmp(drvname, "armwar") == 0);

   char button_select[15];
   char button_shot[15];

   pgi = GameInp;
   pgi_reset = NULL;
   pgi_diag = NULL;

   normal_input_descriptors.clear();

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;

      // Store the pgi that controls the reset input
      if (strcmp(bii.szInfo, "reset") == 0)
      {
         value_found = true;
         pgi_reset = pgi;
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }

      // Store the pgi that controls the diagnostic input
      if (strcmp(bii.szInfo, "diag") == 0)
      {
         value_found = true;
         pgi_diag = pgi;
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - controlled by core option.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }

      for(int j = 0; j < counter && !value_found; j++)
      {
         unsigned port = bind_map[j].nCode[1];

         sprintf(button_select, "P%d Select", port + 1); // => P1 Select
         sprintf(button_shot,   "P%d Shot",   port + 1); // => P1 Shot

         if (is_neogeo_game && strcmp(bii.szName, button_select) == 0)
         {
            value_found = true;
            // set the retro device id
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
         }
         /* Alien vs. Predator and Armored Warriors both use "Px Shot" which usually serves as the shoot button for shmups
          * To make sure the controls don't overlap with each other if statements are used */
         else if ((is_avsp || is_armwar) && strcmp(bii.szName, button_shot) == 0)
         {
            value_found = true;
            // set the retro device id
            if (port == 1 || port != 2) // port != 2 is to configure the P3 and P4 like the P1 (TODO it would be good to make the handling of gamepad_controls dynamic regarding the number of players of the game
            {
               if (gamepad_controls == false)
                  keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
               else
                  keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            }
            if (port == 2)
            {
               if (gamepad_controls == false)
                  keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
               else
                  keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
            }
         }
         else if(strcmp(bii.szName, bind_map[j].bii_name) == 0)
         {
            value_found = true;
            // set the retro device id
            keybinds[pgi->Input.Switch.nCode][0] = bind_map[j].nCode[0];
         }

         if (!value_found)
            continue;

         // set the port index
         keybinds[pgi->Input.Switch.nCode][1] = port;

         unsigned device = RETRO_DEVICE_JOYPAD;
         unsigned index = 0;
         unsigned id = keybinds[pgi->Input.Switch.nCode][0];

         // "P1 XXX" - try to exclude the "P1 " from the szName
         int offset_player_x = 0;
         if (strlen(bii.szName) > 3 && bii.szName[0] == 'P' && bii.szName[2] == ' ')
            offset_player_x = 3;

         char* description = bii.szName + offset_player_x;
         
         normal_input_descriptors.push_back((retro_input_descriptor){ port, device, index, id, description });

         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - assigned to key [%-25s] on port %2d.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode, print_label(keybinds[pgi->Input.Switch.nCode][0]), port);

         break;
      }

      if (!value_found && bii.nType != BIT_DIPSWITCH)
      {
         log_cb(RETRO_LOG_INFO, "[%-16s] [%-15s] nSwitch.nCode: 0x%04x - WARNING! Button unaccounted.\n", bii.szName, bii.szInfo, pgi->Input.Switch.nCode);
      }
   }

   // Update core option for diagnostic and macro inputs
   set_environment();
   // Read the user core option values
   check_variables();

   // The list of normal and macro input_descriptors are filled, we can assign all the input_descriptors to retroarch
   set_input_descriptors();

   return has_analog;
}
#endif

//#define DEBUG_INPUT
//

static inline INT32 CinpState(INT32 nCode)
{
   INT32 id = keybinds[nCode][0];
   UINT32 port = keybinds[nCode][1];
   return input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);
}

static inline int CinpJoyAxis(int i, int axis)
{
   switch(axis)
   {
      case 0:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 1:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 2:
         return 0;
      case 3:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 4:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 5:
         return 0;
      case 6:
         return 0;
      case 7:
         return 0;
   }
   return 0;
}

static inline int CinpMouseAxis(int i, int axis)
{
   return 0;
}

static bool poll_diag_input()
{
   if (pgi_diag && diag_input)
   {
      one_diag_input_pressed = false;
      all_diag_input_pressed = true;

      for (int combo_idx = 0; diag_input[combo_idx] != RETRO_DEVICE_ID_JOYPAD_EMPTY; combo_idx++)
      {
         if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, diag_input[combo_idx]) == false)
            all_diag_input_pressed = false;
         else
            one_diag_input_pressed = true;
      }

      if (diag_combo_activated == false && all_diag_input_pressed)
      {
         if (diag_input_combo_start_frame == 0) // => User starts holding all the combo inputs
            diag_input_combo_start_frame = nCurrentFrame;
         else if ((nCurrentFrame - diag_input_combo_start_frame) > diag_input_hold_frame_delay) // Delays of the hold reached
            diag_combo_activated = true;
      }
      else if (one_diag_input_pressed == false)
      {
         diag_combo_activated = false;
         diag_input_combo_start_frame = 0;
      }

      if (diag_combo_activated)
      {
         // Cancel each input of the combo at the emulator side to not interfere when the diagnostic menu will be opened and the combo not yet released
         struct GameInp* pgi = GameInp;
         for (int combo_idx = 0; diag_input[combo_idx] != RETRO_DEVICE_ID_JOYPAD_EMPTY; combo_idx++)
         {
            for (int i = 0; i < nGameInpCount; i++, pgi++)
            {
               if (pgi->nInput == GIT_SWITCH)
               {
                  pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
            }
         }

         // Activate the diagnostic key
         pgi_diag->Input.nVal = 1;
         *(pgi_diag->Input.pVal) = pgi_diag->Input.nVal;

         // Return true to stop polling game inputs while diagnostic combo inputs is pressed
         return true;
      }
   }

   // Return false to poll game inputs
   return false;
}

static void InputTick()
{
	struct GameInp *pgi;
	UINT32 i;

	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++) {
		INT32 nAdd = 0;
		if ((pgi->nInput &  GIT_GROUP_SLIDER) == 0) {				// not a slider
			continue;
		}

		if (pgi->nInput == GIT_JOYSLIDER) {
			// Get state of the axis
			nAdd = CinpJoyAxis(pgi->Input.Slider.JoyAxis.nJoy, pgi->Input.Slider.JoyAxis.nAxis);
			nAdd /= 0x100;
		}

		// nAdd is now -0x100 to +0x100

		// Change to slider speed
		nAdd *= pgi->Input.Slider.nSliderSpeed;
		nAdd /= 0x100;

		if (pgi->Input.Slider.nSliderCenter) {						// Attact to center
			INT32 v = pgi->Input.Slider.nSliderValue - 0x8000;
			v *= (pgi->Input.Slider.nSliderCenter - 1);
			v /= pgi->Input.Slider.nSliderCenter;
			v += 0x8000;
			pgi->Input.Slider.nSliderValue = v;
		}

		pgi->Input.Slider.nSliderValue += nAdd;
		// Limit slider
		if (pgi->Input.Slider.nSliderValue < 0x0100) {
			pgi->Input.Slider.nSliderValue = 0x0100;
		}
		if (pgi->Input.Slider.nSliderValue > 0xFF00) {
			pgi->Input.Slider.nSliderValue = 0xFF00;
		}
	}
}

static void InputMake(void)
{
   poll_cb();

   if (poll_diag_input())
      return;

   struct GameInp* pgi;
   UINT32 i;

   InputTick();

   for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->Input.pVal == NULL)
         continue;

      switch (pgi->nInput)
      {
         case 0:                       // Undefined
            pgi->Input.nVal = 0;
            break;
         case GIT_CONSTANT:            // Constant value
            {
               pgi->Input.nVal = pgi->Input.Constant.nConst;
               *(pgi->Input.pVal) = pgi->Input.nVal;
            }
            break;
         case GIT_SWITCH:
            {
               // Digital input
               INT32 s = CinpState(pgi->Input.Switch.nCode);
#if 0
               log_cb(RETRO_LOG_INFO, "GIT_SWITCH: %s, port: %d, pressed: %d.\n", print_label(id), port, state);
#endif

               if (pgi->nType & BIT_GROUP_ANALOG)
               {
                  // Set analog controls to full
                  if (s)
                     pgi->Input.nVal = 0xFFFF;
                  else
                     pgi->Input.nVal = 0x0001;
#ifdef MSB_FIRST
                  *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
                  *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               }
               else
               {
                  // Binary controls
                  if (s)
                     pgi->Input.nVal = 1;
                  else
                     pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
               break;
            }
         case GIT_KEYSLIDER:						// Keyboard slider
         case GIT_JOYSLIDER:						// Joystick slider
#if 0
            log_cb(RETRO_LOG_INFO, "GIT_JOYSLIDER\n");
#endif
            {
               int nSlider = pgi->Input.Slider.nSliderValue;
               if (pgi->nType == BIT_ANALOG_REL) {
                  nSlider -= 0x8000;
                  nSlider >>= 4;
               }

               pgi->Input.nVal = (UINT16)nSlider;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_MOUSEAXIS:						// Mouse axis
            {
               pgi->Input.nVal = (UINT16)(CinpMouseAxis(pgi->Input.MouseAxis.nMouse, pgi->Input.MouseAxis.nAxis) * nAnalogSpeed);
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
            }
            break;
         case GIT_JOYAXIS_FULL:
            {				// Joystick axis
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

               if (pgi->nType == BIT_ANALOG_REL) {
                  nJoy *= nAnalogSpeed;
                  nJoy >>= 13;

                  // Clip axis to 8 bits
                  if (nJoy < -32768) {
                     nJoy = -32768;
                  }
                  if (nJoy >  32767) {
                     nJoy =  32767;
                  }
               } else {
                  nJoy >>= 1;
                  nJoy += 0x8000;

                  // Clip axis to 16 bits
                  if (nJoy < 0x0001) {
                     nJoy = 0x0001;
                  }
                  if (nJoy > 0xFFFF) {
                     nJoy = 0xFFFF;
                  }
               }

               pgi->Input.nVal = (UINT16)nJoy;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_NEG:
            {				// Joystick axis Lo
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy < 32767)
               {
                  nJoy = -nJoy;

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_POS:
            {				// Joystick axis Hi
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy > 32767)
               {

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
      }
   }
}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int i;
   unsigned int ret = ~0U;

   for (i = 0; i < nBurnDrvCount; i++)
   {
      nBurnDrvActive = i;
      if (!strcmp(BurnDrvGetText(DRV_NAME), name))
      {
         ret = i;
         break;
      }
   }
   return ret;
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif

// Set the input descriptors by combininng the two lists of 'Normal' and 'Macros' inputs
static void set_input_descriptors()
{
   struct retro_input_descriptor input_descriptors[normal_input_descriptors.size() + 1]; // + 1 for the empty ending retro_input_descriptor { 0 }

   unsigned input_descriptor_idx = 0;

   for (unsigned i = 0; i < normal_input_descriptors.size(); i++, input_descriptor_idx++)
   {
      input_descriptors[input_descriptor_idx] = normal_input_descriptors[i];
   }

   input_descriptors[input_descriptor_idx] = (struct retro_input_descriptor){ 0 };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
}
