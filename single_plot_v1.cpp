#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h> // подключать после Windows.h

// ==== параметры ==== 
const int SAMPLE_RATE = 500;       // Гц
const int MAX_PLOT_POINTS = 500;   // количество точек на графике

// ==== глобальные переменные для буфера и синхронизации ====
std::vector<float> emg_buffer;
std::mutex buffer_mutex;
std::atomic<bool> running(true);

std::vector<float> emg_filtered_buffer;    // Буффер для фильтрованных данных

// ==== функции для COM-портов ====
std::vector<std::string> ScanPorts(int minPort = 1, int maxPort = 256) {
    std::vector<std::string> ports;
    for (int i = minPort; i <= maxPort; ++i) {
        std::string portName = "\\\\.\\COM" + std::to_string(i);
        HANDLE h = CreateFileA(portName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ports.push_back(portName);
            CloseHandle(h);
        }
    }
    return ports;
}

/**
 * @brief Усредняет значения для
 */
float moving_average(const std::vector<float>& data, size_t window = 5) {
    if (data.empty()) return 0.0f;
    float sum = 0.0f;
    size_t count = 0;
    for (size_t i = data.size() >= window ? data.size()-window : 0; i < data.size(); i++) {
        sum += data[i];
        count++;
    }
    return sum / count;
}


// ==== класс для работы с EMG-сенсором ====
class SensorEMG {
private:
    std::string comPort;
    HANDLE hComm;
    DWORD bytesWritten;

public:
    SensorEMG(const std::string& port) : comPort(port), hComm(INVALID_HANDLE_VALUE) {}

    void connect() {
        hComm = CreateFileA(comPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hComm == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open port " + comPort);
        std::cout << "Port opened: " << comPort << std::endl;
    }

    void sendSTART() {
        uint8_t cmd[] = {0xAA, 0x04, 0x80, 0x12, 0x01, 0x00, 0x00, 0xBB};
        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(cmd); ++i) xorVal ^= cmd[i];
        cmd[sizeof(cmd)-2] = xorVal;
        WriteFile(hComm, cmd, sizeof(cmd), &bytesWritten, nullptr);
        PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }

    // Чтение данных и возвращение всех новых сэмплов
    std::vector<float> pollData() {
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
};

// ==== поток для чтения данных ====
void emg_thread(SensorEMG* sensor) {
    while (running) {
        std::vector<float> newData = sensor->pollData();
        if (!newData.empty()) {
            std::lock_guard<std::mutex> lock(buffer_mutex);
            for (float v : newData) {
                emg_buffer.push_back(v);
                if (emg_buffer.size() > MAX_PLOT_POINTS)
                    emg_buffer.erase(emg_buffer.begin());
            }
        }
    }
}

// void emg_filtered_thread(SensorEMG* sensor) {
//     while (running) {
//         std::vector<float> newDataFiltered = sensor->pollData();
//         if (!newDataFiltered.empty()) {
//             std::lock_guard<std::mutex> lock(buffer_mutex);
//             for (float v : newData) {
//                 emg_buffer.push_back(v);
//                 if (emg_buffer.size() > MAX_PLOT_POINTS)
//                     emg_buffer.erase(emg_buffer.begin());
//             }
//         }
//     }
// }


// ==== main ====
int main() {
    try {
        auto ports = ScanPorts();
        if (ports.empty()) {
            std::cerr << "No COM ports found!" << std::endl;
            return -1;
        }

        SensorEMG sensor(ports[0]);
        sensor.connect();
        sensor.sendSTART();

        std::thread reader(emg_thread, &sensor);

        // ==== init GLFW + OpenGL + ImGui ====
        if (!glfwInit()) return 1;
        GLFWwindow* window = glfwCreateWindow(1280, 720, "EMG Realtime Plot", nullptr, nullptr);
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 130");

        // ==== Main loop ====
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("EMG Signal");
            ImVec2 plot_size(2000, 1000);
            if (ImPlot::BeginPlot("Realtime EMG", plot_size)) {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!emg_buffer.empty()) {
                    // x-координаты
                    std::vector<float> x(emg_buffer.size());
                    for (size_t i = 0; i < x.size(); i++)
                        x[i] = static_cast<float>(i);

                    // Здесь сейчас рисуется сырый сигнал
                    ImPlot::PlotLine("Raw EMG", x.data(), emg_buffer.data(), emg_buffer.size());

                    // Здесь рисуется твой фильтрованный сигнал
                    // ... твой старый high-pass код ...
                }
                ImPlot::EndPlot();
            }

            // --- второй график (Filtered) ---
            if (ImPlot::BeginPlot("Filtered EMG", plot_size)) {
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!emg_filtered_buffer.empty()) {
                    std::vector<float> x(emg_filtered_buffer.size());
                    for (size_t i = 0; i < x.size(); i++)
                        x[i] = static_cast<float>(i);

                    ImPlot::PlotLine("Filtered", x.data(), emg_filtered_buffer.data(), emg_filtered_buffer.size());
                }
                ImPlot::EndPlot();
            }
            ImGui::End();

            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        // cleanup
        running = false;
        reader.join();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
