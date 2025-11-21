#include "mc_MeshCodec.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

struct AppState {
    char inputPath[1024] = {0};
    char outputPath[1024] = {0};
    std::atomic<bool> isProcessing{false};
    std::string statusText = "Ready";
    int successCount = 0;
    int failCount = 0;
    std::mutex statusMutex;
    float progress = 0.0f;
    int totalFiles = 0;
    int processedFiles = 0;
};

AppState g_appState;

bool ReadFile(const std::string& path, std::vector<mc::u8>& data) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        return false;
    
    data.resize(file.tellg());
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), data.size());
    file.close();
    return true;
}

void WriteFile(const std::string& path, const std::span<const mc::u8>& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    file.close();
}

bool Decompress(const std::filesystem::path& compressedPath, void* workMem, const std::filesystem::path& outputPath) {
    std::vector<mc::u8> data;
    if (!ReadFile(compressedPath.string(), data))
        return false;

    auto header = reinterpret_cast<const mc::ResMeshCodecPackageHeader*>(data.data());
    size_t decompressedSize = header->GetDecompressedSize();
    std::vector<mc::u8> outputBuffer(decompressedSize);

    if (mc::DecompressMC(outputBuffer.data(), decompressedSize, data.data(), data.size(), workMem, 0x10000000)) {
        std::filesystem::create_directories(outputPath);
        WriteFile((outputPath / compressedPath.stem()).string(), outputBuffer);
        return true;
    }
    return false;
}

bool DecompressCave(const std::filesystem::path& compressedPath, void* workMem, const std::filesystem::path& outputPath) {
    std::vector<mc::u8> data;
    if (!ReadFile(compressedPath.string(), data))
        return false;

    auto header = reinterpret_cast<const mc::ResChunkHeader*>(data.data());
    size_t decompressedSize = header->decompressedSize;
    std::vector<mc::u8> outputBuffer(decompressedSize);

    if (mc::DecompressChunk(outputBuffer.data(), decompressedSize, data.data(), data.size(), workMem, 0x10000000)) {
        std::filesystem::create_directories(outputPath);
        WriteFile((outputPath / compressedPath.stem()).string(), outputBuffer);
        return true;
    }
    return false;
}

#ifdef _WIN32
std::string BrowseForFolder(const std::string& title) {
    std::string result;
    IFileOpenDialog* pFileOpen = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        hr = pFileOpen->GetOptions(&dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        
        if (SUCCEEDED(hr)) {
            if (!title.empty()) {
                pFileOpen->SetTitle(std::wstring(title.begin(), title.end()).c_str());
            }
            
            hr = pFileOpen->Show(nullptr);
            
            if (SUCCEEDED(hr)) {
                IShellItem* pItem;
                hr = pFileOpen->GetResult(&pItem);
                if (SUCCEEDED(hr)) {
                    PWSTR pszFilePath;
                    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    if (SUCCEEDED(hr)) {
                        int size_needed = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, nullptr, 0, nullptr, nullptr);
                        result.resize(size_needed - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, &result[0], size_needed, nullptr, nullptr);
                        CoTaskMemFree(pszFilePath);
                    }
                    pItem->Release();
                }
            }
        }
        pFileOpen->Release();
    }
    return result;
}
#else
std::string BrowseForFolder(const std::string& title) {
    return "";
}
#endif

void ProcessFiles() {
    std::string inputPathStr(g_appState.inputPath);
    std::string outputPathStr(g_appState.outputPath);
    
    if (inputPathStr.empty() || outputPathStr.empty()) {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = "Please select both input and output folders";
        return;
    }

    g_appState.isProcessing = true;
    {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = "Scanning files...";
        g_appState.progress = 0.0f;
        g_appState.successCount = 0;
        g_appState.failCount = 0;
        g_appState.processedFiles = 0;
        g_appState.totalFiles = 0;
    }

    try {
        std::filesystem::path dirPath(inputPathStr);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
            if (entry.path().extension() == ".mc" || entry.path().extension() == ".chunk") {
                g_appState.totalFiles++;
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = std::string("Error scanning: ") + e.what();
        g_appState.isProcessing = false;
        return;
    }

    if (g_appState.totalFiles == 0) {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = "No .mc or .chunk files found";
        g_appState.isProcessing = false;
        return;
    }

    void* workMem = malloc(0x10000000);

    try {
        std::filesystem::path dirPath(inputPathStr);
        std::filesystem::path outputBasePath(outputPathStr);

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
            if (entry.path().extension() == ".mc") {
                auto relativePath = std::filesystem::relative(entry.path().parent_path(), dirPath);
                if (Decompress(entry.path(), workMem, outputBasePath / relativePath)) {
                    g_appState.successCount++;
                } else {
                    g_appState.failCount++;
                }
                g_appState.processedFiles++;
            } else if (entry.path().extension() == ".chunk") {
                auto relativePath = std::filesystem::relative(entry.path().parent_path(), dirPath);
                if (DecompressCave(entry.path(), workMem, outputBasePath / relativePath)) {
                    g_appState.successCount++;
                } else {
                    g_appState.failCount++;
                }
                g_appState.processedFiles++;
            }

            if (entry.path().extension() == ".mc" || entry.path().extension() == ".chunk") {
                std::lock_guard<std::mutex> lock(g_appState.statusMutex);
                g_appState.progress = static_cast<float>(g_appState.processedFiles) / static_cast<float>(g_appState.totalFiles);
                g_appState.statusText = "Processing: " + std::to_string(g_appState.processedFiles) + " / " + std::to_string(g_appState.totalFiles);
            }
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = std::string("Error: ") + e.what();
    }

    free(workMem);

    {
        std::lock_guard<std::mutex> lock(g_appState.statusMutex);
        g_appState.statusText = "Complete! Success: " + std::to_string(g_appState.successCount) + ", Failed: " + std::to_string(g_appState.failCount);
        g_appState.progress = 1.0f;
    }
    g_appState.isProcessing = false;
}

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.19f, 0.19f, 0.19f, 0.92f);
    colors[ImGuiCol_Border] = ImVec4(0.19f, 0.19f, 0.19f, 0.29f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.24f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.19f, 0.54f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.23f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.54f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.34f, 0.34f, 0.34f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.54f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.54f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.33f, 0.67f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.34f, 0.67f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.73f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.45f, 0.78f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.55f, 0.88f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.40f, 0.68f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.45f, 0.78f, 0.31f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.55f, 0.88f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.40f, 0.68f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.29f, 0.30f, 0.31f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.45f, 0.78f, 0.80f);
    colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.45f, 0.78f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.00f, 0.00f, 0.00f, 0.52f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 0.29f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    style.WindowPadding = ImVec2(15, 15);
    style.FramePadding = ImVec2(12, 10);
    style.CellPadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.TouchExtraPadding = ImVec2(0, 0);
    style.IndentSpacing = 21;
    style.ScrollbarSize = 14;
    style.GrabMinSize = 10;
    style.WindowBorderSize = 1;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.WindowRounding = 7;
    style.ChildRounding = 4;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.ScrollbarRounding = 9;
    style.GrabRounding = 3;
    style.LogSliderDeadzone = 4;
    style.TabRounding = 4;
}

int main() {
#ifdef _WIN32
    HWND hwnd = GetConsoleWindow();
    if (hwnd != nullptr) {
        ShowWindow(hwnd, SW_HIDE);
    }
#endif

    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(520, 320, "MeshCodec Decompressor", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float dpiScale = (xscale + yscale) / 2.0f;
    
    if (dpiScale <= 0.0f || dpiScale < 1.0f) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (monitor) {
            glfwGetMonitorContentScale(monitor, &xscale, &yscale);
            dpiScale = (xscale + yscale) / 2.0f;
        }
    }
    
    if (dpiScale < 1.0f) dpiScale = 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.FontGlobalScale = dpiScale;

    float baseFontSize = 13.0f;
    float fontSize = baseFontSize * dpiScale;
    ImFont* font = nullptr;
    
#ifdef _WIN32
    font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", fontSize);
    if (font == nullptr) {
        font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/arial.ttf", fontSize);
    }
#endif
    
    if (font == nullptr) {
        font = io.Fonts->AddFontDefault();
    }

    SetupImGuiStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    
    static int baseWidth = 520;
    static int baseHeight = 325;
    static int baseHeightWithProgress = 380;
    int scaledWidth = static_cast<int>(baseWidth * dpiScale);
    int scaledHeight = static_cast<int>(baseHeight * dpiScale);
    int scaledHeightWithProgress = static_cast<int>(baseHeightWithProgress * dpiScale);
    glfwSetWindowSize(window, scaledWidth, scaledHeight);
    glfwSetWindowSizeLimits(window, scaledWidth, scaledHeight, scaledWidth, scaledHeightWithProgress);

#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        bool showProgress = g_appState.isProcessing || g_appState.progress > 0.0f;
        int currentWidth, currentHeight;
        glfwGetWindowSize(window, &currentWidth, &currentHeight);
        
        if (showProgress) {
            int targetHeight = static_cast<int>(baseHeightWithProgress * dpiScale);
            if (currentHeight < targetHeight) {
                glfwSetWindowSize(window, currentWidth, targetHeight);
            }
        } else {
            int targetHeight = static_cast<int>(baseHeight * dpiScale);
            if (currentHeight > targetHeight) {
                glfwSetWindowSize(window, currentWidth, targetHeight);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int windowWidth, windowHeight;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth), static_cast<float>(windowHeight)));
        ImGui::Begin("MeshCodec Decompressor", nullptr, 
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        const char* browseText = "Browse...";
        float browseTextWidth = ImGui::CalcTextSize(browseText).x;
        float buttonPadding = ImGui::GetStyle().FramePadding.x * 2;
        float buttonWidth = browseTextWidth + buttonPadding + 15.0f;
        float buttonHeight = ImGui::GetFrameHeight() * 1.05f;
        float spacing = 15.0f;
        float windowPadding = ImGui::GetStyle().WindowPadding.x;
        float availableWidth = ImGui::GetWindowWidth() - windowPadding * 2;
        float inputWidth = availableWidth - buttonWidth - spacing;
        
        if (buttonWidth < 100.0f) buttonWidth = 100.0f;
        
        ImGui::Text("Input Folder");
        float textHeight = ImGui::GetTextLineHeight();
        float desiredPaddingY = (buttonHeight - textHeight) / 2.0f;
        if (desiredPaddingY < 8.0f) desiredPaddingY = 8.0f;
        
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(15, desiredPaddingY));
        ImGui::SetNextItemWidth(inputWidth);
        ImGui::InputText("##input", g_appState.inputPath, sizeof(g_appState.inputPath));
        ImGui::PopStyleVar();
        
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Browse...", ImVec2(buttonWidth, buttonHeight)) && !g_appState.isProcessing) {
            std::string folder = BrowseForFolder("Select Input Folder");
            if (!folder.empty()) {
                size_t len = folder.length();
                if (len >= sizeof(g_appState.inputPath)) len = sizeof(g_appState.inputPath) - 1;
                std::memcpy(g_appState.inputPath, folder.c_str(), len);
                g_appState.inputPath[len] = '\0';
            }
        }
        ImGui::Spacing();

        ImGui::Text("Output Folder");
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(15, desiredPaddingY));
        ImGui::SetNextItemWidth(inputWidth);
        ImGui::InputText("##output", g_appState.outputPath, sizeof(g_appState.outputPath));
        ImGui::PopStyleVar();
        
        ImGui::SameLine(0, spacing);
        if (ImGui::Button("Browse...##output", ImVec2(buttonWidth, buttonHeight)) && !g_appState.isProcessing) {
            std::string folder = BrowseForFolder("Select Output Folder");
            if (!folder.empty()) {
                size_t len = folder.length();
                if (len >= sizeof(g_appState.outputPath)) len = sizeof(g_appState.outputPath) - 1;
                std::memcpy(g_appState.outputPath, folder.c_str(), len);
                g_appState.outputPath[len] = '\0';
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        const char* decompressText = "Decompress";
        float decompressTextWidth = ImGui::CalcTextSize(decompressText).x;
        float decompressButtonPadding = ImGui::GetStyle().FramePadding.x * 2;
        float decompressButtonWidth = decompressTextWidth + decompressButtonPadding + 30.0f;
        float decompressButtonHeight = ImGui::GetFrameHeight() * 1.3f;
        
        if (decompressButtonWidth < 180.0f) decompressButtonWidth = 180.0f;
        if (decompressButtonHeight < 40.0f) decompressButtonHeight = 40.0f;
        
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - decompressButtonWidth) * 0.5f);
        bool canDecompress = !g_appState.isProcessing && 
                            strlen(g_appState.inputPath) > 0 && 
                            strlen(g_appState.outputPath) > 0;
        
        if (!canDecompress) {
            ImGui::BeginDisabled();
        }
        
        if (ImGui::Button("Decompress", ImVec2(decompressButtonWidth, decompressButtonHeight))) {
            std::thread(ProcessFiles).detach();
        }
        
        if (!canDecompress) {
            ImGui::EndDisabled();
        }
        
        ImGui::Spacing();
        ImGui::Spacing();

        static float lastDpiScale = dpiScale;
        if (lastDpiScale != dpiScale) {
            lastDpiScale = dpiScale;
            baseWidth = 520;
            baseHeight = 325;
            baseHeightWithProgress = 380;
        }
        
        if (showProgress) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.33f, 0.67f, 1.00f, 1.00f));
            ImGui::ProgressBar(g_appState.progress, ImVec2(-1, 0));
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();
        {
            std::lock_guard<std::mutex> lock(g_appState.statusMutex);
            ImGui::TextWrapped("%s", g_appState.statusText.c_str());
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

#ifdef _WIN32
    CoUninitialize();
#endif

    return 0;
}

