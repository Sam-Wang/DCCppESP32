/**********************************************************************
DCC++ BASE STATION FOR ESP32

COPYRIGHT (c) 2017 Mike Dunston

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses
**********************************************************************/

#include "DCCppESP32.h"
#include <esp32-hal-timer.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include "SignalGenerator.h"
#include "MotorBoard.h"

// Define constants for DCC Signal pattern

// this controls the timer tick frequency
#define DCC_TIMER_PRESCALE 80

// number of microseconds for sending a zero via the DCC encoding
#define DCC_ZERO_BIT_TOTAL_DURATION 196
// number of microseconds for each half of the DCC signal for a zero
#define DCC_ZERO_BIT_PULSE_DURATION 98

// number of microseconds for sending a one via the DCC encoding
#define DCC_ONE_BIT_TOTAL_DURATION 116
// number of microseconds for each half of the DCC signal for a one
#define DCC_ONE_BIT_PULSE_DURATION 58

SignalGenerator dccSignal[MAX_DCC_SIGNAL_GENERATORS];

uint8_t idlePacket[] = {0xFF, 0x00};
uint8_t resetPacket[] = {0x00, 0x00};

void configureDCCSignalGenerators() {
  dccSignal[DCC_SIGNAL_OPERATIONS].configureSignal<DCC_SIGNAL_OPERATIONS>("OPS",
    DCC_SIGNAL_PIN_OPERATIONS, 512);
  dccSignal[DCC_SIGNAL_PROGRAMMING].configureSignal<DCC_SIGNAL_PROGRAMMING>("PROG",
    DCC_SIGNAL_PIN_PROGRAMMING, 64);
}

void startDCCSignalGenerators() {
  dccSignal[DCC_SIGNAL_OPERATIONS].startSignal<DCC_SIGNAL_OPERATIONS>();
  dccSignal[DCC_SIGNAL_PROGRAMMING].startSignal<DCC_SIGNAL_PROGRAMMING>();
}

void stopDCCSignalGenerators() {
  dccSignal[DCC_SIGNAL_OPERATIONS].stopSignal<DCC_SIGNAL_OPERATIONS>();
  dccSignal[DCC_SIGNAL_PROGRAMMING].stopSignal<DCC_SIGNAL_PROGRAMMING>();
}

void loadBytePacket(SignalGenerator &signalGenerator, uint8_t *data, uint8_t length, uint8_t repeatCount) {
  std::vector<uint8_t> packet;
  for(int i = 0; i < length; i++) {
    packet.push_back(data[i]);
  }
  signalGenerator.loadPacket(packet, repeatCount);
}

bool IRAM_ATTR SignalGenerator::getNextBitToSend() {
  const uint8_t bitMask[] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
  bool result = false;
  // if we are processing a packet, check if we have sent all bits or repeats
  if(_currentPacket != NULL) {
    if(_currentPacket->currentBit == _currentPacket->numberOfBits) {
      if(_currentPacket->numberOfRepeats > 0) {
        _currentPacket->numberOfRepeats--;
        _currentPacket->currentBit = 0;
      } else {
        // if the current packet is not the idle pack get rid of it
        if(_currentPacket != &_idlePacket) {
          _availablePackets.push(_currentPacket);
        }
        _currentPacket = NULL;
      }
    }
  }
  // if we don't have a packet, check if we have any to send otherwise
  // queue up an idle packet
  if (_currentPacket == NULL) {
    if(!_toSend.empty()) {
      _currentPacket = _toSend.front();
      _toSend.pop();
    } else {
      _currentPacket = &_idlePacket;
      _currentPacket->currentBit = 0;
    }
  }
  // if we have a packet to send, get the next bit from the packet
  if(_currentPacket != NULL) {
    result = _currentPacket->buffer[_currentPacket->currentBit / 8] & bitMask[_currentPacket->currentBit % 8];
    _currentPacket->currentBit++;
  }
  return result;
}

void SignalGenerator::loadPacket(std::vector<uint8_t> data, int numberOfRepeats) {
  #if DEBUG_SIGNAL_GENERATOR
    log_v("[%s] Preparing DCC Packet containing %d bytes, %d repeats [%d in queue]", _name.c_str(), data.size(), numberOfRepeats, _toSend.size());
  #endif
  while(_availablePackets.empty()) {
    delay(2);
  }
  Packet *packet = _availablePackets.front();
  _availablePackets.pop();

  packet->numberOfRepeats = numberOfRepeats;
  packet->currentBit = 0;

  // calculate checksum (XOR)
  // add first byte as checksum byte
  uint8_t checksum = data[0];
  for(int i = 1; i < data.size(); i++)
    checksum ^= data[i];
  data.push_back(checksum);

  // standard DCC preamble
  packet->buffer[0] = 0xFF;
  packet->buffer[1] = 0xFF;
  // first bit of actual data at the end of the preamble
  packet->buffer[2] = 0xFC + bitRead(data[0], 7);
  packet->buffer[3] = data[0] << 1;
  packet->buffer[4] = data[1];
  packet->buffer[5] = data[2] >> 1;
  packet->buffer[6] = data[2] << 7;

  if(data.size() == 3){
    packet->numberOfBits = 49;
  } else{
    packet->buffer[6] += data[3] >> 2;
    packet->buffer[7] = data[3] << 6;
    if(data.size() == 4){
      packet->numberOfBits = 58;
    } else{
      packet->buffer[7] += data[4] >> 3;
      packet->buffer[8] = data[4] << 5;
      if(data.size() == 5){
        packet->numberOfBits = 67;
      } else{
        packet->buffer[8] += data[5] >> 4;
        packet->buffer[9] = data[5] << 4;
        packet->numberOfBits = 76;
      } // >5 bytes
    } // >4 bytes
  } // >3 bytes

#if SHOW_DCC_PACKETS
  String packetHex = "";
  for(int i = 0; i < data.size() + 1; i++) {
    packetHex += String(packet->buffer[i], HEX) + " ";
  }
  log_v("[%s] <* %s / %d / %d>\n", _name.c_str(), packetHex.c_str(),
    packet->numberOfBits, packet->numberOfRepeats);
#endif
  _toSend.push(packet);
}

template<int timerIndex>
void IRAM_ATTR signalGeneratorPulseTimer(void)
{
  auto& signalGenerator = dccSignal[timerIndex];
  if(signalGenerator.getNextBitToSend()) {
    timerAlarmWrite(signalGenerator._pulseTimer, DCC_ONE_BIT_PULSE_DURATION, false);
    timerAlarmWrite(signalGenerator._fullCycleTimer, DCC_ONE_BIT_TOTAL_DURATION, true);
  } else {
    timerAlarmWrite(signalGenerator._pulseTimer, DCC_ZERO_BIT_PULSE_DURATION, false);
    timerAlarmWrite(signalGenerator._fullCycleTimer, DCC_ZERO_BIT_TOTAL_DURATION, true);
  }
  timerWrite(signalGenerator._pulseTimer, 0);
  timerAlarmEnable(signalGenerator._pulseTimer);
  digitalWrite(signalGenerator._directionPin, HIGH);
}

template<int timerIndex>
void IRAM_ATTR signalGeneratorDirectionTimer()
{
  auto& signalGenerator = dccSignal[timerIndex];
  digitalWrite(signalGenerator._directionPin, LOW);
}

template<int timerIndex>
void SignalGenerator::configureSignal(String name, uint8_t directionPin, uint16_t maxPackets) {
  _name = name;
  _directionPin = directionPin;
  _currentPacket = NULL;

  // create packets for this signal generator up front, they will be reused until
  // the base station is shutdown
  for(int index = 0; index < maxPackets; index++) {
    _availablePackets.push(new Packet());
  }

  // force the directionPin to low since it will be controlled by the DCC timer
  pinMode(_directionPin, INPUT);
  digitalWrite(_directionPin, LOW);
  pinMode(_directionPin, OUTPUT);
  startSignal<timerIndex>();
}

template<int timerIndex>
void SignalGenerator::startSignal() {
  // inject the required reset and idle packets into the queue
  // this is required as part of S-9.2.4 section A
  // at least 20 reset packets and 10 idle packets must be sent upon initialization
  // of the base station to force decoders to exit service mode.
  log_i("[%s] Adding reset packet to packet queue", _name.c_str());
  loadBytePacket(dccSignal[timerIndex], resetPacket, 2, 20);
  log_i("[%s] Adding idle packet to packet queue", _name.c_str());
  loadBytePacket(dccSignal[timerIndex], idlePacket, 2, 10);

  log_i("[%s] Configuring Timer(%d) for generating DCC Signal (Full Wave)", _name.c_str(), 2 * timerIndex);
  _fullCycleTimer = timerBegin(2 * timerIndex, DCC_TIMER_PRESCALE, true);
  log_i("[%s] Attaching interrupt handler to Timer(%d)", _name.c_str(), 2 * timerIndex);
  timerAttachInterrupt(_fullCycleTimer, &signalGeneratorPulseTimer<timerIndex>, true);
  log_i("[%s] Configuring alarm on Timer(%d) to %dus", _name.c_str(), 2 * timerIndex, DCC_ONE_BIT_TOTAL_DURATION);
  timerAlarmWrite(_fullCycleTimer, DCC_ONE_BIT_TOTAL_DURATION, true);
  log_i("[%s] Setting load on Timer(%d) to zero", _name.c_str(), 2 * timerIndex);
  timerWrite(_fullCycleTimer, 0);

  log_i("[%s] Configuring Timer(%d) for generating DCC Signal (Half Wave)", _name.c_str(), 2 * timerIndex + 1);
  _pulseTimer = timerBegin(2*timerIndex + 1, DCC_TIMER_PRESCALE, true);
  log_i("[%s] Attaching interrupt handler to Timer(%d)", _name.c_str(), 2 * timerIndex + 1);
  timerAttachInterrupt(_pulseTimer, &signalGeneratorDirectionTimer<timerIndex>, true);
  log_i("[%s] Configuring alarm on Timer(%d) to %dus", _name.c_str(), 2 * timerIndex + 1, DCC_ONE_BIT_TOTAL_DURATION / 2);
  timerAlarmWrite(_pulseTimer, DCC_ONE_BIT_PULSE_DURATION, false);
  log_i("[%s] Setting load on Timer(%d) to zero", _name.c_str(), 2 * timerIndex + 1);
  timerWrite(_pulseTimer, 0);

  log_i("[%s] Enabling alarm on Timer(%d)", _name.c_str(), 2 * timerIndex);
  timerAlarmEnable(_fullCycleTimer);
  log_i("[%s] Enabling alarm on Timer(%d)", _name.c_str(), 2 * timerIndex + 1);
  timerAlarmEnable(_pulseTimer);
}

template<int timerIndex>
void SignalGenerator::stopSignal() {
  log_i("[%s] Shutting down Timer(%d) (Full Wave)", _name.c_str(), 2 * timerIndex);
  timerStop(_fullCycleTimer);
  timerAlarmDisable(_fullCycleTimer);
  timerDetachInterrupt(_fullCycleTimer);
  timerEnd(_fullCycleTimer);

  log_i("[%s] Shutting down Timer(%d) (Half Wave)", _name.c_str(), 2 * timerIndex + 1);
  timerStop(_pulseTimer);
  timerAlarmDisable(_pulseTimer);
  timerDetachInterrupt(_pulseTimer);
  timerEnd(_pulseTimer);

  // give enough time for any timer ISR calls to complete before draining
  // the packet queue and returning
  delay(250);

  // if we have a current packet being processed move it to the available
  // queue if it is not the pre-canned idle packet.
  if(_currentPacket != NULL && _currentPacket != &_idlePacket) {
    _availablePackets.push(_currentPacket);
    _currentPacket = NULL;
  }

  // drain any remaining packets that were not sent back into the available
  // to use packets.
  while(!_toSend.empty()) {
    _currentPacket = _toSend.front();
    _toSend.pop();
    // make sure the packet is zeroed before pushing it back to the queue
    memset(_currentPacket, 0, sizeof(Packet));
    _availablePackets.push(_currentPacket);
  }
}

void SignalGenerator::waitForQueueEmpty() {
  while(!_toSend.empty()) {
    log_i("[%s] Waiting for %d packets to send...", _name.c_str(), _toSend.size());
    delay(10);
  }
}

bool SignalGenerator::isQueueEmpty() {
  return _toSend.empty();
}

uint64_t sampleADCChannel(adc1_channel_t channel, uint8_t sampleCount) {
  uint64_t current = 0;
  int successfulReads = 0;
  for(uint8_t sampleReadCount = 0; sampleReadCount < sampleCount; sampleReadCount++) {
    int reading = adc1_get_raw(channel);
    if(reading > 0) {
      current += reading;
      successfulReads++;
    }
    delay(2);
  }
  if(successfulReads) {
    current /= successfulReads;
  }
  return current;
}

// number of analogRead samples to take when monitoring current after a CV verify (bit or byte) has been sent
const uint8_t CVSampleCount = 250;

int16_t readCV(const uint16_t cv) {
  const adc1_channel_t adcChannel = MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getADC1Channel();
  const uint16_t milliAmpAck = (4096 * 60 / MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getMaxMilliAmps());
  uint8_t readCVBitPacket[4] = { (uint8_t)(0x78 + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), 0x00, 0x00};
  uint8_t verifyCVBitPacket[4] = { (uint8_t)(0x74 + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), 0x00, 0x00};
  int16_t cvValue = 0;
  log_d("[PROG] Attempting to read CV %d, samples: %d, ack value: %d", cv, CVSampleCount, milliAmpAck);
  auto& signalGenerator = dccSignal[DCC_SIGNAL_PROGRAMMING];

  for(uint8_t bit = 0; bit < 8; bit++) {
    log_d("[PROG] CV %d, bit [%d/7]", cv, bit);
    readCVBitPacket[2] = 0xE8 + bit;
    loadBytePacket(signalGenerator, resetPacket, 2, 3);
    loadBytePacket(signalGenerator, readCVBitPacket, 3, 5);
    signalGenerator.waitForQueueEmpty();
    if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
      log_d("[PROG] CV %d, bit [%d/7] ON", cv, bit);
      bitWrite(cvValue, bit, 1);
    } else {
      log_d("[PROG] CV %d, bit [%d/7] OFF", cv, bit);
    }
  }

  // verify the byte we received
  verifyCVBitPacket[2] = cvValue & 0xFF;
  log_d("[PROG] CV %d, read value %d, verifying", cv, cvValue);
  loadBytePacket(signalGenerator, resetPacket, 2, 3);
  loadBytePacket(signalGenerator, verifyCVBitPacket, 3, 5);
  signalGenerator.waitForQueueEmpty();
  bool verified = false;
  if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
    verified = true;
    log_d("[PROG] CV %d, verified", cv);
  }
  if(!verified) {
    log_w("[PROG] CV %d, could not be verified", cv);
    cvValue = -1;
  }
  return cvValue;
}

bool writeProgCVByte(const uint16_t cv, const uint8_t cvValue) {
  const adc1_channel_t adcChannel = MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getADC1Channel();
  const uint16_t milliAmpAck = (4096 * 60 / MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getMaxMilliAmps());
  const uint8_t maxWriteAttempts = 5;
  uint8_t writeCVBytePacket[4] = { (uint8_t)(0x7C + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), cvValue, 0x00};
  uint8_t verifyCVBytePacket[4] = { (uint8_t)(0x74 + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), cvValue, 0x00};
  bool writeVerified = false;
  auto& signalGenerator = dccSignal[DCC_SIGNAL_PROGRAMMING];

  for(uint8_t attempt = 1; attempt <= maxWriteAttempts && !writeVerified; attempt++) {
    log_d("[PROG %d/%d] Attempting to write CV %d as %d", attempt, maxWriteAttempts, cv, cvValue);
    loadBytePacket(signalGenerator, resetPacket, 2, 1);
    loadBytePacket(signalGenerator, writeCVBytePacket, 3, 4);
    signalGenerator.waitForQueueEmpty();
    // verify that the decoder received the write byte packet and sent an ACK
    if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
      loadBytePacket(signalGenerator, resetPacket, 2, 3);
      loadBytePacket(signalGenerator, verifyCVBytePacket, 3, 5);
      signalGenerator.waitForQueueEmpty();
      // check that decoder sends an ACK for the verify operation
      if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
        writeVerified = true;
        log_d("[PROG] CV %d write value %d verified.", cv, cvValue);
      }
    } else {
      log_w("[PROG] CV %d write value %d could not be verified.", cv, cvValue);
    }
    log_i("[PROG] Sending decoder reset packet");
    loadBytePacket(signalGenerator, resetPacket, 2, 3);
  }
  return writeVerified;
}

bool writeProgCVBit(const uint16_t cv, const uint8_t bit, const bool value) {
  const adc1_channel_t adcChannel = MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getADC1Channel();
  const uint16_t milliAmpAck = (4096 * 60 / MotorBoardManager::getBoardByName(MOTORBOARD_NAME_PROG)->getMaxMilliAmps());
  const uint8_t maxWriteAttempts = 5;
  uint8_t writeCVBitPacket[4] = { (uint8_t)(0x78 + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), (uint8_t)(0xF0 + bit + value * 8), 0x00};
  uint8_t verifyCVBitPacket[4] = { (uint8_t)(0x74 + (highByte(cv - 1) & 0x03)), lowByte(cv - 1), (uint8_t)(0xB0 + bit + value * 8), 0x00};
  bool writeVerified = false;
  auto& signalGenerator = dccSignal[DCC_SIGNAL_PROGRAMMING];

  for(uint8_t attempt = 1; attempt <= maxWriteAttempts && !writeVerified; attempt++) {
    log_d("[PROG %d/%d] Attempting to write CV %d bit %d as %d", attempt, maxWriteAttempts, cv, bit, value);
    loadBytePacket(signalGenerator, resetPacket, 2, 1);
    loadBytePacket(signalGenerator, writeCVBitPacket, 3, 4);
    signalGenerator.waitForQueueEmpty();
    // verify that the decoder received the write byte packet and sent an ACK
    if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
      loadBytePacket(signalGenerator, resetPacket, 2, 3);
      loadBytePacket(signalGenerator, verifyCVBitPacket, 3, 5);
      signalGenerator.waitForQueueEmpty();
      // check that decoder sends an ACK for the verify operation
      if(sampleADCChannel(adcChannel, CVSampleCount) > milliAmpAck) {
        writeVerified = true;
        log_d("[PROG %d/%d] CV %d write bit %d verified.", attempt, maxWriteAttempts, cv, bit);
      }
    } else {
      log_w("[PROG %d/%d] CV %d write bit %d could not be verified.", attempt, maxWriteAttempts, cv, bit);
    }
    log_i("[PROG] Sending decoder reset packet");
    loadBytePacket(signalGenerator, resetPacket, 2, 3);
  }
  return writeVerified;
}

void writeOpsCVByte(const uint16_t locoNumber, const uint16_t cv, const uint8_t cvValue) {
  auto& signalGenerator = dccSignal[DCC_SIGNAL_OPERATIONS];
  log_d("[OPS] Updating CV %d to %d for loco %d", cv, cvValue, locoNumber);
  if(locoNumber > 127) {
    uint8_t writeCVBytePacket[] = {
      (uint8_t)(0xC0 | highByte(locoNumber)),
      lowByte(locoNumber),
      (uint8_t)(0xEC + (highByte(cv - 1) & 0x03)),
      lowByte(cv - 1),
      cvValue,
      0x00};
    loadBytePacket(signalGenerator, writeCVBytePacket, 5, 4);
  } else {
    uint8_t writeCVBytePacket[] = {
      lowByte(locoNumber),
      (uint8_t)(0xEC + (highByte(cv - 1) & 0x03)),
      lowByte(cv - 1),
      cvValue,
      0x00};
    loadBytePacket(signalGenerator, writeCVBytePacket, 4, 4);
  }
}

void writeOpsCVBit(const uint16_t locoNumber, const uint16_t cv, const uint8_t bit, const bool value) {
  auto& signalGenerator = dccSignal[DCC_SIGNAL_OPERATIONS];
  log_d("[OPS] Updating CV %d bit %d to %d for loco %d", cv, bit, value, locoNumber);
  if(locoNumber > 127) {
    uint8_t writeCVBitPacket[] = {
      (uint8_t)(0xC0 | highByte(locoNumber)),
      lowByte(locoNumber),
      (uint8_t)(0xE8 + (highByte(cv - 1) & 0x03)),
      lowByte(cv - 1),
      (uint8_t)(0xF0 + bit + value * 8),
      0x00};
    loadBytePacket(signalGenerator, writeCVBitPacket, 5, 4);
  } else {
    uint8_t writeCVBitPacket[] = {
      lowByte(locoNumber),
      (uint8_t)(0xE8 + (highByte(cv - 1) & 0x03)),
      lowByte(cv - 1),
      (uint8_t)(0xF0 + bit + value * 8),
      0x00};
    loadBytePacket(signalGenerator, writeCVBitPacket, 4, 4);
  }
}
