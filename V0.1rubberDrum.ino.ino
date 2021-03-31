/*XXXXXXXXXXXXXX Description XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

  A programmable midi Drumset with midi effects
  based on the Teensy 3.2

  Code is based on oddson's multi-pin extension of Paul Stoffregen's
  Piezo trigger code (teensy examples)

  The curveApply function is basically a copy of Rob Tillaart's multiMap function
  XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
#include <SD.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>
#include <Encoder.h>

#define encoder_A_pin       11
#define encoder_B_pin       12
#define encoder_button_pin  10    // use internal pullup

#define button_long_press    800   // ms
#define button_short_press   80   // ms

LiquidCrystal_I2C lcd(0x27, 40, 2);
Encoder ENCODER(encoder_A_pin, encoder_B_pin);

unsigned long  button_press_time = millis();



/*XXXXXXXXXX Menu & Screens Vars XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

uint8_t curser_pos = 0;
uint8_t numOfHeadScreens = 3;
uint8_t numOfSubScreens = 10;
uint8_t currentHeadScreen = 0;
uint8_t currentSubScreen = 0;

uint8_t selected_pad = 1;

bool subMenu = LOW;
bool editParam = LOW;



String headMenu[numOfHeadScreens][3] = {
  {"Midi Note Out", "Vel Note Mapping", "Expression Dest"},
  {"Velocity Dest", "Expression Limits", "Velocity Limits"},
  {"Retrigger Effect", "Pedal & Seq Modes", "Pad Sequence"}
};

String subMenu[numOfSubScreens][4] = {
  {"PadChannel", "PadCurve", "PadNote", "Alt Note"},
  {"VelTrig1", "VelTrig2", "Treshold1", "Treshold2"},
  {"VelCC1", "VelCC2", "VelCC3", "VelCC4"},
  {"ExpCC1", "ExpCC2", "ExpCC3", "ExpCC4"},
  {"Vel1 min", "Vel1 max", "Vel2 min", "Vel2 max"},
  {"Vel3 min", "Vel3 max", "Vel4 min", "Vel4 max"},
  {"Exp1 min", "Exp1 max", "Exp2 min", "Exp2 max"},
  {"Exp3 min", "Exp3 max", "Exp4 min", "Exp4 max"},
  {"Speed", "Repeats", "Pitch", "Damp"},
  {"SeqMode", "SusMode", "Latch", " "},
};

/*XXXXXXXXXX Drum Logic Vars XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

const int drumPINS = 8;     // number of drum signals incoming
const int expPin = A0;
const int susPin = 2;
const int slrPin = A11;

const int analogPin[drumPINS] = {A1, A2, A3, A6, A7, A8, A9, A10}; //array of analog PINs
const int thresholdMin = 60;  // minimum reading, avoid noise and false starts
const int peakTrackMillis = 12;
const int aftershockMillis = 25; // aftershocks & vibration reject

int state[drumPINS];  // 0=idle, 1=looking for peak, 2=ignore aftershocks
int peak[drumPINS];   // remember the highest reading
int piezo[drumPINS];
elapsedMillis msec[drumPINS]; // timers to end states 1 and 2

const int velTrig1 = 90;
const int velTrig2 = 120;

// velocity "curves" for interpolation
int curve0[] = {0, 16, 32, 48, 64, 80, 96, 112, 127}; //linear
int curve1[] = {0, 8, 16, 24, 32, 40, 48, 80, 127};  //almost exp
int curve2[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max
int curve3[] = {127, 127, 127, 127, 127, 127, 127, 127, 127}; //all max

// counter for sequence mode
int counter[drumPINS];

// last played note on pad
int lastPlayed[drumPINS];

bool sus;
bool retrigger[drumPINS];

// expression Pedal
int solar;
int expr;
int lastExpr[drumPINS];

/*XXXXXXXXXXXXXX definition of  padSettings XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
struct pad_settings {

  uint8_t channel;      // midi Channel to send to
  uint8_t velCurve;     // ID of table containing the curve
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
  int8_t retrigFX[4];   // [speed,repeats,pitch,damp]
  bool latching;        // noteOff waits for next stroke

} ;

// an array of settings struct for each of 8 pads (update to eeprom as scenes ?)
struct pad_settings padSettings[drumPINS];


/*XXXXXXXXXXXXXXX SETUP XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void setup() {
  //Serial.begin(115200);
  pinMode (susPin, INPUT_PULLUP);
  pinMode(encoder_button_pin  , INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.begin(40, 2);

  // Scene 0  holding the defaults
  for (int i = 0; i < drumPINS; i++) {
    padSettings[i] =  {
      10,                               //channnel
      0,                                //curve
      {60, 64, 128, 128},               //noteNumbers
      {90, 120},                        //velTreshold
      { 60, 60, 60, 64, 60, 60, 60, 64, //sequence
        60, 60, 60, 64, 60, 60, 60, 64
      },
      0,                                //sequence mode
      0,                                //sustain pedal mode
      {10, 255, 255, 255},              //velocity CCs
      {2, 255, 255, 255},               //expression CCs
      { 0,   0,   0,   0},              //velMin
      { 127, 127, 127, 127},            //velMax
      { 0,   0,   0,   0},              //expMin
      { 127, 127, 127, 127},            //expMax
      {0, 0, 0, 0},                     //retrigger FX
      false                             //latching?
    };
  }
}

/*XXXXXXXXXXXXX handle EEProm XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

  void saveScene(sceneNr, _padSettings) {

  // keep track of where scenes are saved
  //Move address to the next byte after size of Settings * SceneNr
  int eeAddress = (sizeof(padSettings) * sceneNr);
  EEPROM.put(eeAddress, _padSettings);

  };

  void loadScene(sceneNr) {

  //Move address to the next byte after size of Settings * SceneNr
  int eeAddress = (sizeof(padSettings) * sceneNr);
  EEPROM.get(eeAddress, padSettings);

  };

  XXXXXXXXXXXXXXXX LOOP XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void loop() {

  for (int i = 0; i < drumPINS; i++) {
    //delay(20);
    piezo[i] = analogRead(analogPin[i]);

    peakDetect(i);
    // Add other tasks to loop, but avoid using delay() or waiting.
    // You need loop() to keep running rapidly to detect Piezo peaks!


  }

  expr = analogRead(expPin);
  sus = digitalRead(susPin);
  solar = analogRead(slrPin);

  //  Serial.print(expr);
  //  Serial.print("\t");
  //  Serial.print(susPin);
  //  Serial.print("\t");
  //  Serial.println(solar);

  // MIDI Controllers should discard incoming MIDI messages.
  while (usbMIDI.read()) {
    // ignore incoming messages, do nothing
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

/*XXXXXXXXXXXXXXXX handle midi On XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


void handleMidiOn(byte padNr, int _velocity) {

  int velocity;

  /*---------------shape velocity data---------------------------------------------------*/
  switch (padSettings[padNr].velCurve) {
    case 0:
      velocity =  _velocity;
      break;  // go on

    case 1:
      velocity =  applyCurve(_velocity, curve0, curve1, sizeof(curve0));
      break;  // go on

    case 2:
      velocity =  applyCurve(_velocity, curve0, curve2, sizeof(curve0));
      break; // go on

    case 3:
      velocity =  applyCurve(_velocity, curve0, curve3, sizeof(curve0));
      break; // go on
  }

  /*---------------check for velocityCCs-------------------------------------------------*/
  for (int i = 0; i < 4; i++) {
    // if there is a velCC stored
    if (padSettings[padNr].velCC[i] < 128) {
      int vlct = map(velocity, 0, 127,
                     padSettings[padNr].velMin[i],
                     padSettings[padNr].velMax[i]);
      usbMIDI.sendControlChange(padSettings[padNr].velCC[i], vlct, padSettings[padNr].channel);
    }
  }

  /*---------------check for expressionCCs------------------------------------------------*/
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

  /*---------------check sus pedal mode---------------------------------------------------*/
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
        retrigger[padNr] = true;
        break;

      case 2://trigger last seqNote again
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        // in any case send same pad note
        usbMIDI.sendNoteOn(lastPlayed[padNr],
                           velocity,
                           padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[1];
        return;

      case 3://reset sequence counter
        counter[padNr]  = 0;
        break;
    }
  }

  /*---------------check for sequenceMode-------------------------------------------------*/
  switch (padSettings[padNr].seqMode) {
    case 0:
      break; // go on

    case 1:  // sequence
      // if latching send noteOff on last Note in the Sequence
      if (padSettings[padNr].latching) {
        usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
      }
      // in any case send next note in sequence
      usbMIDI.sendNoteOn(padSettings[padNr].padSeq[counter[padNr]],
                         velocity,
                         padSettings[padNr].channel);
      //keep track of the Notenumber to send noteOff
      lastPlayed[padNr] = padSettings[padNr].padSeq[counter[padNr]];
      //and take care of the counter
      counter[padNr] ++;
      if (counter[padNr] > 16) counter[padNr] = 0;
      return; // stop handleMidiOn

    case 2: // random
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


  /*---------------check for velocityTriggers--------------------------------------------*/
  if (velocity > velTrig1) {
    if (velocity < velTrig2) {
      //check if a velTrig1 is set. Then send that NoteNumber with fixed velocity
      if (padSettings[padNr].padNNs[3] < 128)
      {
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[3], velTrig1, padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[3];
        return;
      }
    } else {
      //check if a velTrig2 is set. Then send that NoteNumber with fixed velocity
      if (padSettings[padNr].padNNs[4] < 128)
      {
        if (padSettings[padNr].latching) {
          usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
        }
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[4], velTrig1, padSettings[padNr].channel);
        //keep track of the Notenumber to send noteOff
        lastPlayed[padNr] = padSettings[padNr].padNNs[4];
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
/*XXXXXXXXXXXXXXXX handle midi off XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void handleMidiOff(byte padNr) {
  if (!padSettings[padNr].latching) {
    usbMIDI.sendNoteOff(lastPlayed[padNr], 0, padSettings[padNr].channel);
  } else {
    return;
  }
}
/*XXXXXXXXXXXXXXXX curve interpolation XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

int applyCurve(int val, int* _in, int* _out, uint8_t size)
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

/*XXXXXXXXXXXXXXXX encoder input handler XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/



/*XXXXXXXXXXXXXXXX print Screen XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void printScreen() {

  if (!subMenu) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Pad:" + " " + selected_pad);
    lcd.setCursor(20, 0);
    lcd.print(headMenu[currentHeadScreen][1]);
    lcd.setCursor(0, 1);
    lcd.print(headMenu[currentHeadScreen][2]);
    lcd.setCursor(20, 1);
    lcd.print(headMenu[currentHeadScreen][3]);
  }
  if (subMenu) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(subMenu[currentSubScreen][1]);
    lcd.setCursor(10, 0);
    lcd.print(subMenu[currentSubScreen][2]);
    lcd.setCursor(20, 0);
    lcd.print(subMenu[currentSubScreen][3]);
    lcd.setCursor(30, 0);
    lcd.print(subMenu[currentSubScreen][4]);


    if (currentSubScreen == 1) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].channel);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].velCurve);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].padNNs[1]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].padNNs[2]);
    }
    if (currentSubScreen == 2) {
      lcd.setCursor(0, 1);
      if (padSettings[selected_pad].padNNs[3] < 128) {
        lcd.print(padSettings[selected_pad].padNNs[3]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(10, 1);
      if (padSettings[selected_pad].padNNs[4] < 128) {
        lcd.print(padSettings[selected_pad].padNNs[4]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].velTres[1]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].velTres[2]);
    }
    if (currentSubScreen == 3) {
      lcd.setCursor(0, 1);
      if (padSettings[selected_pad].velCC[1] < 128) {
        lcd.print(padSettings[selected_pad].velCC[1]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(10, 1);
      if (padSettings[selected_pad].velCC[2] < 128) {
        lcd.print(padSettings[selected_pad].velCC[2]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(20, 1);
      if (padSettings[selected_pad].velCC[3] < 128) {
        lcd.print(padSettings[selected_pad].velCC[3]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(30, 1);
      if (padSettings[selected_pad].velCC[4] < 128) {
        lcd.print(padSettings[selected_pad].velCC[4]);
      } else {
        lcd.print("OFF");
      }
    }
    if (currentSubScreen == 4) {
      lcd.setCursor(0, 1);
      if (padSettings[selected_pad].expCC[1] < 128) {
        lcd.print(padSettings[selected_pad].expCC[1]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(10, 1);
      if (padSettings[selected_pad].expCC[2] < 128) {
        lcd.print(padSettings[selected_pad].expCC[2]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(20, 1);
      if (padSettings[selected_pad].expCC[3] < 128) {
        lcd.print(padSettings[selected_pad].expCC[3]);
      } else {
        lcd.print("OFF");
      }
      lcd.setCursor(30, 1);
      if (padSettings[selected_pad].expCC[4] < 128) {
        lcd.print(padSettings[selected_pad].expCC[4]);
      } else {
        lcd.print("OFF");
      }
    }
    if (currentSubScreen == 5) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].velMin[1]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].velMax[1]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].velMin[2]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].velMax[2]);
    }
    if (currentSubScreen == 6) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].velMin[3]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].velMax[3]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].velMin[4]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].velMax[4]);
    }
    if (currentSubScreen == 7) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].expMin[1]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].expMax[1]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].expMin[2]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].expMax[2]);
    }
    if (currentSubScreen == 8) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].expMin[3]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].expMax[3]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].expMin[4]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].expMax[4]);
    }
    //HLIASBDVCÖJIBASDCOABSDCVHASÖCOBAÖKISBCAOISCHBVLÖIAUSHC
    if (currentSubScreen == 9) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].velMin[3]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].velMax[3]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].velMin[4]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].velMax[4]);
    }
    if (currentSubScreen == 10) {
      lcd.setCursor(0, 1);
      lcd.print(padSettings[selected_pad].velMin[3]);
      lcd.setCursor(10, 1);
      lcd.print(padSettings[selected_pad].velMax[3]);
      lcd.setCursor(20, 1);
      lcd.print(padSettings[selected_pad].velMin[4]);
      lcd.setCursor(30, 1);
      lcd.print(padSettings[selected_pad].velMax[4]);
    }

  }
}
