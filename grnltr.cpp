// GRNLTR test vehicle
//
// Check UpdateEncoder function for controls
//
#include <stdio.h>
#include <string.h>
#include "daisy_pod.h"
#include "fatfs.h"
#include "led_colours.h"
#include "util/wav_format.h"
#include "Effects/decimator.h"
#include "PagedParam.h"
#include "windows.h"
#include "granulator.h"
#include "MidiMsgHandler.h"

using namespace daisy;
using namespace daisysp;

static Granulator grnltr;
static Decimator crush;
static DaisyPod hw;
static Parameter knob1, knob2;
static MidiMsgHandler mmh;

#define GRAIN_ENV_SIZE 1024
#define NUM_GRAIN_ENVS 6
float rect_env[GRAIN_ENV_SIZE];
float gauss_env[GRAIN_ENV_SIZE];
float hamming_env[GRAIN_ENV_SIZE];
float hann_env[GRAIN_ENV_SIZE];
float expo_env[GRAIN_ENV_SIZE];
float rexpo_env[GRAIN_ENV_SIZE];
float *grain_envs[] = {rect_env, gauss_env, hamming_env, hann_env, expo_env, rexpo_env};
size_t cur_grain_env = 2; 

#define MAX_WAVES 16

/**< Maximum LFN (set to same in FatFs (ffconf.h) */
#define WAV_FILENAME_MAX 256 

// 64 MB of memory - how many 16bit samples can we fit in there?
static int16_t DSY_SDRAM_BSS sm[(64 * 1024 * 1024) / sizeof(int16_t)];
size_t sm_size = sizeof(sm);
//size_t cur_sm_bytes = 0;

// put a record buffer at the start of memory
// It should be sizeof(int16_t) * sr * MAX_GRAIN_DUR * MAX_GRAIN_PITCH * 2 in length 
// cur_sm_bytes needs to be set in init now to catch the sample rate
// but done before SD file waveform reading
size_t cur_sm_bytes;
size_t live_rec_buf_len;

// Buffer for copying wav files to SDRAM
#define BUF_SIZE 8192
char buf[BUF_SIZE];

WavFileInfo wav_file_names[MAX_WAVES];
uint8_t	    wav_file_count = 0;
size_t	    wav_start_pos[MAX_WAVES];

#define PARAM_THRESH 0.01f
static PagedParam pitch_p, rate_p, crush_p, downsample_p, grain_duration_p, \
		  grain_density_p, scatter_dist_p, pitch_dist_p, sample_start_p, sample_end_p;

#define NUM_PAGES 6
int8_t cur_page = 0;
int8_t cur_wave = 0;

#define DEFAULT_BPM 120.0f
float sample_bpm = DEFAULT_BPM;

float sr;

SdmmcHandler   sd;
FatFSInterface fsi;
FIL            SDFile;

struct {
  float	  GrainPitch;
  float   ScanRate;
  float   GrainDur;
  int32_t GrainDens;
  float	  ScatterDist;
  float	  PitchDist;
  float	  SampleStart;
  float	  SampleEnd;
  float	  Crush;
  float	  DownSample;
} grnltr_params;

#define MIDI_CHANNEL	    0 // todo - make this settable somehow. Daisy starts counting MIDI channels from 0
#define CC_SCAN	       	    1 // MOD wheel controls Scan Rate
#define CC_GRAINPITCH	    3 // CC3 and PitchBend control GrainPitch
#define CC_GRAINDUR	    9 
#define	CC_GRAINDENS	    14  
#define CC_TOG_SCATTER	    15
#define CC_SCATTERDIST	    20
#define CC_TOG_PITCH	    21
#define CC_PITCHDIST	    22
#define CC_SAMPLESTART_MSB  12
#define CC_SAMPLESTART_LSB  44
#define CC_SAMPLEEND_MSB    13
#define CC_SAMPLEEND_LSB    45
#define	CC_CRUSH	    23
#define	CC_DOWNSAMPLE	    24
#define CC_TOG_GREV	    25
#define CC_TOG_SREV	    26
#define CC_TOG_FREEZE	    27
#define CC_TOG_LOOP	    28
#define CC_TOG_DENS	    29
#define	CC_BPM		    30
//C3
#define BASE_NOTE	    60

void UpdateEncoder();
void UpdateButtons();
int  ReadWavsFromDir(const char *dir_path);
void HandleMidiMessage();
void InitControls();
inline void Controls();

void UpdateEncoder()
{
  cur_page = (int8_t)(cur_page + hw.encoder.Increment());
  if (cur_page >= NUM_PAGES) { cur_page = 0; }
  if (cur_page < 0) { cur_page += NUM_PAGES; }
  switch(cur_page)
  {
    case 0:
      hw.led1.Set(RED);
      /*
       * k1 = Grain Pitch
       * k2 = Scan Rate (disabled in live mode)
       * b1 = cycle env type
       * b2 = Reset Grain Pitch and Scan Rate
       */
      break;
    case 1:
      hw.led1.Set(ORANGE);
      /*
       * k1 = Grain Duration
       * k2 = Grain Density
       * b1 = Grain Reverse
       * b2 = Scan Reverse (disabled in live mode)
       */
      break;
    case 2:
      hw.led1.Set(YELLOW);
      /*
       * k1 = Scatter Distance
       * b1 = Toggle Scatter
       * b2 = Toggle Freeze
       */
      break;
    case 3:
      hw.led1.Set(GREEN);
      /*
       * k1 = Pitch Distance
       * b1 = Toggle Random Pitch
       * b2 = Toggle Random Density
       */
      break;
    case 4:
      hw.led1.Set(BLUE);
      /*
       * NOTE: led flashes blue during start up to indicate file reading
       * k1 = Sample Start
       * k2 = Sample End
       * b1 = Cycle Wave
       * b2 = Toggle Wave Loop
       */
      break;
    case 5:
      hw.led1.Set(PURPLE);
      /*
       * k1 = Bit Crush
       * k2 = Downsample
       * b1 = Live Mode
       * b2 = Sample Mode
       */
      break;
    default:
      break;
  }
  hw.UpdateLeds();
}

void UpdateButtons()
{
  switch(cur_page)
  {
    case 0:
      if(hw.button1.RisingEdge()) {
	cur_grain_env++;
	if (cur_grain_env == NUM_GRAIN_ENVS) {
	  cur_grain_env = 0;
	}
	grnltr.ChangeEnv(grain_envs[cur_grain_env]);
      }
      if(hw.button2.RisingEdge()) {
	pitch_p.Lock(1.0);
  	rate_p.Lock(1.0);
	//got_clock = 0;
	mmh.ResetGotClock();
      }
      break;
    case 1:
      if(hw.button1.RisingEdge()) {
	grnltr.ToggleGrainReverse();
      }
      if(hw.button2.RisingEdge()) {
	grnltr.ToggleScanReverse();
      }
      break;
    case 2:
      if(hw.button1.RisingEdge()) {
	grnltr.ToggleScatter();
      }
      if(hw.button2.RisingEdge()) {
	grnltr.ToggleFreeze();
      }
      break;
    case 3:
      if(hw.button1.RisingEdge()) {
	grnltr.ToggleRandomPitch();
      }
      if(hw.button2.RisingEdge()) {
	grnltr.ToggleRandomDensity();
      }
      break;
    case 4:
      if(hw.button1.RisingEdge()) {
	cur_wave++;
	if (cur_wave >= wav_file_count) cur_wave = 0;
	grnltr.Stop();
	InitControls();
	grnltr.Reset( \
	    &sm[wav_start_pos[cur_wave]], \
	    wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
    	grnltr.Dispatch(0);
      }
      if(hw.button2.RisingEdge()) {
	grnltr.ToggleSampleLoop();
      }
      break;
    case 5:
      if(hw.button1.RisingEdge()) {
	grnltr.Stop();
	InitControls();
	grnltr.Live( \
	    &sm[0], \
	    live_rec_buf_len);
      }
      if(hw.button2.RisingEdge()) {
	grnltr.Stop();
	InitControls();
/*
 *	grnltr.Reset( \
 *	    &sm[wav_start_pos[cur_wave]], \
 *	    wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
 */
	grnltr.Reset( \
	    &sm[0], \
	    live_rec_buf_len);
    	grnltr.Dispatch(0);
      }
      break;
    default:
      break;
  }
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
  float sample;

  grnltr.SetGrainPitch(grnltr_params.GrainPitch);
  grnltr.SetScanRate(grnltr_params.ScanRate);
  grnltr.SetGrainDuration(grnltr_params.GrainDur);
  grnltr.SetDensity(grnltr_params.GrainDens);
  grnltr.SetScatterDist(grnltr_params.ScatterDist);
  grnltr.SetPitchDist(grnltr_params.PitchDist);
  grnltr.SetSampleStart(grnltr_params.SampleStart);
  grnltr.SetSampleEnd(grnltr_params.SampleEnd);
  crush.SetBitcrushFactor(grnltr_params.Crush);
  crush.SetDownsampleFactor(grnltr_params.DownSample);

  //audio
  for(size_t i = 0; i < size; i++)
  {
    sample = grnltr.Process(f2s16(in[0][i]));
    sample = crush.Process(sample);
    out[0][i] = sample;
    out[1][i] = sample;
  }
}

int ReadWavsFromDir(const char *dir_path)
{
  DIR dir;
  FILINFO fno;
  char *  fn;

  size_t bytesread;

  hw.led1.Set(GREEN);
  hw.UpdateLeds();

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
      strcat(wav_file_names[wav_file_count].name, fn);
      wav_file_count++;
    }
  }
  f_closedir(&dir);
  
  // Now we'll go through each file and load the WavInfo.
  for(size_t i = 0; i < wav_file_count; i++)
  {
    hw.led1.Set(BLUE);
    hw.UpdateLeds();
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
	f_read(&SDFile, (void *)buf, BUF_SIZE, &bytesread);
	memcpy((void *)&sm[this_wav_start_pos], buf, bytesread);
      	cur_sm_bytes += bytesread;
	this_wav_start_pos = (cur_sm_bytes / sizeof(int16_t));
      } while (bytesread == BUF_SIZE);

      f_close(&SDFile);
    }
    hw.led1.Set(OFF);
    hw.UpdateLeds();
    System::Delay(250);
  }
  return 0;
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
}

void RTBeatCB()
{
  hw.led2.Set(RED);
}

void RTHalfBeatCB()
{
  hw.led2.Set(OFF);
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

void MidiNOHCB(uint8_t n, uint8_t vel) 
{ 
  if ((n >= BASE_NOTE) && (n < (BASE_NOTE + wav_file_count))) {
    cur_wave = n - BASE_NOTE;
    grnltr.Stop();
    InitControls();
    grnltr.Reset( \
        &sm[wav_start_pos[cur_wave]], \
        wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t));
    grnltr.Dispatch(0);
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
}

inline void Controls()
{
  float k1, k2;

  k1 = knob1.Process();
  k2 = knob2.Process();
  
  grnltr_params.GrainPitch =   pitch_p.Process(k1, cur_page);
  if (mmh.GotClock()) {
    rate_p.Set((mmh.GetBPM() / sample_bpm));
  }
  grnltr_params.ScanRate =     rate_p.Process(k2, cur_page);
  grnltr_params.GrainDur =     grain_duration_p.Process(k1, cur_page);
  grnltr_params.GrainDens =    (int32_t)grain_density_p.Process(k2, cur_page);
  grnltr_params.ScatterDist =  scatter_dist_p.Process(k1, cur_page);
  grnltr_params.PitchDist =    pitch_dist_p.Process(k1, cur_page);
  grnltr_params.SampleStart =  sample_start_p.Process(k1, cur_page);
  grnltr_params.SampleEnd =    sample_end_p.Process(k2, cur_page);
  grnltr_params.Crush =        crush_p.Process(k1, cur_page);
  grnltr_params.DownSample =   downsample_p.Process(k2, cur_page);
}

int main(void)
{
  rectangular_window(rect_env, GRAIN_ENV_SIZE);
  gaussian_window(gauss_env, GRAIN_ENV_SIZE, 0.5);
  hamming_window(hamming_env, GRAIN_ENV_SIZE, EQUIRIPPLE_HAMMING_COEF);
  hann_window(hann_env, GRAIN_ENV_SIZE);
  expodec_window(expo_env, rexpo_env, GRAIN_ENV_SIZE, TAU);
  
  // Init hardware
  hw.Init();
  sr = hw.AudioSampleRate();
  cur_sm_bytes = sizeof(int16_t) * MAX_GRAIN_DUR * sr * MAX_GRAIN_PITCH * 2;
  live_rec_buf_len = cur_sm_bytes / sizeof(int16_t);
  
  hw.led1.Set(RED);
  hw.UpdateLeds();

  // Init SD Card
  SdmmcHandler::Config sd_cfg;
  sd_cfg.Defaults();
  sd.Init(sd_cfg);
  
  System::Delay(250);
  
  hw.led1.Set(ORANGE);
  hw.UpdateLeds();
  
  // Links libdaisy i/o to fatfs driver.
  fsi.Init(FatFSInterface::Config::MEDIA_SD);
  
  System::Delay(250);
  
  hw.led1.Set(YELLOW);
  hw.UpdateLeds();
  
  // Mount SD Card
  f_mount(&fsi.GetSDFileSystem(), "/", 1);
  
  System::Delay(250);
  
  ReadWavsFromDir("/");
  
  // unmount
  f_mount(0, "/", 0);
  
  System::Delay(250);
  
  hw.led1.Set(PURPLE);
  hw.UpdateLeds();
  
  grnltr.Init(sr, \
      &sm[wav_start_pos[cur_wave]], \
      wav_file_names[cur_wave].raw_data.SubCHunk2Size / sizeof(int16_t), \
      grain_envs[cur_grain_env], \
      GRAIN_ENV_SIZE);
  grnltr.Dispatch(0);
  
  crush.Init();
  crush.SetDownsampleFactor(0.0f);

  knob1.Init(hw.knob1, 0.0f, 1.0f, knob1.LINEAR);
  knob2.Init(hw.knob2, 0.0f, 1.0f, knob2.LINEAR);
  
  InitControls();

  // Setup Midi and Callbacks
  mmh.SetChannel(MIDI_CHANNEL);
  mmh.SetHWHandle(&hw);

  mmh.SetSRTCB(MidiMsgHandler::Start,	  RTStartCB);
  mmh.SetSRTCB(MidiMsgHandler::Continue,  RTContCB);
  mmh.SetSRTCB(MidiMsgHandler::Stop,	  RTStopCB);
  mmh.SetSRTCB(MidiMsgHandler::Beat, 	  RTBeatCB);
  mmh.SetSRTCB(MidiMsgHandler::HalfBeat,  RTHalfBeatCB);
  mmh.SetMNOHCB(MidiNOHCB);
  mmh.SetMCCHCB(MidiCCHCB);
  mmh.SetMPBHCB(MidiPBHCB);
  
  // GO!
  hw.StartAdc();
  hw.StartAudio(AudioCallback);
  hw.midi.StartReceive();

  int blink_mask = 511; 
  int blink_cnt = 0;
  int dly_mask = 255;
  int dly_cnt = 0;

  bool led_state = true;
  for(;;)
  {
    mmh.Process();

    hw.ProcessDigitalControls();
    UpdateButtons();
    UpdateEncoder();

    // counter here so we don't do this too often if it's called repeatedly in the main loop
    dly_cnt &= dly_mask;
    if (dly_cnt == 0) {
      Controls();

      blink_cnt &= blink_mask;
      if (blink_cnt == 0) {
        hw.seed.SetLed(led_state);
        led_state = !led_state;
      }
      blink_cnt++;
    } 
    dly_cnt++;
  }
}
