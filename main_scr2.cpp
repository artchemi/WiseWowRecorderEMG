#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring> // memcpy
#include <chrono>

int main() {
    // -------------------------
    //  SCAN COM PORTS
    // -------------------------
    std::vector<std::string> availablePorts;
    std::cout << "Scanning COM ports..." << std::endl;
    for (int i = 1; i <= 256; i++) {
        std::string portName = "\\\\.\\COM" + std::to_string(i);
        HANDLE hComm = CreateFileA(portName.c_str(), GENERIC_READ,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hComm != INVALID_HANDLE_VALUE) {
            std::cout << "Found: COM" << i << std::endl;
            availablePorts.push_back(portName);
            CloseHandle(hComm);
        }
    }

    if (availablePorts.empty()) {
        std::cerr << "No ports found!" << std::endl;
        return 1;
    }

    std::string selectedPort = availablePorts[0];
    HANDLE hComm = CreateFileA(selectedPort.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hComm == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot open port!" << std::endl;
        return 2;
    }
    std::cout << "Connected: " << selectedPort << std::endl;

    // -------------------------
    //  CONFIGURE PORT
    // -------------------------
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hComm, &dcb)) {
        std::cerr << "Error getting port state" << std::endl;
        CloseHandle(hComm);
        return 4;
    }
    dcb.BaudRate = 256000;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(hComm, &dcb)) {
        std::cerr << "Error setting port state" << std::endl;
        CloseHandle(hComm);
        return 5;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hComm, &timeouts);

    DWORD bytesWritten;

    // -------------------------
    //  SEND STOP (OPTIONAL)
    // -------------------------
    uint8_t stopCmd[] = {0xAA, 0x04, 0x80, 0x11, 0x00, 0x00, 0x00, 0xBB};
    {
        uint8_t xor_tmp = 0;
        for (size_t i = 1; i < sizeof(stopCmd); ++i) xor_tmp ^= stopCmd[i];
        stopCmd[sizeof(stopCmd)-2] = xor_tmp;
        WriteFile(hComm, stopCmd, sizeof(stopCmd), &bytesWritten, nullptr);
        Sleep(50);
    }

    uint8_t setRateCmd[] = {0xAA, 0x06, 0x80, 0x10, 0x02, 0x00, 0x00, 0x00, 0xBB};    // fs: 250 - 0x00; 500 - 0x01; 1000 - 0x03 ... 
    {
        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(setRateCmd); ++i) xorVal ^= setRateCmd[i];
        setRateCmd[sizeof(setRateCmd)-2] = xorVal;
    }
    WriteFile(hComm, setRateCmd, sizeof(setRateCmd), &bytesWritten, nullptr);
    Sleep(100);
    std::cout << "Set sample rate_reg = 0x3 (1000 Hz)" << std::endl;

    // -------------------------
    //  START EMG
    // -------------------------
    uint8_t startEmgCmd[] = {0xAA, 0x04, 0x80, 0x12, 0x01, 0x00, 0x00, 0xBB};
    {
        uint8_t xorVal2 = 0;
        for (size_t i = 1; i < sizeof(startEmgCmd); ++i) xorVal2 ^= startEmgCmd[i];
        startEmgCmd[sizeof(startEmgCmd)-2] = xorVal2;
    }
    WriteFile(hComm, startEmgCmd, sizeof(startEmgCmd), &bytesWritten, nullptr);
    Sleep(50);

    // -------------------------
    //  CLEAR BUFFER
    // -------------------------
    PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // -------------------------
    //  OPEN CSV
    // -------------------------
    std::ofstream file("example.csv");
    file << "EMG\n";

    // -------------------------
    //  READ & PARSE EMG FRAMES
    // -------------------------
    std::vector<uint8_t> rxBuff;
    char buf[512];
    DWORD bytesRead;

    uint64_t total_samples = 0;
    uint64_t frame_count = 0;

    auto captureStart = std::chrono::steady_clock::now();
    auto captureDuration = std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() - captureStart < captureDuration) {
        if (ReadFile(hComm, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            rxBuff.insert(rxBuff.end(), buf, buf + bytesRead);

            size_t idx = 0;
            while (rxBuff.size() - idx >= 7) {
                if (rxBuff[idx] == 0xA5) { 
                    uint8_t len  = rxBuff[idx+1];
                    uint8_t addr = rxBuff[idx+2];
                    uint8_t check = rxBuff[idx+3];
                    if ((uint8_t)(len ^ addr) == check) {
                        size_t frameLen = (size_t)len + 3;
                        if (rxBuff.size() - idx >= frameLen &&
                            rxBuff[idx + frameLen - 1] == 0x5A) {

                            if (addr == 0x12) { // EMG frame
                                const size_t METADATA_BYTES = 4;
                                size_t dataStart = idx + 4;
                                size_t firstFloatPos = dataStart + METADATA_BYTES;
                                size_t diffsStart = firstFloatPos + 4;

                                int payloadBytes = (int)len - 2;
                                if (payloadBytes < (int)METADATA_BYTES + 4) { idx += frameLen; continue; }
                                size_t dataNum = (size_t)((payloadBytes - METADATA_BYTES - 4) / 2);
                                size_t frameLastIndex = idx + frameLen - 1;
                                if (!(firstFloatPos + 4 <= frameLastIndex && diffsStart + dataNum*2 <= frameLastIndex)) {
                                    idx += frameLen; 
                                    continue;
                                }

                                float emg_v0 = 0.0f;
                                std::memcpy(&emg_v0, &rxBuff[firstFloatPos], sizeof(float));

                                std::vector<float> emg_vals;
                                emg_vals.reserve(1 + dataNum);
                                emg_vals.push_back(emg_v0);

                                const float factor = 3.1457f;
                                for (size_t k = 0; k < dataNum; ++k) {
                                    size_t b0 = diffsStart + 2*k;
                                    size_t b1 = diffsStart + 2*k + 1;
                                    int16_t rawDiff = (int16_t)((rxBuff[b1] << 8) | rxBuff[b0]);
                                    emg_v0 += static_cast<float>(rawDiff) / factor;
                                    emg_vals.push_back(emg_v0);
                                }

                                if (file.is_open()) {
                                    for (float v : emg_vals) file << v << "\n";
                                }

                                frame_count++;
                                total_samples += emg_vals.size();
                                std::cout << "Frame #" << frame_count 
                                          << " samples=" << emg_vals.size() << std::endl;
                            } // addr == 0x12

                            idx += frameLen;
                            continue;
                        }
                    }
                }
                idx++;
            }
            if (idx > 0) rxBuff.erase(rxBuff.begin(), rxBuff.begin() + idx);
        } else {
            Sleep(1);
        }
    }

    // -------------------------
    //  SUMMARY
    // -------------------------
    double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - captureStart).count();
    double samples_per_sec = (elapsed > 0.0) ? (double)total_samples / elapsed : 0.0;
    double avg_samples_per_frame = (frame_count > 0) ? (double)total_samples / frame_count : 0.0;

    std::cout << "=== SUMMARY ===" << std::endl;
    std::cout << "Duration (s): " << elapsed << std::endl;
    std::cout << "Total frames: " << frame_count << std::endl;
    std::cout << "Total samples: " << total_samples << std::endl;
    std::cout << "Avg samples/frame: " << avg_samples_per_frame << std::endl;
    std::cout << "Measured sample rate (sps): " << samples_per_sec << std::endl;

    file.close();
    CloseHandle(hComm);
    std::cout << "Finished." << std::endl;
    return 0;
}
