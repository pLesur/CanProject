#include <MCP23X08.h>
#include <Spi.h>
#include <MCP2510.h>
#include <Canutil.h>
#include <Wire.h>
#include <SPI.h>
#include <LiquidCrystal.h>
#include "Adafruit_MCP4725.h"

/********* VALUES USED FOR THE STATE MACHINE ************/
#define I2C_INTERRUPT -1
#define UNDEFINED_STATE 0
#define SLAVE 1
#define MASTER 2
#define NORMAL_MODE 3
#define RECEIVED_WAKE_UP 4
#define RECEIVED_LIST 5
#define SPI_INTERRUPT 6
#define NORMAL_MODE_RECEIVED 7

volatile int state = UNDEFINED_STATE;
volatile int oldState = UNDEFINED_STATE;


/************* TODO : WILL PROBABLY HAVE TO DEFINE SUBSTATE VARIABLE *****************/

/************** PIN USED  FOR THE INTERRUPTS *************/
#define I2C_INTERRUPT_PIN 1
#define SPI_CAN_INTERRUPT_PIN 0

/************ DEFINE THE NODE ID *********************/
#define OWN_ID 2

/**************** SETUP THE I2C/SPI/CAN OBJECTS ****************/
MCP23008 i2c_io(MCP23008_ADDR);         // Init MCP23008
MCP23S08 spi_io(MCP23S08_ADDR, 10);        // Init MCP23S08
MCP2510  can_dev (9); // defines pb1 (arduino pin9) as the _CS pin for MCP2510
Canutil  canutil(can_dev);
LiquidCrystal  lcd(15, 14, 4, 5, 6, 7);
Adafruit_MCP4725 dac_io;
/************* DECLARE VOLATILE VARIABLE FOR THE GPIOS RAEADS, AND FORWARD DECLARE FUNCTIONS ***************/
volatile uint8_t i2cGpiosValues;
volatile uint8_t spiGpiosValues;
volatile uint8_t oldSpioGpiosValues;
uint8_t txstatus;
volatile uint8_t opmode;
volatile uint8_t idsList[8];
volatile uint16_t msgID;
volatile int recSize;
int storedIdsNumber = 1;
volatile uint8_t canDataReceived[8];

/////////// USED OR THE LCD
int currentId = -1;
int oldId = -1;

void spiCanInterrupt();
void i2cInterruptCallback();
void scanNetwork();
void i2cInterruptLogic();
bool handleWakeUp();
bool getIdsList();
void sendIdsList();
void displayKeyboardSelection();
void sendI2cButtonsValues(uint8_t opCode);
void handleSpiInterrupt();
void sendPotentiometerValue(uint8_t sender);
void askPotentiometerValue();
void handlePotentiometerValue();
/********************** SETUP **********************/
void setup() {

  for (int i = 0; i < 8; i++) {
    idsList[i] = 0;
  }

  // SETUP UP LCD
  lcd.begin(16, 2);

  // SETUP DAC
  uint8_t mcpAddress = 0b11000000;
  dac_io.begin(mcpAddress);

  //  SETUP SPI INTERRUPTS
  spi_io.Write(IOCON, VAL23S08_IOCON );  // Sets defaults for MCP23S08, in particular puts interrupt output open-drain
  spi_io.Write(IODIR, 0x0F);   // sets port direction for individual bits
  spi_io.Write(INTCON, 0x0F); // activate interrupts on the inputs (buttons)
  spi_io.Write(DEFVAL, 0x0F);  // DEFVAL  sets the default values for the pin (high state here). Interrupts occurs on a low state
  spi_io.Write(GPINTEN, 0x00);  // activate interrupts on the inputs (buttons)

  //  SETUP I2C INTERRUPTS
  i2c_io.Write(IOCON, VAL23S08_IOCON );  // Sets defaults for MCP23S08, in particular puts interrupt output open-drain
  i2c_io.Write(IODIR, 0x0F);   // sets port direction for individual bits
  i2c_io.Write(INTCON, 0x0F);  // interrupts are triggered when compared to DEFVAL values, not on pin-change
  i2c_io.Write(DEFVAL, 0x0F);  // DEFVAL  sets the default values for the pin (high state here). Interrupts occurs on a low state
  i2c_io.Write(GPINTEN, 0x0F);  // activate interrupts on the inputs (buttons)

  // SETUP CANUTIL (COPY PASTA)
  canutil.flashRxbf();  //just for fun!
  can_dev.write(CANINTE, 0x01); //disables all interrupts but RX0IE (received message in RX buffer 0)
  can_dev.write(CANINTF, 0x00);  // Clears all interrupts flags

  canutil.setClkoutMode(0, 0); // disables CLKOUT
  canutil.setTxnrtsPinMode(0, 0, 0); // all TXnRTS pins as all-purpose digital input

  canutil.setOpMode(4); // sets configuration mode
  // IMPORTANT NOTE: configuration mode is the ONLY mode where bit timing registers (CNF1, CNF2, CNF3), acceptance
  // filters and acceptance masks can be modified

  canutil.waitOpMode(4);  // waits configuration mode

  // Bit timing section
  //  setting the bit timing registers with Fosc = 16MHz -> Tosc = 62,5ns
  // data transfer = 125kHz -> bit time = 8us, we choose arbitrarily 8us = 16 TQ  (8 TQ <= bit time <= 25 TQ)
  // time quanta TQ = 2(BRP + 1) Tosc, so BRP =3
  // sync_seg = 1 TQ, we choose prop_seg = 2 TQ
  // Phase_seg1 = 7TQ yields a sampling point at 10 TQ (60% of bit length, recommended value)
  // phase_seg2 = 6 TQ SJSW <=4 TQ, SJSW = 1 TQ chosen
  can_dev.write(CNF1, 0x03); // SJW = 1, BRP = 3
  can_dev.write(CNF2, 0b10110001); //BLTMODE = 1, SAM = 0, PHSEG = 6, PRSEG = 1
  can_dev.write(CNF3, 0x05);  // WAKFIL = 0, PHSEG2 = 5

  // SETUP MASKS / FILTERS FOR CAN
  canutil.setRxOperatingMode(1, 1, 0);  // standard ID messages only  and rollover
  canutil.setAcceptanceFilter(0x102, 0, 0, 1); // 0 <= stdID <= 2047, 0 <= extID <= 262143, 1 = extended, filter# 0
  canutil.setAcceptanceFilter(0x101, 0, 0, 5); // 0 <= stdID <= 2047, 0 <= extID <= 262143, 1 = extended, filter# 0
  canutil.setAcceptanceFilter(0x100, 0, 0, 0); // 0 <= stdID <= 2047, 0 <= extID <= 262143, 1 = extended, filter# 1
  canutil.setAcceptanceMask(0x000, 0x00000000, 0); // 0 <= stdID <= 2047, 0 <= extID <= 262143, buffer# 0

  canutil.setOpMode(0); // sets normal mode
  opmode = canutil.whichOpMode();

  canutil.setTxBufferDataLength(0, 1, 0); // TX normal data, 1 byte long, with buffer 0

  Serial.begin(9600);  // used for debug purpose only
  Serial.write("leaving setup\n");

  /************** SETUP INTERRUPTS ****************/
  attachInterrupt(I2C_INTERRUPT_PIN, i2cInterruptCallback, FALLING);
  attachInterrupt(SPI_CAN_INTERRUPT_PIN, spiCanInterrupt, FALLING);

}

/******************************* LOOP *********************/
void loop() {
  switch (state) {
    case I2C_INTERRUPT:
      i2cInterruptLogic();
      break;

    case UNDEFINED_STATE:
      lcd.clear();
      lcd.write("undefined state");
      Serial.println(digitalRead(2));
      break;

    case SLAVE:
      lcd.clear();
      lcd.write("slave");
      break;

    case MASTER:
      lcd.clear();
      lcd.write("master");
      scanNetwork();
      delay(500);
      Serial.println("scanned, gonna send the list");
      sendIdsList();
      oldState = MASTER;
      state = NORMAL_MODE;
      //detachInterrupt(I2C_INTERRUPT_PIN);
      break;

    case NORMAL_MODE:
      spiGpiosValues = spi_io.Read(GPIO);
      if ( oldSpioGpiosValues != spiGpiosValues ) {
        Serial.print(" SPI GPIO VALUES: ");
        Serial.println(spiGpiosValues);
        oldSpioGpiosValues = spiGpiosValues;
        if ( (spiGpiosValues & 0b00000111) != 0b00000111 ) {
          oldState = state;
          state = SPI_INTERRUPT;

        }
      }

      displayKeyboardSelection();
      break;

    case RECEIVED_WAKE_UP:
      if (handleWakeUp()) {
        oldState = state;
        state = SLAVE;
      } else {
        oldState = state;
        state = UNDEFINED_STATE;
      } Serial.print("received wake up !");

      break;

    case RECEIVED_LIST:
      getIdsList();
      oldState = state;
      state = NORMAL_MODE;
      Serial.print("Received list !");
      break;

    case NORMAL_MODE_RECEIVED:
      handleNormalModeMessage();
      Serial.print("normal mode received\n");
      oldState = state;
      state = NORMAL_MODE;
      break;

    case SPI_INTERRUPT:
      handleSpiInterrupt();
      oldState = state;
      state = NORMAL_MODE;
      Serial.println("spi int !!!");
      break;

    default:
      if (state != oldState) Serial.write("huge problem !\n");
      break;
  }
  //  Serial.println("\nMsgID = \n");
  //Serial.println(msgID - 0x100);
}

/************* INTERRUPT CALLBACK SPI/CAN ***************/
void spiCanInterrupt() {



  recSize = canutil.whichRxDataLength(0); // checks the number of bytes received in buffer 0 (max = 8)
  for (int i = 0; i < recSize; i++) { // gets the bytes
    canDataReceived[i] = canutil.receivedDataValue(0, i);
  }
  msgID = canutil.whichStdID(0);

  switch (state) {
    case UNDEFINED_STATE:
      if (msgID == 0x100) {
        oldState = state;
        state = RECEIVED_WAKE_UP;
      }
      break;

    case SLAVE:
      if (msgID == 0x102) {
        oldState = state;
        state = RECEIVED_LIST;
      }
      break;

    case NORMAL_MODE:// TODO : ALL THE WEIRD SHIT
      oldState = state;
      state = NORMAL_MODE_RECEIVED;
      break;

    case MASTER:
      if (msgID == 0x101) {
        idsList[storedIdsNumber++] = canDataReceived[0];
      }
      break;

    default:
      state = -100;
      break;
  }
  can_dev.write(CANINTF, 0x00);  // Clears all interrupts flags
}

/************* INTERRUPT "CALLBACK" I2C ***************/
void i2cInterruptCallback() {
  oldState = state;
  state = I2C_INTERRUPT;
}

void i2cInterruptLogic() {
  Serial.write("entering interrupt I2C logic\n");
  delay(500);
  i2cGpiosValues = i2c_io.Read(INTCAP);  // Reading the gpio actually clear the flag
  switch (oldState) {
    case UNDEFINED_STATE:
      if ( (i2cGpiosValues | 0b11110111) == 0b11110111 ) {  // test if SW9 has been pressed
        state = MASTER;
      } else {
        state = UNDEFINED_STATE;
      }
      break;
    case NORMAL_MODE:
      // do stuff related to normal MODE
      //which will clear the flag cuz gpio read
      state = NORMAL_MODE;
      break;
    case MASTER:
      state = MASTER;
    default:
      state = oldState;
      Serial.write("problem I2C interrupt\n");
      break;
  }
  Serial.write("leaving interrupt I2C logic\n");
  Serial.print("state = ");
  Serial.println(state);
}

void scanNetwork() {
  uint8_t message[8];
  message[0] = 1;
  idsList[0] = OWN_ID;
  for (unsigned int i = 1; i <= 5 ; i++) {
    uint16_t message_id = 0x100;
    canutil.setTxBufferID(message_id, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
    canutil.setTxBufferDataField(message, 0);   // fills TX buffer
    Serial.write("writing with id n100 with message: ");
    Serial.println(message[0]);
    canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority*/
    do {
      txstatus = 0;
      txstatus = canutil.isTxError(0);  // checks tx error
      txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
      txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
      txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
    }
    while (txstatus != 0);
    unsigned long startTime = millis();
    while ( (millis() - startTime) < 2000) {}
    message[0]++;
  }
  message[0] = 0;
}


bool handleWakeUp() {

  Serial.println(canDataReceived[0]);
  Serial.println(OWN_ID);
  if (canDataReceived[0] == OWN_ID) { // TODO ANSWER WITH ACKNOWLEDGE
    Serial.println("la valeur que l'on veut");
    Serial.println(canDataReceived[0]);
    Serial.write("Received own ID !");
    uint8_t message[8];
    message[0] = OWN_ID;
    uint16_t message_id = 0x101;
    canutil.setTxBufferID(message_id, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
    canutil.setTxBufferDataField(message, 0);   // fills TX buffer
    canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority*/
    do {
      txstatus = 0;
      txstatus = canutil.isTxError(0);  // checks tx error
      txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
      txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
      txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
    }
    while (txstatus != 0);
    return true;
  }
  return false;
}

bool getIdsList() {
  for (int i = 0; i < recSize; i++) { // gets the bytes
    idsList[i] = canDataReceived[i];
    Serial.println("coucou c est moi");
  }
  storedIdsNumber = recSize;
  return true;
}

void sendIdsList() {
  uint8_t message[8];
  for (int i = 0; i < storedIdsNumber; i++) {
    message[i] = idsList[i];
  }

  uint16_t message_id = 0x102;
  canutil.setTxBufferDataLength(0, storedIdsNumber , 0); // TX normal data, 1 byte long, with buffer 0
  canutil.setTxBufferID(message_id, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
  canutil.setTxBufferDataField(message, 0);   // fills TX buffer

  canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority*/
  do {
    txstatus = 0;
    txstatus = canutil.isTxError(0);  // checks tx error
    txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
    txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
    txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
  }
  while (txstatus != 0);
}


void displayKeyboardSelection() {
  currentId = map(analogRead(3), 0, 1024, 0, storedIdsNumber);
  if (currentId != oldId) {
    lcd.clear();
    lcd.print(idsList[currentId]);
    oldId = currentId;
  }

}

void handleSpiInterrupt() {
  uint8_t interestingValues =  spiGpiosValues & 0b00000111;
  Serial.print("interesting value = ");
  Serial.println(interestingValues);
  if ( interestingValues == 6) {
    //TODO SW8
    sendI2cButtonsValues(0x00);
  } else if (interestingValues == 5) {
    //TODO SW7
    sendI2cButtonsValues(0x01);
  } else if (interestingValues == 3) {
    // TODO SW6
    askPotentiometerValue();
  }
}

void sendI2cButtonsValues(uint8_t opCode) {
  uint8_t message[8];
  message[0] = idsList[currentId];
  message[1] = opCode;
  message[2] = i2c_io.Read(GPIO);
  Serial.print("target id is:");
  Serial.println(message[0]);
  Serial.print("op code is:");
  Serial.println(message[1]);
  Serial.print("op code shoudl be:");
  Serial.println(opCode);
  canutil.setTxBufferDataLength(0, 3 , 0); // TX normal data, 1 byte long, with buffer 0
  canutil.setTxBufferID(0x200+OWN_ID, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
  canutil.setTxBufferDataField(message, 0);   // fills TX buffer

  canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority*/
  do {
    txstatus = 0;
    txstatus = canutil.isTxError(0);  // checks tx error
    txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
    txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
    txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
  }
  while (txstatus != 0);
}




void handleNormalModeMessage() {
  if ( canDataReceived[0] != OWN_ID ) {  // 0 = recipioent node id = our own id
    return;
  }
  uint16_t sender = msgID - 0x200;
  Serial.print("op code is :");
  Serial.println(canDataReceived[1]);
  switch (canDataReceived[1]) { // 1 = opCode = cquon fait
    case 0x00:
      // TODO HANDLE WRITE TO SPI LEDS
      uint8_t newSpiLedsValues;
      newSpiLedsValues = (canDataReceived[2] << 4);  // shift the button values sent to led values
      spi_io.Write(GPIO, newSpiLedsValues);
      break;
    case 0x01:
      // TODO HANDLE WRITE TO I2C LEDS
      uint8_t newI2cLedsValues;
      newI2cLedsValues = (canDataReceived[2] << 4);  // shift the button values sent to led values
      i2c_io.Write(GPIO, newI2cLedsValues);
      break;
    case 0x02:
      // TODO SEND POTAR VALUE
      sendPotentiometerValue(sender);
      break;
    case 0x03:
      handlePotentiometerValue();
      break;
  }
}

void askPotentiometerValue() {
  uint8_t message[8];
  message[0] = idsList[currentId];
  message[1] = 0x02;
  canutil.setTxBufferDataLength(0, 2 , 0); // TX normal data, 1 byte long, with buffer 0
  canutil.setTxBufferID(0x200+OWN_ID, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
  canutil.setTxBufferDataField(message, 0);   // fills TX buffer

  canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority
  do {
    txstatus = 0;
    txstatus = canutil.isTxError(0);  // checks tx error
    txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
    txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
    txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
  }
  while (txstatus != 0);
}

void sendPotentiometerValue(uint8_t sender) {
  uint8_t message[8];
  message[0] = sender;
  message[1] = 0x03;
  int potentiometerValue = analogRead(3);
  message[2] = potentiometerValue & 0xFF;
  message[3] = (potentiometerValue >> 8);
  canutil.setTxBufferDataLength(0, 4 , 0); // TX normal data, 1 byte long, with buffer 0
  canutil.setTxBufferID(0x200+OWN_ID, 0, 0, 0); // sets the message ID, specifies standard message (i.e. short ID) with buffer 0
  canutil.setTxBufferDataField(message, 0);   // fills TX buffer

  canutil.messageTransmitRequest(0, 1, 3); // requests transmission of buffer 0 with highest priority
  do {
    txstatus = 0;
    txstatus = canutil.isTxError(0);  // checks tx error
    txstatus = txstatus + canutil.isArbitrationLoss(0);  // checks for arbitration loss
    txstatus = txstatus + canutil.isMessageAborted(0);  // ckecks for message abort
    txstatus = txstatus + canutil.isMessagePending(0);   // checks transmission
  }
  while (txstatus != 0);
}

void handlePotentiometerValue() {
  int newDacValue = canDataReceived[2] + (canDataReceived[3] << 8);
  dac_io.setVoltage(newDacValue,false);
}

