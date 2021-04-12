/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

                             __         ___    ___    __  __
                            /__\/\ /\  / __\  / __\  /__\/__\
                           / \// / \ \/__\// /__\// /_\ / \//
                          / _  \ \_/ / \/  \/ \/  \//__/ _  \
                          \/ \_/\___/\_____/\_____/\__/\/ \_/

                              ___  __
                             /   \/__\/\ /\  /\/\
                            / /\ / \// / \ \/    \
                           / /_// _  \ \_/ / /\/\ \
                          /___,'\/ \_/\___/\/    \/  V0.2

  XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
  A programmable midi Drumset with midi effects
  based on the Teensy 3.2

  Thanks to the creators of the following bits:

  Piezo to Midi code is based on oddson's multi-pin extension of Paul Stoffregen's -
  Piezo trigger code (teensy examples)
  The curveApply function is basically a copy of Rob Tillaart's multiMap function
  Menu architecture is made with LiquidMenu Library by Vasil Kalchev
  Save settings to SD card via SdFat library (version 1.1.4) by Bill Greiman

  this code is in the public domain.
  XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

#include <SPI.h>
#include <Wire.h>
#include <LiquidMenu.h>
#include <LiquidCrystal_I2C.h>
#include <Encoder.h>
#include "SdFat.h"

#define SD_CS_PIN 10

#define encoder_A_pin       8
#define encoder_B_pin       9
#define encoder_button_pin  7    // use internal pullup
#define buzzer_pin          6
#define led_pin             5

#define button_long_press    300   // ms
#define button_short_press   60   // ms

#define pc_channel 10            //channel on which program change is recieved 

LiquidCrystal_I2C lcd(0x27, 40, 2);
Encoder ENCODER(encoder_A_pin, encoder_B_pin);

SdFat sd;
File dataFile;

/*XXXXXXXXXX Drum Logic Vars XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

const int drumPINS = 8;     // number of drum signals incoming
const int expPin = A0;
const int susPin = 2;
const int slrPin = A11;

const int drumkits = 8;  // number of

const int analogPin[drumPINS] = {A1, A2, A3, A6, A7, A8, A9, A10}; //array of analog PINs
const int thresholdMin = 60;  // minimum reading, avoid noise and false starts
const int peakTrackMillis = 12;
const int aftershockMillis = 15; // aftershocks & vibration reject

int state[drumPINS];  // 0=idle, 1=looking for peak, 2=ignore aftershocks
int peak[drumPINS];   // remember the highest reading
int piezo[drumPINS];
elapsedMillis msec[drumPINS]; // timers to end states 1 and 2

// velocity "curves" for interpolation

uint8_t curve0[] = {0, 16, 32, 48, 64, 80, 96, 112, 127}; //linear
uint8_t curve1[] = {0, 8, 16, 24, 32, 40, 48, 80, 127};  //almost exp
uint8_t curve2[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max
uint8_t curve3[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max
uint8_t curve4[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max
uint8_t curve5[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max

// counter for sequence mode
int counter[drumPINS];

// last played note on pad
int lastPlayed[drumPINS];

bool sus = false;
bool retrigger[drumPINS];

// expression Pedal
int solar;
int expr;
int lastExpr[drumPINS];

/*XXXXXXXXXXXXXX definition of  padSettings XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

struct pad_settings {

  uint8_t channel;      // midi Channel to send to
  uint8_t velCurve;     // ID of table containing the curve
  uint8_t fixVel;       // value for fixed velocity
  uint8_t padNNs[4];    // default, alternative, velTrig1, velTrig2
  uint8_t velTres[2];   // treshold for velocity trigs
  uint8_t padSeq[16];   // NNs of Note Sequence
  uint8_t seqMode;      // Sequence mode [off,on,random]
  uint8_t susMode;      // susPedal mode [altNN, retrigFX, holdSeqCounter, restartSeq]
  uint8_t velCC[4];     // velocity modulation destinations
  uint8_t expCC[4];     // expression modulation destinations
  uint8_t velMin[4];   // min & max for each destination
  uint8_t velMax[4];   // min & max for each destination
  uint8_t expMin[4];   // min & max for each destination
  uint8_t expMax[4];   // min & max for each destination
  int16_t retrigFX[4];   // [speed,repeats,pitch,damp]
  bool latching;        // noteOff waits for next stroke

} ;

// an array of settings structs for each of 8 pads in a kit (upload and read from/to sd)
struct pad_settings padSettings[drumPINS];

/*XXXXXXXXXXXXXX VARIABLES FOR MENU XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

bool inmenu = false;

unsigned long  button_press_time = millis();
unsigned long  buzzer_press_time = millis();
bool  enc_button_prev = HIGH;
bool  buzzer_prev = HIGH;
bool  editmode = false;

//displaySettings Vars , those that need special strings
uint8_t selected_pad;

// per Pad
char* seqM[drumPINS];
char* susM[drumPINS];
char* latch[drumPINS];

enum FunctionTypes {
  incr = 1,
  decr = 2,
};

uint8_t current_line_count = 4;

uint8_t save_kit_nr = 0;
uint8_t load_kit_nr = 0;

///////////////////////////////////////////////////////////////////////////////////
// This strings will be used by the second getter function.
const char fixed[] = "fix";
const char user1[] = "USER1";
const char user2[] =  "USER2";
const char explin[] = "EXPLIN";
const char mostexp[] = "MostEXP";
const char linear[] = "LINEAR";

/*XXXXXXXXXXXXXXX GETTER FUNCTIONS XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
uint8_t get_channel() {
  return padSettings[selected_pad].channel;
}
uint8_t get_NN() {
  return padSettings[selected_pad].padNNs[0];
}
uint8_t get_altNN() {
  return padSettings[selected_pad].padNNs[1];
}
uint8_t get_velTrig1() {
  return padSettings[selected_pad].padNNs[2];
}
uint8_t get_velTrig2() {
  return padSettings[selected_pad].padNNs[3];
}

const char* getCurve() {

  if ( padSettings[selected_pad].velCurve == 5 ) {
    return fixed;
  }
  else if ( padSettings[selected_pad].velCurve == 4) {
    return user1;
  }
  else if ( padSettings[selected_pad].velCurve == 3) {
    return user2;
  }
  else if ( padSettings[selected_pad].velCurve == 2) {
    return explin;
  }
  else if ( padSettings[selected_pad].velCurve == 1) {
    return mostexp;
  }
  else if ( padSettings[selected_pad].velCurve == 0) {
    return linear;
  }
}

uint8_t get_fixVel() {
  return padSettings[selected_pad].fixVel;
}
uint8_t get_velCurve() {
  return padSettings[selected_pad].velCurve;
}
uint8_t get_velTres1() {
  return padSettings[selected_pad].velTres[0];
}
uint8_t get_velTres2() {
  return padSettings[selected_pad].velTres[1];
}

uint8_t get_padSeq1() {
  return padSettings[selected_pad].padSeq[0];
}
uint8_t get_padSeq2() {
  return padSettings[selected_pad].padSeq[1];
}
uint8_t get_padSeq3() {
  return padSettings[selected_pad].padSeq[2];
}
uint8_t get_padSeq4() {
  return padSettings[selected_pad].padSeq[3];
}
uint8_t get_padSeq5() {
  return padSettings[selected_pad].padSeq[4];
}
uint8_t get_padSeq6() {
  return padSettings[selected_pad].padSeq[5];
}
uint8_t get_padSeq7() {
  return padSettings[selected_pad].padSeq[6];
}
uint8_t get_padSeq8() {
  return padSettings[selected_pad].padSeq[7];
}
uint8_t get_padSeq9() {
  return padSettings[selected_pad].padSeq[8];
}
uint8_t get_padSeq10() {
  return padSettings[selected_pad].padSeq[9];
}
uint8_t get_padSeq11() {
  return padSettings[selected_pad].padSeq[10];
}
uint8_t get_padSeq12() {
  return padSettings[selected_pad].padSeq[11];
}
uint8_t get_padSeq13() {
  return padSettings[selected_pad].padSeq[12];
}
uint8_t get_padSeq14() {
  return padSettings[selected_pad].padSeq[13];
}
uint8_t get_padSeq15() {
  return padSettings[selected_pad].padSeq[14];
}
uint8_t get_padSeq16() {
  return padSettings[selected_pad].padSeq[15];
}

uint8_t get_seqMode() {
  return padSettings[selected_pad].seqMode;
}
uint8_t get_susMode() {
  return padSettings[selected_pad].susMode;
}
uint8_t get_velCC1() {
  return padSettings[selected_pad].velCC[0];
}
uint8_t get_velCC2() {
  return padSettings[selected_pad].velCC[1];
}
uint8_t get_velCC3() {
  return padSettings[selected_pad].velCC[2];
}
uint8_t get_velCC4() {
  return padSettings[selected_pad].velCC[3];
}
uint8_t get_expCC1() {
  return padSettings[selected_pad].expCC[0];
}
uint8_t get_expCC2() {
  return padSettings[selected_pad].expCC[1];
}
uint8_t get_expCC3() {
  return padSettings[selected_pad].expCC[2];
}
uint8_t get_expCC4() {
  return padSettings[selected_pad].expCC[3];
}

uint8_t get_velMin1() {
  return padSettings[selected_pad].velMin[0];
}
uint8_t get_velMin2() {
  return padSettings[selected_pad].velMin[1];
}
uint8_t get_velMin3() {
  return padSettings[selected_pad].velMin[2];
}
uint8_t get_velMin4() {
  return padSettings[selected_pad].velMin[3];
}

uint8_t get_velMax1() {
  return padSettings[selected_pad].velMax[0];
}
uint8_t get_velMax2() {
  return padSettings[selected_pad].velMax[1];
}
uint8_t get_velMax3() {
  return padSettings[selected_pad].velMax[2];
}
uint8_t get_velMax4() {
  return padSettings[selected_pad].velMax[3];
}


uint8_t get_expMin1() {
  return padSettings[selected_pad].expMin[0];
}
uint8_t get_expMin2() {
  return padSettings[selected_pad].expMin[1];
}
uint8_t get_expMin3() {
  return padSettings[selected_pad].expMin[2];
}
uint8_t get_expMin4() {
  return padSettings[selected_pad].expMin[3];
}

uint8_t get_expMax1() {
  return padSettings[selected_pad].expMax[0];
}
uint8_t get_expMax2() {
  return padSettings[selected_pad].expMax[1];
}
uint8_t get_expMax3() {
  return padSettings[selected_pad].expMax[2];
}
uint8_t get_expMax4() {
  return padSettings[selected_pad].expMax[3];
}


uint16_t get_speed() {
  return padSettings[selected_pad].retrigFX[0];
}
uint16_t get_rpts() {
  return padSettings[selected_pad].retrigFX[1];
}
uint16_t get_pitch() {
  return padSettings[selected_pad].retrigFX[2];
}
uint16_t get_damp() {
  return padSettings[selected_pad].retrigFX[3];
}

uint8_t get_latch() {
  return padSettings[selected_pad].latching;
}

//////////HOME MENU/////////////////////////////////////////////////////////////////
LiquidLine drumkit_line(1, 0, "Current Kit");
LiquidLine drumkit_nr_line( 1, 1, "Kit", load_kit_nr);
LiquidLine sel_pad_Line(21, 0, "Selected Pad");
LiquidLine sel_nr_Line( 21, 1, "Pad", selected_pad);
LiquidScreen home_screen(drumkit_line, drumkit_nr_line, sel_pad_Line, sel_nr_Line);

LiquidMenu home_menu(lcd, home_screen);

//////////MAIN MENU/////////////////////////////////////////////////////////////////
LiquidLine pad_dest_line(1, 0, "PAD DESTINATION");
LiquidLine vel_trg_line( 1, 1, "VELOCITY TRIGGER");
LiquidLine CC_dest_line(21, 0, "CC DESTINATIONS");
LiquidLine CC_rng_line( 21, 1, "CC LIMITS");
LiquidScreen main1_screen(pad_dest_line, vel_trg_line, CC_dest_line, CC_rng_line);

LiquidLine seq_line(     1, 0, "PAD SEQUENCE");
LiquidLine seq_mod_line( 1, 1, "SEQUENCE MODES");
LiquidLine retrFX_line( 21, 0, "RETRIGGER FX");
LiquidLine kit_line(    21, 1, "DRUMKIT");
LiquidScreen main2_screen(seq_line, seq_mod_line, retrFX_line, kit_line);

LiquidMenu main_menu(lcd, main1_screen, main2_screen);

//////////PAD DESTINATION///////////////////////////////////////////////////////////
LiquidLine padCH_line(1,  0, "padCH: ",  get_channel);
LiquidLine padNN_line(21, 0, "padNN: ",  get_NN);
LiquidLine altNN_line(1,  1, "altNN: ",  get_altNN);
LiquidLine curve_line(21, 1, "curve: ",  getCurve, " ", get_fixVel);
LiquidScreen NN_screen(padCH_line, padNN_line, altNN_line, curve_line);

LiquidMenu pad_dest_menu(lcd, NN_screen);

///////////VELOCITY TRIGGER/////////////////////////////////////////////////////////
LiquidLine vTRG1_line(1,  0, "vTRG1:",  get_velTrig1);
LiquidLine vTRG2_line(21, 0, "vTRG2:",  get_velTrig2);
LiquidLine tr1TH_line(1,  1, "tr1TH:",  get_velTres1);
LiquidLine tr2TH_line(21, 1, "tr2TH:",  get_velTres2);
LiquidScreen vTRG_screen(vTRG1_line, vTRG2_line, tr1TH_line, tr2TH_line);

LiquidMenu vel_trig_menu(lcd, vTRG_screen);

///////////CC DESTINATIONS//////////////////////////////////////////////////////////
LiquidLine vCC1_line(1,  0, "vCC1:", get_velCC1);
LiquidLine vCC2_line(1,  1, "vCC2:", get_velCC2);
LiquidLine vCC3_line(11, 0, "vCC3:", get_velCC3);
LiquidLine vCC4_line(11, 1, "vCC4:", get_velCC4);
LiquidLine xCC1_line(21, 0, "xCC1:", get_expCC1);
LiquidLine xCC2_line(21, 1, "xCC2:", get_expCC2);
LiquidLine xCC3_line(31, 0, "xCC3:", get_expCC3);
LiquidLine xCC4_line(31, 1, "xCC4:", get_expCC4);
LiquidScreen CC_screen;

LiquidMenu CC_dest_menu(lcd, CC_screen);

///////////CC LIMITS////////////////////////////////////////////////////////////////
LiquidLine v1MIN_line(1,  0, "v" , get_velMin1);
LiquidLine v1MAX_line(1,  1, "v" , get_velMax1);
LiquidLine v2MIN_line(6,  0, "v" , get_velMin2);
LiquidLine v2MAX_line(6,  1, "v" , get_velMax2);
LiquidLine v3MIN_line(11, 0, "v" , get_velMin3);
LiquidLine v3MAX_line(11, 1, "v" , get_velMax3);
LiquidLine v4MIN_line(16, 0, "v" , get_velMin4);
LiquidLine v4MAX_line(16, 1, "v" , get_velMax4);
LiquidLine x1MIN_line(21, 0, "x" , get_expMin1);
LiquidLine x1MAX_line(21, 1, "x" , get_expMax1);
LiquidLine x2MIN_line(26, 0, "x" , get_expMin2);
LiquidLine x2MAX_line(26, 1, "x" , get_expMax2);
LiquidLine x3MIN_line(31, 0, "x" , get_expMin3);
LiquidLine x3MAX_line(31, 1, "x" , get_expMax3);
LiquidLine x4MIN_line(36, 0, "x" , get_expMin4);
LiquidLine x4MAX_line(36, 1, "x" , get_expMax4);
LiquidScreen RNG_screen;

LiquidMenu CC_range_menu(lcd, RNG_screen);

//////////PAD SEQUENCE//////////////////////////////////////////////////////////////
LiquidLine note1_line(  1, 0, get_padSeq1);
LiquidLine note2_line(  6, 0, get_padSeq2);
LiquidLine note3_line( 11, 0, get_padSeq3);
LiquidLine note4_line( 16, 0, get_padSeq4);
LiquidLine note5_line( 21, 0, get_padSeq5);
LiquidLine note6_line( 26, 0, get_padSeq6);
LiquidLine note7_line( 31, 0, get_padSeq7);
LiquidLine note8_line( 36, 0, get_padSeq8);
LiquidLine note9_line(  1, 1, get_padSeq9);
LiquidLine note10_line( 6, 1, get_padSeq10);
LiquidLine note11_line(11, 1, get_padSeq11);
LiquidLine note12_line(16, 1, get_padSeq12);
LiquidLine note13_line(21, 1, get_padSeq13);
LiquidLine note14_line(26, 1, get_padSeq14);
LiquidLine note15_line(31, 1, get_padSeq15);
LiquidLine note16_line(36, 1, get_padSeq16);
LiquidScreen seq_screen;

LiquidMenu sequence_menu(lcd, seq_screen);

//////////SEQUENCE MODES////////////////////////////////////////////////////////////
LiquidLine seqM_line(1,   0, "SeqMode: ", get_seqMode);
LiquidLine susM_line(21,  0, "SusMode: ", get_susMode);
LiquidLine latch_line(1,  1, "latch: ", get_latch);
LiquidScreen seqM_screen(seqM_line, susM_line, latch_line);

LiquidMenu seqmode_menu(lcd, seqM_screen);

//////////RETRIGGER FX//////////////////////////////////////////////////////////////
LiquidLine sped_line( 1,  0, "Speed: ", get_speed, "ms");
LiquidLine rpts_line(21,  0, "Rpts: ",  get_rpts);
LiquidLine ptch_line( 1,  1, "Pitch: ", get_pitch);
LiquidLine damp_line(21,  1, "Damp: ",  get_damp);
LiquidScreen rTRG_screen(sped_line, rpts_line, ptch_line, damp_line);

LiquidMenu retrig_menu(lcd, rTRG_screen);

//////////DRUMKIT MENU//////////////////////////////////////////////////////////////
LiquidLine save_line( 1,  0, "SAVE KIT TO: ", "Kit", save_kit_nr);
LiquidLine load_line( 1,  1, "LOAD KIT FROM: ", "Kit", load_kit_nr);
LiquidScreen kit_screen(save_line, load_line);

LiquidMenu kit_menu(lcd, kit_screen);

//////////MENU SYSTEM///////////////////////////////////////////////////////////////
LiquidSystem menu_system;

//Pointer to int for switch case
const uint16_t home_screen_address = &home_screen;
const uint16_t main1_screen_address = &main1_screen;
const uint16_t main2_screen_address = &main2_screen;
const uint16_t NN_screen_address = &NN_screen;
const uint16_t vTRG_screen_address = &vTRG_screen;
const uint16_t CC_screen_address = &CC_screen;
const uint16_t RNG_screen_address = &RNG_screen;
const uint16_t seq_screen_address = &seq_screen;
const uint16_t seqM_screen_address = &seqM_screen;
const uint16_t rTRG_screen_address = &rTRG_screen;
const uint16_t kit_screen_address = &kit_screen;


uint8_t numSymbols[8][8] = {
  {0B11111, 0B11111, 0B10001, 0B10101, 0B10101, 0B10101, 0B10001, 0B11111}, // 0
  {0b11111, 0b11111, 0b11011, 0b10011, 0b11011, 0b11011, 0b11011, 0b11111}, // 1
  {0b11111, 0b11111, 0b10001, 0b11101, 0b11001, 0b10111, 0b10001, 0b11111}, // 2
  {0B11111, 0B11111, 0B10001, 0B11101, 0B11001, 0B11101, 0B10001, 0B11111}, // 3
  {0B11111, 0B11111, 0B10101, 0B10101, 0B10001, 0B11101, 0B11101, 0B11111}, // 4
  {0B11111, 0B11111, 0B10001, 0B10111, 0B10001, 0B11101, 0B10001, 0B11111}, // 5
  {0B11111, 0B11111, 0B10001, 0B10111, 0B10001, 0B10101, 0B10001, 0B11111}, // 6
  {0B11111, 0B11111, 0B10001, 0B11101, 0B11101, 0B11101, 0B11101, 0B11111}  // 7
};

/*XXXXXXXXXXXXXXX Handle Program Change XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void onProgramChange(byte channel, byte program) {
  loadProgram(program);
  load_kit_nr = program;
  analogWrite(led_pin,255);
  menu_system.update();
}

/*XXXXXXXXXXXXXXX SETUP XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
void setup() {

  pinMode(led_pin, OUTPUT);
  pinMode(buzzer_pin, INPUT_PULLUP);
  pinMode(susPin, INPUT_PULLUP);
  pinMode(encoder_button_pin  , INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

 

  if (!sd.begin(SD_CS_PIN)) {
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    return;
  } else {
    for (int f = 0 ; f < 2; f++) {
      for (int i = 0 ; i < 255; i++) {
        analogWrite(led_pin, i);
        delay(1);
      }
      for (int i = 255 ; i > 0; i--) {
        analogWrite(led_pin, i);
        delay(1);
      }
    }
  }

  usbMIDI.setHandleProgramChange(onProgramChange);


  // compose bigger Screens
  CC_screen.add_line(vCC1_line);    CC_screen.add_line(vCC2_line);
  CC_screen.add_line(vCC3_line);    CC_screen.add_line(vCC4_line);
  CC_screen.add_line(xCC1_line);    CC_screen.add_line(xCC2_line);
  CC_screen.add_line(xCC3_line);    CC_screen.add_line(xCC4_line);

  RNG_screen.add_line(v1MIN_line); RNG_screen.add_line(v1MAX_line);
  RNG_screen.add_line(v2MIN_line); RNG_screen.add_line(v2MAX_line);
  RNG_screen.add_line(v3MIN_line); RNG_screen.add_line(v3MAX_line);
  RNG_screen.add_line(v4MIN_line); RNG_screen.add_line(v4MAX_line);
  RNG_screen.add_line(x1MIN_line); RNG_screen.add_line(x1MAX_line);
  RNG_screen.add_line(x2MIN_line); RNG_screen.add_line(x2MAX_line);
  RNG_screen.add_line(x3MIN_line); RNG_screen.add_line(x3MAX_line);
  RNG_screen.add_line(x4MIN_line); RNG_screen.add_line(x4MAX_line);

  seq_screen.add_line(note1_line);   seq_screen.add_line(note2_line);
  seq_screen.add_line(note3_line);   seq_screen.add_line(note4_line);
  seq_screen.add_line(note5_line);   seq_screen.add_line(note6_line);
  seq_screen.add_line(note7_line);   seq_screen.add_line(note8_line);
  seq_screen.add_line(note9_line);   seq_screen.add_line(note10_line);
  seq_screen.add_line(note11_line);  seq_screen.add_line(note12_line);
  seq_screen.add_line(note13_line);  seq_screen.add_line(note14_line);
  seq_screen.add_line(note15_line);  seq_screen.add_line(note16_line);


  //compose menu_system
  menu_system.add_menu(home_menu);
  menu_system.add_menu(main_menu);
  menu_system.add_menu(pad_dest_menu);
  menu_system.add_menu(vel_trig_menu);
  menu_system.add_menu(CC_dest_menu);
  menu_system.add_menu(CC_range_menu);
  menu_system.add_menu(sequence_menu);
  menu_system.add_menu(seqmode_menu);
  menu_system.add_menu(retrig_menu);
  menu_system.add_menu(kit_menu);


  menu_system.set_focusPosition(Position::LEFT);
  home_menu.set_focusPosition(Position::LEFT);
  main_menu.set_focusPosition(Position::LEFT);
  pad_dest_menu.set_focusPosition(Position::LEFT);
  vel_trig_menu.set_focusPosition(Position::LEFT);
  CC_dest_menu.set_focusPosition(Position::LEFT);
  CC_range_menu.set_focusPosition(Position::LEFT);
  sequence_menu.set_focusPosition(Position::LEFT);
  seqmode_menu.set_focusPosition(Position::LEFT);
  retrig_menu.set_focusPosition(Position::LEFT);
  kit_menu.set_focusPosition(Position::LEFT);


  ////// attaching functions ///////////////////////////////////////////////////////
  pad_dest_line.attach_function(1, goto_pad_dest_menu);
  vel_trg_line.attach_function( 1, goto_vel_trig_menu);
  CC_dest_line.attach_function( 1, goto_CC_dest_menu);
  CC_rng_line.attach_function(  1, goto_CC_range_menu);
  seq_line.attach_function(     1, goto_sequence_menu);
  seq_mod_line.attach_function( 1, goto_seqmode_menu);
  retrFX_line.attach_function(  1, goto_retrig_menu);
  kit_line.attach_function(     1, goto_kit_menu);

  padCH_line.attach_function(incr, incAtPosition);  padCH_line.attach_function(decr, decAtPosition);
  padNN_line.attach_function(incr, incAtPosition);  padNN_line.attach_function(decr, decAtPosition);
  altNN_line.attach_function(incr, incAtPosition);  altNN_line.attach_function(decr, decAtPosition);
  curve_line.attach_function(incr, incAtPosition);  curve_line.attach_function(decr, decAtPosition);

  vTRG1_line.attach_function(incr, incAtPosition);  vTRG1_line.attach_function(decr, decAtPosition);
  vTRG2_line.attach_function(incr, incAtPosition);  vTRG2_line.attach_function(decr, decAtPosition);
  tr1TH_line.attach_function(incr, incAtPosition);  tr1TH_line.attach_function(decr, decAtPosition);
  tr2TH_line.attach_function(incr, incAtPosition);  tr2TH_line.attach_function(decr, decAtPosition);

  vCC1_line.attach_function(incr, incAtPosition);   vCC1_line.attach_function(decr, decAtPosition);
  vCC2_line.attach_function(incr, incAtPosition);   vCC2_line.attach_function(decr, decAtPosition);
  vCC3_line.attach_function(incr, incAtPosition);   vCC3_line.attach_function(decr, decAtPosition);
  vCC4_line.attach_function(incr, incAtPosition);   vCC4_line.attach_function(decr, decAtPosition);
  xCC1_line.attach_function(incr, incAtPosition);   xCC1_line.attach_function(decr, decAtPosition);
  xCC2_line.attach_function(incr, incAtPosition);   xCC2_line.attach_function(decr, decAtPosition);
  xCC3_line.attach_function(incr, incAtPosition);   xCC3_line.attach_function(decr, decAtPosition);
  xCC4_line.attach_function(incr, incAtPosition);   xCC4_line.attach_function(decr, decAtPosition);

  v1MIN_line.attach_function(incr, incAtPosition);    v1MAX_line.attach_function(incr, incAtPosition);
  v2MIN_line.attach_function(incr, incAtPosition);    v2MAX_line.attach_function(incr, incAtPosition);
  v3MIN_line.attach_function(incr, incAtPosition);    v3MAX_line.attach_function(incr, incAtPosition);
  v4MIN_line.attach_function(incr, incAtPosition);    v4MAX_line.attach_function(incr, incAtPosition);
  x1MIN_line.attach_function(incr, incAtPosition);    x1MAX_line.attach_function(incr, incAtPosition);
  x2MIN_line.attach_function(incr, incAtPosition);    x2MAX_line.attach_function(incr, incAtPosition);
  x3MIN_line.attach_function(incr, incAtPosition);    x3MAX_line.attach_function(incr, incAtPosition);
  x4MIN_line.attach_function(incr, incAtPosition);    x4MAX_line.attach_function(incr, incAtPosition);

  v1MIN_line.attach_function(decr, decAtPosition);    v1MAX_line.attach_function(decr, decAtPosition);
  v2MIN_line.attach_function(decr, decAtPosition);    v2MAX_line.attach_function(decr, decAtPosition);
  v3MIN_line.attach_function(decr, decAtPosition);    v3MAX_line.attach_function(decr, decAtPosition);
  v4MIN_line.attach_function(decr, decAtPosition);    v4MAX_line.attach_function(decr, decAtPosition);
  x1MIN_line.attach_function(decr, decAtPosition);    x1MAX_line.attach_function(decr, decAtPosition);
  x2MIN_line.attach_function(decr, decAtPosition);    x2MAX_line.attach_function(decr, decAtPosition);
  x3MIN_line.attach_function(decr, decAtPosition);    x3MAX_line.attach_function(decr, decAtPosition);
  x4MIN_line.attach_function(decr, decAtPosition);    x4MAX_line.attach_function(decr, decAtPosition);

  note1_line.attach_function(incr, incAtPosition);   note1_line.attach_function(decr, decAtPosition);
  note2_line.attach_function(incr, incAtPosition);   note2_line.attach_function(decr, decAtPosition);
  note3_line.attach_function(incr, incAtPosition);   note3_line.attach_function(decr, decAtPosition);
  note4_line.attach_function(incr, incAtPosition);   note4_line.attach_function(decr, decAtPosition);
  note5_line.attach_function(incr, incAtPosition);   note5_line.attach_function(decr, decAtPosition);
  note6_line.attach_function(incr, incAtPosition);   note6_line.attach_function(decr, decAtPosition);
  note7_line.attach_function(incr, incAtPosition);   note7_line.attach_function(decr, decAtPosition);
  note8_line.attach_function(incr, incAtPosition);   note8_line.attach_function(decr, decAtPosition);
  note9_line.attach_function(incr, incAtPosition);   note9_line.attach_function(decr, decAtPosition);
  note10_line.attach_function(incr, incAtPosition);  note10_line.attach_function(decr, decAtPosition);
  note11_line.attach_function(incr, incAtPosition);  note11_line.attach_function(decr, decAtPosition);
  note12_line.attach_function(incr, incAtPosition);  note12_line.attach_function(decr, decAtPosition);
  note13_line.attach_function(incr, incAtPosition);  note13_line.attach_function(decr, decAtPosition);
  note14_line.attach_function(incr, incAtPosition);  note14_line.attach_function(decr, decAtPosition);
  note15_line.attach_function(incr, incAtPosition);  note15_line.attach_function(decr, decAtPosition);
  note16_line.attach_function(incr, incAtPosition);  note16_line.attach_function(decr, decAtPosition);

  seqM_line.attach_function(incr, incAtPosition);    seqM_line.attach_function(decr, decAtPosition);
  susM_line.attach_function(incr, incAtPosition);    susM_line.attach_function(decr, decAtPosition);
  latch_line.attach_function(incr, incAtPosition);   latch_line.attach_function(decr, decAtPosition);

  sped_line.attach_function(incr, incAtPosition);   sped_line.attach_function(decr, decAtPosition);
  rpts_line.attach_function(incr, incAtPosition);   rpts_line.attach_function(decr, decAtPosition);
  ptch_line.attach_function(incr, incAtPosition);   ptch_line.attach_function(decr, decAtPosition);
  damp_line.attach_function(incr, incAtPosition);   damp_line.attach_function(decr, decAtPosition);

  save_line.attach_function(incr, incAtPosition);   save_line.attach_function(decr, decAtPosition);
  load_line.attach_function(incr, incAtPosition);   load_line.attach_function(decr, decAtPosition);

  ////////////////////////////////////////////////////////////////////////////////////
  for (int i = 0; i < drumPINS; i++) {
    seqM[i] = (char*)"OFF";
    susM[i] = (char*)"OFF";
    latch[i] = (char*)"OFF";
    int padnn = 36 + i;
    int altnn = 46 + i;

    padSettings[i] =  {
      10,                               //channnel
      0,                                //curve
      0,                              //fixVel
      {padnn, altnn, 128, 128},               //noteNumbers
      {90, 120},                        //velTreshold
      { 36, 36, 36, 38, 36, 36, 36, 38, //sequence
        38, 36, 36, 36, 36, 36, 36, 36
      },
      0,                                //sequence mode
      0,                                //sustain pedal mode
      {10, 10, 10, 10},              //velocity CCs
      {2, 10, 10, 10},               //expression CCs
      { 0,   0,   0,   0},              //velMin
      { 127, 127, 127, 127},            //velMax
      { 0,   0,   0,   0},              //expMin
      { 127, 127, 127, 127},            //expMax
      {0, 0, 0, 0},                     //retrigger FX
      false                             //latching?
    };
  }
  // Initialize System
  menu_system.set_focusedLine(0);
  menu_system.update();
}

/*XXXXXXXXXXXXXXX PROCESS ENCODER XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void processEncoder() {

  int32_t encoder_pos = ENCODER.read();
  bool enc_button   = digitalRead(encoder_button_pin);
  bool buzzer = digitalRead(buzzer_pin);
  menu_system.set_focusSymbol(Position::LEFT, numSymbols[selected_pad]);

  //////////////////process encoder button///////////////////////////

  //falling edge, button pressed, no action
  if (enc_button == LOW && enc_button_prev == HIGH)
  {
    enc_button_prev = LOW;
    button_press_time = millis();
  }
  // rising edge, button not pressed, check how long was it pressed
  else if (enc_button == HIGH && enc_button_prev == LOW)
  {
    enc_button_prev = HIGH;

    // check how long was the button pressed and detect a long press or a short press
    // check long press situation
    if ((millis() - button_press_time) >= button_long_press)
    {
      if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
          (uint16_t)menu_system.get_currentScreen() == main2_screen_address ) {
        menu_system.change_menu(home_menu);
        inmenu = false;
        analogWrite(led_pin, 0);
      } else {
        analogWrite(led_pin, 255);
        menu_system.change_menu(main_menu);
        menu_system.set_focusedLine(0);
        editmode = false;
      }
    }
    // check short press situation
    else if ((millis() - button_press_time) >= button_short_press)
    {
      if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
          (uint16_t)menu_system.get_currentScreen() == main2_screen_address ) {
        menu_system.call_function(1);
      } else if (menu_system.get_focusedLine() < current_line_count) {
        editmode = !editmode;
      }
    }
  }

  //////////////////process encoder turns///////////////////////////////////
  if (encoder_pos <= -3) {

    if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
        (uint16_t)menu_system.get_currentScreen() == main2_screen_address ) {
      if (menu_system.get_focusedLine() == 4) {
        menu_system.previous_screen();
      }
    }
    if (!editmode) {
      menu_system.switch_focus(true);
    } else {
      menu_system.call_function(incr);
      menu_system.update();
    }
    ENCODER.write(encoder_pos + 4);
  }

  else if (encoder_pos >= 3) {

    if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
        (uint16_t)menu_system.get_currentScreen() == main2_screen_address ) {
      if (menu_system.get_focusedLine() == 4) {
        menu_system.next_screen();
      }
    }
    if (!editmode) {
      menu_system.switch_focus(false);
    } else {
      menu_system.call_function(decr);
      menu_system.update();
    }
    ENCODER.write(encoder_pos - 4);
  }

  ///////////////////////process buzzer/////////////////////////////////////////////

  // SAVE CURRENT KIT TO SELECTED LOCATION

  //falling edge, button pressed, no action
  if (buzzer == LOW && buzzer_prev == HIGH)
  {
    buzzer_prev = LOW;
    buzzer_press_time = millis();
  }
  // rising edge, button not pressed, check how long was it pressed
  else if (buzzer == HIGH && buzzer_prev == LOW)
  {
    buzzer_prev = HIGH;

    if ((millis() - buzzer_press_time) >= button_short_press)
    {
      if ((uint16_t)menu_system.get_currentScreen() == kit_screen_address &&
          menu_system.get_focusedLine() == 0) {
        saveProgram(save_kit_nr);

      } else if ((uint16_t)menu_system.get_currentScreen() == kit_screen_address &&
                 menu_system.get_focusedLine() == 1) {
        loadProgram(load_kit_nr);
      }
    }
  }

}
/*XXXXXXXXX LOAD AND SAVE FUNCTIONS XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

// save current program to card at selected location
void saveProgram(int _filenr) {
  int structSize = 0;
  int fileNumber = _filenr;
  // open file for writing
  char filename[6];
  sprintf (filename, "Kit%03d", fileNumber);
  dataFile = sd.open(filename, FILE_WRITE);
  if (dataFile) {
    for (int i = 0; i < drumPINS; i++) {
      structSize = sizeof(padSettings[i]);
      dataFile.seek(i * structSize);
      dataFile.write(&padSettings[i], structSize);
      analogWrite(led_pin, 255 / i);
    }
    dataFile.close();
    for (int f = 0 ; f < 2; f++) {
      for (int i = 0 ; i < 255; i++) {
        analogWrite(led_pin, i);
        delay(1);
      }
      for (int i = 255 ; i > 0; i--) {
        analogWrite(led_pin, i);
        delay(1);
      }
    }
  } else {
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
  }
}


// load program from card
void loadProgram(int _filenr) {
  int structSize = 0;
  int fileNumber = _filenr;
  char filename[6];
  sprintf (filename, "Kit%03d", fileNumber);
  // open file for reading
  dataFile = sd.open(filename);
  if (dataFile) {
    for (int i = 0; i < drumPINS; i++) {
      structSize = sizeof(padSettings[i]);
      dataFile.seek(i * structSize);
      dataFile.read(&padSettings[i], structSize);
    }
    dataFile.close();
    analogWrite(led_pin, 127);

  } else {
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
    delay(100);
    analogWrite(led_pin, 255);
    delay(100);
    analogWrite(led_pin, 0);
  }
}

/*XXXXXXXXX MAIN MENU CALLBACK FUNCTIONS XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void goto_pad_dest_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(pad_dest_menu);
  current_line_count = 4;
}

void goto_vel_trig_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(vel_trig_menu);
  current_line_count = 4;
}

void goto_CC_dest_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(CC_dest_menu);
  current_line_count = 8;
}

void goto_CC_range_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(CC_range_menu);
  current_line_count = 16;
}

void goto_sequence_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(sequence_menu);
  current_line_count = 16;
}

void goto_seqmode_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(seqmode_menu);
  current_line_count = 3;
}

void goto_retrig_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(retrig_menu);
  current_line_count = 4;
}

void goto_kit_menu() {
  analogWrite(led_pin, 0);
  menu_system.set_focusedLine(0);
  menu_system.change_menu(kit_menu);
  current_line_count = 2;
}

///////////////////////////////////////////////////////////////////////////////////
void incAtPosition() {
  bool buzzer = digitalRead(buzzer_pin);
  if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
      (uint16_t)menu_system.get_currentScreen() == main2_screen_address) {
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == NN_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //pad channel
        if (padSettings[selected_pad].channel < 16) {
          padSettings[selected_pad].channel++;
        }
        break;
      case 1: //pad NN
        if (padSettings[selected_pad].padNNs[0] < 128) {
          padSettings[selected_pad].padNNs[0]++;
        }
        break;
      case 2: //alt NN
        if (padSettings[selected_pad].padNNs[1] < 128) {
          padSettings[selected_pad].padNNs[1]++;
        }
        break;
      case 3: //curva
        if (padSettings[selected_pad].velCurve < 5) {
          padSettings[selected_pad].velCurve ++;
        } else if (padSettings[selected_pad].velCurve == 5 &&
                   padSettings[selected_pad].fixVel < 127) {
          padSettings[selected_pad].fixVel++;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == vTRG_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //velTrig1 NN
        if (padSettings[selected_pad].padNNs[2] < 128) {
          padSettings[selected_pad].padNNs[2]++;
        }
        break;
      case 1: //velTrig2 NN
        if (padSettings[selected_pad].padNNs[3] < 128) {
          padSettings[selected_pad].padNNs[3]++;
        }
        break;
      case 2: //velTrig1 Treshold
        if (padSettings[selected_pad].velTres[0] < 127) {
          padSettings[selected_pad].velTres[0]++;
        }
        break;
      case 3: //velTrig2 Treshold
        if (padSettings[selected_pad].velTres[1] < 127) {
          padSettings[selected_pad].velTres[1]++;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == CC_screen_address) {

    uint8_t i = menu_system.get_focusedLine();

    if (i <= 3) {
      if (padSettings[selected_pad].velCC[i] < 128) {
        padSettings[selected_pad].velCC[i]++;
      }
    }
    else if (i > 3) {
      if (padSettings[selected_pad].expCC[i - 4] < 128) {
        padSettings[selected_pad].expCC[i - 4]++;
      }
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == RNG_screen_address) {

    uint8_t i = menu_system.get_focusedLine();
    // focus is even number < 7 is velocity minimum
    if (i < 7 && i % 2 == 0) {
      if (padSettings[selected_pad].velMin[i / 2] < 127) {
        padSettings[selected_pad].velMin[i / 2]++;
      }
    }
    // focus is even number > 7 is expression minimum
    else if (i > 7 && i % 2 == 0) {
      if (padSettings[selected_pad].expMin[(i - 8) / 2] < 127) {
        padSettings[selected_pad].expMin[(i - 8) / 2]++;
      }
    }
    // focus is uneven number < 7 is velocity maximum
    else if (i <= 7 && i % 2 == !0) {
      if (padSettings[selected_pad].velMax[(i - 1) / 2] < 127) {
        padSettings[selected_pad].velMax[(i - 1) / 2]++;
      }
    }
    // focus is uneven number > 7 is expression maximum
    else if (i > 7 && i % 2 == !0) {
      if (padSettings[selected_pad].expMax[(i - 9) / 2] < 127) {
        padSettings[selected_pad].expMax[(i - 9) / 2]++;
      }
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == seq_screen_address) {

    uint8_t i = menu_system.get_focusedLine();
    if (padSettings[selected_pad].padSeq[i] < 128) {
      padSettings[selected_pad].padSeq[i]++;
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == seqM_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //Sequence Mode
        if (padSettings[selected_pad].seqMode < 2) {
          padSettings[selected_pad].seqMode++;
        } if (padSettings[selected_pad].seqMode == 0) {
          seqM[selected_pad] =  (char*)"OFF";
        } else if (padSettings[selected_pad].seqMode == 1) {
          seqM[selected_pad] =  (char*)"ON";
        } else if (padSettings[selected_pad].seqMode == 2) {
          seqM[selected_pad] =  (char*)"RANDOM";
        }
        break;
      case 1: // susPedal mode [altNN, retrigFX, holdSeqCounter, restartSeq]
        if (padSettings[selected_pad].susMode < 3) {
          padSettings[selected_pad].susMode++;
        } if (padSettings[selected_pad].susMode == 0) {
          susM[selected_pad] =  (char*)"altNN";
        } else if (padSettings[selected_pad].susMode == 1) {
          susM[selected_pad] =  (char*)"retrigFX";
        } else if (padSettings[selected_pad].susMode == 2) {
          susM[selected_pad] =  (char*)"holdSeqCnt";
        } else if (padSettings[selected_pad].susMode == 3) {
          susM[selected_pad] =  (char*)"restartSeq";
        }
        break;
      case 2:
        padSettings[selected_pad].latching = true;
        latch[selected_pad] = (char*)"ON";
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == rTRG_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //Speed in ms
        if (padSettings[selected_pad].retrigFX[0] < 2000) {
          if (buzzer) {
            padSettings[selected_pad].retrigFX[0]++;
          } else if (padSettings[selected_pad].retrigFX[0] < 1990) {
            padSettings[selected_pad].retrigFX[0] += 10;
          }
        } else if (padSettings[selected_pad].retrigFX[0] == 2000) {
          padSettings[selected_pad].retrigFX[0] = 0;
        }
        break;
      case 1: //Repeats
        if (padSettings[selected_pad].retrigFX[1] < 64) {
          padSettings[selected_pad].retrigFX[1]++;
        }
        break;
      case 2: //Pitch in semitones
        if (padSettings[selected_pad].retrigFX[2] < 16) {
          padSettings[selected_pad].retrigFX[2]++;
        }
        break;
      case 3: //Damp in steps [int] of velocity
        if (padSettings[selected_pad].retrigFX[3] < 127) {
          padSettings[selected_pad].retrigFX[3]++;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == kit_screen_address) {
    if (menu_system.get_focusedLine() == 0 && save_kit_nr < 255) {
      save_kit_nr ++;
      char filename[6];
      sprintf (filename, "Kit%03d", save_kit_nr);
      if (sd.exists(filename)) {
        analogWrite(led_pin, 80);
      }
      else {
        analogWrite(led_pin, 0);
      }
    } else if (menu_system.get_focusedLine() == 1 && load_kit_nr < 255) {
      load_kit_nr ++;
    }
    return;
  }
}
////////////////////////////////////////////////////////////////////////////////////////////

void decAtPosition() {
  bool buzzer = digitalRead(buzzer_pin);
  if ((uint16_t)menu_system.get_currentScreen() == main1_screen_address ||
      (uint16_t)menu_system.get_currentScreen() == main2_screen_address) {
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == NN_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //pad channel
        if (padSettings[selected_pad].channel > 0) {
          padSettings[selected_pad].channel--;
        }
        break;
      case 1: //pad NN
        if (padSettings[selected_pad].padNNs[0] > 0) {
          padSettings[selected_pad].padNNs[0]--;
        }
        break;
      case 2: //alt NN
        if (padSettings[selected_pad].padNNs[1] > 0) {
          padSettings[selected_pad].padNNs[1]--;
        }
        break;
      case 3: //curva
        if (padSettings[selected_pad].velCurve > 0 &&
            padSettings[selected_pad].velCurve < 5) {
          padSettings[selected_pad].velCurve--;
        } else if (padSettings[selected_pad].velCurve == 5
                   && padSettings[selected_pad].fixVel > 0) {
          padSettings[selected_pad].fixVel--;
        } else if (padSettings[selected_pad].velCurve == 5
                   && padSettings[selected_pad].fixVel == 0) {
          padSettings[selected_pad].velCurve--;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == vTRG_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //velTrig1 NN
        if (padSettings[selected_pad].padNNs[2] > 0) {
          padSettings[selected_pad].padNNs[2]--;
        }
        break;
      case 1: //velTrig2 NN
        if (padSettings[selected_pad].padNNs[3] > 0) {
          padSettings[selected_pad].padNNs[3]--;
        }
        break;
      case 2: //velTrig1 Treshold
        if (padSettings[selected_pad].velTres[0] > 0) {
          padSettings[selected_pad].velTres[0]--;
        }
        break;
      case 3: //velTrig2 Treshold
        if (padSettings[selected_pad].velTres[1] > 0) {
          padSettings[selected_pad].velTres[1]--;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == CC_screen_address) {

    uint8_t i = menu_system.get_focusedLine();

    if (i <= 3) {
      if (padSettings[selected_pad].velCC[i] > 0) {
        padSettings[selected_pad].velCC[i]--;
      }
    }
    if (i > 3) {
      if (padSettings[selected_pad].expCC[i - 4] > 0) {
        padSettings[selected_pad].expCC[i - 4]--;
      }
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == RNG_screen_address) {

    uint8_t i = menu_system.get_focusedLine();
    // focus is even number < 7 is velocity minimum
    if (i < 7 && i % 2 == 0) {
      if (padSettings[selected_pad].velMin[i / 2] > 0) {
        padSettings[selected_pad].velMin[i / 2]--;
      }
    }
    // focus is even number > 7 is expression minimum
    else if (i > 7 && i % 2 == 0) {
      if (padSettings[selected_pad].expMin[(i - 8) / 2] > 0) {
        padSettings[selected_pad].expMin[(i - 8) / 2]--;
      }
    }
    // focus is uneven number < 7 is velocity maximum
    else if (i <= 7 && i % 2 == !0) {
      if (padSettings[selected_pad].velMax[(i - 1) / 2] > 0) {
        padSettings[selected_pad].velMax[(i - 1) / 2]--;
      }
    }
    // focus is uneven number > 7 is expression maximum
    else if (i > 7 && i % 2 == !0) {
      if (padSettings[selected_pad].expMax[(i - 9) / 2] > 0) {
        padSettings[selected_pad].expMax[(i - 9) / 2]--;
      }
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == seq_screen_address) {

    uint8_t i = menu_system.get_focusedLine();
    if (padSettings[selected_pad].padSeq[i] > 0) {
      padSettings[selected_pad].padSeq[i]--;
    } else {
      return;
    }
  } else if ((uint16_t)menu_system.get_currentScreen() == seqM_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //Sequence Mode
        if (padSettings[selected_pad].seqMode > 0) {
          padSettings[selected_pad].seqMode--;
        } if (padSettings[selected_pad].seqMode == 0) {
          seqM[selected_pad] =  (char*)"OFF";
        } else if (padSettings[selected_pad].seqMode == 1) {
          seqM[selected_pad] =  (char*)"ON";
        } else if (padSettings[selected_pad].seqMode == 2) {
          seqM[selected_pad] =  (char*)"RANDOM";
        }
        break;
      case 1: // susPedal mode [altNN, retrigFX, holdSeqCounter, restartSeq]
        if (padSettings[selected_pad].susMode > 0) {
          padSettings[selected_pad].susMode--;
        } if (padSettings[selected_pad].susMode == 0) {
          susM[selected_pad] =  (char*)"altNN";
        } else if (padSettings[selected_pad].susMode == 1) {
          susM[selected_pad] =  (char*)"retrigFX";
        } else if (padSettings[selected_pad].susMode == 2) {
          susM[selected_pad] =  (char*)"holdSeqCnt";
        } else if (padSettings[selected_pad].susMode == 3) {
          susM[selected_pad] =  (char*)"restartSeq";
        }
        break;
      case 2:
        padSettings[selected_pad].latching = false;
        latch[selected_pad] = (char*)"OFF";
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == rTRG_screen_address) {
    switch (menu_system.get_focusedLine()) {
      case 0: //Speed in ms
        if (padSettings[selected_pad].retrigFX[0] > 0) {
          if (buzzer) {
            padSettings[selected_pad].retrigFX[0]--;
          } else if (padSettings[selected_pad].retrigFX[0] > 10) {
            padSettings[selected_pad].retrigFX[0] -= 10;
          }
        } else if (padSettings[selected_pad].retrigFX[0] == 0) {
          padSettings[selected_pad].retrigFX[0] = 2000;
        }
        break;
      case 1: //Repeats
        if (padSettings[selected_pad].retrigFX[1] > 0) {
          padSettings[selected_pad].retrigFX[1]--;
        }
        break;
      case 2: //Pitch in semitones
        if (padSettings[selected_pad].retrigFX[2] > -16) {
          padSettings[selected_pad].retrigFX[2]--;
        }
        break;
      case 3: //Damp in steps [int] of velocity
        if (padSettings[selected_pad].retrigFX[3] > -127) {
          padSettings[selected_pad].retrigFX[3]--;
        }
        break;
    }
    return;
  } else if ((uint16_t)menu_system.get_currentScreen() == kit_screen_address) {
    if (menu_system.get_focusedLine() == 0 && save_kit_nr > 0) {
      save_kit_nr --;
      char filename[6];
      sprintf (filename, "Kit%03d", save_kit_nr);
      if (sd.exists(filename)) {
        analogWrite(led_pin, 80);
      } else {
        analogWrite(led_pin, 0);
      }
    } else if (menu_system.get_focusedLine() == 1 && load_kit_nr > 0) {
      load_kit_nr --;
    }
    return;
  }
}

/*XXXXXXXXXXXXXXXX Peak Detect XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void peakDetect(int i) {

  //Serial.println(state[i]);

  switch (state[i]) {
    // IDLE state: wait for any reading is above threshold.
    case 0:
      if (piezo[i] > thresholdMin) {
        //Serial.print("begin peak track ");
        //Serial.println(piezo[i]);
        peak[i] = piezo[i];
        msec[i] = 0;
        state[i] = 1;
      }
      return;

    // Peak Tracking state: capture largest reading
    case 1:
      if (piezo[i] > peak[i]) {
        peak[i] = piezo[i];
      }
      if (msec[i] >= peakTrackMillis) {
        //Serial.print("peak = ");
        //Serial.println(peak);
        int velocity = map(peak[i], thresholdMin, 1023, 1, 127);
        //usbMIDI.sendNoteOn(note[i], velocity, channel);
        handleMidiOn(i, velocity);
        msec[i] = 0;
        state[i] = 2;
      }
      return;

    // Ignore Aftershock state: wait for things to be quiet again.
    default:
      if (piezo[i] > thresholdMin) {
        msec[i] = 0; // keep resetting timer if above threshold
      } else if (msec[i] > aftershockMillis) {
        handleMidiOff(i);
        state[i] = 0; // go back to idle when
      }
  }
}
/*XXXXXXXXXXXXXXXX MIDI HANDLING XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void handleMidiOn(byte padNr, int _velocity) {

  int velocity;

  /*---------------shape velocity data---------------------------------------------*/
  switch (padSettings[padNr].velCurve) {


    case 4:
      velocity =  applyCurve(_velocity, curve0, curve1, sizeof(curve0));
      break;  // go on

    case 3:
      velocity =  applyCurve(_velocity, curve0, curve2, sizeof(curve0));
      break; // go on

    case 2:
      velocity =  applyCurve(_velocity, curve0, curve3, sizeof(curve0));
      break; // go on

    case 1:
      velocity =  applyCurve(_velocity, curve0, curve4, sizeof(curve0));
      break; // go on

    case 0:
      velocity =  _velocity;
      break; // go on

    case 5:
      velocity =  padSettings[padNr].fixVel;
      break; // go on
  }

  /*---------------check for velocityCCs-------------------------------------------*/
  for (int i = 0; i < 4; i++) {
    // if there is a velCC stored
    if (padSettings[padNr].velCC[i] < 128) {
      int vlct = map(velocity, 0, 127,
                     padSettings[padNr].velMin[i],
                     padSettings[padNr].velMax[i]);
      usbMIDI.sendControlChange(padSettings[padNr].velCC[i], vlct, padSettings[padNr].channel);
    }
  }

  /*---------------check for expressionCCs-----------------------------------------*/
  for (int i = 0; i < 4; i++) {
    // if there is a expCC stored
    if (padSettings[padNr].expCC[i] < 128) {
      // if the foot is on the pedal and the cc value changed
      if (solar < 5 && expr != lastExpr[padNr]) {
        int xprss = map(expr, 0, 1023,
                        padSettings[padNr].expMin[i],
                        padSettings[padNr].expMax[i]);
        usbMIDI.sendControlChange(padSettings[padNr].expCC[i], xprss, padSettings[padNr].channel);
        lastExpr[padNr] = expr;
      }
    }
  }

  /*---------------check sus pedal mode--------------------------------------------*/
  if (sus) {
    switch (padSettings[padNr].susMode) {

      case 0://alternative Note Number
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        // in any case send alternative pad note
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[1],
                           velocity,
                           padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[1];
        return;

      case 1://enable Trigger FX
        {
          int retrig_note = lastPlayed[padNr];
          int retrig_vel = velocity;
          for (int i = 0; i < padSettings[padNr].retrigFX[1]; i++) {
            usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
            usbMIDI.sendNoteOn( retrig_note, retrig_vel,
                                padSettings[padNr].channel);
            delay(padSettings[padNr].retrigFX[0]);
            lastPlayed[padNr] = retrig_note;
            retrig_note += padSettings[padNr].retrigFX[2];
            if (retrig_vel > 0 &&
                retrig_vel < 127 ) {
              retrig_vel +=  padSettings[padNr].retrigFX[3];
            }
          }
          return;
        }
      case 2://trigger last seqNote again
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        // in any case send same pad note
        usbMIDI.sendNoteOn(lastPlayed[padNr], velocity,
                           padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        //lastPlayed[padNr] = lastPlayed[padNr];
        return;

      case 3://reset sequence counter
        counter[padNr]  = 0;
        break;
    }
  }

  /*---------------check for sequenceMode-------------------------------------------*/
  switch (padSettings[padNr].seqMode) {

    case 0:
      break; // go on

    case 1:  // sequence
      {
        uint8_t stepnr = counter[padNr];
        // if latching send noteOff on last Note in the Sequence
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        // in any case send next note in sequence
        usbMIDI.sendNoteOn(padSettings[padNr].padSeq[stepnr],
                           velocity,
                           padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padSeq[stepnr];
        //and take care of the counter
        counter[padNr] ++;
        if (counter[padNr] > 15) counter[padNr] = 0;
        return; // stop handleMidiOn
      }
    case 2: // random
      {
        long randNum = random(17);
        // if latching send noteOff on last Note in the Sequence
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        // in any case send random note in sequence
        usbMIDI.sendNoteOn(padSettings[padNr].padSeq[randNum],
                           velocity,
                           padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padSeq[randNum];
        return; // stop handleMidiOn
      }
  }


  /*---------------check for velocityTriggers--------------------------------------*/
  if (velocity > padSettings[padNr].velTres[0]) {
    if (velocity < padSettings[padNr].velTres[1]) {
      //check if a velTrig1 is set. Then send that NoteNumber with fixed velocity
      if (padSettings[padNr].padNNs[2] < 128)
      {
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[2], velocity, padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[2];
        return;
      }
    } else {
      //check if a velTrig2 is set. Then send that NoteNumber
      if (padSettings[padNr].padNNs[3] < 128)
      {
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[3], velocity, padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[3];
        return;
      }
    }
  }

  // if nothing of the above applied
  usbMIDI.sendNoteOn(padSettings[padNr].padNNs[0], velocity, padSettings[padNr].channel);
  //keep track of the Notenumber to send noteOff
  lastPlayed[padNr] = padSettings[padNr].padNNs[0];
  return;
}
/*XXXXXXXXXXXXXXXX handle midi off XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void handleMidiOff(byte padNr) {
  if (!padSettings[padNr].latching) {
    usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
  } else {
    return;
  }
}
/*XXXXXXXXXXXXXXXX curve interpolation XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

int applyCurve(uint8_t val, uint8_t* _in, uint8_t* _out, uint8_t size)
{
  // take care the value is within range
  // val = constrain(val, _in[0], _in[size-1]);
  if (val <= _in[0]) return _out[0];
  if (val >= _in[size - 1]) return _out[size - 1];

  // search right interval
  uint8_t pos = 1;  // _in[0] allready tested
  while (val > _in[pos]) pos++;

  // this will handle all exact "points" in the _in array
  if (val == _in[pos]) return _out[pos];

  // interpolate in the right segment for the rest
  return (val - _in[pos - 1]) * (_out[pos] - _out[pos - 1]) / (_in[pos] - _in[pos - 1]) + _out[pos - 1];
}

/*XXXXXXXXXXXXXXXXXX LOOP XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void loop() {

  while (!inmenu) {
    for (int i = 0; i < drumPINS; i++) {
      piezo[i] = analogRead(analogPin[i]);
      peakDetect(i);
      // Add other tasks to loop, but avoid using delay() or waiting.
      // You need loop() to keep running rapidly to detect Piezo peaks!
      expr = analogRead(expPin);
      sus = digitalRead(susPin);
      solar = analogRead(slrPin);

    }
    checkInput();
    usbMIDI.read(pc_channel);
  
  }
  while (inmenu) {
    processEncoder();
  }

}

void checkInput () {

  bool enc_button  = digitalRead(encoder_button_pin);
  bool buzzer = digitalRead(buzzer_pin);

  if (buzzer == LOW && buzzer_prev == HIGH) {
    buzzer_prev = LOW;
    buzzer_press_time = millis();
  } else if  (buzzer == HIGH && buzzer_prev == LOW) {
    buzzer_prev = HIGH;
    if ((millis() - buzzer_press_time) >= button_short_press)
    {
      selected_pad++;
      if (selected_pad > 7) {
        selected_pad = 0;
      }
      menu_system.update();
    }
  }
  if (enc_button == LOW && enc_button_prev == HIGH)
  {
    enc_button_prev = LOW;
    button_press_time = millis();
  }
  // rising edge, button not pressed, check how long was it pressed
  else if (enc_button == HIGH && enc_button_prev == LOW)
  {
    enc_button_prev = HIGH;

    // check short press situation
    if ((millis() - button_press_time) >= button_short_press)
    {
      inmenu = true;
      analogWrite(led_pin, 255);
      ENCODER.write(0);
      menu_system.set_focusedLine(0);
      menu_system.change_menu(main_menu);
    }
  }

}
