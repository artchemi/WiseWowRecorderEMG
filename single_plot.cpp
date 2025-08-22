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

#include "Iir.h"    // Фильтры

#include "SensorEMG.h"

// ==== параметры ==== 
const int SAMPLE_RATE = 500;       // Гц
const int MAX_PLOT_POINTS = 500;   // количество точек на графике

std::vector<float> emg_buffer;             // Глобальный буффер для сырых данных
std::vector<float> emg_filtered_buffer;    // Глобальный буффер для фильтрованных данных
std::mutex buffer_mutex;                   // Буффер для синхронизации?
std::atomic<bool> running(true);

double measuredSampleRate;                 // Эмпирическая частота дискретизации

float HIGHPASS_CUTOFF = 30;                // Частота обрезки для High-Pass фильтра, изменяется слайдером

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
 * @brief Фильтрует 
 */
std::vector<float> filterEMG(const std::vector<float> inputEMG) {
    Iir::Butterworth::HighPass<4> hp;    // FIXME: вынести за рамки функции
    hp.setup(SAMPLE_RATE, HIGHPASS_CUTOFF);

    std::vector<float> output;
    output.reserve(inputEMG.size());

    for (float sample : inputEMG) output.push_back(hp.filter(sample));

    return output;
}

// --- Поток для чтения данных --- 
void emg_thread(SensorEMG* sensor) {
    Iir::Butterworth::HighPass<4> hp;
    float lastCutOff = HIGHPASS_CUTOFF;    // Последняя частота обрезки (для обновления фильтра при изменении слайдера)
    hp.setup(SAMPLE_RATE, lastCutOff);     // Установка параметров фильтра

    while (running) {
        if (lastCutOff != HIGHPASS_CUTOFF) {
            hp.setup(SAMPLE_RATE, lastCutOff);    // Переустановка параметров фильтра
            lastCutOff = HIGHPASS_CUTOFF;
        }

        std::vector<float> newData = sensor->pollData();

        if (!newData.empty()) {
            std::lock_guard<std::mutex> lock(buffer_mutex);

            for (float v : newData) {
                // сохраняем сырой
                emg_buffer.push_back(v);
                if (emg_buffer.size() > MAX_PLOT_POINTS)
                    emg_buffer.erase(emg_buffer.begin());

                // фильтруем и сохраняем отфильтрованный
                double yf = hp.filter(static_cast<double>(v));
                emg_filtered_buffer.push_back(static_cast<float>(yf));
                if (emg_filtered_buffer.size() > MAX_PLOT_POINTS)
                    emg_filtered_buffer.erase(emg_filtered_buffer.begin());
            }
        } else {
            // чтобы не заполнять CPU если нет данных
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

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
        // GLFWwindow* window = glfwCreateWindow(1280, 720, "EMG Realtime Plot", nullptr, nullptr);
        const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());    // Полноэкранный режим
        GLFWwindow* window = glfwCreateWindow(
            mode->width, mode->height,   // ширина и высота экрана
            "EMG Realtime Plot", 
            glfwGetPrimaryMonitor(),     // <- полноэкранный режим
            nullptr
        );
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

            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            // ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);            // позиция окна 
            ImGui::SetNextWindowSize(io.DisplaySize);        // размер окна   ImVec2(2000, 1000), ImGuiCond_Always
            ImGuiWindowFlags main_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

            // График 1 (raw)
            ImGui::Begin("EMG Signal");
            ImVec2 plot_size(2000, 500); 
            if (ImPlot::BeginPlot("Realtime EMG", plot_size)) {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, MAX_PLOT_POINTS); 
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!emg_buffer.empty()) {
                    // x-координаты
                    std::vector<float> x(emg_buffer.size());
                    for (size_t i = 0; i < x.size(); i++)
                        x[i] = static_cast<float>(i);

                    // Здесь сейчас рисуется сырой сигнал
                    ImPlot::PlotLine("Raw EMG", x.data(), emg_buffer.data(), emg_buffer.size());
                    // std::cout << emg_filtered_buffer.size() << std::endl;
                    // std::cout << sensor.getSampleRate() << std::endl;
                }
                ImPlot::EndPlot();
            }

            // График 2 (filtered)
            if (ImPlot::BeginPlot("Filtered EMG", plot_size)) {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, MAX_PLOT_POINTS);    // Ограничение диапазонов осей
                ImPlot::SetupAxisLimits(ImAxis_Y1, -400, 400); 
                std::lock_guard<std::mutex> lock(buffer_mutex);
                if (!emg_filtered_buffer.empty()) {
                    std::vector<float> x(emg_filtered_buffer.size());
                    for (size_t i = 0; i < x.size(); i++) x[i] = static_cast<float>(i);

                    ImPlot::PlotLine("Filtered", x.data(), emg_filtered_buffer.data(), (int)emg_filtered_buffer.size());
                }
                ImPlot::EndPlot();
            } 

            ImGui::SliderFloat("float", &HIGHPASS_CUTOFF, 0.1f, SAMPLE_RATE/2);    // Слайдер для регуляции нижней частоты обрезки

            ImGui::End();

            // --- Текстовое окно для вывода частоты дискретизации и общего количества собранных сэмплов --- 
            ImGui::SetNextWindowPos(ImVec2(1020, 10), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(240, 80), ImGuiCond_Always);
            ImGuiWindowFlags small_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::Begin("Stats", nullptr, small_flags);

            ImGui::Text("Sample rate: %.1f Hz", measuredSampleRate);
            ImGui::Text("Samples: %zu", emg_buffer.size());

            ImGui::End(); // конец маленького окна

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
