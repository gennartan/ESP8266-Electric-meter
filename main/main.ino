// Reads off P1 port and uploads to Thingspeak
// Use SPIFFS to store first values of the day, set Flash size to something with SPIFFS, e.g. 4M (1M SPIFFS)

#include <FS.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ArduinoMqttClient.h>
#include <cerrno>


// Set Wifi settings here
#define WLANSSID ""
#define WLANPWD ""

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

const char broker[] = "192.168.1.35";
int        port     = 1883;

// Set the amount of debugging detail
#define DEBUG_LEVEL 3
#define DEBUG_ERROR 1
#define DEBUG_MIN_INFO 2
#define DEBUG_MAX_INFO 3
#define DEBUG_OUT(level, fmt, ...) if(DEBUG_LEVEL>=level) Serial.printf_P( (PGM_P)PSTR(fmt), ## __VA_ARGS__ )

// Time between Thingspeak updates (in ms)
#define SEND_INTERVAL 60000
// Time between reads of the P1 port (in ms)
#define UPDATE_INTERVAL 10000

// Number of failed Wifi connections that lead to a reset
#define FAILED_WIFI_RESET 10
#define FAILED_METER_CONNECT_RESET 15


#define MAXLINELENGTH 128 // longest normal line is 47 char (+3 for \r\n\0)
char dataline[MAXLINELENGTH];

// For valid telegram, whether to check phase current/voltages
#define CHECK_PHASE_INFO false

struct EnergyDataSmall: public Printable {

  size_t printTo(Print& p) const {
    size_t r = 0;
    r += p.printf("Tarif: %s, timestamp: %s", isPeak?"peak":"off peak", validTimestamp?asctime(&timestamp): "null\n");
    r += p.printf("Consumption: peak: %.3f, off peak: %.3f\n", peakConsumption, offPeakConsumption);
    r += p.printf("Injection: peak: %.3f, off peak: %.3f\n", peakInjection, offPeakInjection);
    return r;
  }

  bool isValid() const {
    bool ch1=validTimestamp&&peakConsumption>=0.0&&offPeakConsumption>=0.0&&peakInjection>=0.0&&offPeakInjection>=0.0;
    return ch1;
  }

  void writeReferenceDataSPIFFS() {
    if(SPIFFS.begin()) {
      File dataFile = SPIFFS.open("/refdata", "w");
      if(dataFile) {
        DEBUG_OUT(2, "Writing reference data to SPIFFS\n");
        dataFile.printf("%04d%02d%02d%02d%02d%02d\n%.3f\n%.3f\n%.3f\n%.3f\n",
        timestamp.tm_year, timestamp.tm_mon, timestamp.tm_mday,
        timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec,
        peakConsumption, offPeakConsumption, 
        peakInjection, offPeakInjection);
        dataFile.close();
      } else {
        DEBUG_OUT(0, "Failed to open data file for writing\n");
      }
      SPIFFS.end();
    } else {
      DEBUG_OUT(0, "Failed to mount SPIFFS\n");
    }
  }

  void readReferenceDataSPIFFS() {
    if(SPIFFS.begin()) {
      File dataFile = SPIFFS.open("/refdata", "r");
      if(dataFile) {
        char dataline[20];
        if(dataFile.available()) validTimestamp=readDateFromFile(dataFile, dataline, "%04d%02d%02d%02d%02d%02d", &timestamp);
        if(dataFile.available()) readValueFromFile(dataFile, dataline, "%lf", &peakConsumption);
        if(dataFile.available()) readValueFromFile(dataFile, dataline, "%lf", &offPeakConsumption);
        if(dataFile.available()) readValueFromFile(dataFile, dataline, "%lf", &peakInjection);
        if(dataFile.available()) readValueFromFile(dataFile, dataline, "%lf", &offPeakInjection);
        DEBUG_OUT(2, "Reading reference data to SPIFFS, valid: %s\n", isValid()?"true":"false");
      } else {
        DEBUG_OUT(0, "Data file does not exist\n");
      }
      SPIFFS.end();
    } else {
      DEBUG_OUT(0, "Failed to mount SPIFFS\n");
    }
  }

  static bool readValueFromFile(File& file, char* dataline, const char* format, double* result) {
    memset(dataline, 0, 20);
    size_t len=file.readBytesUntil('\n', dataline, 20);
    dataline[len]=0;
    int n=sscanf(dataline, format, result);
    return n>0;
  }

  static bool readDateFromFile(File& file, char* dataline, const char* format, tm* timestamp) {
    memset(dataline, 0, 20);
    size_t len=file.readBytesUntil('\n', dataline, 20);
    dataline[len]=0;
    int n=sscanf(dataline, format, &timestamp->tm_year, &timestamp->tm_mon, &timestamp->tm_mday, &timestamp->tm_hour, &timestamp->tm_min, &timestamp->tm_sec);
    return n==6;
  }

  bool isPeak=true;
  
  double peakConsumption=-1.0;
  double offPeakConsumption=-1.0;
  double peakInjection=-1.0;
  double offPeakInjection=-1.0;

  tm timestamp;
  bool validTimestamp=false;
  
};


struct EnergyData: public EnergyDataSmall {
  bool isValid(bool checkPhaseInfo=false) {
    bool ch1=currentConsumption>=0.0&&currentInjection>=0.0;
    bool ch2=phase1Voltage>=0.0&&phase2Voltage>=0.0&&phase3Voltage>=0.0&&phase1Current>=0.0&&phase2Current>=0.0&&phase3Current>=0.0;
    return EnergyDataSmall::isValid()&&ch1&&(ch2||!checkPhaseInfo);
  }

  size_t printTo(Print& p) const {
    size_t r=EnergyDataSmall::printTo(p);
    r += p.printf("Current: consumption: %.3f, injection: %.3f\n", currentConsumption, currentInjection);
    r += p.printf("Phase voltages: %.1f, %.1f, %.1f\n", phase1Voltage, phase2Voltage, phase3Voltage);
    r += p.printf("Phase current: %.0f, %.0f, %.0f\n", phase1Current, phase2Current, phase3Current);
    return r;
  }

  void sendMQTT() {
    /* 
    handleMeterTag(dataline, "1-0:1.8.1", result.peakConsumption);
    handleMeterTag(dataline, "1-0:1.8.2", result.offPeakConsumption);
    handleMeterTag(dataline, "1-0:2.8.1", result.peakInjection);
    handleMeterTag(dataline, "1-0:2.8.2", result.offPeakInjection);
    */
    char topic181[] = "electricMeter/1.8.1";
    char topic182[] = "electricMeter/1.8.2";
    char topic281[] = "electricMeter/2.8.1";
    char topic282[] = "electricMeter/2.8.2";

    if (mqttClient.connected()) {
      mqttClient.beginMessage(topic181);
      mqttClient.print(peakConsumption);
      mqttClient.endMessage();

      mqttClient.beginMessage(topic182);
      mqttClient.print(offPeakConsumption);
      mqttClient.endMessage();

      mqttClient.beginMessage(topic281);
      mqttClient.print(peakInjection);
      mqttClient.endMessage();

      mqttClient.beginMessage(topic282);
      mqttClient.print(offPeakInjection);
      mqttClient.endMessage();
    }
  }
  
  double currentConsumption=-1.0;
  double currentInjection=-1.0;

  double phase1Voltage=-1.0;
  double phase2Voltage=-1.0;
  double phase3Voltage=-1.0; 

  double phase1Current=-1.0;
  double phase2Current=-1.0;
  double phase3Current=-1.0;

  
};

ESP8266WebServer webServer(80);

// Data of beginning of the day, used to calculate daily energy values
EnergyDataSmall referenceData;
EnergyDataSmall previousData;
EnergyDataSmall referenceData15;

// Current energy data

EnergyData currentData;

unsigned long lastUpdateTime=0;
unsigned long lastSendTime=0;

unsigned long uptimeEpoch=0;

unsigned int failedWifi=0;
unsigned int failedWifiConsecutive=0;

unsigned int failedMeterConnect=0;
unsigned int failedMeterConnectConsecutive=0;
 
void setup() {
  //Serial Port begin
  Serial.begin(115200);
  // Buffer size enough to remember one telegram
  Serial.setRxBufferSize(650);
  //Define inputs and outputs
  DEBUG_OUT(2, "Start serial port meter\n");
  connectWifi(true);

  // Read reference data from SPIFFS

  referenceData.readReferenceDataSPIFFS();
  DEBUG_OUT(2, "Reference data valid: %s\n", referenceData.isValid()?"true":"false");

  // Setup web server
  webServer.on("/", serveHtmlPage);
  webServer.onNotFound(serveHtmlPage);
  webServer.begin();

  // Setup MQTT client
  mqttClient.connect(broker, port);
  
  DEBUG_OUT(2, "Setup finished\n");
}

double getValue(const char* line, char endChar='*') {
  char* st=strrchr(line, '(');
  if(!st) return -1.0;
  char* en=strrchr(line, endChar);
  if(!en||en-st<=0) return -1.0;
  char res[16];
  memset(res, 0, sizeof(res));
  if(strncpy(res, st+1, en-st-1)) {
    char * e;
    errno = 0;
    double val = std::strtod(res, &e);
    if (*e != '\0'||errno!=0) return -1.0;
    return val;
  }
  return -1.0;
}

bool getDateValue(const char* line, tm& result) {
  char* st=strchr(line, '(');
  if(!st) return false;
  // W: daylight savings: off (Winter), S: daylight savings: on (Summer)
  char* en=strchr(line, 'W');
  if(!en) { 
    en=strchr(line, 'S');
    result.tm_isdst=1;
  } else {
    result.tm_isdst=0;
  }
  if(!en||en-st<=0) return false;
  char res[13];
  memset(res, 0, sizeof(res));
  if(strncpy(res, st+1, en-st-1)) {
    int n=sscanf(res, "%2d%2d%2d%2d%2d%2d", &result.tm_year, &result.tm_mon, &result.tm_mday, &result.tm_hour, &result.tm_min, &result.tm_sec);
    // year in tm: years after 1900
    result.tm_year+=100;
    // month in tm: 0-11
    result.tm_mon--;
    // Fill in the rest of the values: tm_wday, tm_yday
    mktime(&result);
    return n==6;
  }
  return false;
}


bool handleMeterTag(const char* line, const char* tag, double& val, char endChar='*') {
  if (strncmp(line, tag, strlen(tag))==0) {
    val=getValue(line, endChar);
    return true;
  }
  return false;
}

bool handleDateValue(const char* line, const char* tag, tm& timestamp, bool& validTimestamp) {
  if (strncmp(line, tag, strlen(tag))==0) {
    validTimestamp=getDateValue(line, timestamp);
    return true;
  }
  return false;
}


bool readTelegram(EnergyData& result, unsigned long  timeOut, bool checkCRC) {
  unsigned long startTime=millis();
  bool timedOut = false;
  while(!timedOut) {
    timedOut = millis()-startTime < timeOut ? false : true;
    DEBUG_OUT(2, "Timed out ? %s\n", timedOut ? "True" : "False");
    if (timedOut) {
      return true;
    }
    bool telegramStarted=false;
    unsigned int currentCRC=0;
    while(Serial.available()) {
      memset(dataline, 0, sizeof(dataline));
      int len = Serial.readBytesUntil('\n', dataline, MAXLINELENGTH);
      dataline[len] = '\n';
      dataline[len+1] = 0;
      DEBUG_OUT(3, "Line: len: %d %s", len+1, dataline);
      //Wait until start line
      char* thisStart=strchr(dataline,'/');
      if(thisStart) DEBUG_OUT(2, "Start of new telegram, begin reading\n");
      if(telegramStarted||thisStart) {
        telegramStarted=true;

        // Check whether we are at the end of the telegram
        if(strchr(dataline,'!')) {
          DEBUG_OUT(2, "End of telegram\n");
          currentCRC=CRC16(currentCRC,(unsigned char*)dataline, 1);
          char messageCRC[5];
          strncpy(messageCRC, dataline+1, 4);
          messageCRC[4]=0;
          unsigned int messageCRCint=strtol(messageCRC, NULL, 16);
          bool validCRCFound = (messageCRCint==currentCRC);
          DEBUG_OUT(2, "Message CRC: hex: %s, int: %u\n", messageCRC, messageCRCint);
          DEBUG_OUT(2, "Calculated CRC: hex: %x, int: %u\n", currentCRC, currentCRC);
          DEBUG_OUT(2, "Valid CRC: %d\n", validCRCFound);
          // If we don't have to check the CRC or the CRC is valid we are done, otherwise we have to start all over
          if(!checkCRC||validCRCFound) return true; 
          else {
            telegramStarted=false;
            continue;
          }
        }

        currentCRC=CRC16(currentCRC,(unsigned char *)dataline, len+1);
        if(thisStart) continue;

        handleDateValue(dataline, "0-0:1.0.0", result.timestamp, result.validTimestamp);
        handleMeterTag(dataline, "1-0:1.8.1", result.peakConsumption);
        handleMeterTag(dataline, "1-0:1.8.2", result.offPeakConsumption);
        handleMeterTag(dataline, "1-0:2.8.1", result.peakInjection);
        handleMeterTag(dataline, "1-0:2.8.2", result.offPeakInjection);
        double peakValue=-1.0;
        handleMeterTag(dataline, "0-0:96.14.0", peakValue, ')');
        if((int)peakValue!=-1) result.isPeak=(int)peakValue==1;
        handleMeterTag(dataline, "1-0:1.7.0", result.currentConsumption);
        handleMeterTag(dataline, "1-0:2.7.0", result.currentInjection);
        handleMeterTag(dataline, "1-0:32.7.0", result.phase1Voltage);
        handleMeterTag(dataline, "1-0:52.7.0", result.phase2Voltage);
        handleMeterTag(dataline, "1-0:72.7.0", result.phase3Voltage);
        handleMeterTag(dataline, "1-0:31.7.0", result.phase1Current);
        handleMeterTag(dataline, "1-0:51.7.0", result.phase2Current);
        handleMeterTag(dataline, "1-0:71.7.0", result.phase3Current);
      }
    }
    delay(500);
  }
  return false;
}

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len) {
  for(int pos = 0; pos < len; pos++) {
    crc ^= (unsigned int)buf[pos];    // XOR byte into least sig. byte of crc

    for(int i = 8; i != 0; i--) {    // Loop over each bit
      if((crc & 0x0001) != 0) {      // If the LSB is set
        crc >>= 1;                    // Shift right and XOR 0xA001
        crc ^= 0xA001;
      } else                            // Else LSB is not set
        crc >>= 1;                    // Just shift right
    }
  }
  return crc;
}


 
void loop() {

  unsigned long currentTime=millis();
  if(currentTime-lastUpdateTime>UPDATE_INTERVAL) {
      if(currentTime<lastUpdateTime) {
        // unsigned long has rolled over, congratulations device is running for 49 days
        uptimeEpoch++;
      }
      lastUpdateTime=currentTime;
      EnergyData newData=EnergyData();
      readTelegram(newData, 5000, false);
      bool newValid=newData.isValid(CHECK_PHASE_INFO);
      DEBUG_OUT(2, "Telegram valid: %s\n", newValid?"true":"false");
      if(newValid) {
        failedMeterConnectConsecutive=0;
        currentData=newData;
        if(referenceData.isValid()) {
          // If we started a new day, previous data becomes the new reference
          DEBUG_OUT(2, "Valid referenceData\n");
          if(previousData.isValid()&&currentData.timestamp.tm_mday!=previousData.timestamp.tm_mday) {
            DEBUG_OUT(1, "New day, updating reference data\n");
            referenceData=previousData;
            referenceData.writeReferenceDataSPIFFS();
          }
  
          if(previousData.isValid()&&currentData.timestamp.tm_min/15!=previousData.timestamp.tm_min/15) {
            DEBUG_OUT(1, "Started new quarter of an hour\n");
            referenceData15=previousData;
          }
  
          if(currentTime-lastSendTime>SEND_INTERVAL) {
            lastSendTime=currentTime;
            webServer.handleClient();
            yield();
            currentData.sendMQTT();
  
            char electricityData[130];
            sprintf(electricityData,
            "[{\"delta_t\":\"0\","
            "\"field1\":\"%.3f\","
            "\"field2\":\"%.3f\","
            "\"field3\":\"%.3f\","
            "\"field4\":\"%.3f\","
            "\"field5\":\"%.f\","
            "\"field6\":\"%.f\"}]",
            currentData.peakConsumption-referenceData.peakConsumption, currentData.offPeakConsumption-referenceData.offPeakConsumption, 
            currentData.peakInjection-referenceData.peakInjection, currentData.offPeakInjection-referenceData.offPeakInjection,
            currentData.currentConsumption*1000, currentData.currentInjection*1000);
            // Send data here
            // TODO
            DEBUG_OUT(3, "%s", electricityData);
  
          }
        } else {
          // There is no beginning of the day reference yet, current data becomes the reference, this happens at startup
          DEBUG_OUT(2, "Invalid referenceData, initialize with currentData\n");
          referenceData=currentData;
          referenceData.writeReferenceDataSPIFFS();
        }
        previousData=currentData;
      } else {
        failedMeterConnect++;
        failedMeterConnectConsecutive++;
      }
  }
  wdt_reset();
  yield();

  if(WiFi.status()!=WL_CONNECTED) { // Lost connection
    failedWifi++;
    failedWifiConsecutive++;
    connectWifi(false);
  }
  if(failedWifiConsecutive>FAILED_WIFI_RESET||failedMeterConnectConsecutive>FAILED_METER_CONNECT_RESET) {
    ESP.restart();
  }
  if(WiFi.status()==WL_CONNECTED) {
    failedWifiConsecutive=0;
    webServer.handleClient();

    if (!mqttClient.connected()) {
      DEBUG_OUT(3, "Reconnect to broker\n");
      mqttClient.connect(broker, port);
    }
  }
}

int connectWifi(bool firstConnect) {
  unsigned long startwifi=micros();
  unsigned int retry_count = 0;

  if(!firstConnect) {
    WiFi.reconnect();
  } else {
    WiFi.mode(WIFI_STA);
    //A static IP address could be handy if you want to use the webinterface.  
    //To use a static IP address uncomment the line below and set the variables staticIP, dns, gateway, subnet
    //WiFi.config(staticIP, dns, gateway, subnet);
    WiFi.begin(WLANSSID, WLANPWD); // Start WiFI
  }

  DEBUG_OUT(2, "%sonnecting to %s\n", firstConnect?"C":"Rec", WLANSSID);

  while ((WiFi.status() != WL_CONNECTED) && (retry_count < 40)) {
    delay(500);
    DEBUG_OUT(2, ".");
    retry_count++;
  }
  int success=WiFi.status();
  if(success==WL_CONNECTED) {
    DEBUG_OUT(2, "WiFi connected\nIP address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DEBUG_OUT(DEBUG_ERROR, "Failed to connect\n");
  }
  DEBUG_OUT(2, "Number of tries: %d\n", retry_count);
  DEBUG_OUT(2, "Connecting time (microseconds): %lu\n", micros()-startwifi);

  return success;
}

const char htmlTemplate[] PROGMEM = "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
<title>Energy consumption and injection values</title>\n\
<style>\n\
.r { text-align: right }\
</style>\n\
</head>\n\
<body>\n\
<table cellspacing='0' border='1' cellpadding='5'>\n\
<tr><td>Timestamp</td><td class='r'>%s</td></tr>\n\
<tr><td>Tarif</td><td class='r'>%s</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td>Peak consumption</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Off peak consumption</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Peak injection</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Off peak injection</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td>Current consumption</td><td class='r'>%.0f&nbsp;W</td></tr>\n\
<tr><td>Current injection</td><td class='r'>%.0f&nbsp;W</td></tr>\n\
<tr><td>15 min consumption</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td>Phase 1 voltage</td><td class='r'>%.1f&nbsp;V</td></tr>\n\
<tr><td>Phase 2 voltage</td><td class='r'>%.1f&nbsp;V</td></tr>\n\
<tr><td>Phase 3 voltage</td><td class='r'>%.1f&nbsp;V</td></tr>\n\
<tr><td>Phase 1 current</td><td class='r'>%.0f&nbsp;A</td></tr>\n\
<tr><td>Phase 2 current</td><td class='r'>%.0f&nbsp;A</td></tr>\n\
<tr><td>Phase 3 current</td><td class='r'>%.0f&nbsp;A</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td>Total peak consumption</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Total off peak consumption</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Total peak injection</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td>Total off peak injection</td><td class='r'>%.3f&nbsp;kWh</td></tr>\n\
<tr><td colspan='2'>&nbsp;</td></tr>\n\
<tr><td>Uptime</td><td class='r'>%lud&nbsp;%uh&nbsp;%um&nbsp;%us</td></tr>\n\
<tr><td>Failed Wifi connection count</td><td class='r'>%u</td></tr>\n\
<tr><td>Failed meter connection count</td><td class='r'>%u</td></tr>\n\
</table>\n\
</body>\n\
</html>\r\n";

void serveHtmlPage() {
  char timestampStr[21]="-";
  if(currentData.validTimestamp) strftime(timestampStr, 21, "%F %T", &currentData.timestamp); 

  double consumption15=-1.0;
  if(referenceData15.isValid()) {
    consumption15=currentData.isPeak?currentData.peakConsumption-referenceData15.peakConsumption:currentData.offPeakConsumption-referenceData15.offPeakConsumption;
  }

  unsigned long currentTime=millis();
  unsigned long maxlong=static_cast<unsigned long>(-1);
  unsigned long uptimeDays=static_cast<unsigned long>(uptimeEpoch*(maxlong/24.0/3600.0/1000.0)+(currentTime+uptimeEpoch)/24.0/3600.0/1000.0);
  unsigned int uptimeHours=static_cast<unsigned int>(static_cast<unsigned long>((maxlong%86400000L+1)/3600.0/1000.0*uptimeEpoch+(currentTime%86400000L)/3600.0/1000.0)%24);
  unsigned int uptimeMins=static_cast<unsigned int>(static_cast<unsigned long>((maxlong%3600000L+1)/60.0/1000.0*uptimeEpoch+(currentTime%3600000L)/60.0/1000.0)%60);
  unsigned int uptimeSecs=static_cast<unsigned int>(static_cast<unsigned long>((maxlong%60000L+1)/1000.0*uptimeEpoch+(currentTime%60000L)/1000.0)%60);

  char* pageContent=new char[2700];
  DEBUG_OUT(2, "Creating webpage: %s\n", pageContent?"true":"false");
  sprintf_P(pageContent, htmlTemplate, 
  timestampStr, currentData.isPeak?"peak":"off peak",
  currentData.peakConsumption>=0.0?currentData.peakConsumption-referenceData.peakConsumption:-1.0, currentData.offPeakConsumption>=0.0?currentData.offPeakConsumption-referenceData.offPeakConsumption:-1.0,
  currentData.peakInjection>=0.0?currentData.peakInjection-referenceData.peakInjection:-1.0, currentData.offPeakInjection>=0.0?currentData.offPeakInjection-referenceData.offPeakInjection:-1.0,
  currentData.currentConsumption>=0.0?currentData.currentConsumption*1000.0:-1.0, currentData.currentInjection>=0.0?currentData.currentInjection*1000.0:-1.0, 
  consumption15,
  currentData.phase1Voltage, currentData.phase2Voltage, currentData.phase3Voltage,
  currentData.phase1Current, currentData.phase2Current, currentData.phase3Current,
  currentData.peakConsumption, currentData.offPeakConsumption,
  currentData.peakInjection, currentData.offPeakInjection,
  uptimeDays, uptimeHours, uptimeMins, uptimeSecs, failedWifi, failedMeterConnect);
  
  webServer.send(200, FPSTR("text/html; charset=utf-8"), pageContent);
  delete[] pageContent;
}
