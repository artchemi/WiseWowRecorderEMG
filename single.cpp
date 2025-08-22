#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring> // memcpy
#include <chrono>

#include "SensorEMG.h"

const int SAMPLE_RATE = 500;          // Частота дискретизации, Гц
const int BAUD_RATE = 256000;
const int BYTES_SIZE = 8;
const auto PARITY_TYPE = NOPARITY;    // Четность

/**
 * @brief Сканирует COM порты
 * @return Массив доступных COM портов
 */
std::vector<std::string> ScanPorts(int minPort = 1, int maxPort = 256) {
    std::vector<std::string> availablePorts;
    std::cout << "Scanning COM ports..." << std::endl;

    for (int comIndex = minPort; comIndex <= maxPort; ++comIndex) {
        std::string portName = "\\\\.\\COM" + std::to_string(comIndex);                                       // На винде почему то нужно писать  "\\\\.\\COM" для портов > 9
        HANDLE hComm = CreateFileA(portName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);    // Проверка только на чтение
        if (hComm != INVALID_HANDLE_VALUE) {
            availablePorts.push_back(portName);
            CloseHandle(hComm);
        }
    }

    if (availablePorts.empty()) {
        std::cerr << "No ports found!" << std::endl;
    } 
    return availablePorts;
}

class SensorEMG {
private:
    std::string addressCOM;
    int sampleRate;
    HANDLE hComm; 

    DWORD bytesWritten;

public:
    SensorEMG(std::string addressCOM_, int sampleRate_) : addressCOM(addressCOM_), sampleRate(sampleRate_), hComm(INVALID_HANDLE_VALUE){}

    // --- Подключение ---

    /**
     * @brief Подключает порт
     */
    void connect() {
        hComm = CreateFileA(                 // Дескриптор порта
            addressCOM.c_str(),              // Имя порта
            GENERIC_READ | GENERIC_WRITE,    // Доступ: чтение и запись 
            0,                               // Режим совместного доступа 
            nullptr, 
            OPEN_EXISTING,                   // Открыть только если существует
            0, 
            nullptr
        );

        if (hComm == INVALID_HANDLE_VALUE) {
            std::cerr << "Cannot open port!" << std::endl;
        } else {
            std::cout << "Port was opened" << std::endl;
        }
    }

    void configureDCB() {    // Device Control Block, структура Windows API
        DCB dcb = {0};
        dcb.DCBlength = sizeof(DCB);
        
        // Заполнение структуры настройками порта
        if (!GetCommState(hComm, &dcb)) {
            std::cerr << "Error getting port state" << std::endl;
            CloseHandle(hComm);
        }

        // Новые параметры порта
        dcb.BaudRate = 256000;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;

        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;           // Максимальная пауза между двумя байтами
        timeouts.ReadTotalTimeoutConstant = 50;      // Фиксированная задержка для чтения 
        timeouts.ReadTotalTimeoutMultiplier = 10;    // Умножается на количество ожидаемых байтов 
        SetCommTimeouts(hComm, &timeouts);
    }

    // --- Команды управления ---
    void sendSET() {    // FIXME
        uint8_t sampleRateByte;    // Байт для частоты дискретизации 
        switch (sampleRate)        // fs: 250 - 0x00; 500 - 0x01; 1000 - 0x03; 1500 - 0x04 ...
        {
            case 500: sampleRateByte = 0x01;
            case 1000: sampleRateByte = 0x03;
            case 1500: sampleRateByte = 0x04;
        }
        uint8_t setRateCmd[] = {0xAA, 0x06, 0x80, 0x10, sampleRateByte, 0x00, 0x00, 0x00, 0xBB};  
        
        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(setRateCmd); ++i) xorVal ^= setRateCmd[i];
        setRateCmd[sizeof(setRateCmd)-2] = xorVal;

        WriteFile(hComm, setRateCmd, sizeof(setRateCmd), &bytesWritten, nullptr);
        Sleep(100);
    }

    /**
     * @brief Запуск записи.
     * @param duration Прожолжительность записи.
     * @param verbose Уровень логирования. 0 - без логирования, 1 - таймер, 2 - таймер, средняя частота дискретизации...
     */
    void sendSTART(int duration = 10, int verbose = 0) {
        uint8_t startEmgCmd[] = {0xAA, 0x04, 0x80, 0x12, 0x01, 0x00, 0x00, 0xBB};

        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(startEmgCmd); ++i) xorVal ^= startEmgCmd[i];
        startEmgCmd[sizeof(startEmgCmd)-2] = xorVal;

        WriteFile(hComm, startEmgCmd, sizeof(startEmgCmd), &bytesWritten, nullptr);
        Sleep(100);

        PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);    // Очистка буффера

        std::ofstream file("example.csv");
        file << "value\n";    // Добавить первую колонку - время 

        std::vector<uint8_t> rxBuff;    // Буффер для накопления всех байт
        char buf[512];                  // Временный буффер для вызова ReadFile(...)
        DWORD bytesRead;                // Фактическое количество считанных байт

        uint64_t total_samples = 0;     // Счетчик сэмлов ЭМГ
        uint64_t frame_count = 0;       // Счетчик фреймов

        // Фиксация времени
        auto captureStart = std::chrono::steady_clock::now();
        auto captureDuration = std::chrono::seconds(duration);

        while (std::chrono::steady_clock::now() - captureStart < captureDuration) {
            // Считывание 512 байтов в buf
            if (ReadFile(hComm, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                rxBuff.insert(rxBuff.end(), buf, buf + bytesRead);    // Если есть данные, то добавляются в общий буффер / частичная передача фрейма

                // Перебор накопленного буфера
                size_t idx = 0;
                while (rxBuff.size() - idx >= 7) {        // 7 - минимальный размер фрейма
                    if (rxBuff[idx] == 0xA5) {            // Проверка на начало фрейма
                        uint8_t len  = rxBuff[idx+1];     // Длина полезной части фрейма
                        uint8_t addr = rxBuff[idx+2];     // Тип фрейма 
                        uint8_t check = rxBuff[idx+3];    // Контрольный XOR для проверки целостности

                        if ((uint8_t)(len ^ addr) == check) {
                            size_t frameLen = (size_t)len + 3;    // Общая длина фрейма

                            // Проверка конца фрейма
                            if (rxBuff.size() - idx >= frameLen && rxBuff[idx + frameLen - 1] == 0x5A) {
                                if (addr == 0x12) {                                       // EMG фрейм
                                    const size_t METADATA_BYTES = 4;                      // Служебныек байты (заголовок, длина...)
                                    size_t dataStart = idx + 4;
                                    size_t firstFloatPos = dataStart + METADATA_BYTES;    // Позиция первого байта float (4 байта)
                                    size_t diffsStart = firstFloatPos + 4;                // Позиция разниц следующих сэмплов

                                    int payloadBytes = (int)len - 2;                      // Проверка достаточности байтов для первого float
                                    if (payloadBytes < (int)METADATA_BYTES + 4) {idx += frameLen; continue;}
                                    size_t dataNum = (size_t)((payloadBytes - METADATA_BYTES - 4) / 2);
                                    size_t frameLastIndex = idx + frameLen - 1;
                                    if (!(firstFloatPos + 4 <= frameLastIndex && diffsStart + dataNum*2 <= frameLastIndex)) {
                                        idx += frameLen; 
                                        continue;
                                    }
                                    
                                    // Чтение фрейма
                                    float emg_v0 = 0.0f;
                                    std::memcpy(&emg_v0, &rxBuff[firstFloatPos], sizeof(float));

                                    std::vector<float> emg_vals;
                                    emg_vals.reserve(1 + dataNum);
                                    emg_vals.push_back(emg_v0);

                                    const float factor = 3.1457f;    // Фактор ... 
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
                                    
                                    // Блок логирования
                                    if (verbose == 1) {    // Настроить логирование
                                        std::cout << "Frame #" << frame_count << " samples=" << emg_vals.size() << std::endl;
                                    } else if (verbose == 2) {
                                        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - captureStart).count();
                                        double samples_per_sec = (elapsed > 0.0) ? (double)total_samples / elapsed : 0.0;
                                        double avg_samples_per_frame = (frame_count > 0) ? (double)total_samples / frame_count : 0.0;

                                        std::cout << "Duration (s): " << elapsed << " | " << "Total frames: " << frame_count << 
                                        " | " << "Total samples: " << total_samples << " | " << "Avg samples/frame: " << 
                                        avg_samples_per_frame << " | " << "Measured sample rate (sps): " << samples_per_sec << "\r" << std::flush;
                                        // std::cout << "Total frames: " << frame_count << std::flush << "\r";
                                        // std::cout << "Total samples: " << total_samples << std::flush << "\r";
                                        // std::cout << "Avg samples/frame: " << avg_samples_per_frame << std::flush << "\r";
                                        // std::cout << "Measured sample rate (sps): " << samples_per_sec << std::flush << "\r";
                                    }

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

        file.close();
        CloseHandle(hComm);
        std::cout << "Finished." << std::endl;

    }

    void sendSTOP() {
        uint8_t stopCmd[] = {0xAA, 0x04, 0x80, 0x11, 0x00, 0x00, 0x00, 0xBB};
    
        uint8_t xor_tmp = 0;
        for (size_t i = 1; i < sizeof(stopCmd); ++i) xor_tmp ^= stopCmd[i];
        stopCmd[sizeof(stopCmd)-2] = xor_tmp;

        WriteFile(hComm, stopCmd, sizeof(stopCmd), &bytesWritten, nullptr);
        Sleep(50);
    }


};

int main() {
    std::vector<std::string> availablePorts = ScanPorts();    // Сканирование доступных портов

    SensorEMG sensor(availablePorts[0], SAMPLE_RATE);         // Инициализация переменной датчика
    sensor.connect();

    sensor.sendSTOP();                                        // Остановка
    // sensor.sendSET();                                         // Установка частоты дискретизации
    sensor.sendSTART(10, 2);                                       // Старт записи
}
