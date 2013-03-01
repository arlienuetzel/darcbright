/* darcbright - A custom firmware for the Hexbright flashlight.
** Based on `hexbright4` from https://github.com/hexbright/hexbright-samples
** Modifications authored by Robert Quattlebaum <darco@deepdarc.com>
**
** Added Features:
**  * Debounce power-on button press.
**  * Smooth pulsing charging indicator.
**  * Accelerometer filtering.
**
** TODO:
**  * Make mode transitions happen immediately on button press
**    instead of button release.
**  * Figure out how to add a momentary operation mode.
**  * Add Smooth transitions between brightness levels.
*/

#include <math.h>
#include <Wire.h>

// Settings
#define OVERTEMP                330
// Constants
#define ACC_ADDRESS             0x4C
#define ACC_REG_XOUT            0
#define ACC_REG_YOUT            1
#define ACC_REG_ZOUT            2
#define ACC_REG_TILT            3
#define ACC_REG_INTS            6
#define ACC_REG_MODE            7
// Pin assignments
#define DPIN_RLED_SW            2
#define DPIN_GLED               5
#define DPIN_PGOOD              7
#define DPIN_PWR                8
#define DPIN_DRV_MODE           9
#define DPIN_DRV_EN             10
#define DPIN_ACC_INT            3
#define APIN_TEMP               0
#define APIN_CHARGE             3
// Interrupts
#define INT_SW                  0
#define INT_ACC                 1
// Modes
#define MODE_POWERUP            0
#define MODE_OFF                1
#define MODE_LOW                2
#define MODE_MED                3
#define MODE_HIGH               4
#define MODE_KNOBBING           5
#define MODE_KNOBBED            6
#define MODE_BLINKING           7
#define MODE_BLINKING_PREVIEW   8
#define MODE_DAZZLING           9
#define MODE_DAZZLING_PREVIEW   10

// State
byte mode = 0;
unsigned long btnTime = 0;
boolean btnDown = false;

void
setup(void)
{
  // We just powered on!  That means either we got plugged 
  // into USB, or (more likely) the user is pressing the 
  // power button.  We need to pull up the enable pin of 
  // the regulator very soon so we don't lose power.

  // We don't pull the pin high here quite yet because
  // we will do that when we transition out of MODE_OFF,
  // somewhere in the main loop. Delaying this as long
  // as possible acts like a debounce for accidental button
  // taps.
  pinMode(DPIN_PWR,      INPUT);
  digitalWrite(DPIN_PWR, LOW);

  // Initialize GPIO
  pinMode(DPIN_RLED_SW,  INPUT);
  pinMode(DPIN_GLED,     OUTPUT);
  pinMode(DPIN_DRV_MODE, OUTPUT);
  pinMode(DPIN_DRV_EN,   OUTPUT);
  pinMode(DPIN_ACC_INT,  INPUT);
  pinMode(DPIN_PGOOD,    INPUT);
  digitalWrite(DPIN_DRV_MODE, LOW);
  digitalWrite(DPIN_DRV_EN,   LOW);
  digitalWrite(DPIN_ACC_INT,  HIGH);
  
  // Initialize serial busses
  Serial.begin(9600);
  Wire.begin();

  // Configure accelerometer
  byte config[] = {
    ACC_REG_INTS,  // First register (see next line)
    0xE4,  // Interrupts: shakes, taps
    0x00,  // Mode: not enabled yet
    0x00,  // Sample rate: 120 Hz
    0x0F,  // Tap threshold
    0x10   // Tap debounce samples
  };
  Wire.beginTransmission(ACC_ADDRESS);
  Wire.write(config, sizeof(config));
  Wire.endTransmission();

  // Enable accelerometer
  byte enable[] = {ACC_REG_MODE, 0x01};  // Mode: active!
  Wire.beginTransmission(ACC_ADDRESS);
  Wire.write(enable, sizeof(enable));
  Wire.endTransmission();
  
  btnTime = millis();
  btnDown = digitalRead(DPIN_RLED_SW);

  // We are initially in the "off" state. We will transition
  // to something else in the main loop.
  mode = MODE_OFF;

  Serial.println("Powered up!");
}

void
loop(void)
{
  static unsigned long lastTime, lastTempTime, lastAccTime;
  static float lastKnobAngle, knob;
  static byte blink;
  unsigned long time = millis();
  
  // Check the state of the charge controller
  int chargeState = analogRead(APIN_CHARGE);

  if (chargeState < 128) {  // Low - charging
    // Smoothly pulse the green LED over a two-second interval,
    // as if it were "breathing". This is the charging indication.
    byte pulse = ((time>>2)&0xFF);
    pulse = ((pulse * pulse) >> 8);
    analogWrite(DPIN_GLED, ((time>>2)&0x0100)?0xFF-pulse:pulse);

  } else if (chargeState > 768) {  // High - fully charged.
    // Solid green LED.
    analogWrite(DPIN_GLED, 255);
    digitalWrite(DPIN_GLED, HIGH);

  } else {  // Hi-Z - Not charging, not pulged in.
    // Blink the indicator LED now and then.
    digitalWrite(DPIN_GLED, (time&0x03FF)?LOW:HIGH);
  }

  // Check the serial port
  if(Serial.available()) {
    char c = Serial.read();
    switch(c) {
    case 's':
      {
        int temperature = analogRead(APIN_TEMP);
        Serial.print("Temperature = ");
        Serial.println(temperature);

        char accel[3];
        readAccel(accel);
        Serial.print("Acceleration = ");
        Serial.print(accel[0], DEC);
        Serial.print(", ");
        Serial.print(accel[1], DEC);
        Serial.print(", ");
        Serial.println(accel[2], DEC);
      
        byte pgood = digitalRead(DPIN_PGOOD);
        Serial.print("LED driver power good = ");
        Serial.println(pgood?"Yes":"No");
      }
      break;
    }
  }

  // Check the temperature sensor
  if(time-lastTempTime > 1000) {
    lastTempTime = time;
    int temperature = analogRead(APIN_TEMP);
    Serial.print("chargeState = ");
    Serial.print(chargeState);
    Serial.print(" Temperature = ");
    Serial.println(temperature);
    if(temperature > OVERTEMP) {
      Serial.println("Overheat shutdown!");
      mode = MODE_OFF;
      digitalWrite(DPIN_DRV_MODE, LOW);
      digitalWrite(DPIN_DRV_EN, LOW);
      digitalWrite(DPIN_PWR, LOW);
    }
  }

  // Check if the accelerometer wants to interrupt
  byte tapped = 0, shaked = 0;
  if (!digitalRead(DPIN_ACC_INT)) {
    Wire.beginTransmission(ACC_ADDRESS);
    Wire.write(ACC_REG_TILT);
    Wire.endTransmission(false);       // End, but do not stop!
    Wire.requestFrom(ACC_ADDRESS, 1);  // This one stops.
    byte tilt = Wire.read();
    
    if (time-lastAccTime > 500) {
      lastAccTime = time;
  
      tapped = !!(tilt & 0x20);
      shaked = !!(tilt & 0x80);
  
      if (tapped) Serial.println("Tap!");
      if (shaked) Serial.println("Shake!");
    }
  }

  // Do whatever this mode does
  switch (mode) {
  case MODE_KNOBBED:
  case MODE_KNOBBING:
    {
      if (time-lastTime > 100 && !tapped) {
        lastTime = time;
  
        float angle = readAccelAngleXZ();
        float change = angle - lastKnobAngle;
        lastKnobAngle = angle;

        // Don't bother updating our brightness reading if our angle isn't good.
        char acc[3];
        readAccel(acc);
        if(acc[0]*acc[0] + acc[2]*acc[2] >= 8*8) {
          if (change >  PI) change -= 2.0*PI;
          if (change < -PI) change += 2.0*PI;
          knob += -change * 40.0;
          if (knob < 0)   knob = 0;
          if (knob > 255) knob = 255;
        }
      }

      // Make apparent brightness changes linear by squaring the
      // value and dividing back down into range.  This gives us
      // a gamma correction of 2.0, which is close enough.
      byte bright = (long)(knob * knob) >> 8;

      // Avoid ever appearing off in this mode!
      if (bright < 8) bright = 8;

      static byte actual_bright = 8;

      if(bright>actual_bright)
        actual_bright++;

      if(bright<actual_bright)
        actual_bright--;

      analogWrite(DPIN_DRV_EN, actual_bright);
  
//      Serial.print("Ang = ");
//      Serial.print(angle);
//      Serial.print("\tChange = ");
//      Serial.print(change);
//      Serial.print("\tKnob = ");
//      Serial.print(knob);
//      Serial.print("\tBright = ");
//      Serial.println(bright);
    }
    break;
  case MODE_BLINKING:
  case MODE_BLINKING_PREVIEW:
    blink = ((time&(255))<64);
    digitalWrite(DPIN_DRV_EN, blink);
    break;
  case MODE_DAZZLING:
  case MODE_DAZZLING_PREVIEW:
    if (time-lastTime < 10) break;
    lastTime = time;
    
    digitalWrite(DPIN_DRV_EN, random(4)<1);
    break;
  }
  
  // Check for mode changes
  byte newMode = mode;
  byte newBtnDown = digitalRead(DPIN_RLED_SW);
  switch(mode) {
  case MODE_OFF:
    if (btnDown && !newBtnDown && (time-btnTime)>50)  // Button released
      newMode = MODE_LOW;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_KNOBBING;
    break;
  case MODE_LOW:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_MED;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_KNOBBING;
    break;
  case MODE_MED:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_HIGH;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_KNOBBING;
    break;
  case MODE_HIGH:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_OFF;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_KNOBBING;
    break;
  case MODE_KNOBBING:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_KNOBBED;
    if (btnDown && newBtnDown && tapped)
      newMode = MODE_BLINKING_PREVIEW;
    break;
  case MODE_KNOBBED:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_OFF;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_KNOBBING;
    break;
  case MODE_BLINKING:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_OFF;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_BLINKING_PREVIEW;
    break;
  case MODE_BLINKING_PREVIEW:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_BLINKING;
    if (btnDown && newBtnDown && tapped)
      newMode = MODE_DAZZLING_PREVIEW;
    break;
  case MODE_DAZZLING:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_OFF;
    if (btnDown && newBtnDown && (time-btnTime)>500)  // Held
      newMode = MODE_DAZZLING_PREVIEW;
    break;
  case MODE_DAZZLING_PREVIEW:
    if (btnDown && !newBtnDown)  // Button released
      newMode = MODE_DAZZLING;
    if (btnDown && newBtnDown && tapped)
      newMode = MODE_BLINKING_PREVIEW;
    break;
  }

  // Do the mode transitions
  if (newMode != mode)
  {
    switch (newMode)
    {
    case MODE_OFF:
      Serial.println("Mode = off");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, LOW);
      digitalWrite(DPIN_DRV_MODE, LOW);
      digitalWrite(DPIN_DRV_EN, LOW);
      break;
    case MODE_LOW:
      Serial.println("Mode = low");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      digitalWrite(DPIN_DRV_MODE, LOW);
      analogWrite(DPIN_DRV_EN, 64);
      break;
    case MODE_MED:
      Serial.println("Mode = medium");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      digitalWrite(DPIN_DRV_MODE, LOW);
      analogWrite(DPIN_DRV_EN, 255);
      break;
    case MODE_HIGH:
      Serial.println("Mode = high");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      digitalWrite(DPIN_DRV_MODE, HIGH);
      analogWrite(DPIN_DRV_EN, 255);
      break;
    case MODE_KNOBBING:
      Serial.println("Mode = knobbing");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      lastKnobAngle = readAccelAngleXZ();
      //knob = (mode==MODE_OFF) ? 0 : 255;
      break;
    case MODE_KNOBBED:
      Serial.println("Mode = knobbed");
      break;
    case MODE_BLINKING:
    case MODE_BLINKING_PREVIEW:
      Serial.println("Mode = blinking");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      digitalWrite(DPIN_DRV_MODE, LOW);
      break;
    case MODE_DAZZLING:
    case MODE_DAZZLING_PREVIEW:
      Serial.println("Mode = dazzling");
      pinMode(DPIN_PWR, OUTPUT);
      digitalWrite(DPIN_PWR, HIGH);
      digitalWrite(DPIN_DRV_MODE, HIGH);
      break;
    }

    mode = newMode;
  }

  // Remember button state so we can detect transitions
  if (newBtnDown != btnDown)
  {
    btnTime = time;
    btnDown = newBtnDown;
    delay(50);
  }
}

void readAccel(char *acc)
{
  while (1)
  {
    Wire.beginTransmission(ACC_ADDRESS);
    Wire.write(ACC_REG_XOUT);
    Wire.endTransmission(false);       // End, but do not stop!
    Wire.requestFrom(ACC_ADDRESS, 3);  // This one stops.

    for (int i = 0; i < 3; i++)
    {
      if (!Wire.available())
        continue;
      acc[i] = Wire.read();
      if (acc[i] & 0x40)  // Indicates failed read; redo!
        continue;
      if (acc[i] & 0x20)  // Sign-extend
        acc[i] |= 0xC0;
    }
    break;
  }
}

/* Returns the median value of the given three parameters */
char median_char(char a, char b, char c) {
    if(a<c) {
        if(b<a) {
            return a;
        } else if(c<b) {
            return c;
        }
    } else {
        if(a<b) {
            return a;
        } else if(b<c) {
            return c;
        }
    }
    return b;
}


float readAccelAngleXZ()
{
  static char acc[3][3];
  static char i;
  readAccel(acc[i++]);
  if(i==3)
    i=0;
  return atan2(median_char(acc[0][0],acc[1][0],acc[2][0]),median_char(acc[0][2],acc[1][2],acc[2][2]));
}

