/*XXXXXXXXXXXXXX Description XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX

  A programmable midi Drumset with midi effects
  based on the Teensy 3.2

  Code is based on oddson's multi-pin extension of Paul Stoffregen's
  Piezo trigger code (teensy examples)
  
  The curveApply function is basically a copy of Rob Tillaart's multiMap function
  XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


const int drumPINS = 7;     // number of drum signals incoming


const int analogPin[drumPINS] = {A1, A2, A3, A6, A7, A8, A9}; //array of analog PINs
const int thresholdMin = 60;  // minimum reading, avoid noise and false starts
const int peakTrackMillis = 12;
const int aftershockMillis = 25; // aftershocks & vibration reject

int state[drumPINS];  // 0=idle, 1=looking for peak, 2=ignore aftershocks
int peak[drumPINS];   // remember the highest reading
int piezo[drumPINS];
elapsedMillis msec[drumPINS]; // timers to end states 1 and 2

int velTrig1 = 90;
int velTrig2 = 120;

// velocity "curves" for interpolation
int curve0[] = {0, 16, 32, 48, 64, 80, 96, 112, 127}; //linear
int curve1[] = {0, 8, 16, 24, 32, 40, 48, 80, 127};  //almost exp
int curve2[] = {0, 2, 6, 8, 16, 36, 64, 96, 127};    //flat min to lin max
int curve3[] = {127, 127, 127, 127, 127, 127, 127, 127, 127}; //all max


/*XXXXXXXXXXXXXX definition of  padSettings XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/
struct pad_settings {

  uint8_t channel;      // midi Channel to send to
  uint8_t velCurve;     // ID of table containing the curve
  uint8_t padNNs[4];    // default, alternative, velTrig1, velTrig2
  uint8_t padSeq[16];   // NNs of Note Sequence
  uint8_t seqMode;      // Sequence mode [off,on,latching]
  uint8_t susMode;      // sustainPedal operation [altNN, holdSeq, restartSeq]
  uint8_t velCC[4];     // velocity modulation destinations
  uint8_t expCC[4];     // expression modulation destinations
  uint8_t modRng[16];   // min & max for each destination cc,cc,cc,cc,xp,xp,xp,xp
  int8_t retrigFX[5];   // [(off,on),speed,repeats,pitch,damp]

} ;

// an array of settings struct for each of 7 pads (update to eeprom as scenes ?)
struct pad_settings padSettings[7];


/*XXXXXXXXXXXXXXX SETUP XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/

void setup() {

  // Scene 0  holding the defaults
  for (int i = 0; i < sizeof(padSettings); i++) {
    padSettings[i] =  {
      10,                               //channnel
      0,                                //curve
      {60, 64, 128, 128},               //noteNumbers
      { 60, 60, 60, 64, 60, 60, 60, 64, //sequence
        60, 60, 60, 64, 60, 60, 60, 64
      },
      0,                                //sequence mode
      0,                                //sustain pedal mode
      {10, 255, 255, 255},              //velocity CCs
      {2, 255, 255, 255},               //expression CCs
      { 0, 127, 0, 127, 0, 127, 0, 127, //modulation ranges
        0, 127, 0, 127, 0, 127, 0, 127
      },
      {0, 0, 0, 0, 0}                   //retrigger FX
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
  // MIDI Controllers should discard incoming MIDI messages.
  // http://forum.pjrc.com/threads/24179-Teensy-3-Ableton-Analog-CC-causes-midi-crash
  while (usbMIDI.read()) {
    // ignore incoming messages
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
        //**************************************usbMIDI.sendNoteOff(note[i], 0, channel);
        state[i] = 0; // go back to idle when
      }
  }
}

/*XXXXXXXXXXXXXXXX handle USB Midi XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX*/


void handleMidiOn(byte padNr, int _velocity) {

  int velocity;
  /*---------------shape velocity data---------------------------------------------------*/

  switch (padSettings[padNr].velCurve) {
    case 0:
      velocity =  _velocity;
      return;

    case 1:
      velocity =  applyCurve(_velocity, curve0, curve1, sizeof(curve0));
      return;

    case 2:
      velocity =  applyCurve(_velocity, curve0, curve2, sizeof(curve0));
      return;

    case 3:
      velocity =  applyCurve(_velocity, curve0, curve3, sizeof(curve0));
      return;
  }

  /*---------------check for sequenceMode-------------------------------------------------*/
  if (padSettings[padNr].seqMode != 0) {
    //get note from padSeq, save a reference to index, iterate

    // if latching is on, dont send a noteoff
  }

  /*---------------check for velocityCCs-------------------------------------------------*/
  for (int i = 0; i < 4; i++) {
    // if there is a velCC stored
    if (padSettings[padNr].velCC[i] < 128) {
      usbMIDI.sendControlChange(padSettings[padNr].velCC[i], velocity, padSettings[padNr].channel);
    }
  }


  /*---------------check for velocityTriggers--------------------------------------------*/
  if (velocity > velTrig1) {
    if (velocity < velTrig2) {
      //check if a velTrig1 is set. Then send that NoteNumber with fixed velocity
      if (padSettings[padNr].padNNs[3] < 127)
      {
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[3], velTrig1, padSettings[padNr].channel);
        //if not send the velocity with standard NoteNumber [0]
      } else {
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[0], velocity, padSettings[padNr].channel);
      }
    } else {
      //check if a velTrig2 is set. Then send that NoteNumber with fixed velocity
      if (padSettings[padNr].padNNs[4] < 127)
      {
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[4], velTrig1, padSettings[padNr].channel);
      } else {
        //if not send the velocity with standard NoteNumber [0]
        usbMIDI.sendNoteOn(padSettings[padNr].padNNs[0], velocity, padSettings[padNr].channel);
      }
    }
  } else {
    // if velocity is below velTrig1 also send the velocity with standard NoteNumber [0]
    usbMIDI.sendNoteOn(padSettings[padNr].padNNs[0], velocity, padSettings[padNr].channel);
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
