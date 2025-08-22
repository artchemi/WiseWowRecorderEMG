#pragma once
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <stdexcept>

class SensorEMG {
private:
    std::string comPort;
    HANDLE hComm;
    DWORD bytesWritten;

    uint64_t total_samples;    // Всего считанных сэмплов
    uint64_t frame_count;      // Количество фреймов
    std::chrono::steady_clock::time_point captureStart; // Время старта

    double measuredSampleRate; // Текущая оценка частоты дискретизации

public:
    explicit SensorEMG(const std::string& port);

    void connect();
    void sendSTART();

    // Чтение данных и возвращение новых сэмплов
    std::vector<float> pollData();

    // Метрики
    double getSampleRate() const;
    uint64_t getFrameCount() const;
    uint64_t getTotalSamples() const;
};
