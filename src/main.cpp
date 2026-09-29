#include <M5GFX.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "esp_sleep.h"
#include "secrets.h"

#include "SpaceMono26.h"
#include "SpaceMono42.h"
// NB: SpaceMono36.h has been removed as no display.loadFont(SpaceMono36) appears in this file. Re-add it if you use it elsewhere in the project.


M5GFX display;

// ---------- Page Buttons ----------
constexpr gpio_num_t PIN_BUTTON_PREV = GPIO_NUM_37;    // previous page button
constexpr gpio_num_t PIN_BUTTON_NEXT = GPIO_NUM_39;    // next page button
constexpr uint32_t BUTTON_DEBOUNCE_MS = 250;

int currentPage = 1;
unsigned long lastButtonPressTime = 0, lastActivityTime = 0;
void resetActivityTimer();

// ---------- Deep sleep ----------
constexpr bool DEEPSLEEP_ENABLED = true;
constexpr uint32_t DEEPSLEEP_TIMEOUT_MS = 60000;
constexpr gpio_num_t PIN_TOUCH_INTERRUPT = GPIO_NUM_36;
constexpr gpio_num_t PIN_MAIN_POWER      = GPIO_NUM_2;

// ---------- Home Assistant ----------
const char* ENTITY_CLIMATE  = "climate.clim";
const char* ENTITY_TEMP     = "sensor.salon_temperature";
const char* ENTITY_HUMIDITY = "sensor.salon_humidite";
// Becky's Vivarium
const char* BECKY_LIGHTS_PWR_SWITCH = "switch.becky_s_lights";
const char* BECKY_LIGHTS_PWR_CURRENT = "sensor.becky_s_lights_current";
// Warm Side
const char* ENTITY_BECKY_WARMSIDE_TEMP     = "sensor.becky_s_vivarium_warm_side_temperature";
const char* ENTITY_BECK_WARMSIDE_HUMIDITY = "sensor.becky_s_vivarium_warm_side_humidity";
// Cool Side
const char* ENTITY_BECKY_COOLSIDE_TEMP     = "sensor.becky_s_vivarium_cool_side_temperature";
const char* ENTITY_BECKY_COOLSIDE_HUMIDITY = "sensor.becky_s_vivarium_cool_side_humidity";
// Outdoor Sensors
const char* ENTITY_OUTDOOR_TEMP     = "sensor.stardustweather_temperature";
const char* ENTITY_OUTDOOR_HUMIDITY = "sensor.stardustweather_humidity";
// Air Quality
const char* ENTITY_BEDROOM_TEMP     = "sensor.alpstuga_air_quality_monitor_temperature";
const char* ENTITY_BEDROOM_HUMIDITY = "sensor.alpstuga_air_quality_monitor_humidite";
// Wifi
const char* ENTITY_WIFI_BUTTON = "input_button.creer_voucher";
const char* ENTITY_WIFI_SENSOR = "sensor.liste_hotspot_vouchers";


// ---------- Geometry ----------
// A Rect describes a rectangle on the screen: top-left corner (x,y) + width/height (w,h).
struct Rect { int x,y,w,h; };
bool inRect(const Rect& rect,int x,int y){return x>=rect.x&&x<=rect.x+rect.w&&y>=rect.y&&y<=rect.y+rect.h;}

constexpr int CORNER_RADIUS=0, BORDER_THICKNESS=2, LINE_THICKNESS=2;

// ---------- Line height ----------
constexpr int LINE_SMALL  = 40;
constexpr int LINE_MEDIUM = 80;
constexpr int LINE_LARGE  = 100;

// ---------- Actions ----------
constexpr int MAX_BUTTONS_PER_CARD = 6; // max number of buttons in an Actions card (Lighting/Modes)
struct ActionCard {
  const char* title;
  const char* domain[MAX_BUTTONS_PER_CARD];
  const char* service[MAX_BUTTONS_PER_CARD];
  const char* entity[MAX_BUTTONS_PER_CARD];
  const char* label[MAX_BUTTONS_PER_CARD];
  const char* state[MAX_BUTTONS_PER_CARD];
  bool active[MAX_BUTTONS_PER_CARD];
  Rect bounds;
  int titleHeight, buttonsHeight, cellWidth;
};

ActionCard lighting={
  "Lighting",
  {"scene","scene","scene","scene","scene","scene"},
  {"turn_on","turn_on","turn_on","turn_on","turn_on","turn_on"},
  {"scene.canap","scene.diner","scene.all_off_cuisine_et_salon",
   "scene.welcome_home","scene.film_2","scene.veilleuse"},
  {"DEN","KITCHEN","DINING","ALL OFF","PORCH","BEDROOMS"},
  {nullptr,nullptr,nullptr,nullptr,nullptr,nullptr}
};

ActionCard modes={
  "Modes",
  {"input_boolean","input_boolean","automation",nullptr,nullptr,nullptr},
  {"toggle","toggle","toggle",nullptr,nullptr,nullptr},
  {"input_boolean.annonce_audio_bus","input_boolean.absence_prolongee",
   "automation.fermeture_automatique_volets",nullptr,nullptr,nullptr},
  {"HOME","AWAY","NIGHT",nullptr,nullptr,nullptr},
  {"input_boolean.annonce_audio_bus","input_boolean.absence_prolongee",
   "automation.fermeture_automatique_volets",nullptr,nullptr,nullptr}
};

// All "Actions" cards on page 1, used in generic loop functions
// (setupLayout, drawPage, refreshAction, touchAction).
ActionCard* actionCards[]={&lighting,&modes};

// ---------- Blinds/Covers ----------
// A single command line (up/stop/down), shared between blinds.
// A selection line allows you to choose which blind it controls.
struct CoverCard {
  const char* title;
  const char* entity;
  String state;   // "open","closed","opening","closing",...
};
CoverCard covers[]={
  // Update These - Or do something with the code block
  {"Bedroom","cover.shellyplus2pm_485519965dac"},
  {"Living Room","cover.shellyswitch25_4c752532f095"}
};
constexpr int COVER_COUNT=2;

struct CoversBlock { Rect bounds; int titleHeight,selectHeight,buttonsHeight; } coversBlock;
int selectedCover=0;   // index of the blind currently being controlled
bool page2DataLoaded=false; // Blinds/Wi-Fi voucher data is loaded only on the first display of page 2

// ---------- Climate ----------
// Operational mode displayed/labels: Heating / Cooling / Off
// (corresponds to hvac_mode Home Assistant "heat" / "cool" / "off")
constexpr float TEMP_STEP = 1.0f;
constexpr float TEMP_MIN  = 10.0f;
constexpr float TEMP_MAX  = 30.0f;
const char* HVAC_LABELS[3] =  {"HEATING","COOLING","OFF"};
const char* HVAC_MODES[3]  = {"heat","cool","off"};

struct ClimateCard {
  Rect bounds;
  int titleHeight,modeHeight,tempHeight,indoorInfoHeight,bedroomInfoHeight,outdoorInfoHeight,cellWidth;
} climate;
String hvacMode="off";
bool hasTarget=false, hasCurrent=false, hasHumidity=false;
float targetTemp=20.0f, currentTemp=0, currentHumidity=0;
bool hasOutdoorTemp=false, hasOutdoorHumidity=false;
float outdoorTemp=0, outdoorHumidity=0;
bool hasBedroomTemp=false, hasBedroomHumidity=false;
float bedroomTemp=0, bedroomHumidity=0;

// ---------- Wi-Fi ----------
struct WifiCard { Rect bounds; int titleHeight,buttonHeight,passwordHeight,codeHeight; } wifiCard;
String voucherCode="";
bool hasVoucher=false;

// ---------- Spotify ----------
struct SpotifyCard { Rect bounds; int titleHeight,infoHeight,buttonsHeight; } spotifyCard;
String spotifyTitle="";
String spotifyArtist="";
String spotifyState="";
bool hasSpotify=false;

// ---------- Drawing: Basic Primitives ----------
void drawThickRoundedRect(int x,int y,int width,int height,int radius,uint32_t color,int thickness){
  for(int i=0;i<thickness;i++) display.drawRoundRect(x+i,y+i,width-2*i,height-2*i,max(0,radius-i),color);
}
void drawHorizontalLine(int x,int y,int width,uint32_t color,int thickness){display.fillRect(x,y-thickness/2,width,thickness,color);}
void drawVerticalLine(int x,int y,int height,uint32_t color,int thickness){display.fillRect(x-thickness/2,y,thickness,height,color);}

// ---------- Drawing: Icons ----------
void drawStopIcon(int centerX,int centerY,int size,uint32_t color){
  display.fillRect(centerX-size,centerY-size,2*size,2*size,color);
}
void drawPreviousIcon(int centerX,int centerY,int size,uint32_t color){
  display.fillRect(centerX-size,centerY-size,size/3,2*size,color);
  display.fillTriangle(centerX+size,centerY-size,centerX-size/3,centerY,centerX+size,centerY+size,color);
}
void drawNextIcon(int centerX,int centerY,int size,uint32_t color){
  display.fillRect(centerX+size-size/3,centerY-size,size/3,2*size,color);
  display.fillTriangle(centerX-size,centerY-size,centerX+size/3,centerY,centerX-size,centerY+size,color);
}
void drawPauseIcon(int centerX,int centerY,int size,uint32_t color){
  int barWidth=max(2,size/3);
  display.fillRect(centerX-size/2,centerY-size,barWidth,2*size,color);
  display.fillRect(centerX+size/2-barWidth,centerY-size,barWidth,2*size,color);
}

// Trace a thick line by drawing multiple parallel lines shifted perpendicularly to the direction of the segment.
// Works regardless of the angle (unlike a simple +1 in X).
void drawThickLine(int x0,int y0,int x1,int y1,uint32_t color,int thickness){
  float dx=x1-x0, dy=y1-y0;
  float len=sqrtf(dx*dx+dy*dy);
  if(len<0.001f){
    display.drawLine(x0,y0,x1,y1,color);
    return;
  }
  float px=-dy/len, py=dx/len;   // vecteur perpendiculaire unitaire
  int half=thickness/2;
  for(int offset=-half; offset<=thickness-half-1; offset++){
    int ox=(int)roundf(px*offset);
    int oy=(int)roundf(py*offset);
    display.drawLine(x0+ox,y0+oy,x1+ox,y1+oy,color);
  }
}

// Wavy line (heat wave), drawn in successive segments.
void drawHeatWaveSegment(int centerX,int topY,int bottomY,int amplitude,uint32_t color){
  const int SEGMENT_COUNT=8;
  const int THICKNESS=3;  // line thickness, adjust as needed
  int previousX=centerX,previousY=topY;
  for(int i=1;i<=SEGMENT_COUNT;i++){
    float progress=(float)i/SEGMENT_COUNT;
    int x=centerX+(int)(amplitude*sinf(progress*6.9f));
    int y=topY+(int)(progress*(bottomY-topY));
    drawThickLine(previousX,previousY,x,y,color,THICKNESS);
    previousX=x;previousY=y;
  }
}

// Heating icon: unchanged, the bar is already full (fillRect).
void drawHeatingIcon(int centerX,int centerY,int size,uint32_t color){
  int barY=centerY+(int)(size*0.85f);
  display.fillRect(centerX-(int)(size*0.8f),barY,(int)(size*1.6f),(int)(size*0.22f),color);
  int bottomY=barY-(int)(size*0.15f), topY=centerY-(int)(size*1.0f);
  int amplitude=(int)(size*0.22f);
  drawHeatWaveSegment(centerX-(int)(size*0.55f),topY,bottomY,amplitude,color);
  drawHeatWaveSegment(centerX,               topY,bottomY,amplitude,color);
  drawHeatWaveSegment(centerX+(int)(size*0.55f),topY,bottomY,amplitude,color);
}

// Snowflake icon: hexagonal snowflake (3 axes) with ramifications on each branch.
void drawSnowflakeIcon(int centerX,int centerY,int size,uint32_t color){

  const int THICKNESS=3;
  
  for(int axis=0;axis<3;axis++){
    float angle=axis*PI/3.0f, dx=cosf(angle), dy=sinf(angle);

    drawThickLine(centerX-dx*size, centerY-dy*size, centerX+dx*size, centerY+dy*size, color, THICKNESS);
    for(int side=-1;side<=1;side+=2){
      float branchX=centerX+dx*size*0.55f*side, branchY=centerY+dy*size*0.55f*side;
      for(int direction=-1;direction<=1;direction+=2){
        float branchAngle=angle+direction*(PI/3.0f);

        drawThickLine(branchX,branchY,
          branchX+cosf(branchAngle)*size*0.32f*side,
          branchY+sinf(branchAngle)*size*0.32f*side,
          color,THICKNESS);
      }
    }
  }
}

// ---------- Drawing : common frame for all cards -------
void drawCardFrame(const Rect& bounds,const char* title,int titleHeight){
  display.fillRoundRect(bounds.x,bounds.y,bounds.w,bounds.h,CORNER_RADIUS,TFT_WHITE);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_BLACK,TFT_WHITE);
  display.setTextSize(1);
  display.drawString(title,bounds.x+bounds.w/2,bounds.y+titleHeight/2);
  drawHorizontalLine(bounds.x,bounds.y+titleHeight,bounds.w,TFT_BLACK,LINE_THICKNESS);
}

// Generic three-button card (used for "Modes").
void drawThreeButtonCard(const Rect& bounds,const char* title,int titleHeight,int buttonsHeight,
                          int pressedIndex,const bool* activeStates,const char* labels[3]){
  drawCardFrame(bounds,title,titleHeight);
  int cellWidth=bounds.w/3, rowY=bounds.y+titleHeight;
  for(int i=0;i<3;i++){
    bool isHighlighted=(i==pressedIndex)||(activeStates&&activeStates[i]);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=bounds.x+i*cellWidth,centerX=cellX+cellWidth/2,centerY=rowY+buttonsHeight/2;
    display.fillRect(cellX,rowY,cellWidth,buttonsHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1);
    display.drawString(labels[i],centerX,centerY);
  }
  drawVerticalLine(bounds.x+cellWidth,rowY,buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(bounds.x+2*cellWidth,rowY,buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawThickRoundedRect(bounds.x,bounds.y,bounds.w,bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}

// Lighting card: grid of 6 buttons (3 columns x 2 rows).
void drawLightingCard(ActionCard& actionCard,int pressedIndex=-1){
  drawCardFrame(actionCard.bounds,actionCard.title,actionCard.titleHeight);
  int cellWidth=actionCard.bounds.w/3, rowHeight=actionCard.buttonsHeight/2, gridTop=actionCard.bounds.y+actionCard.titleHeight;
  for(int i=0;i<6;i++){
    int row=i/3, col=i%3;
    int cellX=actionCard.bounds.x+col*cellWidth, cellY=gridTop+row*rowHeight;
    bool isHighlighted=(i==pressedIndex)||actionCard.active[i];
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    display.fillRect(cellX,cellY,cellWidth,rowHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1);
    display.drawString(actionCard.label[i],cellX+cellWidth/2,cellY+rowHeight/2);
  }
  drawVerticalLine(actionCard.bounds.x+cellWidth,gridTop,actionCard.buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(actionCard.bounds.x+2*cellWidth,gridTop,actionCard.buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawHorizontalLine(actionCard.bounds.x,gridTop+rowHeight,actionCard.bounds.w,TFT_BLACK,LINE_THICKNESS);
  drawThickRoundedRect(actionCard.bounds.x,actionCard.bounds.y,actionCard.bounds.w,actionCard.bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}

void drawAction(ActionCard& actionCard,int pressedIndex=-1){
  if(&actionCard==&lighting) drawLightingCard(actionCard,pressedIndex);
  else drawThreeButtonCard(actionCard.bounds,actionCard.title,actionCard.titleHeight,actionCard.buttonsHeight,
                            pressedIndex,actionCard.active,actionCard.label);
}

// pressedSelector : button of the currently pressed section (-1 = none)
// pressedButton   : up/down button being pressed (-1 = none)
void drawCovers(int pressedSelector=-1,int pressedButton=-1){
  drawCardFrame(coversBlock.bounds,"Blinds",coversBlock.titleHeight);
  int selectorCellWidth=coversBlock.bounds.w/COVER_COUNT;
  int currentY=coversBlock.bounds.y+coversBlock.titleHeight;

  // ---- Blinds selector ----
  for(int i=0;i<COVER_COUNT;i++){
    bool isHighlighted=(i==pressedSelector)||(i==selectedCover);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=coversBlock.bounds.x+i*selectorCellWidth;
    display.fillRect(cellX,currentY,selectorCellWidth,coversBlock.selectHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1);
    display.drawString(covers[i].title,cellX+selectorCellWidth/2,currentY+coversBlock.selectHeight/2);
  }
  for(int i=1;i<COVER_COUNT;i++) drawVerticalLine(coversBlock.bounds.x+i*selectorCellWidth,currentY,coversBlock.selectHeight,TFT_BLACK,LINE_THICKNESS);
  currentY+=coversBlock.selectHeight; drawHorizontalLine(coversBlock.bounds.x,currentY,coversBlock.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- Commands for the selected cover (displayed only once) ----
  int buttonCellWidth=coversBlock.bounds.w/3, stopIconSize=min(buttonCellWidth,coversBlock.buttonsHeight)/4;
  display.loadFont(SpaceMono42);  // larger font for the up/down arrows (loaded once, not every button)
  for(int i=0;i<3;i++){
    bool isHighlighted=(i==pressedButton);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=coversBlock.bounds.x+i*buttonCellWidth, centerX=cellX+buttonCellWidth/2, centerY=currentY+coversBlock.buttonsHeight/2;
    display.fillRect(cellX,currentY,buttonCellWidth,coversBlock.buttonsHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor);
    if(i==0) display.drawString("↑",centerX,centerY);
    else if(i==1) drawStopIcon(centerX,centerY,(int)(stopIconSize*.4),foregroundColor);
    else display.drawString("↓",centerX,centerY);
  }
  display.loadFont(SpaceMono26);  // back to the default font for the rest

  drawVerticalLine(coversBlock.bounds.x+buttonCellWidth,currentY,coversBlock.buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(coversBlock.bounds.x+2*buttonCellWidth,currentY,coversBlock.buttonsHeight,TFT_BLACK,LINE_THICKNESS);

  drawThickRoundedRect(coversBlock.bounds.x,coversBlock.bounds.y,coversBlock.bounds.w,coversBlock.bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}


// Aligns the columns (name / temperature / humidity) on a fixed X position,
// independently of the length of the label ("Living Room" vs "Outside"), to keep
// the temperature and humidity values aligned between lines.
void drawClimateInfoRow(int y,int height,const char* label,const String& tempLabel,const String& humidityLabel){
  constexpr float LABEL_X_RATIO = 0.03f;  // left margin
  constexpr float TEMP_X_RATIO  = 0.408f; // temperature column
  constexpr float HUM_X_RATIO   = 0.788f; // humidity column

  int centerY=y+height/2;
  display.setTextColor(TFT_BLACK,TFT_WHITE);
  display.setTextSize(1);

  display.setTextDatum(middle_left); // local datum : the 3 columns are aligned to the left of their X
  display.drawString(label, climate.bounds.x+(int)(climate.bounds.w*LABEL_X_RATIO), centerY);
  display.drawString(tempLabel.c_str(), climate.bounds.x+(int)(climate.bounds.w*TEMP_X_RATIO), centerY);
  display.drawString(humidityLabel.c_str(), climate.bounds.x+(int)(climate.bounds.w*HUM_X_RATIO), centerY);
  display.setTextDatum(middle_center); // reset to the default datum used everywhere else
}

// pressedModeIndex : button mode currently pressed (-1 = none)
// pressedTempIndex : +/- button currently pressed (-1 = none)
void drawClimate(int pressedModeIndex=-1,int pressedTempIndex=-1){
  drawCardFrame(climate.bounds,"Climate Control",climate.titleHeight);
  int cellWidth=climate.bounds.w/3;
  int currentY=climate.bounds.y+climate.titleHeight;

  // ---- mode line : cloud icon (heating) / snowflake icon (cooling) / OFF ----
  int modeIconSize=min(cellWidth,climate.modeHeight)/4;
  for(int i=0;i<3;i++){
    bool isHighlighted=(i==pressedModeIndex)||(hvacMode==HVAC_MODES[i]);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=climate.bounds.x+i*cellWidth, centerX=cellX+cellWidth/2, centerY=currentY+climate.modeHeight/2;
    display.fillRect(cellX,currentY,cellWidth,climate.modeHeight,backgroundColor);
    if(i==0) drawHeatingIcon(centerX,centerY,modeIconSize,foregroundColor);
    else if(i==1) drawSnowflakeIcon(centerX,centerY,modeIconSize,foregroundColor);
    else{ display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1); display.drawString(HVAC_LABELS[i],centerX,centerY); }
  }
  drawVerticalLine(climate.bounds.x+cellWidth,currentY,climate.modeHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(climate.bounds.x+2*cellWidth,currentY,climate.modeHeight,TFT_BLACK,LINE_THICKNESS);
  currentY+=climate.modeHeight; drawHorizontalLine(climate.bounds.x,currentY,climate.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- line of target : - / value / + ----
  String targetTempLabel=hasTarget?String(targetTemp,1)+"°F":"--";
  for(int i=0;i<3;i++){
    bool isHighlighted=(i==pressedTempIndex);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=climate.bounds.x+i*cellWidth, centerX=cellX+cellWidth/2, centerY=currentY+climate.tempHeight/2;
    display.fillRect(cellX,currentY,cellWidth,climate.tempHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1);
    if(i==0) display.drawString("-",centerX,centerY);
    else if(i==1) display.drawString(targetTempLabel.c_str(),centerX,centerY);
    else display.drawString("+",centerX,centerY);
  }
  drawVerticalLine(climate.bounds.x+cellWidth,currentY,climate.tempHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(climate.bounds.x+2*cellWidth,currentY,climate.tempHeight,TFT_BLACK,LINE_THICKNESS);
  currentY+=climate.tempHeight; drawHorizontalLine(climate.bounds.x,currentY,climate.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- line of info : current temperature + humidity (living room) ----
  // currentTemp & currentHumidity
  String indoorTempLabel=hasCurrent?String(currentTemp,1)+"°F":"";
  String indoorHumidityLabel=hasHumidity?String(currentHumidity,0)+"rH":"";
  drawClimateInfoRow(currentY,climate.indoorInfoHeight,"Den",indoorTempLabel,indoorHumidityLabel);
  currentY+=climate.indoorInfoHeight; drawHorizontalLine(climate.bounds.x,currentY,climate.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- line of info : temperature/humidity bedroom ----
  //bedroomTemp & bedroomHumidity
  String bedroomTempLabel=hasBedroomTemp?String(bedroomTemp,1)+"°F":"";
  String bedroomHumidityLabel=hasBedroomHumidity?String(bedroomHumidity,0)+"%":"";
  drawClimateInfoRow(currentY,climate.bedroomInfoHeight,"Bedroom",bedroomTempLabel,bedroomHumidityLabel);
  currentY+=climate.bedroomInfoHeight; drawHorizontalLine(climate.bounds.x,currentY,climate.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- line of info : outdoor temperature/humidity ----
  // outdoorTemp & outdoorHumidity
  String outdoorTempLabel=hasOutdoorTemp?String(outdoorTemp,1)+"°F":"";
  String outdoorHumidityLabel=hasOutdoorHumidity?String(outdoorHumidity,0)+"%":"";
  drawClimateInfoRow(currentY,climate.outdoorInfoHeight,"Outside",outdoorTempLabel,outdoorHumidityLabel);

  drawThickRoundedRect(climate.bounds.x,climate.bounds.y,climate.bounds.w,climate.bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}

void drawSpotify(int pressedIndex=-1){
  drawCardFrame(spotifyCard.bounds,"Spotify",spotifyCard.titleHeight);

  int currentY=spotifyCard.bounds.y+spotifyCard.titleHeight;

  // ---- title + artist ----
  display.setTextColor(TFT_BLACK,TFT_WHITE);
  display.setTextSize(1);
  String trackTitle=hasSpotify && spotifyTitle.length()?spotifyTitle:"Nothing Playing";
  String trackArtist=hasSpotify && spotifyArtist.length()?spotifyArtist:"";

  int lineHeight=spotifyCard.infoHeight/2;

  if(trackArtist.length()){
    // Title + artist : each centered in its half of the block.
    display.drawString(
      trackTitle.c_str(),
      spotifyCard.bounds.x+spotifyCard.bounds.w/2,
      currentY+lineHeight/2
    );
    display.drawString(
      trackArtist.c_str(),
      spotifyCard.bounds.x+spotifyCard.bounds.w/2,
      currentY+lineHeight+lineHeight/2
    );
  }else{
    // No artist (or no playback in progress) : the title alone is centered
    // on the entire height of the block, to avoid a large empty space below.
    display.drawString(
      trackTitle.c_str(),
      spotifyCard.bounds.x+spotifyCard.bounds.w/2,
      currentY+spotifyCard.infoHeight/2
    );
  }

  currentY+=spotifyCard.infoHeight;
  drawHorizontalLine(spotifyCard.bounds.x,currentY,spotifyCard.bounds.w,TFT_BLACK,LINE_THICKNESS);

  // ---- Commands: Previous / Pause / Next ----
  int cellWidth=spotifyCard.bounds.w/3;
  int iconSize=min(cellWidth,spotifyCard.buttonsHeight)/4;
  for(int i=0;i<3;i++){
    bool isHighlighted=(i==pressedIndex);
    uint32_t backgroundColor=isHighlighted?TFT_BLACK:TFT_WHITE, foregroundColor=isHighlighted?TFT_WHITE:TFT_BLACK;
    int cellX=spotifyCard.bounds.x+i*cellWidth, centerX=cellX+cellWidth/2, centerY=currentY+spotifyCard.buttonsHeight/2;
    display.fillRect(cellX,currentY,cellWidth,spotifyCard.buttonsHeight,backgroundColor);

    if(i==0) drawPreviousIcon(centerX,centerY,iconSize,foregroundColor);
    else if(i==1) drawPauseIcon(centerX,centerY,iconSize,foregroundColor);
    else drawNextIcon(centerX,centerY,iconSize,foregroundColor);
  }
  drawVerticalLine(spotifyCard.bounds.x+cellWidth,currentY,spotifyCard.buttonsHeight,TFT_BLACK,LINE_THICKNESS);
  drawVerticalLine(spotifyCard.bounds.x+2*cellWidth,currentY,spotifyCard.buttonsHeight,TFT_BLACK,LINE_THICKNESS);

  drawThickRoundedRect(spotifyCard.bounds.x,spotifyCard.bounds.y,spotifyCard.bounds.w,spotifyCard.bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}

void drawWifi(bool pressed=false){
  drawCardFrame(wifiCard.bounds,"Hotspot Wi-Fi",wifiCard.titleHeight);
  int currentY=wifiCard.bounds.y+wifiCard.titleHeight;
  if(hasVoucher){
    display.setTextColor(TFT_BLACK,TFT_WHITE); display.setTextSize(1);
    display.drawString(WIFI_NAME,wifiCard.bounds.x+wifiCard.bounds.w/2,currentY+wifiCard.buttonHeight/2);
    currentY+=wifiCard.buttonHeight; drawHorizontalLine(wifiCard.bounds.x,currentY,wifiCard.bounds.w,TFT_BLACK,LINE_THICKNESS);
    display.drawString(("Password : "+String(WIFI_GUEST_PASSWORD)).c_str(),
                       wifiCard.bounds.x+wifiCard.bounds.w/2,currentY+wifiCard.passwordHeight/2);
    currentY+=wifiCard.passwordHeight; drawHorizontalLine(wifiCard.bounds.x,currentY,wifiCard.bounds.w,TFT_BLACK,LINE_THICKNESS);
    display.drawString(("Code : "+voucherCode).c_str(),
                       wifiCard.bounds.x+wifiCard.bounds.w/2,currentY+wifiCard.codeHeight/2);
  }else{
    int totalHeight=wifiCard.buttonHeight+wifiCard.passwordHeight+wifiCard.codeHeight;
    uint32_t backgroundColor=pressed?TFT_BLACK:TFT_WHITE, foregroundColor=pressed?TFT_WHITE:TFT_BLACK;
    display.fillRect(wifiCard.bounds.x,currentY,wifiCard.bounds.w,totalHeight,backgroundColor);
    display.setTextColor(foregroundColor,backgroundColor); display.setTextSize(1);
    display.drawString("Enable hotspot",wifiCard.bounds.x+wifiCard.bounds.w/2,currentY+totalHeight/2);
  }
  drawThickRoundedRect(wifiCard.bounds.x,wifiCard.bounds.y,wifiCard.bounds.w,wifiCard.bounds.h,CORNER_RADIUS,TFT_BLACK,BORDER_THICKNESS);
}

void showStatus(const char* message){
  display.startWrite();
  display.fillRect(0,0,display.width(),35,TFT_WHITE);
  display.setTextDatum(top_left); display.setTextColor(TFT_BLACK,TFT_WHITE);
  display.setTextSize(1); display.drawString(message,10,8);
  display.endWrite();
}

// ---------- Layout ----------
void setupLayout(){
  int screenWidth=display.width(), marginX=12, cardWidth=screenWidth-2*marginX, gapBetweenCards=12;

  // ---------- Page 1 ----------
  // Lighting, then Modes, then Spotify, then Climate.
  int currentY=45;

  for(auto actionCard:actionCards){
    actionCard->titleHeight=LINE_SMALL;
    actionCard->buttonsHeight=(actionCard==&lighting)?(2*LINE_MEDIUM):LINE_MEDIUM;
    actionCard->cellWidth=cardWidth/3;

    actionCard->bounds={marginX,currentY,cardWidth,actionCard->titleHeight+actionCard->buttonsHeight};

    currentY+=actionCard->bounds.h+gapBetweenCards;

    // Spotify is placed just below the Modes.
    if(actionCard==&modes){
      spotifyCard.titleHeight=LINE_SMALL;
      spotifyCard.infoHeight=LINE_MEDIUM;
      spotifyCard.buttonsHeight=LINE_MEDIUM;

      spotifyCard.bounds={marginX,currentY,cardWidth,
        spotifyCard.titleHeight+spotifyCard.infoHeight+spotifyCard.buttonsHeight};

      currentY+=spotifyCard.bounds.h+gapBetweenCards;
    }
  }

  // ---------- Climate ----------
  climate.titleHeight=LINE_SMALL;
  climate.modeHeight=LINE_MEDIUM;
  climate.tempHeight=LINE_MEDIUM;
  climate.indoorInfoHeight=LINE_SMALL;
  climate.bedroomInfoHeight=LINE_SMALL;
  climate.outdoorInfoHeight=LINE_SMALL;
  climate.cellWidth=cardWidth/3;

  climate.bounds={marginX,currentY,cardWidth,
    climate.titleHeight+climate.modeHeight+climate.tempHeight+climate.indoorInfoHeight+climate.bedroomInfoHeight+climate.outdoorInfoHeight};

  // ---------- Page 2 ----------
  // Blinds first, then Wi-Fi Hotspot.
  int page2Y=45;

  coversBlock.titleHeight=LINE_SMALL;
  coversBlock.selectHeight=LINE_MEDIUM; // line of piece selection (KITCHEN/LIVING ROOM)
  coversBlock.buttonsHeight=LINE_MEDIUM;

  coversBlock.bounds={marginX,page2Y,cardWidth,
    coversBlock.titleHeight+coversBlock.selectHeight+coversBlock.buttonsHeight};

  page2Y+=coversBlock.bounds.h+gapBetweenCards;

  // ---------- Wi-Fi Hotspot ----------
  wifiCard.titleHeight=LINE_SMALL;
  wifiCard.buttonHeight=LINE_SMALL;
  wifiCard.passwordHeight=LINE_SMALL;
  wifiCard.codeHeight=LINE_SMALL;

  wifiCard.bounds={marginX,page2Y,cardWidth,
    wifiCard.titleHeight+wifiCard.buttonHeight+wifiCard.passwordHeight+wifiCard.codeHeight};
}

// ---------- Home Assistant ----------
// extra : additional JSON fields, e.g., ",\"temperature\":21.5"
bool haCall(const char* domain,const char* service,const char* entity,const String& extra=""){
  if(WiFi.status()!=WL_CONNECTED){showStatus("Wi-Fi Disconnected");return false;}
  HTTPClient http;
  http.begin(String(HA_HOST)+"/api/services/"+domain+"/"+service);
  http.addHeader("Authorization",String("Bearer ")+HA_TOKEN);
  http.addHeader("Content-Type","application/json");
  String requestBody=String("{\"entity_id\":\"")+entity+"\""+extra+"}";
  int httpStatusCode=http.POST(requestBody);
  http.end();
  return httpStatusCode>0&&httpStatusCode<300;
}

bool haState(const char* entity,DynamicJsonDocument& doc){
  if(WiFi.status()!=WL_CONNECTED)return false;
  HTTPClient http;
  http.begin(String(HA_HOST)+"/api/states/"+entity);
  http.addHeader("Authorization",String("Bearer ")+HA_TOKEN);
  int httpStatusCode=http.GET(); bool success=false;
  if(httpStatusCode==200) success=deserializeJson(doc,http.getString())==DeserializationError::Ok;
  http.end(); return success;
}

void refreshAction(ActionCard& actionCard){
  int buttonCount=&actionCard==&lighting?6:3;
  for(int i=0;i<buttonCount;i++){
    if(!actionCard.state[i]){actionCard.active[i]=false;continue;}
    DynamicJsonDocument stateDoc(512);
    actionCard.active[i]=haState(actionCard.state[i],stateDoc)&&stateDoc["state"].as<String>()=="on";
  }
}

void refreshCover(CoverCard& coverCard){
  DynamicJsonDocument stateDoc(512);
  if(haState(coverCard.entity,stateDoc)) coverCard.state=stateDoc["state"].as<String>();
}

void refreshClimate(){
  DynamicJsonDocument climateDoc(2048);
  if(haState(ENTITY_CLIMATE,climateDoc)){
    hvacMode=climateDoc["state"].as<String>();   // "heat" / "cool" / "off" / other
    if(!climateDoc["attributes"]["temperature"].isNull()){
      targetTemp=climateDoc["attributes"]["temperature"].as<float>();hasTarget=true;
    }
  }
  DynamicJsonDocument indoorTempDoc(1024);
  if(haState(ENTITY_TEMP,indoorTempDoc)){
    const char* v=indoorTempDoc["state"]; if(v){currentTemp=atof(v);hasCurrent=true;}
  }
  DynamicJsonDocument indoorHumidityDoc(1024);
  if(haState(ENTITY_HUMIDITY,indoorHumidityDoc)){
    const char* v=indoorHumidityDoc["state"]; if(v){currentHumidity=atof(v);hasHumidity=true;}
  }
  DynamicJsonDocument outdoorTempDoc(1024);
  if(haState(ENTITY_OUTDOOR_TEMP,outdoorTempDoc)){
    const char* v=outdoorTempDoc["state"]; if(v){outdoorTemp=atof(v);hasOutdoorTemp=true;}
  }
  DynamicJsonDocument outdoorHumidityDoc(1024);
  if(haState(ENTITY_OUTDOOR_HUMIDITY,outdoorHumidityDoc)){
    const char* v=outdoorHumidityDoc["state"]; if(v){outdoorHumidity=atof(v);hasOutdoorHumidity=true;}
  }
  DynamicJsonDocument bedroomTempDoc(1024);
  if(haState(ENTITY_BEDROOM_TEMP,bedroomTempDoc)){
    const char* v=bedroomTempDoc["state"]; if(v){bedroomTemp=atof(v);hasBedroomTemp=true;}
  }
  DynamicJsonDocument bedroomHumidityDoc(1024);
  if(haState(ENTITY_BEDROOM_HUMIDITY,bedroomHumidityDoc)){
    const char* v=bedroomHumidityDoc["state"]; if(v){bedroomHumidity=atof(v);hasBedroomHumidity=true;}
  }
}

void refreshSpotify(){
  DynamicJsonDocument spotifyDoc(2048);
  if(!haState(ENTITY_SPOTIFY,spotifyDoc)){
    hasSpotify=false;
    spotifyTitle="";
    spotifyArtist="";
    spotifyState="";
    return;
  }

  spotifyState=spotifyDoc["state"].as<String>();
  spotifyTitle=spotifyDoc["attributes"]["media_title"].as<String>();
  spotifyArtist=spotifyDoc["attributes"]["media_artist"].as<String>();
  // Some Spotify/HA metadata strings contain leading/trailing spaces:
  // without trim(), they skew the horizontal centering calculation.
  spotifyTitle.trim();
  spotifyArtist.trim();
  hasSpotify=(spotifyTitle.length()>0 || spotifyArtist.length()>0);
}

void refreshVoucher(){
  DynamicJsonDocument voucherListDoc(8192);
  if(!haState(ENTITY_WIFI_SENSOR,voucherListDoc))return;
  JsonArray vouchers=voucherListDoc["attributes"]["data"].as<JsonArray>();
  if(vouchers.isNull()||!vouchers.size()){hasVoucher=false;voucherCode="";return;}
  JsonObject latestVoucher; String latestDate;
  for(JsonObject voucher:vouchers){
    String createdAt=voucher["createdAt"].as<String>();
    if(latestVoucher.isNull()||createdAt>latestDate){latestVoucher=voucher;latestDate=createdAt;}
  }
  const char* code=latestVoucher["code"];
  if(code){
    String rawCode=code;
    voucherCode=rawCode.length()==10?rawCode.substring(0,5)+"-"+rawCode.substring(5):rawCode;
    hasVoucher=true;
  }
}

// Calls the Home Assistant template API (POST /api/template): the Jinja2
// template is evaluated server-side and MUST return valid JSON as text.
// This allows retrieving multiple entities in a SINGLE HTTP request instead
// of one request per entity (see refreshStartupData below).
bool haTemplate(const String& templateStr,DynamicJsonDocument& responseDoc){
  if(WiFi.status()!=WL_CONNECTED)return false;
  HTTPClient http;
  http.begin(String(HA_HOST)+"/api/template");
  http.addHeader("Authorization",String("Bearer ")+HA_TOKEN);
  http.addHeader("Content-Type","application/json");

  // We use ArduinoJson to build the request body: it automatically escapes quotes/newlines present in templateStr.
  DynamicJsonDocument requestDoc(templateStr.length()+64);
  requestDoc["template"]=templateStr;
  String requestBody;
  serializeJson(requestDoc,requestBody);

  int httpStatusCode=http.POST(requestBody);
  bool success=false;
  if(httpStatusCode==200) success=deserializeJson(responseDoc,http.getString())==DeserializationError::Ok;
  http.end();
  return success;
}

// Retrieves in a single HTTP call all the data needed to draw page 1 at startup:
// climate, indoor temperature/humidity, bedroom and outdoor, state of the 3 "Modes",
// and Spotify. Replaces ~11 sequential requests by 1 single one.
// NB: if you modify the entities below, verify the rendering of the template in
// Home Assistant (Developer tools > Template) before relying on the result.
void refreshStartupData(){
  String jinjaTemplate =
    String("{")
    + "\"hvac_mode\":{{ states('" + ENTITY_CLIMATE + "') | tojson }},"
    + "\"target_temp\":{{ state_attr('" + ENTITY_CLIMATE + "','temperature') | float(0) }},"
    + "\"indoor_temp\":{{ states('" + ENTITY_TEMP + "') | float(0) }},"
    + "\"indoor_humidity\":{{ states('" + ENTITY_HUMIDITY + "') | float(0) }},"
    + "\"outdoor_temp\":{{ states('" + ENTITY_OUTDOOR_TEMP + "') | float(0) }},"
    + "\"outdoor_humidity\":{{ states('" + ENTITY_OUTDOOR_HUMIDITY + "') | float(0) }},"
    + "\"bedroom_temp\":{{ states('" + ENTITY_BEDROOM_TEMP + "') | float(0) }},"
    + "\"bedroom_humidity\":{{ states('" + ENTITY_BEDROOM_HUMIDITY + "') | float(0) }},"
    + "\"bus_on\":{{ (states('" + modes.entity[0] + "')=='on') | tojson }},"
    + "\"absence_on\":{{ (states('" + modes.entity[1] + "')=='on') | tojson }},"
    + "\"sunshield_on\":{{ (states('" + modes.entity[2] + "')=='on') | tojson }},"
    + "\"spotify_state\":{{ states('" + ENTITY_SPOTIFY + "') | tojson }},"
    + "\"spotify_title\":{{ state_attr('" + ENTITY_SPOTIFY + "','media_title') | default('',true) | tojson }},"
    + "\"spotify_artist\":{{ state_attr('" + ENTITY_SPOTIFY + "','media_artist') | default('',true) | tojson }}"
    + "}";

  DynamicJsonDocument startupDoc(1024);
  if(!haTemplate(jinjaTemplate,startupDoc)){
    // Fallback: if the grouped call fails (template rejected, etc.), we fall back to the usual individual requests - slower but more reliable.
    refreshClimate();
    refreshAction(modes);
    refreshSpotify();
    return;
  }

  hvacMode=startupDoc["hvac_mode"].as<String>(); hasTarget=true;
  targetTemp=startupDoc["target_temp"].as<float>();
  currentTemp=startupDoc["indoor_temp"].as<float>(); hasCurrent=true;
  currentHumidity=startupDoc["indoor_humidity"].as<float>(); hasHumidity=true;
  outdoorTemp=startupDoc["outdoor_temp"].as<float>(); hasOutdoorTemp=true;
  outdoorHumidity=startupDoc["outdoor_humidity"].as<float>(); hasOutdoorHumidity=true;
  bedroomTemp=startupDoc["bedroom_temp"].as<float>(); hasBedroomTemp=true;
  bedroomHumidity=startupDoc["bedroom_humidity"].as<float>(); hasBedroomHumidity=true;

  modes.active[0]=startupDoc["bus_on"].as<bool>();
  modes.active[1]=startupDoc["absence_on"].as<bool>();
  modes.active[2]=startupDoc["sunshield_on"].as<bool>();

  spotifyState=startupDoc["spotify_state"].as<String>();
  spotifyTitle=startupDoc["spotify_title"].as<String>();
  spotifyArtist=startupDoc["spotify_artist"].as<String>();
  spotifyTitle.trim();
  spotifyArtist.trim();
  hasSpotify=(spotifyTitle.length()>0 || spotifyArtist.length()>0);
}

void connectWifi(){
  showStatus("Connexion Wi-Fi...");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  unsigned long startTime=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-startTime<15000)delay(300);
  showStatus(WiFi.status()==WL_CONNECTED? "WiFi Connected!":"WiFi Failed");
}

// ---------- Pages ----------
// Full physical cleaning before each page change.
// We use high quality EPD mode to avoid artifacts/ghosting.
void cleanScreen(){
  display.setEpdMode(epd_mode_t::epd_quality);
  display.startWrite();
  display.fillScreen(TFT_WHITE);
  display.endWrite();
  display.setEpdMode(epd_mode_t::epd_fastest);
}

void drawPage(){
  // Always completely clean the old content before drawing the new page.
  cleanScreen();

  display.startWrite();
  if(currentPage==1){
    for(auto actionCard:actionCards)drawAction(*actionCard);
    drawSpotify();
    drawClimate();
  }else{
    drawCovers();
    drawWifi();
  }
  display.endWrite();
  showStatus(currentPage==1?"Page 1/2":"Page 2/2");
}

void changePage(int delta){
  currentPage += delta;
  if (currentPage < 1)
    currentPage = 2;
  if (currentPage > 2)
    currentPage = 1;
  resetActivityTimer();
  if (currentPage == 1)
  {
    refreshSpotify();
  }
  else if (currentPage == 2 && !page2DataLoaded)
  {
    // Blinds and Wi-Fi voucher : loaded only on the first visit to page 2, not at startup (we don't load what is not shown).
    for (auto &coverCard : covers)
      refreshCover(coverCard);
    refreshVoucher();
    page2DataLoaded = true;
  }
  drawPage();
}

void resetActivityTimer(){lastActivityTime=millis();}

bool pageButtons(){
  if(millis()-lastButtonPressTime<BUTTON_DEBOUNCE_MS)return false;
  if(digitalRead(PIN_BUTTON_PREV)==LOW){lastButtonPressTime=millis();changePage(-1);return true;}
  if(digitalRead(PIN_BUTTON_NEXT)==LOW){lastButtonPressTime=millis();changePage(1);return true;}
  return false;
}

// ---------- Deep sleep ----------
void enterDeepSleep(){
  lgfx::touch_point_t touchPoint;
  while(digitalRead(PIN_TOUCH_INTERRUPT)==LOW){display.getTouch(&touchPoint);delay(20);}
  esp_sleep_enable_ext0_wakeup(PIN_TOUCH_INTERRUPT,0);
  // Do this several times so that it 'blanks' the data behind it
  display.startWrite(); display.fillScreen(TFT_WHITE);
  display.startWrite(); display.fillScreen(TFT_WHITE);
  display.startWrite(); display.fillScreen(TFT_WHITE);
  display.startWrite(); display.fillScreen(TFT_WHITE);
  display.setTextDatum(middle_center);display.setTextColor(TFT_BLACK,TFT_WHITE);
  display.loadFont(SpaceMono42);
  // Add friendlier messages
  display.setTextSize(1);display.drawString("Tap to Wake Me Up!",display.width()/2,display.height()/2);
  display.setTextSize(0.6);display.drawString("ZzZZz ... I'm Sleeping",display.width()/2,display.height()/3);
  display.endWrite(); display.display();
  WiFi.disconnect(true);WiFi.mode(WIFI_OFF);display.setBrightness(0);
  gpio_hold_en(PIN_MAIN_POWER);gpio_deep_sleep_hold_en();Serial.flush();esp_deep_sleep_start();
}

// ---------- Touch ----------
// Returns the column index (0,1,2) of the touched cell in a 3-cell row.
int columnIndexFromX(const Rect& bounds,int cellWidth,int touchX){
  return constrain((touchX-bounds.x)/cellWidth,0,2);
}

// Returns the button index (0..5) of the touched button in the 3x2 grid of the Lighting card.
int gridIndexFromXY(const ActionCard& actionCard,int touchX,int touchY){
  int cellWidth=actionCard.bounds.w/3, rowHeight=actionCard.buttonsHeight/2, gridTop=actionCard.bounds.y+actionCard.titleHeight;
  return constrain((touchY-gridTop)/rowHeight,0,1)*3+constrain((touchX-actionCard.bounds.x)/cellWidth,0,2);
}

bool touchAction(ActionCard& actionCard,int touchX,int touchY){
  if(!inRect(actionCard.bounds,touchX,touchY)||touchY<actionCard.bounds.y+actionCard.titleHeight)return false;
  int pressedIndex=&actionCard==&lighting?gridIndexFromXY(actionCard,touchX,touchY):columnIndexFromX(actionCard.bounds,actionCard.cellWidth,touchX);
  display.startWrite();drawAction(actionCard,pressedIndex);display.endWrite();
  showStatus((String(actionCard.label[pressedIndex])+"...").c_str());
  bool ok=haCall(actionCard.domain[pressedIndex],actionCard.service[pressedIndex],actionCard.entity[pressedIndex]);
  showStatus(ok?"OK":"Error HA Command");delay(300);
  refreshAction(actionCard);display.startWrite();drawAction(actionCard);display.endWrite();return true;
}

bool touchCovers(int touchX,int touchY){
  if(!inRect(coversBlock.bounds,touchX,touchY))return false;
  int selectorTop=coversBlock.bounds.y+coversBlock.titleHeight;
  int buttonsTop=selectorTop+coversBlock.selectHeight;

  // ---- selection of the blind to control ----
  if(touchY>=selectorTop&&touchY<buttonsTop){
    int selectorCellWidth=coversBlock.bounds.w/COVER_COUNT;
    selectedCover=constrain((touchX-coversBlock.bounds.x)/selectorCellWidth,0,COVER_COUNT-1);
    display.startWrite();drawCovers();display.endWrite();
    return true;
  }

  // ---- command to raise/lower/stop : always sent to HA ----
  if(touchY>=buttonsTop){
    int cellWidth=coversBlock.bounds.w/3;
    int pressedIndex=constrain((touchX-coversBlock.bounds.x)/cellWidth,0,2);
    CoverCard& selectedCoverCard=covers[selectedCover];
    display.startWrite();drawCovers(-1,pressedIndex);display.endWrite();
    const char* service=pressedIndex==0?"open_cover":pressedIndex==1?"stop_cover":"close_cover";
    showStatus((String(selectedCoverCard.title)+" : "+(pressedIndex==0?"raising...":pressedIndex==1?"stopping...":"lowering...")).c_str());
    bool ok=haCall("cover",service,selectedCoverCard.entity);
    showStatus(ok?"OK":"Error HA Command");delay(300);
    refreshCover(selectedCoverCard);
    display.startWrite();drawCovers();display.endWrite();
  }
  return true;
}

bool touchClimate(int touchX,int touchY){
  if(!inRect(climate.bounds,touchX,touchY))return false;

  int modeRowTop=climate.bounds.y+climate.titleHeight;
  int tempRowTop=modeRowTop+climate.modeHeight;
  int infoRowTop=tempRowTop+climate.tempHeight;

  // ---- mode row : Heating / Cooling / Off ----
  if(touchY>=modeRowTop&&touchY<tempRowTop){
    int pressedIndex=constrain((touchX-climate.bounds.x)/climate.cellWidth,0,2);
    display.startWrite();drawClimate(pressedIndex,-1);display.endWrite();
    showStatus((String("Mode : ")+HVAC_LABELS[pressedIndex]+"...").c_str());
    bool ok=haCall("climate","set_hvac_mode",ENTITY_CLIMATE,String(",\"hvac_mode\":\"")+HVAC_MODES[pressedIndex]+"\"");
    if(ok)hvacMode=HVAC_MODES[pressedIndex];
    showStatus(ok?"OK":"Error HA Command");delay(300);
    refreshClimate();
    display.startWrite();drawClimate();display.endWrite();
    showStatus(currentPage==1?"Page 1/2":"Page 2/2");
    return true;
  }

  // ---- temperature row : -, value, + ----
  if(touchY>=tempRowTop&&touchY<infoRowTop){
    int pressedIndex=constrain((touchX-climate.bounds.x)/climate.cellWidth,0,2);
    if(pressedIndex!=1){
      float newTargetTemp=constrain(targetTemp+(pressedIndex==0?-TEMP_STEP:TEMP_STEP),TEMP_MIN,TEMP_MAX);
      display.startWrite();drawClimate(-1,pressedIndex);display.endWrite();
      showStatus(pressedIndex==0?"Temperature -...":"Temperature +...");
      char tempBuffer[8];snprintf(tempBuffer,sizeof(tempBuffer),"%.1f",newTargetTemp);
      bool ok=haCall("climate","set_temperature",ENTITY_CLIMATE,String(",\"temperature\":")+tempBuffer);
      if(ok){targetTemp=newTargetTemp;hasTarget=true;}
      showStatus(ok?"OK":"Error HA Command");delay(300);
    }
    // Redraws only the climate card, without full screen erase.
    display.startWrite();drawClimate();display.endWrite();
    showStatus(currentPage==1?"Page 1/2":"Page 2/2");
    return true;
  }

  return true; // touching the title: ignored but the event is consumed
}

bool touchSpotify(int touchX,int touchY){
  if(!inRect(spotifyCard.bounds,touchX,touchY))return false;

  int buttonsTop=spotifyCard.bounds.y+spotifyCard.titleHeight+spotifyCard.infoHeight;
  if(touchY<buttonsTop)return false;

  int cellWidth=spotifyCard.bounds.w/3;
  int pressedIndex=constrain((touchX-spotifyCard.bounds.x)/cellWidth,0,2);

  display.startWrite();drawSpotify(pressedIndex);display.endWrite();

  const char* service = pressedIndex==0 ? "media_previous_track" :
                        pressedIndex==1 ? "media_play_pause" :
                               "media_next_track";
  const char* label = pressedIndex==0 ? "Spotify : Previous..." :
                      pressedIndex==1 ? "Spotify : Play/Pause..." :
                             "Spotify : Next ...";
  showStatus(label);

  bool ok=haCall("media_player",service,ENTITY_SPOTIFY);
  showStatus(ok?"OK":"Error HA command");
  delay(300);

  // No periodic refresh: we only refresh after a command.
  refreshSpotify();
  display.startWrite();drawSpotify();display.endWrite();
  showStatus(currentPage==1?"Page 1/2":"Page 2/2");
  return true;
}

bool touchWifi(int touchX,int touchY){
  if(hasVoucher||!inRect(wifiCard.bounds,touchX,touchY))return false;
  int buttonTop=wifiCard.bounds.y+wifiCard.titleHeight;
  if(touchY<buttonTop)return false;
  display.startWrite();drawWifi(true);display.endWrite();
  showStatus("Creating WiFi Hotspot... ");
  bool ok=haCall("input_button","press",ENTITY_WIFI_BUTTON);
  if(ok){delay(2000);refreshVoucher();showStatus(hasVoucher?("Code : "+voucherCode).c_str():"Code introuvable");}
  else showStatus("Error HA Command");
  drawPage();delay(300);return true;
}

// ---------- Setup / Loop ----------
void setup(){
  Serial.begin(115200);
  pinMode(PIN_BUTTON_PREV,INPUT_PULLUP); pinMode(PIN_BUTTON_NEXT,INPUT_PULLUP);

  // The screen must be initialized (begin) before loading a font on it.
  display.begin();display.setRotation(0);display.setEpdMode(epd_mode_t::epd_fastest);
  display.loadFont(SpaceMono26);

  setupLayout();
  resetActivityTimer();

  // Immediate display with default values ("--", OFF...) : the screen
  // becomes usable immediately, without waiting for Wi-Fi or HA connection.
  drawPage();

  WiFi.setSleep(false); // no Wi-Fi power-saving : faster connections/requests
  connectWifi();

  // Single HTTP call grouped for the entire page 1 (climate, indoor/outdoor/bedroom,
  // modes, Spotify), instead of a dozen sequential requests.
  refreshStartupData();

  // Blinds and Wi-Fi voucher (page 2) : loaded only on the first display
  // of this page, see changePage().

  // Redraws only the cards concerned (no full screen erase, cleanScreen() already
  // occurred in the drawPage() above).
  display.startWrite();
  for(auto actionCard:actionCards)drawAction(*actionCard);
  drawSpotify();
  drawClimate();
  display.endWrite();
  showStatus(currentPage==1?"Page 1/2":"Page 2/2");
}

void loop(){
  if(DEEPSLEEP_ENABLED&&millis()-lastActivityTime>=DEEPSLEEP_TIMEOUT_MS)enterDeepSleep();
  if(pageButtons())return;

  lgfx::touch_point_t touchPoint;
  if(display.getTouch(&touchPoint)){
    resetActivityTimer();
    if(currentPage==1){
      for(auto actionCard:actionCards)if(touchAction(*actionCard,touchPoint.x,touchPoint.y))return;
      if(touchSpotify(touchPoint.x,touchPoint.y))return;
      if(touchClimate(touchPoint.x,touchPoint.y))return;
    }else{
      if(touchCovers(touchPoint.x,touchPoint.y))return;
      touchWifi(touchPoint.x,touchPoint.y);
    }
  }
  delay(50);
}
