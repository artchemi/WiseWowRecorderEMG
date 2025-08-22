#include "SensorEMG.h"

SensorEMG::SensorEMG(const std::string& port) 
    : comPort(port),
      hComm(INVALID_HANDLE_VALUE),
      total_samples(0),
      frame_count(0),
      measuredSampleRate(0.0),
      captureStart(std::chrono::steady_clock::now()) {}

void SensorEMG::connect() {
    hComm = CreateFileA(comPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hComm == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open port " + comPort);
    std::cout << "Port opened: " << comPort << std::endl;
}

void SensorEMG::sendSTART() {
    uint8_t cmd[] = {0xAA, 0x04, 0x80, 0x12, 0x01, 0x00, 0x00, 0xBB};
    uint8_t xorVal = 0;
    for (size_t i = 1; i < sizeof(cmd); ++i) xorVal ^= cmd[i];
    cmd[sizeof(cmd)-2] = xorVal;
    WriteFile(hComm, cmd, sizeof(cmd), &bytesWritten, nullptr);
    PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

std::vector<float> SensorEMG::pollData() {
    char buf[512];
    DWORD bytesRead;
    static std::vector<uint8_t> rxBuff;
    std::vector<float> emg_vals;   

    if (ReadFile(hComm, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        rxBuff.insert(rxBuff.end(), buf, buf + bytesRead);

        size_t idx = 0;
        while (rxBuff.size() - idx >= 7) {
            if (rxBuff[idx] == 0xA5) {
                uint8_t len = rxBuff[idx+1];
                uint8_t addr = rxBuff[idx+2];
                uint8_t check = rxBuff[idx+3];
                if ((uint8_t)(len ^ addr) == check) {
                    size_t frameLen = len + 3;
                    if (rxBuff.size() - idx >= frameLen && rxBuff[idx + frameLen - 1] == 0x5A) {
                        if (addr == 0x12) { // EMG frame
                            const size_t METADATA = 4;
                            size_t firstFloatPos = idx + 4 + METADATA;
                            size_t diffsStart = firstFloatPos + 4;
                            int payloadBytes = len - 2;

                            if (payloadBytes >= (int)METADATA + 4) {
                                float val = 0.0f;
                                std::memcpy(&val, &rxBuff[firstFloatPos], sizeof(float));
                                emg_vals.push_back(val);

                                size_t dataNum = (payloadBytes - METADATA - 4) / 2;
                                const float factor = 3.1457f;
                                for (size_t k = 0; k < dataNum; ++k) {
                                    size_t b0 = diffsStart + 2*k;
                                    size_t b1 = diffsStart + 2*k + 1;
                                    int16_t rawDiff = (int16_t)((rxBuff[b1] << 8) | rxBuff[b0]);
                                    val += static_cast<float>(rawDiff) / factor;
                                    emg_vals.push_back(val);
                                }

                                // обновляем статистику
                                frame_count++;
                                total_samples += emg_vals.size();

                                // считаем частоту дискретизации
                                auto now = std::chrono::steady_clock::now();
                                double elapsed = std::chrono::duration_cast<
                                    std::chrono::duration<double>>(now - captureStart).count();
                                measuredSampleRate = (elapsed > 0.0) 
                                    ? static_cast<double>(total_samples) / elapsed 
                                    : 0.0;
                            }
                        }
                        idx += frameLen;
                        continue;
                    }
                }
            }
            idx++;
        }
        if (idx > 0) rxBuff.erase(rxBuff.begin(), rxBuff.begin() + idx);
    }

    return emg_vals;
}

double SensorEMG::getSampleRate() const {
    return measuredSampleRate;
}

uint64_t SensorEMG::getFrameCount() const {
    return frame_count;
}

uint64_t SensorEMG::getTotalSamples() const {
    return total_samples;
}
