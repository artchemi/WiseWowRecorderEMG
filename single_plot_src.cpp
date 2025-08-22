#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring> // memcpy
#include <chrono>

// ==== ImGui + ImPlot + GLFW + OpenGL ====
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>    // подключать после Windows.h
#include <GL/gl.h>

// ==== параметры ====
const int   SAMPLE_RATE     = 500;      // Гц
const int   BAUD_RATE       = 256000;
const int   BYTES_SIZE      = 8;
const auto  PARITY_TYPE     = NOPARITY; // Четность
const int   MAX_PLOT_POINTS = 500;      // окно отображения (последние N отсчётов)

// ---------------- Кольцевой буфер (минимум задержки) ----------------
static float   g_ring[MAX_PLOT_POINTS];   // данные (Y)
static int     g_head = 0;                // индекс самого старого
static int     g_count = 0;               // текущее число отсчётов в буфере (<= MAX_PLOT_POINTS)
static uint64_t g_total_samples = 0;      // всего принятых отсчётов (для времени по X)

inline void PushSample(float v) {
    // позиция для записи "самого нового"
    int pos = (g_head + g_count) % MAX_PLOT_POINTS;
    if (g_count < MAX_PLOT_POINTS) {
        g_ring[pos] = v;
        g_count++;
    } else {
        // кольцевой сдвиг окна: перезаписываем самый старый
        g_ring[pos] = v;
        g_head = (g_head + 1) % MAX_PLOT_POINTS;
    }
    g_total_samples++;
}

// ----------------------------------------------------------
std::vector<std::string> ScanPorts(int minPort = 1, int maxPort = 256) {
    std::vector<std::string> availablePorts;
    for (int comIndex = minPort; comIndex <= maxPort; ++comIndex) {
        std::string portName = "\\\\.\\COM" + std::to_string(comIndex);
        HANDLE hComm = CreateFileA(portName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hComm != INVALID_HANDLE_VALUE) {
            availablePorts.push_back(portName);
            CloseHandle(hComm);
        }
    }
    return availablePorts;
}

// ==========================================================
class SensorEMG {
private:
    std::string addressCOM;
    int sampleRate;
    HANDLE hComm;
    DWORD bytesWritten;

public:
    SensorEMG(std::string addressCOM_, int sampleRate_)
        : addressCOM(std::move(addressCOM_)), sampleRate(sampleRate_), hComm(INVALID_HANDLE_VALUE) {}

    void connect() {
        hComm = CreateFileA(addressCOM.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hComm == INVALID_HANDLE_VALUE) {
            std::cerr << "Cannot open port: " << addressCOM << std::endl;
        } else {
            // Настройка таймаутов чтения, чтобы не блокироваться надолго
            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout         = 1;
            timeouts.ReadTotalTimeoutConstant    = 1;
            timeouts.ReadTotalTimeoutMultiplier  = 0;
            SetCommTimeouts(hComm, &timeouts);
            std::cout << "Port was opened: " << addressCOM << std::endl;
        }
    }

    void sendSET() {
        if (hComm == INVALID_HANDLE_VALUE) return;
        uint8_t sampleRateByte = 0x01; // 500 Гц
        uint8_t setRateCmd[] = {0xAA, 0x06, 0x80, 0x10, sampleRateByte, 0x00, 0x00, 0x00, 0xBB};
        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(setRateCmd); ++i) xorVal ^= setRateCmd[i];
        setRateCmd[sizeof(setRateCmd)-2] = xorVal;
        WriteFile(hComm, setRateCmd, sizeof(setRateCmd), &bytesWritten, nullptr);
        Sleep(50);
    }

    void sendSTART() {
        if (hComm == INVALID_HANDLE_VALUE) return;
        uint8_t startEmgCmd[] = {0xAA, 0x04, 0x80, 0x12, 0x01, 0x00, 0x00, 0xBB};
        uint8_t xorVal = 0;
        for (size_t i = 1; i < sizeof(startEmgCmd); ++i) xorVal ^= startEmgCmd[i];
        startEmgCmd[sizeof(startEmgCmd)-2] = xorVal;
        WriteFile(hComm, startEmgCmd, sizeof(startEmgCmd), &bytesWritten, nullptr);
        Sleep(50);
        PurgeComm(hComm, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }

    void pollData() {
        if (hComm == INVALID_HANDLE_VALUE) return;

        char  buf[512];
        DWORD bytesRead = 0;
        static std::vector<uint8_t> rxBuff; // накопитель пакетов

        if (ReadFile(hComm, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            rxBuff.insert(rxBuff.end(), buf, buf + bytesRead);

            size_t idx = 0;
            while (rxBuff.size() - idx >= 7) {
                if (rxBuff[idx] == 0xA5) {
                    uint8_t len   = rxBuff[idx+1];
                    uint8_t addr  = rxBuff[idx+2];
                    uint8_t check = rxBuff[idx+3];
                    if ((uint8_t)(len ^ addr) == check) {
                        size_t frameLen = (size_t)len + 3;
                        if (rxBuff.size() - idx >= frameLen && rxBuff[idx + frameLen - 1] == 0x5A) {
                            if (addr == 0x12) { // EMG
                                const size_t METADATA_BYTES = 4;
                                size_t dataStart     = idx + 4;
                                size_t firstFloatPos = dataStart + METADATA_BYTES;
                                size_t diffsStart    = firstFloatPos + 4;

                                int payloadBytes = (int)len - 2;
                                if (payloadBytes >= (int)METADATA_BYTES + 4) {
                                    float emg_v0 = 0.0f;
                                    std::memcpy(&emg_v0, &rxBuff[firstFloatPos], sizeof(float));

                                    // первая точка
                                    PushSample(emg_v0);

                                    // далее — дельты
                                    size_t dataNum = (payloadBytes - METADATA_BYTES - 4) / 2;
                                    const float factor = 3.1457f;
                                    for (size_t k = 0; k < dataNum; ++k) {
                                        size_t b0 = diffsStart + 2*k;
                                        size_t b1 = diffsStart + 2*k + 1;
                                        int16_t rawDiff = (int16_t)((rxBuff[b1] << 8) | rxBuff[b0]);
                                        emg_v0 += static_cast<float>(rawDiff) / factor;
                                        PushSample(emg_v0);
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
            if (idx > 0) rxBuff.erase(rxBuff.begin(), rxBuff.begin() + (long)idx);
        }
    }
};

// ==========================================================
int main() {
    // --- COM ---
    std::vector<std::string> availablePorts = ScanPorts();
    if (availablePorts.empty()) {
        std::cerr << "No COM ports found\n";
        return -1;
    }

    SensorEMG sensor(availablePorts[0], SAMPLE_RATE);
    sensor.connect();
    sensor.sendSET();
    sensor.sendSTART();

    // ==== init GLFW + OpenGL + ImGui ====
    if (!glfwInit()) return 1;
    GLFWwindow* window = glfwCreateWindow(1280, 720, "EMG Realtime Plot", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);

    // Отключаем vsync для минимальной задержки
    glfwSwapInterval(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Временный буфер для отрисовки непрерывного участка
    std::vector<float> draw_buf;
    draw_buf.reserve(MAX_PLOT_POINTS);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // читаем данные с датчика (часто и мелкими порциями)
        sensor.pollData();

        // начало нового кадра
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("EMG Signal");

        if (ImPlot::BeginPlot("Realtime EMG")) {
            // Собираем непрерывный отрезок из кольцевого буфера (старый->новый)
            draw_buf.clear();
            draw_buf.resize(g_count);
            for (int i = 0; i < g_count; ++i) {
                int idx = (g_head + i) % MAX_PLOT_POINTS;
                draw_buf[i] = g_ring[idx];
            }

            // Время по X: шаг = 1/SAMPLE_RATE, старт — время самого старого
            const double xscale = 1.0 / double(SAMPLE_RATE);
            const double xstart = (g_total_samples >= (uint64_t)g_count)
                                  ? double(g_total_samples - (uint64_t)g_count) * xscale
                                  : 0.0;

            if (g_count > 1) {
                ImPlot::SetupAxes("Time [s]", "EMG");
                ImPlot::PlotLine("EMG", draw_buf.data(), g_count,
                                 xscale, xstart /*, ImPlotLineFlags_None*/);
                // Можно зафиксировать видимый диапазон по X на последнее окно:
                ImPlot::SetupAxisLimits(ImAxis_X1, xstart, xstart + g_count * xscale, ImGuiCond_Always);
            }
            ImPlot::EndPlot();
        }
        ImGui::End();

        // рендер
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
