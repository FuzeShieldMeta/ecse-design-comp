
// Example sketch which shows how to display some patterns on two
// vertically stacked 64x32 panels as one 64x64 display.
//

#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>


#define PANEL_RES_X 64      // Number of pixels wide of each INDIVIDUAL panel module. 
#define PANEL_RES_Y 32     // Number of pixels tall of each INDIVIDUAL panel module.
#define PANEL_ROWS  2      // Two panels stacked vertically
#define PANEL_COLS  1
#define PANEL_CHAIN (PANEL_ROWS * PANEL_COLS)

// Panel 1 starts at the top-right and its OUT connector feeds panel 2.
// Change this if your physical input connector is in a different corner.
#define PANEL_CHAIN_TYPE CHAIN_TOP_RIGHT_DOWN

// HUB75 wiring for a 38-pin ESP32-WROOM-32 development board.
// These GPIOs avoid the flash bus (6-11), input-only pins (34-39),
// UART0 (1 and 3), and boot-strapping pins (0, 2, 4, 5, 12 and 15).
#define HUB75_R1   25
#define HUB75_G1   26
#define HUB75_B1   27
#define HUB75_R2   14
#define HUB75_G2   13
#define HUB75_B2   33
#define HUB75_A    23
#define HUB75_B    19
#define HUB75_C    18
#define HUB75_D    17
#define HUB75_E    -1      // Not used by a 64x32 (1/16 scan) panel
#define HUB75_LAT  32
#define HUB75_OE   21
#define HUB75_CLK  22
 
//MatrixPanel_I2S_DMA dma_display;
MatrixPanel_I2S_DMA *dma_display = nullptr;
VirtualMatrixPanel_T<PANEL_CHAIN_TYPE> *display = nullptr;

uint16_t myBLACK, myWHITE, myRED, myGREEN, myBLUE;

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
// From: https://gist.github.com/davidegironi/3144efdc6d67e5df55438cc3cba613c8
uint16_t colorWheel(uint8_t pos) {
  if(pos < 85) {
    return display->color565(pos * 3, 255 - pos * 3, 0);
  } else if(pos < 170) {
    pos -= 85;
    return display->color565(255 - pos * 3, 0, pos * 3);
  } else {
    pos -= 170;
    return display->color565(0, pos * 3, 255 - pos * 3);
  }
}

void drawText(int colorWheelOffset)
{
  
  // draw text with a rotating colour
  display->setTextSize(1);     // size 1 == 8 pixels high
  display->setTextWrap(false); // Don't wrap at end of line - will do ourselves

  display->setCursor(5, 0);    // start at top left, with 8 pixel of spacing
  uint8_t w = 0;
  const char *str = "ESP32 DMA";
  for (w=0; w<strlen(str); w++) {
    display->setTextColor(colorWheel((w*32)+colorWheelOffset));
    display->print(str[w]);
  }

  display->println();
  display->print(" ");
  for (w=9; w<18; w++) {
    display->setTextColor(colorWheel((w*32)+colorWheelOffset));
    display->print("*");
  }
  
  display->println();

  display->setTextColor(display->color444(15,15,15));
  display->println("LED MATRIX!");

  // print each letter with a fixed rainbow color
  display->setTextColor(display->color444(0,8,15));
  display->print('6');
  display->setTextColor(display->color444(15,4,0));
  display->print('4');
  display->setTextColor(display->color444(15,15,0));
  display->print('x');
  display->setTextColor(display->color444(8,15,0));
  display->print('6');
  display->setTextColor(display->color444(8,0,15));
  display->print('4');

  // Jump a half character
  display->setCursor(34, 24);
  display->setTextColor(display->color444(0,15,15));
  display->print("*");
  display->setTextColor(display->color444(15,0,0));
  display->print('R');
  display->setTextColor(display->color444(0,15,0));
  display->print('G');
  display->setTextColor(display->color444(0,0,15));
  display->print("B");
  display->setTextColor(display->color444(15,0,8));
  display->println("*");

}


void setup() {

  // Module configuration
  HUB75_I2S_CFG mxconfig(
    PANEL_RES_X,   // module width
    PANEL_RES_Y,   // module height
    PANEL_CHAIN    // Chain length
  );

  // Do not rely on the library defaults: they differ between ESP32 variants
  // and commonly include GPIO12, an ESP32-WROOM-32 boot-strapping pin.
  mxconfig.gpio.r1 = HUB75_R1;
  mxconfig.gpio.g1 = HUB75_G1;
  mxconfig.gpio.b1 = HUB75_B1;
  mxconfig.gpio.r2 = HUB75_R2;
  mxconfig.gpio.g2 = HUB75_G2;
  mxconfig.gpio.b2 = HUB75_B2;
  mxconfig.gpio.a = HUB75_A;
  mxconfig.gpio.b = HUB75_B;
  mxconfig.gpio.c = HUB75_C;
  mxconfig.gpio.d = HUB75_D;
  mxconfig.gpio.e = HUB75_E;
  mxconfig.gpio.lat = HUB75_LAT;
  mxconfig.gpio.oe = HUB75_OE;
  mxconfig.gpio.clk = HUB75_CLK;

  //mxconfig.clkphase = false;
  //mxconfig.driver = HUB75_I2S_CFG::FM6126A;

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(90); //0-255
  dma_display->clearScreen();

  // Convert the electrical 128x32 chain into a 64x64 drawing surface.
  display = new VirtualMatrixPanel_T<PANEL_CHAIN_TYPE>(
    PANEL_ROWS, PANEL_COLS, PANEL_RES_X, PANEL_RES_Y
  );
  display->setDisplay(*dma_display);

  myBLACK = display->color565(0, 0, 0);
  myWHITE = display->color565(255, 255, 255);
  myRED = display->color565(255, 0, 0);
  myGREEN = display->color565(0, 255, 0);
  myBLUE = display->color565(0, 0, 255);
  

  display->fillScreen(myWHITE);
  
  // fix the screen with green
  display->fillRect(0, 0, display->width(), display->height(), display->color444(0, 15, 0));
  delay(500);

  // draw a box in yellow
  display->drawRect(0, 0, display->width(), display->height(), display->color444(15, 15, 0));
  delay(500);

  // draw an 'X' in red
  display->drawLine(0, 0, display->width()-1, display->height()-1, display->color444(15, 0, 0));
  display->drawLine(display->width()-1, 0, 0, display->height()-1, display->color444(15, 0, 0));
  delay(500);

  // draw a blue circle
  display->drawCircle(10, 10, 10, display->color444(0, 0, 15));
  delay(500);

  // fill a violet circle
  display->fillCircle(40, 21, 10, display->color444(15, 0, 15));
  delay(500);

  // fill the screen with 'black'
  display->fillScreen(display->color444(0, 0, 0));

  //drawText(0);

}

uint8_t wheelval = 0;
void loop() {

    // animate by going through the colour wheel for the first two lines
    drawText(wheelval);
    wheelval +=1;

    delay(20); 
/*
  drawText(0);
  delay(2000);
  dma_display->clearScreen();
  dma_display->fillScreen(myBLACK);
  delay(2000);
  dma_display->fillScreen(myBLUE);
  delay(2000);
  dma_display->fillScreen(myRED);
  delay(2000);
  dma_display->fillScreen(myGREEN);
  delay(2000);
  dma_display->fillScreen(myWHITE);
  dma_display->clearScreen();
  */
  
}
