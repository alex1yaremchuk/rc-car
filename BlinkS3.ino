#define STBY 7
#define AIN1 5
#define AIN2 6
#define PWMA 4
#define BIN1 8
#define BIN2 9
#define PWMB 10

int speedVal = 180;

// текущее состояние движения
int dirA = 0; // -1 back, 0 stop, +1 fwd
int dirB = 0;

void apply() {
  // A
  if (dirA == 0) {
    ledcWrite(PWMA, 0);
  } else {
    digitalWrite(AIN1, dirA > 0);
    digitalWrite(AIN2, dirA < 0);
    ledcWrite(PWMA, speedVal);
  }
  // B
  if (dirB == 0) {
    ledcWrite(PWMB, 0);
  } else {
    digitalWrite(BIN1, dirB > 0);
    digitalWrite(BIN2, dirB < 0);
    ledcWrite(PWMB, speedVal);
  }

  Serial.print("dirA="); Serial.print(dirA);
  Serial.print(" dirB="); Serial.print(dirB);
  Serial.print(" speed="); Serial.println(speedVal);
}

void setup() {
  Serial.begin(115200);

  pinMode(STBY, OUTPUT);
  pinMode(AIN1, OUTPUT); pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT); pinMode(BIN2, OUTPUT);

  digitalWrite(STBY, HIGH);

  ledcAttach(PWMA, 1000, 8);
  ledcAttach(PWMB, 1000, 8);

  Serial.println("Controls: w/a/s/d, x=stop, +/- speed");
  apply();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();

  if (c == 'w') { dirA = +1; dirB = +1; apply(); }
  if (c == 's') { dirA = -1; dirB = -1; apply(); }
  if (c == 'a') { dirA = -1; dirB = +1; apply(); }
  if (c == 'd') { dirA = +1; dirB = -1; apply(); }
  if (c == 'x') { dirA = 0;  dirB = 0;  apply(); }

  if (c == '+') { speedVal = min(255, speedVal + 20); apply(); }
  if (c == '-') { speedVal = max(0, speedVal - 20);  apply(); }
}
