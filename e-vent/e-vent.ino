/**
 * MIT Emergency Ventilator Controller
 * 
 * MIT License:
 * 
 * Copyright (c) 2020 MIT
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * e-vent.ino
 * Main Arduino file.
 */

#include "LiquidCrystal.h"
//#include "src/thirdparty/RoboClaw/RoboClaw.h"
#include "cpp_utils.h"  // Redefines macros min, max, abs, etc. into proper functions,
#include <arduino-timer.h>                        // should be included after third-party code, before E-Vent includes

#include "Alarms.h"
#include "Buttons.h"
#include "Constants.h"
#include "Display.h"
#include "Input.h"
#include "Logging.h"
#include "Pressure.h"
#include "AutoPID.h"

using namespace input;
using namespace utils;


/////////////////////
// Initialize Vars //
/////////////////////

// Cycle parameters
unsigned long cycleCount = 0;
float tCycleTimer;     // Absolute time (s) at start of each breathing cycle
float tIn;             // Calculated time (s) since tCycleTimer for end of IN_STATE
float tHoldIn;         // Calculated time (s) since tCycleTimer for end of HOLD_IN_STATE
float tEx;             // Calculated time (s) since tCycleTimer for end of EX_STATE
float tPeriod;         // Calculated time (s) since tCycleTimer for end of cycle
float tPeriodActual;   // Actual time (s) since tCycleTimer at end of cycle (for logging)
float tLoopTimer;      // Absolute time (s) at start of each control loop iteration
float tLoopBuffer;     // Amount of time (s) left at end of each loop

// States
States state;
bool enteringState;
float tStateTimer;

// Roboclaw
//RoboClaw roboclaw(&Serial3, 10000);
//int motorCurrent, 
int motorPosition = 0;
#define RANGE 5
bool test;
double Setpoint; 
double Input1 = 0;
double Output;
int counterlog;
volatile long contador =  0; 
auto timer = timer_create_default();
byte         cmd       =  0;             // Use for serial comunication.  
byte         flags; 
double percentageError = 0; 
double Kp=2, Ki=0.1, Kd=0.01;  
byte  ant =  0;    
byte  act =  0;

byte Home = false;
byte Close = false;

AutoPID myPID(&Input1, &Setpoint, &Output, OUTPUT_MIN_PID, OUTPUT_MAX_PID, Kp, Ki, Kd, RANGE);


// LCD Screen
LiquidCrystal lcd(LCD_RS_PIN, LCD_EN_PIN, LCD_D4_PIN, dLCD_D5_PIN, LCD_D6_PIN, LCD_D7_PIN);
display::Display displ(&lcd, AC_MIN);

// Alarms
alarms::AlarmManager alarm(BEEPER_PIN, SNOOZE_PIN, LED_ALARM_PIN, &displ, &cycleCount);

// Pressure
Pressure pressureReader(PRESS_SENSE_PIN);

// Buttons
buttons::PressHoldButton offButton(OFF_PIN, 2000);
buttons::DebouncedButton confirmButton(CONFIRM_PIN);

// Logger
logging::Logger logger(false/*Serial*/, false/*SD*/, false/*labels*/, ",\t"/*delim*/);

// Knobs
struct Knobs {
  int volume();  // Tidal volume
  int bpm();     // Respiratory rate
  float ie();    // Inhale/exhale ratio
  float ac();    // Assist control trigger sensitivity
  SafeKnob<int> volume_ = SafeKnob<int>(&displ, display::VOLUME, CONFIRM_PIN, &alarm, VOL_RES);
  SafeKnob<int> bpm_ = SafeKnob<int>(&displ, display::BPM, CONFIRM_PIN, &alarm, BPM_RES);
  SafeKnob<float> ie_ = SafeKnob<float>(&displ, display::IE_RATIO, CONFIRM_PIN, &alarm, IE_RES);
  SafeKnob<float> ac_ = SafeKnob<float>(&displ, display::AC_TRIGGER, CONFIRM_PIN, &alarm, AC_RES);
  void begin();
  void update();
} knobs;

// Assist control
bool patientTriggered = false;


///////////////////////
// Declare Functions //
///////////////////////

// Set the current state in the state machine
void setState(States newState);

// Calculates the waveform parameters from the user inputs
void calculateWaveform();

// Check for errors and take appropriate action
void handleErrors();

// Set up logger variables
void setupLogger();

///////////////////
////// Setup //////
///////////////////

void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  while(!Serial);
  pinMode(LED_BUILTIN, OUTPUT);
  if (DEBUG) {
    setState(DEBUG_STATE);
  } else {
    setState(PREHOME_STATE);  // Initial state
  }

  // Wait for the roboclaw to boot up
  delay(1000);
  
  //Initialize
  pinMode(HOME_PIN, INPUT_PULLUP);  // Pull up the limit switch
  setupLogger();
  alarm.begin();
  displ.begin();
  offButton.begin();
  confirmButton.begin();
  knobs.begin();
  tCycleTimer = now();

//  roboclaw.begin(ROBOCLAW_BAUD);
//  roboclaw.SetM1MaxCurrent(ROBOCLAW_ADDR, ROBOCLAW_MAX_CURRENT);
//  roboclaw.SetM1VelocityPID(ROBOCLAW_ADDR, VKP, VKI, VKD, QPPS);
//  roboclaw.SetM1PositionPID(ROBOCLAW_ADDR, PKP, PKI, PKD, KI_MAX, DEADZONE, MIN_POS, MAX_POS);
//  roboclaw.SetEncM1(ROBOCLAW_ADDR, 0);  // Zero the encoder

  Setpoint = 330;
  myPID.setBangBang(0);
  myPID.setTimeStep(SAMPLE_TIME);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  attachInterrupt(digitalPinToInterrupt(ENC1), encoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC2), encoder, CHANGE);
  int counterlog = 0;
  timer.every(SAMPLE_TIME, pid_interupt);
}

//////////////////
////// Loop //////
//////////////////

void loop() {


//  if (percentageError == 0 && !Home && !Close) {
//
//    digitalWrite(LED_BUILTIN, HIGH);
//    Home = true;
//    myPID.stop();
//    delay(1000);
//    Setpoint = CUSTOM_CLOSE;
//    Serial.println("Home");
//  }
//
//  if (percentageError == 0 && Home && !Close) {
//    digitalWrite(LED_BUILTIN, LOW);
//    Home = false;
//    Close = true;
//    delay(1000);
//    Setpoint = CUSTOM_HOME - CUSTOM_CLOSE;
//    Serial.println("close");
//  }
//
//  if (percentageError == 0 && !Home && Close) {
//    digitalWrite(LED_BUILTIN, HIGH);
//    Home = true;
//    Close = false;
//    delay(1000);
//    Setpoint = CUSTOM_CLOSE;
//    Serial.println("home again");
//  }
  
    Serial.print("PWM  :");Serial.print(Output); 
    Serial.print(" |  contador  :");Serial.print(contador);
    Serial.print(" |  Setpoint  :");Serial.println(Setpoint);
    timer.tick(); // tick the timer
  if (DEBUG) {
    if (Serial.available() > 0) {
      setState((States) Serial.parseInt());
      while(Serial.available() > 0) Serial.read();
    }
  }

  // All States
  tLoopTimer = now();  // Start the loop timer
  logger.update();
  knobs.update();
  calculateWaveform();
//  readEncoder(roboclaw, motorPosition);  // TODO handle invalid reading
//  readMotorCurrent(roboclaw, motorCurrent);
  pressureReader.read();
  handleErrors();
  alarm.update();
  displ.update();
  offButton.update();

  if (offButton.wasHeld()) {
//    goToPositionByDur(roboclaw, BAG_CLEAR_POS, motorPosition, MAX_EX_DURATION);
    setState(OFF_STATE);
    alarm.allOff();
  }
  
  // State Machine
  switch (state) {

    case DEBUG_STATE:
      // Stop motor
//     roboclaw.ForwardM1(ROBOCLAW_ADDR, 0);
      break;

    case OFF_STATE: 
      alarm.turningOFF(now() - tStateTimer < TURNING_OFF_DURATION);
      if (confirmButton.is_LOW()) {
        setState(PREHOME_STATE);
        alarm.turningOFF(false);
      }
      break;
  
    case IN_STATE:
      if (enteringState) {
        enteringState = false;
        const float tNow = now();
        tPeriodActual = tNow - tCycleTimer;
        tCycleTimer = tNow;  // The cycle begins at the start of inspiration
//        goToPositionByDur(roboclaw, volume2ticks(knobs.volume()), motorPosition, tIn);
        cycleCount++;
      }

      if (now() - tCycleTimer > tIn) {
        setState(HOLD_IN_STATE);
      }
      break;
  
    case HOLD_IN_STATE:
      if (enteringState) {
        enteringState = false;
      }
      if (now() - tCycleTimer > tHoldIn) {
        pressureReader.set_plateau();
        setState(EX_STATE);
      }
      break;
  
    case EX_STATE:
      if (enteringState) {
        enteringState = false;
//        goToPositionByDur(roboclaw, BAG_CLEAR_POS, motorPosition, tEx - (now() - tCycleTimer));
      }

      if (abs(motorPosition - BAG_CLEAR_POS) < BAG_CLEAR_TOL) {
        setState(PEEP_PAUSE_STATE);
      }
      break;

    case PEEP_PAUSE_STATE:
      if (enteringState) {
        enteringState = false;
      }
      
      if (now() - tCycleTimer > tEx + MIN_PEEP_PAUSE) {
        pressureReader.set_peep();
        
        setState(HOLD_EX_STATE);
      }
      break;

    case HOLD_EX_STATE:
      if (enteringState) {
        enteringState = false;
      }

      // Check if patient triggers inhale
      patientTriggered = pressureReader.get() < (pressureReader.peep() - knobs.ac()) 
          && knobs.ac() > AC_MIN;

      if (patientTriggered || now() - tCycleTimer > tPeriod) {
        if (!patientTriggered) pressureReader.set_peep();  // Set peep again if time triggered
        pressureReader.set_peak_and_reset();
        displ.writePeakP(round(pressureReader.peak()));
        displ.writePEEP(round(pressureReader.peep()));
        displ.writePlateauP(round(pressureReader.plateau()));
        setState(IN_STATE);
      }
      break;

    case PREHOME_STATE:
      if (enteringState) {
        enteringState = false;
//        roboclaw.BackwardM1(ROBOCLAW_ADDR, HOMING_VOLTS);
      }

      if (homeSwitchPressed()) {
        setState(HOMING_STATE);
      }
      break;

    case HOMING_STATE:
      if (enteringState) {
        enteringState = false;
 //       roboclaw.ForwardM1(ROBOCLAW_ADDR, HOMING_VOLTS);
      }
      
      if (!homeSwitchPressed()) {
//        roboclaw.ForwardM1(ROBOCLAW_ADDR, 0);
        delay(HOMING_PAUSE * 1000);  // Wait for things to settle
 //       roboclaw.SetEncM1(ROBOCLAW_ADDR, 0);  // Zero the encoder
        setState(IN_STATE);
      }
      break;
  }

  // Add a delay if there's still time in the loop period
  tLoopBuffer = max(0, tLoopTimer + LOOP_PERIOD - now());
  delay(tLoopBuffer*1000.0);

}


/////////////////
// Definitions //
/////////////////

void Knobs::begin() {
  volume_.begin(&readVolume);
  bpm_.begin(&readBpm);
  ie_.begin(&readIeRatio);
  ac_.begin(&readAc);
}

void Knobs::update() {
  volume_.update();
  bpm_.update();
  ie_.update();
  ac_.update();
}

inline int Knobs::volume() { return volume_.read(); }
inline int Knobs::bpm() { return bpm_.read(); }
inline float Knobs::ie() { return ie_.read(); }
inline float Knobs::ac() { return ac_.read(); }

void setState(States newState) {
  enteringState = true;
  state = newState;
  tStateTimer = now();
}

void calculateWaveform() {
  tPeriod = 60.0 / knobs.bpm();  // seconds in each breathing cycle period
  tHoldIn = tPeriod / (1 + knobs.ie());
  tIn = tHoldIn - HOLD_IN_DURATION;
  tEx = min(tHoldIn + MAX_EX_DURATION, tPeriod - MIN_PEEP_PAUSE);
}

void handleErrors() {
  // Pressure alarms
  const bool over_pressure = pressureReader.get() >= MAX_PRESSURE;
  alarm.highPressure(over_pressure);
  if (over_pressure) setState(EX_STATE);

  // These pressure alarms only make sense after homing 
  if (enteringState && state == IN_STATE) {
    alarm.badPlateau(pressureReader.peak() - pressureReader.plateau() > MAX_RESIST_PRESSURE);
    alarm.lowPressure(pressureReader.plateau() < MIN_PLATEAU_PRESSURE);
    alarm.noTidalPres(pressureReader.peak() - pressureReader.peep() < MIN_TIDAL_PRESSURE);
  }

  // Check if desired volume was reached
  if (enteringState && state == EX_STATE) {
    alarm.unmetVolume(knobs.volume() - ticks2volume(motorPosition) > VOLUME_ERROR_THRESH);
  }

  // Check if maximum motor current was exceeded
//  if (motorCurrent >= MAX_MOTOR_CURRENT) {
//    setState(EX_STATE);
//    alarm.overCurrent(true);
//  } else {
//    alarm.overCurrent(false);
//  }

  // Check if we've gotten stuck in EX_STATE (mechanical cycle didn't finsih)
  alarm.mechanicalFailure(state == EX_STATE && now() - tCycleTimer > tPeriod + MECHANICAL_TIMEOUT);
}

void setupLogger() {
  logger.addVar("Time", &tLoopTimer);
  logger.addVar("CycleStart", &tCycleTimer);
  logger.addVar("State", (int*)&state);
  logger.addVar("Pos", &motorPosition, 3);
  logger.addVar("Pressure", &pressureReader.get(), 6);
  // logger.addVar("Period", &tPeriodActual);
  // logger.addVar("tLoopBuffer", &tLoopBuffer, 6, 4);
  // logger.addVar("Current", &motorCurrent, 3);
  // logger.addVar("Peep", &pressureReader.peep(), 6);
  // logger.addVar("HighPresAlarm", &alarm.getHighPressure());
  // begin called after all variables added to include them all in the header
  logger.begin(&Serial, SD_SELECT);
}



//AutoPID

bool pid_interupt(void *) {
  myPID.run();  // Calculus for PID algorithm 
  RunMotor(Output); // PWM order to DC driver
  return true; // repeat? true
}
// Function for run the motor, backward, forward or stop
void RunMotor(double Usignal){  
  if (Setpoint-Input1==0){
    shaftrev(ENC1,ENC2,PWM1,BACKWARD, 0);
    //Serial.print("cero");
  }else if(Usignal>=0){
    shaftrev(ENC1,ENC2,PWM1,BACKWARD, Usignal);
  }else{
    shaftrev(ENC1,ENC2,PWM1,FORWARD, -1*Usignal);
  }   
}

void shaftrev(int ENC1, int ENC2, int PWM, int sentido, int Wpulse){  
  if(sentido == 0){ //backWARDS
    digitalWrite(MOT1, HIGH);
    digitalWrite(MOT2, LOW);
    analogWrite(PWM,Wpulse);
  }
  if(sentido == 1){ //forWARDS
    digitalWrite(MOT1, LOW);
    digitalWrite(MOT2, HIGH);
    analogWrite(PWM,Wpulse);     
  }
}

// Encoder x4. Execute when interruption pin jumps.
void encoder(void){ 
  //Serial.println(ant);
  ant=act;                            // Saved act (current read) in ant (last read)
  act = digitalRead(ENC1)<<1|digitalRead(ENC2);
  if(ant==0 && act==1)        contador++;  // Increase the counter for forward movement
  else if(ant==1  && act==3)  contador++;
  else if(ant==3  && act==2)  contador++;
  else if(ant==2  && act==0)  contador++;
  else contador--;                         // Reduce the counter for backward movement

  // Enter the counter as input for PID algorith
  Input1 = contador;;
}


// Data catched from serial terminal
void input_data(void){
  if (Serial.available() > 0){           // Check if you have received any data through the serial terminal.
  
      cmd = 0;                            // clean CMD
      cmd = Serial.read();                // "cmd" keep the recived byte
      if (cmd > 31){
      
        flags = 0;                                           // Reboot the flag, that decide what have to be printed
        if (cmd >  'Z') cmd -= 32;                           // Change to Uppercase 
        if (cmd == 'W') { Setpoint += 5.0;     flags = 2; }  // For example is you put 'W' moves 5 steps forward. Relative movement
        if (cmd == 'Q') { Setpoint -= 5.0;     flags = 2; }  // Here is 5 steps backward
        if (cmd == 'S') { Setpoint += 400.0;   flags = 2; }  // The same with another values 
        if (cmd == 'A') { Setpoint -= 400.0;   flags = 2; }
        if (cmd == 'X') { Setpoint += 5000.0;  flags = 2; }
        if (cmd == 'Z') { Setpoint -= 5000.0;  flags = 2; }
        if (cmd == '2') { Setpoint += 12000.0; flags = 2; }
        if (cmd == '1') { Setpoint -= 12000.0; flags = 2; }
        if (cmd == '0') { Setpoint = 0.0;      flags = 2; }  // Ir a Inicio.
        
        // Decode for change the PID gains
        switch(cmd)                                                     // for example, we put "P2.5 I0.5 D40" them the gains will take these values kp, ki y kd.
        {                                                               // You can change every gain independently
          case 'P': Kp  = Serial.parseFloat();        flags = 1; break; // Change the PID gains
          case 'I': Ki  = Serial.parseFloat();        flags = 1; break;
          case 'D': Kd  = Serial.parseFloat();        flags = 1; break;
          case 'G': Setpoint   = -1*Serial.parseFloat(); flags = 2; break; // You can change the setpoint with absolute values Ex: G23000
          case 'K':                                   flags = 3; break;
        }       
        imprimir(flags);
      }
    }
}

// Print date in serial terminal
void imprimir(byte flag){ 

  if ((flag == 1) || (flag == 3))
  {
    Serial.print("KP=");     Serial.print(Kp);
    Serial.print(" KI=");    Serial.print(Ki);
    Serial.print(" KD=");    Serial.print(Kd);
    Serial.print(" Time=");  Serial.println(SAMPLE_TIME);
  }
  if ((flag == 2) || (flag == 3))
  {
    Serial.print("Position:");
    Serial.println((long)Setpoint);
  }
}
