#include "steplens_bench_exe.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Writer.hxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string pathUtf8(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::filesystem::path tempRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("steplens-bench-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

bool writeBox(const std::filesystem::path& path) {
    STEPControl_Writer writer;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
    if (writer.Transfer(box, STEPControl_AsIs) != IFSelect_RetDone) {
        return false;
    }
    return writer.Write(pathUtf8(path).c_str()) == IFSelect_RetDone;
}

void setChildEnvironment() {
#if defined(_WIN32)
    _putenv_s("ASAN_OPTIONS", "halt_on_error=1:detect_leaks=0");
    _putenv_s("LIBGL_ALWAYS_SOFTWARE", "1");
#else
    setenv("ASAN_OPTIONS", "halt_on_error=1:detect_leaks=0", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
#endif
}

int runProcess(const std::vector<std::string>& args, const std::filesystem::path& log) {
#if defined(_WIN32)
    std::wstring command;
    for (const std::string& arg : args) {
        if (!command.empty()) {
            command.push_back(L' ');
        }
        const std::wstring wide = std::filesystem::path(arg).wstring();
        command.push_back(L'"');
        for (wchar_t ch : wide) {
            if (ch == L'"') {
                command += L"\\\"";
            } else {
                command.push_back(ch);
            }
        }
        command.push_back(L'"');
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (output != INVALID_HANDLE_VALUE) {
            CloseHandle(output);
        }
        if (input != INVALID_HANDLE_VALUE) {
            CloseHandle(input);
        }
        return 127;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(output);
    CloseHandle(input);
    if (!started) {
        return 127;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 127;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<int>(code);
#else
    std::vector<std::string> storage = args;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                ::close(fd);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
        return 127;
    }
    return WEXITSTATUS(status);
#endif
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<double> numbersFor(std::string_view json, std::string_view key) {
    const std::string pattern = std::string("\"") + std::string(key) + "\":";
    std::vector<double> values;
    std::size_t cursor = 0;
    while (cursor < json.size()) {
        const std::size_t found = json.find(pattern, cursor);
        if (found == std::string_view::npos) {
            break;
        }
        const char* begin = json.data() + found + pattern.size();
        char* end = nullptr;
        const double value = std::strtod(begin, &end);
        values.push_back(value);
        cursor = static_cast<std::size_t>(end - json.data());
        if (end == begin) {
            cursor = found + pattern.size();
        }
    }
    return values;
}

double medianOf(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

TEST(BenchCli, MedianJsonOnConcatenatedBoxes) {
    const std::filesystem::path root = tempRoot();
    const std::filesystem::path left = root / "left.step";
    const std::filesystem::path right = root / "right.step";
    const std::filesystem::path joined = root / "joined.step";
    const std::filesystem::path jsonPath = root / "out.json";
    const std::filesystem::path concatLog = root / "concat.log";
    const std::filesystem::path benchLog = root / "bench.log";
    ASSERT_TRUE(writeBox(left));
    ASSERT_TRUE(writeBox(right));

    setChildEnvironment();
    const std::string python =
#if defined(_WIN32)
        "python";
#else
        "python3";
#endif
    const std::filesystem::path script =
        std::filesystem::path(kSteplensSourceDir) / "scripts" / "make_big_step.py";
    const int concatCode = runProcess(
        {python, pathUtf8(script), "--inputs", pathUtf8(left), pathUtf8(right), "--output", pathUtf8(joined)},
        concatLog);
    ASSERT_EQ(concatCode, 0) << readFile(concatLog);
    ASSERT_TRUE(std::filesystem::is_regular_file(joined));

    const int benchCode = runProcess({std::string(kSteplensBenchExe), "--file", pathUtf8(joined), "--stages",
                                      "all", "--repeat", "3", "--json", pathUtf8(jsonPath)},
                                     benchLog);
    const std::string log = readFile(benchLog);
    ASSERT_EQ(benchCode, 0) << log;
    EXPECT_NE(log.find("reader_ok=1"), std::string::npos) << log;
    EXPECT_NE(log.find("fine_triangle_count=0"), std::string::npos) << log;

    const std::string json = readFile(jsonPath);
    EXPECT_NE(json.find("\"ok\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("\"aggregation\":\"median\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"repeat\":3"), std::string::npos) << json;
    EXPECT_NE(json.find("\"meshing_ran\":true"), std::string::npos) << json;
    EXPECT_NE(json.find("T07"), std::string::npos) << json;
    EXPECT_NE(json.find("fine_triangle_count"), std::string::npos) << json;
#if defined(_WIN32)
    EXPECT_NE(json.find("PeakWorkingSetSize"), std::string::npos) << json;
    EXPECT_NE(json.find("PrivateUsage"), std::string::npos) << json;
#else
    EXPECT_NE(json.find("\"name\":\"VmHWM\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"name\":\"private_bytes\""), std::string::npos) << json;
    EXPECT_NE(json.find("Private_Clean+Private_Dirty"), std::string::npos) << json;
    EXPECT_NE(json.find("PeakWorkingSetSize"), std::string::npos) << json;
#endif

    const std::vector<double> fine = numbersFor(json, "fine_triangle_count");
    ASSERT_FALSE(fine.empty()) << json;
    for (double value : fine) {
        EXPECT_DOUBLE_EQ(value, 0.0);
    }

    const std::vector<double> prototypes = numbersFor(json, "prototype_count");
    ASSERT_GE(prototypes.size(), 4u) << json;
    EXPECT_GE(prototypes.front(), 2.0);

    const std::vector<double> faces = numbersFor(json, "face_count");
    ASSERT_FALSE(faces.empty()) << json;
    EXPECT_GE(faces.front(), 12.0);

    const std::vector<double> coarse = numbersFor(json, "coarse_triangle_count");
    ASSERT_GE(coarse.size(), 5u) << json;
    EXPECT_GT(coarse.front(), 0.0);
    std::vector<double> coarseRuns(coarse.begin() + 2, coarse.end());
    EXPECT_NEAR(coarse.front(), medianOf(coarseRuns), 1e-6);

    const std::vector<double> reading = numbersFor(json, "reading_seconds");
    ASSERT_EQ(reading.size(), 4u) << json;
    std::vector<double> readingRuns(reading.begin() + 1, reading.end());
    EXPECT_NEAR(reading.front(), medianOf(readingRuns), 1e-6);

    const std::vector<double> colors = numbersFor(json, "color_group_count");
    ASSERT_GE(colors.size(), 4u) << json;
    EXPECT_GE(colors.front(), 0.0);

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
}
