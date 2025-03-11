#include <Arduino.h>
#include <WiFi.h>
#include "mbedtls/md.h"
#include "HTTPClient.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "drivers/storage/storage.h"

extern uint32_t templates;
extern uint32_t hashes;
extern uint32_t Mhashes;
extern uint32_t totalKHashes;
extern uint32_t elapsedKHs;
extern uint64_t upTime;

extern uint32_t shares; // increase if blockhash has 32 bits of zeroes
extern uint32_t valids; // increased if blockhash <= targethalfshares

extern double best_diff; // track best diff

extern monitor_data mMonitor;

//from saved config
extern TSettings Settings; 
bool invertColors = false;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 3600, 60000);
unsigned int bitcoin_price=0;
String current_block = "793261";
global_data gData;
pool_data pData;

void setup_monitor(void){
    /******** TIME ZONE SETTING *****/

    timeClient.begin();
    
    // Adjust offset depending on your zone
    // GMT +2 in seconds (zona horaria de Europa Central)
    timeClient.setTimeOffset(3600 * Settings.Timezone);
    Serial.println("TimeClient setup done");    
}

unsigned long mGlobalUpdate =0;

void updateGlobalData(void){
    
    if((mGlobalUpdate == 0) || (millis() - mGlobalUpdate > UPDATE_Global_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return;
            
        //Make first API call to get global hash and current difficulty
        HTTPClient http;
        try {
        http.begin(getGlobalHash);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            DynamicJsonDocument doc(1024);
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("currentHashrate")) temp = String(doc["currentHashrate"].as<float>());
            if(temp.length()>18 + 3) //Exahashes more than 18 digits + 3 digits decimals
              gData.globalHash = temp.substring(0,temp.length()-18 - 3);
            if (doc.containsKey("currentDifficulty")) temp = String(doc["currentDifficulty"].as<float>());
            if(temp.length()>10 + 3){ //Terahash more than 10 digits + 3 digit decimals
              temp = temp.substring(0,temp.length()-10 - 3);
              gData.difficulty = temp.substring(0,temp.length()-2) + "." + temp.substring(temp.length()-2,temp.length()) + "T";
            }
            doc.clear();

            mGlobalUpdate = millis();
        }
        http.end();

      
        //Make third API call to get fees
        http.begin(getFees);
        httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            DynamicJsonDocument doc(1024);
            deserializeJson(doc, payload);
            String temp = "";
            if (doc.containsKey("halfHourFee")) gData.halfHourFee = doc["halfHourFee"].as<int>();
#ifdef NERDMINER_T_HMI
            if (doc.containsKey("fastestFee"))  gData.fastestFee = doc["fastestFee"].as<int>();
            if (doc.containsKey("hourFee"))     gData.hourFee = doc["hourFee"].as<int>();
            if (doc.containsKey("economyFee"))  gData.economyFee = doc["economyFee"].as<int>();
            if (doc.containsKey("minimumFee"))  gData.minimumFee = doc["minimumFee"].as<int>();
#endif
            doc.clear();

            mGlobalUpdate = millis();
        }
        
        http.end();
        } catch(...) {
          http.end();
        }
    }
}

unsigned long mHeightUpdate = 0;

String getBlockHeight(void){
    
    if((mHeightUpdate == 0) || (millis() - mHeightUpdate > UPDATE_Height_min * 60 * 1000)){
    
        if (WiFi.status() != WL_CONNECTED) return current_block;
            
        HTTPClient http;
        try {
        http.begin(getHeightAPI);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            payload.trim();

            current_block = payload;

            mHeightUpdate = millis();
        }        
        http.end();
        } catch(...) {
          http.end();
        }
    }
  
  return current_block;
}

unsigned long mBTCUpdate = 0;

String getBTCprice(void) {
  // Check if the price needs to be updated
  if ((mBTCUpdate == 0) || (millis() - mBTCUpdate > UPDATE_BTC_min * 60 * 1000)) {
      // Ensure WiFi is connected
      if (WiFi.status() != WL_CONNECTED) {
          return (String(bitcoin_price) + "$");
      }

      HTTPClient http;
      try {
          // Use the new API endpoint
          http.begin(getBTCAPI);
          int httpCode = http.GET();

          if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();

              // Parse the JSON response
              DynamicJsonDocument doc(2048); // Increase size if needed
              deserializeJson(doc, payload);

              // Extract the Bitcoin price from the new API response
              if (doc.containsKey("quotes") && doc["quotes"].containsKey("USD")) {
                  bitcoin_price = doc["quotes"]["USD"]["price"].as<double>();
              }

              doc.clear();

              // Update the last fetch time
              mBTCUpdate = millis();
          }

          http.end();
      } catch (...) {
          http.end();
      }
  }

  // Return the price as a string with a "$" suffix
  return (String(bitcoin_price) + "$");
}

unsigned long mTriggerUpdate = 0;
unsigned long initialMillis = millis();
unsigned long initialTime = 0;
unsigned long mPoolUpdate = 0;

void getTime(unsigned long* currentHours, unsigned long* currentMinutes, unsigned long* currentSeconds){
  
  //Check if need an NTP call to check current time
  if((mTriggerUpdate == 0) || (millis() - mTriggerUpdate > UPDATE_PERIOD_h * 60 * 60 * 1000)){ //60 sec. * 60 min * 1000ms
    if(WiFi.status() == WL_CONNECTED) {
        if(timeClient.update()) mTriggerUpdate = millis(); //NTP call to get current time
        initialTime = timeClient.getEpochTime(); // Guarda la hora inicial (en segundos desde 1970)
        Serial.print("TimeClient NTPupdateTime ");
    }
  }

  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // convierte la hora actual en horas, minutos y segundos
  *currentHours = currentTime % 86400 / 3600;
  *currentMinutes = currentTime % 3600 / 60;
  *currentSeconds = currentTime % 60;
}

String getDate(){
  
  unsigned long elapsedTime = (millis() - mTriggerUpdate) / 1000; // Tiempo transcurrido en segundos
  unsigned long currentTime = initialTime + elapsedTime; // La hora actual

  // Convierte la hora actual (epoch time) en una estructura tm
  struct tm *tm = localtime((time_t *)&currentTime);

  int year = tm->tm_year + 1900; // tm_year es el número de años desde 1900
  int month = tm->tm_mon + 1;    // tm_mon es el mes del año desde 0 (enero) hasta 11 (diciembre)
  int day = tm->tm_mday;         // tm_mday es el día del mes

  char currentDate[20];
  sprintf(currentDate, "%02d/%02d/%04d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900);

  return String(currentDate);
}

String getTime(void){
  unsigned long currentHours, currentMinutes, currentSeconds;
  getTime(&currentHours, &currentMinutes, &currentSeconds);

  char LocalHour[10];
  sprintf(LocalHour, "%02d:%02d", currentHours, currentMinutes);
  
  String mystring(LocalHour);
  return LocalHour;
}

String getCurrentHashRate(unsigned long mElapsed)
{
  return String((1.0 * (elapsedKHs * 1000)) / mElapsed, 2);
}

mining_data getMiningData(unsigned long mElapsed)
{
  mining_data data;

  char best_diff_string[16] = {0};
  suffix_string(best_diff, best_diff_string, 16, 0);

  char timeMining[15] = {0};
  uint64_t secElapsed = upTime + (esp_timer_get_time() / 1000000);
  int days = secElapsed / 86400;
  int hours = (secElapsed - (days * 86400)) / 3600;               // Number of seconds in an hour
  int mins = (secElapsed - (days * 86400) - (hours * 3600)) / 60; // Remove the number of hours and calculate the minutes.
  int secs = secElapsed - (days * 86400) - (hours * 3600) - (mins * 60);
  sprintf(timeMining, "%01d  %02d:%02d:%02d", days, hours, mins, secs);

  data.completedShares = shares;
  data.totalMHashes = Mhashes;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.templates = templates;
  data.bestDiff = best_diff_string;
  data.timeMining = timeMining;
  data.valids = valids;
  data.temp = String(temperatureRead(), 0);
  data.currentTime = getTime();

  return data;
}

clock_data getClockData(unsigned long mElapsed)
{
  clock_data data;

  data.completedShares = shares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.blockHeight = getBlockHeight();
  data.currentTime = getTime();
  data.currentDate = getDate();

  return data;
}

clock_data_t getClockData_t(unsigned long mElapsed)
{
  clock_data_t data;

  data.valids = valids;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  getTime(&data.currentHours, &data.currentMinutes, &data.currentSeconds);

  return data;
}

coin_data getCoinData(unsigned long mElapsed)
{
  coin_data data;

  updateGlobalData(); // Update gData vars asking mempool APIs

  data.completedShares = shares;
  data.totalKHashes = totalKHashes;
  data.currentHashRate = getCurrentHashRate(mElapsed);
  data.btcPrice = getBTCprice();
  data.currentTime = getTime();
#ifdef NERDMINER_T_HMI
  data.hourFee = String(gData.hourFee);
  data.fastestFee = String(gData.fastestFee);
  data.economyFee = String(gData.economyFee);
  data.minimumFee = String(gData.minimumFee);
#endif
  data.halfHourFee = String(gData.halfHourFee) + " sat/vB";
  data.netwrokDifficulty = gData.difficulty;
  data.globalHashRate = gData.globalHash;
  data.blockHeight = getBlockHeight();

  unsigned long currentBlock = data.blockHeight.toInt();
  unsigned long remainingBlocks = (((currentBlock / HALVING_BLOCKS) + 1) * HALVING_BLOCKS) - currentBlock;
  data.progressPercent = (HALVING_BLOCKS - remainingBlocks) * 100 / HALVING_BLOCKS;
  data.remainingBlocks = String(remainingBlocks) + " BLOCKS";

  return data;
}

pool_data getPoolData(void) {
    static pool_data pData;  // Make static to preserve last valid data
    static unsigned long lastSuccessfulUpdate = 0;  // Track successful updates
    
    // Check if it's time to update
    if ((mPoolUpdate == 0) || (millis() - mPoolUpdate > UPDATE_POOL_min * 60 * 1000)) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("No WiFi connection");
            return pData;  // Return last known good data
        }

        HTTPClient http;
        http.setReuse(true);
        
        // Set timeout to prevent hanging
        http.setTimeout(10000);  // 10 second timeout
        
        String btcWallet = Settings.BtcWallet;
        if (btcWallet.indexOf(".") > 0) {
            btcWallet = btcWallet.substring(0, btcWallet.indexOf("."));
        }
        
        String url;
        if (Settings.PoolAddress == "tn.vkbit.com") {
            url = "https://testnet.vkbit.com/miner/" + btcWallet;
        } else {
            url = String(getNerdminerPool) + btcWallet;
        }
        
        bool success = false;
        try {
            http.begin(url);
            int httpCode = http.GET();
            
            if (httpCode == HTTP_CODE_OK) {
                String payload = http.getString();
                
                // Use a larger buffer for JSON parsing
                StaticJsonDocument<384> filter;
                filter["bestDifficulty"] = true;
                filter["workersCount"] = true;
                filter["workers"][0]["sessionId"] = true;
                filter["workers"][0]["hashRate"] = true;
                
                DynamicJsonDocument doc(4096);  // Increased buffer size
                DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
                
                if (!error) {
                    // Create temporary variables
                    pool_data tempData;
                    
                    // Parse workersCount
                    tempData.workersCount = doc.containsKey("workersCount") ? doc["workersCount"].as<int>() : 0;
                    
                    // Calculate total hash rate
                    float totalhashs = 0;
                    const JsonArray& workers = doc["workers"].as<JsonArray>();
                    for (const JsonObject& worker : workers) {
                        if (worker.containsKey("hashRate")) {
                            totalhashs += worker["hashRate"].as<double>();
                        }
                    }
                    
                    // Format hash rate
                    char totalhashs_s[16] = {0};
                    suffix_string(totalhashs, totalhashs_s, 16, 0);
                    tempData.workersHash = String(totalhashs_s);
                    
                    // Parse difficulty
                    if (doc.containsKey("bestDifficulty")) {
                        double temp = doc["bestDifficulty"].as<double>();
                        char best_diff_string[16] = {0};
                        suffix_string(temp, best_diff_string, 16, 0);
                        tempData.bestDifficulty = String(best_diff_string);
                    }
                    
                    // Only update main data if all parsing was successful
                    pData = tempData;
                    lastSuccessfulUpdate = millis();
                    success = true;
                    
                    Serial.println("####### Pool Data OK!");
                } else {
                    Serial.print("JSON Parse Error: ");
                    Serial.println(error.c_str());
                }
            } else {
                Serial.print("HTTP Error: ");
                Serial.println(httpCode);
            }
        } catch (const std::exception& e) {
            Serial.print("Exception: ");
            Serial.println(e.what());
        } catch (...) {
            Serial.println("Unknown error occurred");
        }
        
        http.end();
        
        // Update timestamp only on successful updates
        if (success) {
            mPoolUpdate = millis();
        } else {
            // If update failed and last successful update was too long ago, show error
            if (millis() - lastSuccessfulUpdate > 5 * 60 * 1000) {  // 5 minutes
                pData.bestDifficulty = "P";
                pData.workersHash = "Error";
                pData.workersCount = 0;
            }
            // Retry sooner on failure
            mPoolUpdate = millis() - ((UPDATE_POOL_min * 60 * 1000) / 2);
        }
    }
    
    return pData;
}
