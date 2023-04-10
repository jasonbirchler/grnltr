// GRNLTR test vehicle
//
// Check UpdateEncoder function for controls
//
#include <stdio.h>
#include <string.h>

#ifdef TARGET_POD
#include "pod.h"
#endif

#ifdef TARGET_BLUEMCHEN
#include "bluemchen.h"
#endif

#include "fatfs.h"
#include "led_colours.h"
#include "util/wav_format.h"
#include "Effects/decimator.h"
#include "grain.h"
#include "params.h"
#include "windows.h"
#include "granulator.h"
#include "MidiMsgHandler.h"
#include "EventQueue.h"
#include "grnltr.h"
#include "status.h"

using namespace daisy;
using namespace daisysp;

static Granulator grnltr;
static Decimator crush_l;
static Decimator crush_r;
MidiMsgHandler<HW_TYPE> mmh;
EventQueue<QUEUE_LENGTH> eq;

float rect_env[GRAIN_ENV_SIZE];
float gauss_env[GRAIN_ENV_SIZE];
float hamming_env[GRAIN_ENV_SIZE];
float hann_env[GRAIN_ENV_SIZE];
float expo_env[GRAIN_ENV_SIZE];
float rexpo_env[GRAIN_ENV_SIZE];
float *grain_envs[] = {rect_env, gauss_env, hamming_env, hann_env, expo_env, rexpo_env};
size_t cur_grain_env = 2; 

// 64 MB of memory - how many 16bit samples can we fit in there?
int16_t DSY_SDRAM_BSS sm[(64 * 1024 * 1024) / sizeof(int16_t)];
size_t sm_size = sizeof(sm);

// put a record buffer at the start of memory
// It should be sizeof(int16_t) * sr * MAX_GRAIN_DUR * MAX_GRAIN_PITCH * 2 in length 
// cur_sm_bytes needs to be set in init now to catch the sample rate
// but done before SD file waveform reading
size_t cur_sm_bytes;
size_t live_rec_buf_len;

// Buffer for copying wav files to SDRAM
char buf[CP_BUF_SIZE];

WavFileInfo wav_file_names[MAX_WAVES];
uint8_t	    wav_file_count = 0;
size_t	    wav_start_pos[MAX_WAVES];
int8_t	    cur_wave = 0;
int8_t	    wavs_read = 0;

char	dir_names[MAX_DIRS][MAX_DIR_LENGTH];
char	cur_dir_name[MAX_DIR_LENGTH];
uint8_t	dir_count = 0;
int8_t	cur_dir = 0;

PagedParam  pitch_p, rate_p, crush_p, downsample_p, grain_duration_p, \
	    grain_density_p, scatter_dist_p, pitch_dist_p, sample_start_p, \
	    sample_end_p, pan_p, pan_dist_p;

int8_t cur_page = 0;

float sample_bpm = DEFAULT_BPM;

float sr;

SdmmcHandler   sd;
FatFSInterface fsi;
FIL            SDFile;

grnltr_params_t grnltr_params;

int cur_midi_channel = MIDI_CHANNEL;

int  ReadWavsFromDir(const char *dir_path);
void HandleMidiMessage();
void InitControls();

bool retrig = false;
bool gate = false;

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
  sample_t sample;

  grnltr.SetGrainPitch(grnltr_params.GrainPitch);
  grnltr.SetScanRate(grnltr_params.ScanRate);
  grnltr.SetGrainDuration(grnltr_params.GrainDur);
  grnltr.SetDensity(grnltr_params.GrainDens);
  grnltr.SetScatterDist(grnltr_params.ScatterDist);
  grnltr.SetPitchDist(grnltr_params.PitchDist);
  grnltr.SetSampleStart(grnltr_params.SampleStart);
  grnltr.SetSampleEnd(grnltr_params.SampleEnd);
  grnltr.SetPan(grnltr_params.Pan);
  grnltr.SetPanDist(grnltr_params.PanDist);
  crush_l.SetBitcrushFactor(grnltr_params.Crush);
  crush_l.SetDownsampleFactor(grnltr_params.DownSample);
  crush_r.SetBitcrushFactor(grnltr_params.Crush);
  crush_r.SetDownsampleFactor(grnltr_params.DownSample);

  //audio
  for(size_t i = 0; i < size; i++)
  {
    sample = grnltr.Process(f2s16(in[0][i]));
    sample.l = crush_l.Process(sample.l);
    sample.r = crush_r.Process(sample.r);
    out[0][i] = sample.l;
    out[1][i] = sample.r;
  }
}

// needed to make led pwm work so we can see what's happening
void grnltr_delay(uint32_t delay_ms) {
  uint32_t dly = 0;
  while (dly < delay_ms) {
  #ifdef TARGET_POD
    hw.UpdateLeds();
  #endif
    System::Delay(1);
    dly++;
  }
}

int ListDirs(const char *path)
{
  DIR dir;
  FILINFO fno;

  if(f_opendir(&dir, path) != FR_OK)
  {
      return -1;
  }
  while((f_readdir(&dir, &fno) == FR_OK) && (dir_count < MAX_DIRS)) {
    // Exit if NULL fname
    if(fno.fname[0] == 0)
        break;
    // check if its a directory.
    if(fno.fattrib & AM_DIR) {
      strcpy(&dir_names[dir_count][0], fno.fname);
#ifdef DEBUG_POD
      hw.seed.PrintLine("  %s", fno.fname);
#endif
      dir_count++;
    }
  }
  f_closedir(&dir);
  return 0;
}

int ReadWavsFromDir(const char *dir_path)
{
  DIR dir;
  FILINFO fno;
  char *fn;

  size_t bytesread;

  wav_file_count = 0;
  wavs_read = 0;

#ifdef DEBUG_POD
  hw.seed.PrintLine("Opening %s", cur_dir_name);
#endif

  // See /data/daisy/DaisyExamples-sm/libDaisy/src/hid/wavplayer.*
  // Open Dir and scan for files.
  if(f_opendir(&dir, dir_path) != FR_OK)
  {
      return -1;
  }
  while((f_readdir(&dir, &fno) == FR_OK) && (wav_file_count < MAX_WAVES)) {
    // Exit if NULL fname
    if(fno.fname[0] == 0)
        break;
    // Skip if its a directory or a hidden file.
    if(fno.fattrib & (AM_HID | AM_DIR))
        continue;
    // Now we'll check if its .wav and add to the list.
    fn = fno.fname;
    if(strstr(fn, ".wav") || strstr(fn, ".WAV"))
    {
      strcpy(wav_file_names[wav_file_count].name, dir_path);
      strcat(wav_file_names[wav_file_count].name, "/");
      strcat(wav_file_names[wav_file_count].name, fn);
#ifdef DEBUG_POD
      hw.seed.PrintLine("  %s : %s", fno.fname, wav_file_names[wav_file_count].name);
#endif
      wav_file_count++;
    }
  }
  f_closedir(&dir);

  cur_sm_bytes = sizeof(int16_t) * MAX_GRAIN_DUR * sr * MAX_GRAIN_PITCH * 2;
  live_rec_buf_len = cur_sm_bytes / sizeof(int16_t);
  cur_wave = 0;
  
  // Now we'll go through each file and load the WavInfo.
  for(size_t i = 0; i < wav_file_count; i++)
  {
    Status(READING_WAV);
    // Read the test file from the SD Card.
    if(f_open(&SDFile, wav_file_names[i].name, FA_READ) == FR_OK)
    {
      // TODO: Add checks here to ensure we can deal with the wav file correctly
      f_read(&SDFile, (void *)&wav_file_names[i].raw_data, sizeof(WAV_FormatTypeDef), &bytesread);
      size_t wav_size = wav_file_names[i].raw_data.SubCHunk2Size;
      if ((cur_sm_bytes + wav_size) > sm_size) break;
      size_t this_wav_start_pos = (cur_sm_bytes / sizeof(int16_t)) + 1;
      wav_start_pos[i] = this_wav_start_pos;

      do {
	f_read(&SDFile, (void *)buf, CP_BUF_SIZE, &bytesread);
	memcpy((void *)&sm[this_wav_start_pos], buf, bytesread);
      	cur_sm_bytes += bytesread;
	this_wav_start_pos = (cur_sm_bytes / sizeof(int16_t));
      } while (bytesread == CP_BUF_SIZE);

      f_close(&SDFile);
      wavs_read++;
    }
  }
  return 0;
}

void LoadNewDir() {
  strcpy(cur_dir_name, GRNLTR_PATH);
  strcat(cur_dir_name, "/");
  strcat(cur_dir_name, &dir_names[cur_dir][0]);
  if (ReadWavsFromDir(cur_dir_name) < 0) {
    Status(DIR_ERROR);

    for(;;) {
      grnltr_delay(1);
    }
  }

  if (wav_file_count == 0) {
    Status(NO_WAVS);

    for(;;) {
      grnltr_delay(1);
    }
  }

  if (wavs_read != wav_file_count) {
    Status(MISSING_WAV);
#ifdef DEBUG_POD
    hw.seed.PrintLine("Missing WAV? %d:%d", wavs_read, wav_file_count);
#endif

    grnltr_delay(1000);
  }
}
  

// MIDI Callback Functions
void RTStartCB()
{
  InitControls();
  grnltr.Reset( \
      &sm[wav_start_pos[cur_wave]], \
      wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
  grnltr.Dispatch(0);
}

void RTContCB()
{
  grnltr.ReStart();
}

void RTStopCB()
{
  grnltr.Stop();
#ifdef TARGET_POD
  hw.led2.Set(OFF);
#endif
}

void RTBeatCB()
{
#ifdef TARGET_POD
  hw.led2.Set(RED);
#endif
}

void RTHalfBeatCB()
{
#ifdef TARGET_POD
  hw.led2.Set(OFF);
#endif
}

void MidiCCHCB(uint8_t cc, uint8_t val)
{
  switch(cc)
  {
    case CC_SCAN:
      rate_p.MidiCCIn(val);
      break;
    case CC_GRAINPITCH:
      pitch_p.MidiCCIn(val);
      break;
    case CC_GRAINDUR:
      grain_duration_p.MidiCCIn(val);
      break;
    case CC_GRAINDENS:
      grain_density_p.MidiCCIn(val);
      break;
    case CC_SCATTERDIST:
      scatter_dist_p.MidiCCIn(val);
      break;
    case CC_PITCHDIST:
      pitch_dist_p.MidiCCIn(val);
      break;
    case CC_SAMPLESTART_MSB:
      sample_start_p.MidiCCIn(val);
      break;
    case CC_SAMPLEEND_MSB:
      sample_end_p.MidiCCIn(val);
      break;
    case CC_SAMPLESTART_LSB:
    {
      float cur_val = sample_start_p.CurVal();
      float lsb_val = ((val - 63) / (127.0f * 127.0f));
      sample_start_p.RawSet(cur_val + lsb_val);
      break;
    }
    case CC_SAMPLEEND_LSB:
    {
      float cur_val = sample_end_p.CurVal();
      float lsb_val = ((val - 63) / (127.0f * 127.0f));
      sample_end_p.RawSet(cur_val + lsb_val);
      break;
    }
    case CC_CRUSH:
      crush_p.MidiCCIn(val);
      break;
    case CC_DOWNSAMPLE:
      downsample_p.MidiCCIn(val);
      break;
    case CC_TOG_GREV:
      grnltr.ToggleGrainReverse();
      break;
    case CC_TOG_SREV:
      grnltr.ToggleScanReverse();
      break;
    case CC_TOG_SCATTER:
      grnltr.ToggleScatter();
      break;
    case CC_TOG_PITCH:
      grnltr.ToggleRandomPitch();
      break;
    case CC_TOG_FREEZE:
      grnltr.ToggleFreeze();
      break;
    case CC_TOG_LOOP:
      grnltr.ToggleSampleLoop();
      break;
    case CC_TOG_DENS:
      grnltr.ToggleRandomDensity();
      break;
    case CC_LIVE_REC:
      eq.push_event(eq.LIVE_REC, 0);
      break;
    case CC_LIVE_SAMP:
      eq.push_event(eq.LIVE_PLAY, 0);
      break;
    case CC_PAN:
      pan_p.MidiCCIn(val);
      break;
    case CC_PAN_DIST:
      pan_dist_p.MidiCCIn(val);
      break;
    case CC_TOG_RND_PAN:
      eq.push_event(eq.TOG_RND_PAN, 0);
      break;
    case CC_TOG_RETRIG:
      eq.push_event(eq.TOG_RETRIG, 0);
      break;
    case CC_TOG_GATE:
      eq.push_event(eq.TOG_GATE, 0);
      break;
    case CC_BPM:
      // 60 + CC 
      // Need some concept of bars or beats per sample
      break;
    default: break;
  }
}

void MidiPBHCB(int16_t val)
{
  pitch_p.MidiPBIn(val);
}

void MidiNOnHCB(uint8_t n, uint8_t vel) 
{ 
  int8_t next_wave;
  if ((n >= BASE_NOTE) && (n < (BASE_NOTE + wav_file_count))) {
    next_wave = n - BASE_NOTE;
    if (next_wave != cur_wave) {
      cur_wave = next_wave;
      grnltr.Stop();
      InitControls();
      grnltr.Reset( \
          &sm[wav_start_pos[cur_wave]], \
          wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
      grnltr.Dispatch(0);
    } else {
      if (retrig) {
	grnltr.ReStart();
      } else {
	grnltr.Start();
      }
    }
  }
}

void MidiNOffHCB(uint8_t n, uint8_t vel) 
{ 
  int8_t this_wave = n - BASE_NOTE;
  if (this_wave == cur_wave) {
    if (gate) {
      grnltr.Stop();
    }
  }
}

void process_events()
{
  EventQueue<QUEUE_LENGTH>::event_entry ev = eq.pull_event();
  switch(ev.ev) {
    case eq.PAGE_UP:
      cur_page++;
      if (cur_page >= NUM_PAGES) { cur_page = 0; }
      break;
    case eq.PAGE_DN:
      cur_page--;
      if (cur_page < 0) { cur_page += NUM_PAGES; }
      break;
    case eq.INCR_GRAIN_ENV:
      cur_grain_env++;
      if (cur_grain_env == NUM_GRAIN_ENVS) {
        cur_grain_env = 0;
      }
      grnltr.ChangeEnv(grain_envs[cur_grain_env]);
      break;
    case eq.RST_PITCH_SCAN:
      pitch_p.Lock(1.0);
      rate_p.Lock(1.0);
      mmh.ResetGotClock();
      break;
    case eq.TOG_GRAIN_REV:
      grnltr.ToggleGrainReverse();
      break;
    case eq.TOG_SCAN_REV:
      grnltr.ToggleScanReverse();
      break;
    case eq.TOG_SCAT:
      grnltr.ToggleScatter();
      break;
    case eq.TOG_FREEZE:
      grnltr.ToggleFreeze();
      break;
    case eq.TOG_RND_PITCH:
      grnltr.ToggleRandomPitch();
      break;
    case eq.TOG_RND_DENS:
      grnltr.ToggleRandomDensity();
      break;
    case eq.INCR_WAV:
      cur_wave++;
      if (cur_wave >= wav_file_count) cur_wave = 0;
      grnltr.Stop();
      InitControls();
      grnltr.Reset( \
          &sm[wav_start_pos[cur_wave]], \
          wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
      grnltr.Dispatch(0);
      break;
    case eq.TOG_LOOP:
      grnltr.ToggleSampleLoop();
      break;
    case eq.LIVE_REC:
      grnltr.Stop();
      InitControls();
      grnltr.Live( \
          &sm[0], \
          live_rec_buf_len);
      break;
    case eq.LIVE_PLAY:
      gate = false;
      retrig = false;
      grnltr.Stop();
      InitControls();
      grnltr.Reset( \
          &sm[0], \
          live_rec_buf_len);
      grnltr.Dispatch(0);
      break;
    case eq.INCR_MIDI:
      cur_midi_channel++;
      cur_midi_channel &= 15; //wrap around
      mmh.SetChannel(cur_midi_channel);
      break;
    case eq.NEXT_DIR:
      cur_dir = ev.id;
      grnltr.Stop();
      InitControls();
      LoadNewDir();
      grnltr.Reset( \
          &sm[wav_start_pos[cur_wave]], \
          wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
      grnltr.Dispatch(0);
      break;
    case eq.TOG_RND_PAN:
      grnltr.ToggleRandomPan();
      break;
    case eq.TOG_RETRIG:
      if (!grnltr.IsLive()) {
	retrig = !retrig;
      }
      break;
    case eq.TOG_GATE:
      if (!grnltr.IsLive()) {
	gate = !gate;
      }
      break;
    case eq.NONE:
    default:
      break;
  }
}

void InitControls()
{
  pitch_p.Init(           0,  DEFAULT_GRAIN_PITCH,	MIN_GRAIN_PITCH,  MAX_GRAIN_PITCH,  PARAM_THRESH);
  rate_p.Init(            0,  DEFAULT_SCAN_RATE,        MIN_SCAN_RATE,	  MAX_SCAN_RATE,    PARAM_THRESH);
  grain_duration_p.Init(  1,  DEFAULT_GRAIN_DUR,        MIN_GRAIN_DUR,    MAX_GRAIN_DUR,    PARAM_THRESH);
  grain_density_p.Init(   1,  sr/DEFAULT_GRAIN_DENS,  sr/MIN_GRAIN_DENS, sr/MAX_GRAIN_DENS, PARAM_THRESH);
  scatter_dist_p.Init(    2,  DEFAULT_SCATTER_DIST,	0.0f,   1.0f, PARAM_THRESH);
  pitch_dist_p.Init(      3,  DEFAULT_PITCH_DIST,       0.0f,   1.0f, PARAM_THRESH);
  sample_start_p.Init(    4,  0.0f,			0.0f,   1.0f, PARAM_THRESH);
  sample_end_p.Init(	  4,  1.0f,			0.0f,   1.0f, PARAM_THRESH);
  crush_p.Init(           5,  0.0f,                     0.0f,   1.0f, PARAM_THRESH);
  downsample_p.Init(      5,  0.0f,                     0.0f,   1.0f, PARAM_THRESH);
  pan_p.Init(		  6,  DEFAULT_PAN,              0.0f,   1.0f, PARAM_THRESH);
  pan_dist_p.Init(	  6,  DEFAULT_PAN_DIST,         0.0f,   1.0f, PARAM_THRESH);
}

int main(void)
{
  rectangular_window(rect_env, GRAIN_ENV_SIZE);
  gaussian_window(gauss_env, GRAIN_ENV_SIZE, 0.5);
  hamming_window(hamming_env, GRAIN_ENV_SIZE, EQUIRIPPLE_HAMMING_COEF);
  hann_window(hann_env, GRAIN_ENV_SIZE);
  expodec_window(expo_env, rexpo_env, GRAIN_ENV_SIZE, TAU);
  
  // Init hardware
  sr = hw_init();

#ifdef DEBUG_POD
  hw.seed.StartLog(true);
#endif

  Status(STARTUP);

  grnltr_delay(250);

  // Init SD Card
  SdmmcHandler::Config sd_cfg;
  sd_cfg.Defaults();
  sd.Init(sd_cfg);

  Status(FSI_INIT);

  grnltr_delay(250);

  // Links libdaisy i/o to fatfs driver.
  fsi.Init(FatFSInterface::Config::MEDIA_SD);

  Status(SD_MOUNT);

  grnltr_delay(250);
  
  // Mount SD Card
  if (f_mount(&fsi.GetSDFileSystem(), "/", 1) != FR_OK) {
    Status(MOUNT_ERROR);

    for(;;) {
      grnltr_delay(1);
    }
  }

  Status(LIST_DIRS);

  grnltr_delay(250);

  if (ListDirs(GRNLTR_PATH) < 0) {
    Status(NO_GRNLTR_DIR);

    for(;;) {
      grnltr_delay(1);
    }
  }

  Status(OK);

  LoadNewDir();

  Status(GRNLTR_INIT);

  grnltr.Init(sr, \
      &sm[wav_start_pos[cur_wave]], \
      wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t), \
      grain_envs[cur_grain_env], \
      GRAIN_ENV_SIZE);
  grnltr.Dispatch(0);
  
  crush_l.Init();
  crush_l.SetDownsampleFactor(0.0f);
  crush_r.Init();
  crush_r.SetDownsampleFactor(0.0f);

  InitControls();

  // Setup Midi and Callbacks
  mmh.SetChannel(cur_midi_channel);
  mmh.SetHWHandle(&hw);

  mmh.SetSRTCB(mmh.Start,     RTStartCB);
  mmh.SetSRTCB(mmh.Continue,  RTContCB);
  mmh.SetSRTCB(mmh.Stop,      RTStopCB);
  mmh.SetSRTCB(mmh.Beat,      RTBeatCB);
  mmh.SetSRTCB(mmh.HalfBeat,  RTHalfBeatCB);
  mmh.SetMNOnHCB(MidiNOnHCB);
  mmh.SetMNOffHCB(MidiNOffHCB);
  mmh.SetMCCHCB(MidiCCHCB);
  mmh.SetMPBHCB(MidiPBHCB);

  grnltr_delay(250);

  Status(OK);
  
  // GO!
  hw_start(AudioCallback);
  hw.ProcessDigitalControls();
  while (eq.has_event()) {
    eq.pull_event();
  }
  UpdateUI(cur_page);

  int blink_mask = 15; 
  int blink_cnt = 0;
  uint32_t now = hw.seed.system.GetNow();
  uint32_t loop_dly = now;

  bool led_state = true;
  for(;;)
  {
    mmh.Process();

    hw.ProcessDigitalControls();
    UpdateEncoder(cur_page);

    #ifdef TARGET_POD
    UpdateButtons(cur_page);
    #endif

    while (eq.has_event()) {
      process_events();
    }
    UpdateUI(cur_page);

    // counter here so we don't do this too often if it's called repeatedly in the main loop
    now = hw.seed.system.GetNow();
    if ((now - loop_dly > MAIN_LOOP_DLY) || (now < loop_dly)) {
      Controls(cur_page);

      blink_cnt &= blink_mask;
      if (blink_cnt == 0) {
        hw.seed.SetLed(led_state);
        led_state = !led_state;
      }
      blink_cnt++;
      loop_dly = now;
    } 
  }
}
