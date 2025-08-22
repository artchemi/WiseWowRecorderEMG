#include <SFML/Graphics.hpp>
#include "libs/imgui/imgui.h"
#include "libs/imgui-sfml/imgui-SFML.h"
#include "libs/implot/implot.h"
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>

int main() {
    // Создаём окно SFML
    sf::RenderWindow window(sf::VideoMode(800, 600), "EMG Real-Time Plot");
    window.setFramerateLimit(60);

    // Инициализация ImGui-SFML (void в SFML 2.x)
    ImGui::SFML::Init(window);

    // Буфер данных EMG
    const int bufferSize = 500;
    std::vector<float> emgData(bufferSize, 0.0f);

    sf::Clock deltaClock;
    float t = 0.0f;

    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            ImGui::SFML::ProcessEvent(event); // <-- только событие
            if (event.type == sf::Event::Closed)
                window.close();
        }

        // Обновление ImGui
        ImGui::SFML::Update(window, deltaClock.restart());

        // --- Симуляция EMG данных ---
        t += 0.01f;
        float newVal = 0.5f * sin(2.0f * 3.14159f * 10.0f * t) + 0.05f * ((rand() % 100)/100.0f - 0.5f);

        emgData.push_back(newVal);
        if (emgData.size() > bufferSize)
            emgData.erase(emgData.begin());

        // --- Рисуем график ---
        ImGui::Begin("EMG Signal");
        if (ImPlot::BeginPlot("EMG Real-Time")) {
            ImPlot::PlotLine("Channel 1", emgData.data(), emgData.size());
            ImPlot::EndPlot();
        }
        ImGui::End();

        // Рендеринг
        window.clear();
        ImGui::SFML::Render(window);
        window.display();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ImGui::SFML::Shutdown();
    return 0;
}
