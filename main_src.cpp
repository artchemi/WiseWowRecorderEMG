#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

int main() {
    // Для кириллицы
    // SetConsoleCP(65001);          // входные данные (cin)
    // SetConsoleOutputCP(65001);    // вывод (cout) 

    std::vector<std::string> availablePorts;    // Контейнер с доступными портами

    std::cout << "Scanning COM ports..." << std::endl;

    // Перебор COM-портов 
    for (int i = 1; i <= 256; i++) {
        std::string portName = "\\\\.\\COM" + std::to_string(i);    // ???

        // Дескриптор порта
        HANDLE hComm = CreateFileA(portName.c_str(),    // Имя устройства
                                   GENERIC_READ,        // Доступ: чтение  
                                   0,                   // Режим совместного доступа 
                                   nullptr,             // Безопасность ???
                                   OPEN_EXISTING,       // Открыть только если существует
                                   0,                   // Флаги и атрибуты ???
                                   nullptr              // Шаблонный файл ??? 
                                );      

        // Если дескриптор не получилось создать                           
        if (hComm != INVALID_HANDLE_VALUE) {
            std::cout << "Found: COM" << i << std::endl;
            availablePorts.push_back("COM" + std::to_string(i));
            CloseHandle(hComm);
        }
    }

    // Вывод доступных портов
    std::cout << "Available ports: " << std::endl;
    for (std::string port : availablePorts) std::cout << port << std::endl;

    // Подключение к первому найденному порту
    std::string selectedPort = availablePorts[0];
    HANDLE hComm = CreateFileA(selectedPort.c_str(),
                               GENERIC_READ | GENERIC_WRITE,    // Нужно открывать на запись, чтобы отправить команду старта
                               0,
                               nullptr,
                               OPEN_EXISTING,
                               0,
                               nullptr
                            );

    if (hComm == INVALID_HANDLE_VALUE) {
        std::cerr << "Cannot open port!" << std::endl;
        return 21;
    } else {
        std::cout << "Connected" << std::endl;
    }

    // Настройка порта
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hComm, &dcb)) {
        std::cerr << "Error getting port state" << std::endl;
        CloseHandle(hComm);
        return 1;
    }

    dcb.BaudRate = CBR_256000; // стандартная поддерживаемая скорость
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(hComm, &dcb)) {
        std::cerr << "Error setting port state" << std::endl;
        CloseHandle(hComm);
        return 2;
    }

    // Таймауты для чтения
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hComm, &timeouts);

    std::cout << "Port opened successfully: " << selectedPort << std::endl;

    uint8_t startCmd[] = {170, 4, 128, 18, 1, 0, 187};                           // Команда для старта. Посмотреть что она означает
    DWORD bytesWritten;
    WriteFile(hComm, startCmd, sizeof(startCmd), &bytesWritten, nullptr);        // Отправка команды для запуска

    if (bytesWritten == sizeof(startCmd)) {
        std::cout << "Start command was delivered successfully" << std::endl;
    } else {
        std::cerr << "Start command wasn`t delivered" << std::endl;
        return 3;                                                                // Команда старта не получена полностью (ошибки в настройке порта) 
    }

    std::cout << "Start command sent. Reading EMG..." << std::endl;

    std::ofstream file("example.csv");
    file << "COM\n";
    // Буфер для приёма данных
    std::vector<uint8_t> rxBuff;
    char buf[256];
    DWORD bytesRead;

    int timeThrs = 100;
    int timeCounter = 0;

    while (timeCounter <= timeThrs) {
        if (ReadFile(hComm, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            // Добавляем данные в rxBuff
            rxBuff.insert(rxBuff.end(), buf, buf + bytesRead);

            // Парсинг фреймов, аналог MATLAB
            size_t idx = 0;
            while (rxBuff.size() - idx >= 7) {                                                         // Минимальная длина фрейма
                if (rxBuff[idx] == 0xA5) {                                                             // Старт-байт
                    uint8_t frameType = rxBuff[idx+2];                                                 // Определение типа фрейма (i=2): EMG фрейм - 18, ...
                    if (frameType == 18) {
                        uint8_t len = rxBuff[idx+1];                                                   // Длина информативной части фрейма (i=1)
                        size_t frameLen = len + 3;                                                     // Полная длина фрейма (информативная часть + старт + стоп) 

                        if (rxBuff.size() - idx >= frameLen && rxBuff[idx + frameLen -1] == 0x5A) {    //  Если в буфере есть необходимое число байт
                            std::vector<int16_t> emgVals;
                            size_t dataStart = idx + 12;                                               // С 12-го байта начинаются полезные данные?
                            size_t dataNum = (len - 2 - 4 - 4)/2; // как в MATLAB
                            for (size_t i = 0; i < dataNum; i++) {
                                // int16_t val = rxBuff[dataStart + 2*i] | (rxBuff[dataStart + 2*i + 1] << 8); 
                                int16_t val = (rxBuff[dataStart + 1 + 2*i] << 8) | rxBuff[dataStart + 2*i];       // little endian
                                emgVals.push_back(val);
                            }

                            for (size_t i = 0; i < emgVals.size(); i++) {
                                file << emgVals[i];
                                if (i + 1 < emgVals.size()) file << "\n"; // разделитель
                            }
                            // file << "\n";

                            // Вывод первых 5 значений
                            // std::cout << sizeof(emgVals) << std::endl;    // 12 отсчетов
                            // for (int16_t emg : emgVals) std::cout << emg << std::endl;

                            // std::cout << emgVals << std::endl;
                            // std::cout << "EMG: ";
                            // for (size_t i = 0; i < std::min(size_t(5), emgVals.size()); i++)
                            //     std::cout << emgVals[i] << " ";
                            // std::cout << std::endl;

                            idx += frameLen;
                            continue;
                        }
                    }
                }
                idx++;
            }

            // Убираем обработанные байты
            if (idx > 0) rxBuff.erase(rxBuff.begin(), rxBuff.begin() + idx);
        }
        Sleep(10); // чтобы не перегружать CPU
        std::cout << timeCounter << std::endl;
        timeCounter++;
    }

    CloseHandle(hComm);
    return 0;
}
