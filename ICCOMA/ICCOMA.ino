#include <SPI.h>
#include <Ethernet.h>
#include <Servo.h>
#include <sha256.h>

static char KEY[] = "SudoMakeMeCoffee";
static byte mac[] = { 0xC0, 0xFF, 0xEE, 0xC0, 0xFF, 0xEE };
// Status values
static int S_STANDBY = 0;
static int S_STANDBY_BREW = 1;
static int S_STANDBY_CMD = 2;
static int S_BREWING = 3;
static int S_EXECUTING = 4;
static int S_READY = 5;
// Request type
static int R_INVALID = -1;
static int R_STATUS = 0;
static int R_BREW = 1;
static int R_COMMAND = 2;
static int R_VALIDATE = 3;
// Command type
static int CMD_OPEN_TRAY = 1;
static int CMD_CLOSE_TRAY = 2;
static int CMD_RESET = 3;
// Pinout
static int TRAY_PIN = 4;
static int DRIVE_PIN = 5;
static int WL_A0_PIN = 0;
static int WL_A1_PIN = 1;
static int WL_A2_PIN = 2;
static int WL_GS_PIN = 3;
static int PUMP_PIN = 8;
static int MAKER_PIN = 0;
// PWM Constant
static int TRAY_OPEN = 50;
static int TRAY_CLOSE = 180;
static int DRIVE_FORWARD = 5;
static int DRIVE_STOP = 90;
// Global variable
EthernetServer server = EthernetServer(80);
char buffer[128];
int STATUS = 0;
int CUPS = 0;
int CMD = 0;
char NONCE[9];
int BREWING_STATE = 0;
unsigned long time = 0;
Servo tray, drive;




void answer(EthernetClient *client, char* message) {
  // Most basic HTTP response headers
  client->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  client->write(message);
  client->stop();
}

int getRequestType(char *r) {
  if(strcmp(r, "GET ")) {
    if(strstr(r, " /status") != NULL)
      return R_STATUS; // status request
    else if(strstr(r, " /brew?cups=") != NULL)
      return R_BREW; // brew request
    else if(strstr(r, " /command?cmd=") != NULL)
      return R_COMMAND;
    else if(strstr(r, " /validate?hmac=") != NULL)
      return R_VALIDATE; // validate request
  }
  return R_INVALID; // invalid request
}

int getCups(char *r) {
  char *p = strstr(r, "?cups=");
  if(p != NULL) {
    p += 6;
    return atoi(p);
  }
  return 0;
}

char* getHMAC(char *r) {
  char *h = strstr(r, "?hmac=");
  if(h != NULL) {
    h += 6;
    // check if it's a 32 lowercase hex string
    for(int i = 0; i<32; i++)
      if(h[i] < '0' || (h[i] > '9' && h[i] < 'a') || h[i] > 'f')
        return NULL;
  }
  return h;
}

int getCmd(char *r) {
  char *p = strstr(r, "?cmd=");
  if(p != NULL) {
    p += 5;
    return atoi(p);
  }
  return 0;
}

void longToHex(long n, char *s) {
  int i, j;
  ltoa(n, s, 16);
  // Pad the number with zero in front 
  for(i = 0; s[i] != '\0'; i++);
  for(j = 8; i>=0 && j>=0; j--, i--)
    s[j] = s[i];
  for(; j>=0; j--)
    s[j]= '0';
}

void answerStatus(EthernetClient *client)
{
  if(STATUS < S_BREWING) 
    answer(client, "standby");
  else if(STATUS == S_BREWING)
    answer(client, "brewing");
  else if(STATUS == S_EXECUTING)
    answer(client, "executing");
  else if(STATUS == S_READY)
    answer(client, "ready");
}

int getWaterLevel() {
  if(digitalRead(WL_GS_PIN) == LOW)
    return ((!digitalRead(WL_A2_PIN))<<2) 
           + ((!digitalRead(WL_A1_PIN))<<1) 
           + (!digitalRead(WL_A0_PIN)) + 1;
  return 0;
}

void answerNonce(EthernetClient *client) {
  longToHex(random(), NONCE);
  answer(client, NONCE);
}

void handleClient(EthernetClient *client) {
  int i = 0;
  bool got_line = false;
  // Try to read the first line of text send by
  // the client, which should contain the GET 
  // request. Everything else is ignored.
  while(!got_line && client->connected())
  {
    while(!client->available());
    buffer[i] = client->read();
    if(buffer[i] == '\n' || i == 126)
      got_line = true;
    i++;
  }
  buffer[i] = '\0';
  int type = getRequestType(buffer);
  bool invalid = false;
  if(type == R_STATUS)
    answerStatus(client);
  else if(type == R_BREW && STATUS < S_BREWING) {
    int cups = getCups(buffer);
    // we only brew 1 to 7 cups
    if(cups >= 1 && cups <= 7){
      CUPS = cups;
      STATUS = S_STANDBY_BREW;
      answerNonce(client);        
    }
    else
      invalid = true;
  }
  else if(type == R_COMMAND && STATUS < S_BREWING) {
    int cmd = getCmd(buffer);
    if(cmd>0 && cmd<4) {
      CMD = cmd;
      STATUS = S_STANDBY_CMD;
      answerNonce(client);
    }
  }
  else if(type == R_VALIDATE && (STATUS == S_STANDBY_BREW || STATUS == S_STANDBY_CMD)) {
    char *hmac = getHMAC(buffer);
    if(hmac != NULL) {
      uint8_t *hash;
      // compute the expected HMAC
      Sha256.init();
      Sha256.print(KEY);
      Sha256.print(NONCE);
      if(STATUS == S_STANDBY_BREW) {
        Sha256.print("cups=");
        Sha256.print(CUPS);
      } 
      else if(STATUS == S_STANDBY_CMD) {
        Sha256.print("cmd=");
        Sha256.print(CMD);
      }
      hash = Sha256.result();
      // now let's compare both
      char map[] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
      for(i = 0; i<32; i++)
      {
        if(map[hash[i]>>4] != hmac[2*i] || map[hash[i]&0xF] != hmac[2*i+1])
          break;
      }
      if(i == 32) // hmac is correct
      {
        if(STATUS == S_STANDBY_BREW) {
          BREWING_STATE = 0;
          STATUS = S_BREWING;
        }
        else if(STATUS == S_STANDBY_CMD) {
          STATUS = S_EXECUTING;
        }
        answer(client, "ok");
      }
      else // oops ...
        invalid = true;
    }
    else
      invalid = true;
  }
  else
    invalid = true;
  if(invalid)
    answer(client, "invalid");
}

void brew() {
  if(BREWING_STATE == 0) {
    time = millis();
    tray.write(TRAY_OPEN);
    BREWING_STATE = 1;
  }
  else if(BREWING_STATE == 1) {
    if(millis()-time>1000) {
      drive.write(DRIVE_FORWARD);
      time = millis();
      BREWING_STATE = 2;
      digitalWrite(PUMP_PIN, HIGH);
    }
  }
  else if(BREWING_STATE == 2) {
    if(getWaterLevel()>CUPS)
      digitalWrite(PUMP_PIN, LOW);
    if((millis()-time)>CUPS*180000) {
      BREWING_STATE = 3;
      digitalWrite(PUMP_PIN, LOW);
      drive.write(DRIVE_STOP);
      tray.write(TRAY_CLOSE);
      digitalWrite(MAKER_PIN, HIGH);
    }
  }
  else if(BREWING_STATE == 3) {
    if(getWaterLevel()<1) {
      STATUS = S_READY;
    }
  }
}

void execute() {
  if(CMD == CMD_OPEN_TRAY)
    tray.write(TRAY_OPEN);
  else if(CMD == CMD_CLOSE_TRAY)
    tray.write(TRAY_CLOSE);
  else if(CMD == CMD_RESET) {
    drive.write(DRIVE_STOP);
    tray.write(TRAY_CLOSE);
    digitalWrite(PUMP_PIN, LOW);
    digitalWrite(MAKER_PIN, LOW);
  }
  STATUS = 0;
}

void setup() {
  Serial.begin(9600);
  Serial.println("Hello.");
  if(Ethernet.begin(mac) == 0) {
    Serial.println("No DHCP lease.");
    while(true);
  }
  server.begin();
  Serial.print("Server initialised at ");
  Serial.print(Ethernet.localIP());
  Serial.println(" on port 80.");
  // The 4 water level sensor pins
  pinMode(WL_A0_PIN,INPUT_PULLUP);
  pinMode(WL_A1_PIN,INPUT_PULLUP);
  pinMode(WL_A2_PIN,INPUT_PULLUP);
  pinMode(WL_GS_PIN,INPUT_PULLUP);
  // The 2 relay pins
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(MAKER_PIN, OUTPUT);
  // The 2 servomotor pins
  tray.attach(TRAY_PIN);
  drive.attach(DRIVE_PIN);
  tray.write(TRAY_CLOSE);
  drive.write(DRIVE_STOP);
  // Seed the the random pool with something difficult to predict
  randomSeed(micros());
}

void loop() {
  EthernetClient client = server.available();
  if(client)
    handleClient(&client);
  
  if(STATUS == S_BREWING)
    brew();
  else if(STATUS == S_EXECUTING)
    execute();
}
