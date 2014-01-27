#include <SPI.h>
#include <Ethernet.h>
#include <Servo.h>
#include <sha256.h>

char KEY[] = "SudoMakeMeCoffee";
byte mac[] = { 0xC0, 0xFF, 0xEE, 0xC0, 0xFF, 0xEE };
EthernetServer server = EthernetServer(80);
char buffer[128];
int STATUS = 0;
int CUPS = 0;
char NONCE[9];
int TIMER2_DEM = 0;
int BREWING_STATE = 0;
unsigned long time = 0;
Servo tray, drive;




void answer(EthernetClient *client, char* message)
{
  // Most basic HTTP response headers
  client->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
  client->write(message);
  client->stop();
}

int getRequestType(char *r)
{
  if(strcmp(r, "GET ")){
    if(strstr(r, " /status") != NULL)
      return 0; // status request
    else if(strstr(r, " /brew?cups=") != NULL)
      return 1; // brew request
    else if(strstr(r, " /validate?hmac=") != NULL)
      return 2; // validate request
    else if(strstr(r, " /reset") != NULL)
      return 3;
    else if(strstr(r, " /waterlevel") != NULL)
      return 4;
  }
  return -1; // invalid request
}

int getCups(char *r)
{
  char *p = strstr(r, "?cups=");
  if(p != NULL)
  {
    p += 6;
    return atoi(p);
  }
  return 0;
}

char* getHMAC(char *r)
{
  char *h = strstr(r, "?hmac=");
  if(h != NULL)
  {
    h += 6;
    // check if it's a 32 lowercase hex string
    for(int i = 0; i<32; i++)
      if(h[i] < '0' || (h[i] > '9' && h[i] < 'a') || h[i] > 'f')
        return NULL;
  }
  return h;
}

void longToHex(long n, char *s)
{
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
  if(STATUS < 2)
    answer(client, "standby");
  else if(STATUS == 2)
    answer(client, "brewing");
  else if(STATUS == 3)
    answer(client, "ready");
  else if(STATUS == 4)
    answer(client, "error");
}

int getWaterLevel()
{
  if(digitalRead(3) == LOW)
    return ((!digitalRead(2))<<2) + ((!digitalRead(1))<<1) + (!digitalRead(0)) + 1;
  return 0;
}

void setup() {
  Serial.begin(9600);
  Serial.println("Hello.");
  if(Ethernet.begin(mac) == 0)
  {
    Serial.println("No DHCP lease.");
    while(true);
  }
  server.begin();
  Serial.print("Server initialised at ");
  Serial.print(Ethernet.localIP());
  Serial.println(" on port 80.");
  pinMode(0,INPUT_PULLUP);
  pinMode(1,INPUT_PULLUP);
  pinMode(2,INPUT_PULLUP);
  pinMode(3,INPUT_PULLUP);
  pinMode(8, OUTPUT);
  pinMode(9, OUTPUT);
  tray.attach(4);
  drive.attach(5);
  tray.write(180);
  drive.write(90);
  randomSeed(micros());
}

void loop() {
  EthernetClient client = server.available();
  if(client)
  {
    int i = 0;
    bool got_line = false;
    // Try to read the first line of text send by
    // the client, which should contain the GET 
    // request. Everything else is ignored.
    while(!got_line && client.connected())
    {
      while(!client.available());
      buffer[i] = client.read();
      if(buffer[i] == '\n' || i == 126)
        got_line = true;
      i++;
    }
    buffer[i] = '\0';
    int type = getRequestType(buffer);
    bool invalid = false;
    if(type == 0)
      answerStatus(&client);
    else if(type == 1 && STATUS < 2)
    {
      int cups = getCups(buffer);
      // we only brew 1 to 7 cups
      if(cups >= 1 && cups <= 7)
      {
        CUPS = cups;
        STATUS = 1;
        // generate a new nonce and send it 
        // to the client
        longToHex(random(), NONCE);
        answer(&client, NONCE);
      }
      else
        invalid = true;
    }
    else if(type == 2 && STATUS == 1)
    {
      char *hmac = getHMAC(buffer);
      if(hmac != NULL)
      {
        uint8_t *hash;
        // compute the expected HMAC
        Sha256.init();
        Sha256.print(KEY);
        Sha256.print(NONCE);
        Sha256.print("cups=");
        Sha256.print(CUPS);
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
          BREWING_STATE = 0;
          STATUS = 2;
          answer(&client, "ok");
        }
        else // oops ...
          invalid = true;
      }
      else
        invalid = true;
    }
    else if(type == 3) {
      drive.write(90);
      tray.write(180);
      digitalWrite(8, LOW);
      digitalWrite(9, LOW);
      STATUS = 0;
      answer(&client, "ok");
    }
    else if(type == 4)
    {      
      char waterlevel[8];
      itoa(getWaterLevel(), waterlevel, 10);
      answer(&client, waterlevel);
    }
    else
      invalid = true;
    if(invalid)
      answer(&client, "invalid");
  }
  if(STATUS == 2)
  {
    if(BREWING_STATE == 0) {
      time = millis();
      tray.write(60);
      BREWING_STATE = 1;
    }
    else if(BREWING_STATE == 1) {
      if(millis()-time>1000) {
        drive.write(5);
        time = millis();
        BREWING_STATE = 2;
        digitalWrite(8, HIGH);
      }
    }
    else if(BREWING_STATE == 2) {
      if(getWaterLevel()>CUPS)
        digitalWrite(8, LOW);
      if((millis()-time)>CUPS*180000) {
        BREWING_STATE = 3;
        digitalWrite(8, LOW);
        drive.write(90);
        tray.write(180);
        digitalWrite(9, HIGH);
      }
    }
    else if(BREWING_STATE == 3) {
      if(getWaterLevel()<1) {
        STATUS = 3;
      }
    }
  }
}
