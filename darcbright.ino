/* darcbright - A custom firmware for the Hexbright flashlight.
** Based on `hexbright4` from https://github.com/hexbright/hexbright-samples
** Modifications authored by Robert Quattlebaum <darco@deepdarc.com>
**
** Added Features:
**  * Smooth brightness transitions.
**  * Increases in brightness happen on the button press, decreases on button release.
**  * Smooth pulsing charging indicator.
**  * Accelerometer filtering.
**  * Mode changing by holding down button. (4 "modes": constant, momentary, blinky, knob)
**  * Overtemp condition now attempts to throttle back brightness instead of just turning off.
**  * Debounce power-on button press.
**
*/

/*
Accelorometer axies:

(x,y,z)

If the flashlight is facing straight up:
  (0, 21, 0)

If the flashlight is facing straight down:
  (0, -21, 0)

If the flashlight is battery-up:
  (0, 0, -21)

If the flashlight is battery-down:
  (0, 0, 21)

If the flashlight is logo-up:
  (-21, 0, 0)

If the flashlight is logo-down:
  (21, 0, 0)

*/

#include <math.h>
#include <Wire.h>
#include <EEPROM.h>
#include "pt.h"
#include <avr/wdt.h>

// Settings
#define VOLTAGE_NOMINAL               3330
#define VOLTAGE_LOW                   3100
#define OVERTEMP_SHUTDOWN_C           65
#define OVERTEMP_THROTTLE_C           45
#define OVERTEMP_SHUTDOWN             (OVERTEMP_SHUTDOWN_C*10+500)
#define OVERTEMP_THROTTLE             (OVERTEMP_THROTTLE_C*10+500)
#define BUTTON_BRIGHTNESS_THRESHOLD   1000 // time in ms, after which a button press turns off
#define BUTTON_DEBOUNCE               20
#define POWER_ON_BUTTON_THRESHOLD     150 // Period of time button initially needs to be held to keep the light on.

#define BRT_MIN_LUMEN          4
#define BRT_MED_LUMEN          255                        // 175 lumens
#define BRT_LOW_LUMEN          (BRT_MED_LUMEN/4)            // 43 lumens
#define BRT_MAX_LUMEN          (BRT_MED_LUMEN+(3*255))    // 500 lumens



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
#define DPIN_PWR                8
#define DPIN_DRV_MODE           9
#define DPIN_DRV_EN             10
#define DPIN_ACC_INT            3
#define APIN_TEMP               0
#define APIN_CHARGE             3

// Interrupts
#define INT_SW                  0
#define INT_ACC                 1

// State
byte light_mode;
unsigned short overtemp_max;
unsigned short amount_current;
unsigned short amount_begin;
unsigned short amount_end;
unsigned short amount_fade_duration;
unsigned long amount_fade_start;
byte amount_flash,amount_off;

// Protothread States
struct pt fade_control_pt;
struct pt power_pt;
struct pt button_led_pt;

// Mode Protothread States
struct pt light_pt;
struct pt light_momentary_pt;
struct pt light_blinky_pt;
struct pt light_knob_pt;

// Variables updated every loop
bool button_is_pressed;
unsigned long button_pressed_time;
unsigned long button_released_time;
unsigned long button_pressed_duration;
unsigned long button_released_duration;
short vcc_current;
short vcc_filter_table[3];
short vcc_filtered;
short temp_filter_table[3];
short temp_filtered;
short temp_current;
enum {
    BATT_DISCHARGING,
    BATT_CHARGING,
    BATT_CHARGED,
} batt_state;
unsigned long time_current;
float angle_pitch;
float angle_roll;

#define PT_WAIT_FOR_PERIOD(pt,x) \
    lastTime =  time_current; \
    PT_WAIT_UNTIL(pt, (time_current-lastTime) > (x));

bool orientation_enabled;

void
enable_orientation(void) {
  pinMode(DPIN_ACC_INT,  INPUT);
  digitalWrite(DPIN_ACC_INT,  HIGH);

  // Configure accelerometer
  static const byte config[] = {
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
  static const byte enable[] = {ACC_REG_MODE, 0x01};  // Mode: active!
  Wire.beginTransmission(ACC_ADDRESS);
  Wire.write(enable, sizeof(enable));
  Wire.endTransmission();

  orientation_enabled = 1;
}

void
update_loop_variables(void) {
  static char i;

  vcc_filter_table[i] = vcc_current = readVcc();
  vcc_filtered = median_short(vcc_filter_table[0],vcc_filter_table[1],vcc_filter_table[2]);

  temp_filter_table[i] = analogRead(APIN_TEMP);
  temp_filtered = median_short(temp_filter_table[0],temp_filter_table[1],temp_filter_table[2])*(long)vcc_filtered/1024;
  temp_current = temp_filter_table[i]*(long)vcc_current/1024;

  if(++i==3)
    i=0;

  time_current = millis();

  bool prev_value = digitalRead(DPIN_RLED_SW);
  pinMode(DPIN_RLED_SW,  INPUT);
  digitalWrite(DPIN_RLED_SW, 0);
  button_is_pressed = digitalRead(DPIN_RLED_SW);
  if(button_is_pressed) {
    button_released_time = time_current;
    button_pressed_duration = time_current - button_pressed_time;
  } else {
    button_pressed_time = time_current;
    button_released_duration = time_current - button_released_time;
    button_pressed_duration = 0;
  }
  pinMode(DPIN_RLED_SW,  OUTPUT);
  digitalWrite(prev_value, 1);

  short chargeState = analogRead(APIN_CHARGE);

  if (chargeState < 128) {  // Low - charging
    batt_state = BATT_CHARGING;
  } else if (chargeState > 768) {  // High - fully charged.
    batt_state = BATT_CHARGED;
  } else {  // Hi-Z - Not charging, not pulged in.
    batt_state = BATT_DISCHARGING;
  }

  if(orientation_enabled) {
    char acc[3];
    readAccelFiltered(acc);
    angle_roll = atan2(acc[0],acc[2]);
    angle_pitch = atan2(acc[1],sqrt(acc[0]*acc[0]+acc[2]*acc[2]));
  }

  // Check if the accelerometer wants to interrupt
//  byte tapped = 0, shaked = 0;
//  if (!digitalRead(DPIN_ACC_INT)) {
//    Wire.beginTransmission(ACC_ADDRESS);
//    Wire.write(ACC_REG_TILT);
//    Wire.endTransmission(false);       // End, but do not stop!
//    Wire.requestFrom(ACC_ADDRESS, 1);  // This one stops.
//    byte tilt = Wire.read();
//
//    if (time-lastAccTime > 500) {
//      lastAccTime = time;
//
//      tapped = !!(tilt & 0x20);
//      shaked = !!(tilt & 0x80);
//
//      if (tapped) Serial.println("Tap!");
//      if (shaked) Serial.println("Shake!");
//    }
//  }
}


short
readVcc(void) {
  short result;
  // Read 1.1V reference against AVcc
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(2); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Convert
  while (bit_is_set(ADCSRA,ADSC));
  result = ADCL;
  result |= ADCH<<8;
  result = 1126400L / result; // Back-calculate AVcc in mV
  return result;
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

void readAccelFiltered(char *acc_filtered) {
  static char acc[3][3];
  static char i;
  readAccel(acc[i++]);
  if(i==3)
    i=0;
  acc_filtered[0] = median_char(acc[0][0],acc[1][0],acc[2][0]);
  acc_filtered[1] = median_char(acc[0][1],acc[1][1],acc[2][1]);
  acc_filtered[2] = median_char(acc[0][2],acc[1][2],acc[2][2]);
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
short median_short(short a, short b, short c) {
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


void
retrieve_settings(void) {
  byte data[4];
  data[0] = EEPROM.read(0);
  data[1] = EEPROM.read(1);
  data[2] = EEPROM.read(2);
  data[3] = EEPROM.read(3);
  if(data[0] == (byte)~(data[1]^data[2]^data[3] + sizeof(data))) {
    light_mode = data[1];
    if(light_mode) {
      set_amount(data[2]+((unsigned short)data[3]<<8));
    }
    Serial.println("Settings retrieved");
  }
}

void
save_settings(void) {
  byte data[4];
  data[1] = light_mode;
  data[2] = (byte)amount_current;
  data[3] = (amount_current>>8);
  data[0] = ~(data[1]^data[2]^data[3] + sizeof(data));
  EEPROM.write(0,data[0]);
  EEPROM.write(1,data[1]);
  EEPROM.write(2,data[2]);
  EEPROM.write(3,data[3]);
}

void set_brightness(unsigned short b) {
  if(b<=255) {
    digitalWrite(DPIN_DRV_MODE, LOW);
    analogWrite(DPIN_DRV_EN, b);
  } else if(b<=BRT_MAX_LUMEN) {
    analogWrite(DPIN_DRV_MODE, ((long)b-BRT_MED_LUMEN)*255/((long)BRT_MAX_LUMEN-BRT_MED_LUMEN));
    digitalWrite(DPIN_DRV_EN, HIGH);
  } else {
    digitalWrite(DPIN_DRV_EN, HIGH);
    digitalWrite(DPIN_DRV_MODE, HIGH);
  }
}

void
set_amount(unsigned short amount) {
  amount_current = amount_begin = amount_end = amount;
  amount_fade_duration = 0;
  amount_off = 0;

  set_brightness(amount_current);
  pinMode(DPIN_PWR, OUTPUT);
  if(time_current<POWER_ON_BUTTON_THRESHOLD || amount_current==0)
    digitalWrite(DPIN_PWR, LOW);
  else
    digitalWrite(DPIN_PWR, HIGH);
}

void
fade_to_amount(unsigned short amount, unsigned short fade_duration) {
  amount_begin = amount_current;
  amount_end = amount;
  amount_fade_duration = fade_duration;
  amount_fade_start = millis();
  amount_off = 0;
}

PT_THREAD(fade_control_pt_func(struct pt *pt))
{
  static long fade_time;

  PT_BEGIN(pt);

  do {
    PT_YIELD(pt);

    if(amount_flash) {
      analogWrite(DPIN_DRV_EN, 0);
      fade_time =  time_current;
      PT_WAIT_UNTIL(pt, (time_current-fade_time) > (100));
    }

    amount_flash = 0;

    fade_time = time_current - amount_fade_start;

    if(fade_time >= amount_fade_duration) {
      if(amount_current != amount_end) {
        amount_current = amount_end;
      }
    } else {
      amount_current = (((long)amount_end - (long)amount_begin)*fade_time)/amount_fade_duration + amount_begin;
    }

    pinMode(DPIN_PWR, OUTPUT);
    if(time_current<POWER_ON_BUTTON_THRESHOLD || amount_current==0)
      digitalWrite(DPIN_PWR, LOW);
    else
      digitalWrite(DPIN_PWR, HIGH);

    if(amount_current>overtemp_max) {
      amount_current = overtemp_max;
    }

    set_brightness(amount_off?0:amount_current);
  } while(1);

  PT_END(pt);
}

PT_THREAD(button_led_pt_func(struct pt *pt))
{
  PT_BEGIN(pt);

  do {
    switch(batt_state) {
    case BATT_CHARGING:
      {
        const unsigned long pulseTime = time_current;
        // Smoothly pulse the green LED over a two-second interval,
        // as if it were "breathing". This is the charging indication.
        byte pulse = ((pulseTime>>2)&0xFF);
        pulse = ((pulse * pulse) >> 8);
        pulse = ((pulseTime>>2)&0x0100)?0xFF-pulse:pulse;
        analogWrite(DPIN_GLED, pulse);
      }
      break;
    case BATT_CHARGED:
      // Solid green LED.
      digitalWrite(DPIN_GLED, HIGH);
      break;
    case BATT_DISCHARGING:
      // Blink the indicator LED now and then.
      if(overtemp_max<BRT_MAX_LUMEN) {
        digitalWrite(DPIN_RLED_SW, (time_current&0x03FF)>30?LOW:HIGH);
        digitalWrite(DPIN_GLED, LOW);
      } else {
        digitalWrite(DPIN_GLED, (time_current&0x03FF)>10?LOW:HIGH);
        digitalWrite(DPIN_RLED_SW, LOW);
      }
      break;
    }
    PT_YIELD(pt);
  } while(1);

  PT_END(pt);
}

PT_THREAD(power_pt_func(struct pt *pt))
{
  PT_BEGIN(pt);

  overtemp_max = BRT_MAX_LUMEN;
  do {
    // Check the temperature sensor
    {
      static bool low_power_condition = false;
      static uint8_t anti_flicker = 255;

      if(temp_filtered > OVERTEMP_SHUTDOWN) {
        if(amount_current)
          Serial.println("Overheat shutdown!");
        set_amount(0);
        digitalWrite(DPIN_DRV_MODE, LOW);
        digitalWrite(DPIN_DRV_EN, LOW);
        digitalWrite(DPIN_PWR, LOW);
      }

      if(low_power_condition || (vcc_current<VOLTAGE_LOW)) {
        if(!low_power_condition) {
          overtemp_max = amount_current;
        }
        low_power_condition = true;
        if((vcc_filtered < VOLTAGE_LOW) && (overtemp_max > BRT_MIN_LUMEN)) {
          digitalWrite(DPIN_DRV_EN, LOW);
          digitalWrite(DPIN_DRV_MODE, LOW);
          analogWrite(DPIN_DRV_EN,BRT_MIN_LUMEN);
          overtemp_max = overtemp_max/2 + BRT_MIN_LUMEN;
          anti_flicker = 0;
        }

        if(anti_flicker == 255) {
          if((vcc_filtered > VOLTAGE_NOMINAL+50) && (overtemp_max != BRT_MAX_LUMEN))
            overtemp_max++;
        } else {
          anti_flicker++;
        }
      } else {
        if(anti_flicker == 255) {
          if(overtemp_max != BRT_MAX_LUMEN)
            overtemp_max++;
        } else {
          anti_flicker++;
        }
      }

      if(temp_filtered > OVERTEMP_THROTTLE) {
        unsigned short new_max = ((OVERTEMP_THROTTLE+254-temp_filtered)<<2);
        if(overtemp_max>new_max) {
          overtemp_max--;
          anti_flicker = 0;
        }
      }

      static unsigned long lastStatTime;
      if(time_current-lastStatTime > 1000) {
        while(time_current-lastStatTime > 1000)
          lastStatTime += 1000;

        Serial.print("stat: ");
        Serial.print(time_current);

        switch(batt_state) {
        case BATT_CHARGING:
          Serial.print(" [CHARGING]");
          break;
        case BATT_CHARGED:
          Serial.print(" [CHARGED]");
          break;
        case BATT_DISCHARGING:
          Serial.print(" [BATTERY]");
          break;
        }

        Serial.print(" duty=");
        Serial.print(amount_current);

        Serial.print(" Vcc=");
        Serial.print(vcc_filtered);
        Serial.print("mv");

        Serial.print(" Temp=");
        Serial.print((temp_filtered-500)/10.0f);
        Serial.print("C");

        if(orientation_enabled) {
          Serial.print(" roll=");
          Serial.print(angle_roll);

          Serial.print(" pitch=");
          Serial.print(angle_pitch);
        }

        if(overtemp_max<BRT_MAX_LUMEN) {
          Serial.print(" THRTTL=");
          Serial.print(overtemp_max);
        }
        Serial.println("");
        }
    }

    PT_YIELD(pt);
  } while(1);

  PT_END(pt);
}

PT_THREAD(light_momentary_pt_func(struct pt *pt))
{
  // If more than two minutes go by without the user
  // pressing a button, then go ahead and shut down.
  if(button_released_duration > 120000) {
    pinMode(DPIN_PWR, OUTPUT);
    digitalWrite(DPIN_PWR, LOW);
  }

  PT_BEGIN(pt);

  do {
    pinMode(DPIN_PWR, OUTPUT);
    digitalWrite(DPIN_PWR, HIGH);
    amount_off = 0;

    PT_WAIT_UNTIL(pt, !button_is_pressed);

    amount_off = 1;

    PT_WAIT_UNTIL(pt, button_is_pressed);
  } while(amount_current);

  PT_END(pt);
}

PT_THREAD(light_blinky_pt_func(struct pt *pt))
{
  PT_BEGIN(pt);

  PT_WAIT_UNTIL(pt, !button_is_pressed && (button_released_duration > BUTTON_DEBOUNCE));
  button_pressed_duration = 0;

  do {
    amount_off = !((time_current&0xFF)<=64) || !((time_current&0x1F)<=16);
    PT_YIELD(pt);
  } while(amount_current && (button_pressed_duration<BUTTON_DEBOUNCE));

  amount_off = 0;
  PT_WAIT_UNTIL(pt,!button_is_pressed);

  PT_END(pt);
}

PT_THREAD(light_knob_pt_func(struct pt *pt))
{
  static unsigned long lastTime;
  static float lastKnobAngle, knob;

  PT_BEGIN(pt);

  enable_orientation();

  // Set the initial knob value based on our current light bightness level.
  knob = sqrt((float)amount_current/(float)BRT_MAX_LUMEN);

  // Wait for the user to let go of the button.
  PT_WAIT_UNTIL(pt,!button_is_pressed);

  // Wait for a brief moment for any vibrations to stabalize.
  PT_WAIT_FOR_PERIOD(pt,50);

  lastKnobAngle = angle_roll;

  do {
    {
      // Make apparent brightness changes linear by squaring the
      // value and dividing back down into range.  This gives us
      // a gamma correction of 2.0, which is close enough.
      unsigned short bright = (uint16_t)(knob * knob * BRT_MAX_LUMEN);
  
      // Avoid ever appearing off in this mode!
      if (bright < BRT_MIN_LUMEN) bright = BRT_MIN_LUMEN;
  
      if((amount_current != amount_end) || abs((int16_t)(amount_end-bright)) > 4)
        fade_to_amount(bright,100);
    }

    PT_WAIT_FOR_PERIOD(pt,50);

    {
      #define DEG_TO_RAD(x)    ((PI*(x))/180.0f)
      float change = angle_roll - lastKnobAngle;
      lastKnobAngle = angle_roll;
  
      // Don't bother updating our brightness reading if our angle isn't good.
      if(abs(angle_pitch) < DEG_TO_RAD(60)) {
        if (change >  PI) change -= 2.0f*PI;
        if (change < -PI) change += 2.0f*PI;
        knob += change / -7.0f;
        if (knob < 0) knob = 0;
        if (knob > 1) knob = 1;
      }
    }
  } while(amount_current && (button_pressed_duration<BUTTON_DEBOUNCE));

  PT_WAIT_UNTIL(pt,!button_is_pressed);

  PT_END(pt);
}

PT_THREAD(light_pt_func(struct pt *pt))
{
  static unsigned long lastTime;
  static byte level;

  PT_BEGIN(pt);

  // Estimate what the current brighness level is closest to.
  if(amount_current < (BRT_LOW_LUMEN/2)) {
    level = 0;
  } else if(amount_current < (BRT_MED_LUMEN/2)) {
    level = 1;
  } else if(amount_current < (BRT_MAX_LUMEN/2)) {
    level = 2;
  } else {
    level = 3;
  }

  button_released_time = time_current;
  button_released_duration = 0;

  Serial.println("Starting light thread.");
  do {
    PT_WAIT_UNTIL(pt, button_is_pressed && (button_pressed_duration > BUTTON_DEBOUNCE));

    if(!amount_current || (button_released_duration<BUTTON_BRIGHTNESS_THRESHOLD)) {
      level++;
      level &= 3;
      Serial.print("intensity=");
      Serial.println(level);
    } else {
      Serial.println("turning off");
      level = 0;
    }

    switch(level) {
    case 0:
      break;

    case 1:
      fade_to_amount(BRT_LOW_LUMEN,250);
      break;

    case 2:
      fade_to_amount(BRT_MED_LUMEN,500);
      break;

    case 3:
      fade_to_amount(BRT_MAX_LUMEN,500);
      break;
    }

    PT_WAIT_UNTIL(pt, !button_is_pressed && (button_released_duration > BUTTON_DEBOUNCE));

    if(!level)
      fade_to_amount(0,500);
  } while(1);

  PT_END(pt);
}

void
check_serial_port(void)
{
  // Check the serial port
  if(Serial.available()) {
    char c = Serial.read();
    switch(c) {
    case '+':
      if(BRT_MAX_LUMEN-amount_current<BRT_MIN_LUMEN)
        set_amount(BRT_MAX_LUMEN);
      else
        set_amount(amount_current+BRT_MIN_LUMEN);
      break;

    case '-':
      if(amount_current<BRT_MIN_LUMEN)
        set_amount(0);
      else
        set_amount(amount_current-BRT_MIN_LUMEN);
      break;

    case 'X':
    case 'x': set_amount(0); break;
    case 'H':
    case 'h': set_amount(BRT_MAX_LUMEN); amount_off = 0; break;

    case '0': light_mode = 0; amount_off = 0; break;
    case '1': light_mode = 1; amount_off = 0; break;
    case '2': light_mode = 2; amount_off = 0; break;
    case '3': light_mode = 3; amount_off = 0; break;
    case '4': light_mode = 4; amount_off = 0; break;
    case '5': light_mode = 5; amount_off = 0; break;

    case 'R':
      // Let the watchdog reset us.
      Serial.println("Rebooting");
      wdt_enable(WDTO_15MS);
      while(1) { }
      break;

    default:
      break;
    }
  }
}

void
setup(void)
{
  // Set our watchdog to kill us if we don't
  // check in every two seconds.
  wdt_enable(WDTO_2S);

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
  digitalWrite(DPIN_DRV_MODE, LOW);
  digitalWrite(DPIN_DRV_EN,   LOW);
  
  // Initialize serial busses
  Serial.begin(9600);
  Wire.begin();

  button_released_time = millis();
  button_pressed_time = millis();

  update_loop_variables();
  update_loop_variables();
  update_loop_variables();

  // Don't bother loading the settings if we are connected to USB.
  // Only load the settings when we are running from the battery.
  if (batt_state == BATT_DISCHARGING) {
    retrieve_settings();
  }

  Serial.println("Powered up!");
}

void
loop(void)
{
  static unsigned long lastTime;
  unsigned long time = millis();

  // Reset the watchdog timer
  wdt_reset();

  update_loop_variables();

  check_serial_port();

  button_led_pt_func(&button_led_pt);

  power_pt_func(&power_pt);

  fade_control_pt_func(&fade_control_pt);

#define NUMBER_OF_MODES    (4)

  static byte last_mode;
  static byte save_mode;
  if((button_pressed_duration > 2048+1024+512*NUMBER_OF_MODES+1024)) {
    if(save_mode == 1) {
      amount_off = 1;
      save_settings();
      amount_off = 0;
      save_mode = 2;
    }
  } else if((button_pressed_duration > 2048+1024+512*NUMBER_OF_MODES)) {
    amount_off = ((button_pressed_duration/64) & 1);
    if(save_mode == 0) {
      save_mode = 1;
      last_mode = 0;
    }
  } else if((button_pressed_duration > 2048)) {
    byte selected_mode = (button_pressed_duration-2048)/512 + 1;
    amount_off = 0;
    if(selected_mode-1>=light_mode) {
      selected_mode++;
    }
    if(selected_mode > NUMBER_OF_MODES)
      selected_mode = NUMBER_OF_MODES;
    if(selected_mode-1==light_mode)
      selected_mode--;
    if(selected_mode != last_mode) {
      amount_flash = 1;
      last_mode = selected_mode;
      Serial.print("Mode selected: ");
      Serial.println(selected_mode-1);
    }
  } else {
    char thread_status;
    if(save_mode) {
      save_mode = 0;
      amount_off = 0;
      button_pressed_duration = 0;
      button_released_duration = 0;
    }
    if(last_mode) {
      light_mode = last_mode-1;
      last_mode = 0;
      Serial.print("Mode change: ");
      Serial.println(light_mode);
      PT_INIT(&light_pt);
      PT_INIT(&light_momentary_pt);
      PT_INIT(&light_blinky_pt);
      PT_INIT(&light_knob_pt);
    }
    switch(light_mode) {
      default:
      case 0: thread_status = light_pt_func(&light_pt); break;
      case 1: thread_status = light_momentary_pt_func(&light_momentary_pt); break;
      case 2: thread_status = light_blinky_pt_func(&light_blinky_pt); break;
      case 3: thread_status = light_knob_pt_func(&light_knob_pt); break;
    }
    if(!PT_SCHEDULE(thread_status)) {
      light_mode = 0;
      fade_to_amount(0,500);
    }
  }

  return;
}


