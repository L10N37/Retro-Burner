#include "process_runner.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumCapturedOutput =
    4U * 1024U * 1024U;

[[nodiscard]] std::string ErrnoText(
    const char* operation,
    const int error)
{
    return std::string(operation) + ": " +
        std::strerror(error);
}

void AppendCaptured(
    CapturedProcessResult& result,
    const char* data,
    const std::size_t size)
{
    if (result.output.size() >= kMaximumCapturedOutput) {
        result.outputTruncated = true;
        return;
    }

    const std::size_t available =
        kMaximumCapturedOutput - result.output.size();
    const std::size_t copy =
        size < available ? size : available;

    result.output.append(data, copy);

    if (copy != size)
        result.outputTruncated = true;
}

} // namespace

CapturedProcessResult RunProcessCapture(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory,
    const ProcessOutputCallback& onOutput)
{
    CapturedProcessResult result;

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        result.error = ErrnoText("pipe", errno);
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        result.error = ErrnoText("fork", saved);
        return result;
    }

    if (pid == 0) {
        ::close(pipeFds[0]);

        const int nullInput =
            ::open("/dev/null", O_RDONLY);

        if (nullInput >= 0) {
            ::dup2(nullInput, STDIN_FILENO);
            if (nullInput != STDIN_FILENO)
                ::close(nullInput);
        }

        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);

        if (pipeFds[1] != STDOUT_FILENO &&
            pipeFds[1] != STDERR_FILENO) {
            ::close(pipeFds[1]);
        }

        if (!workingDirectory.empty() &&
            ::chdir(workingDirectory.c_str()) != 0) {
            const std::string message =
                ErrnoText("chdir", errno) + "\n";
            ::write(STDERR_FILENO, message.data(), message.size());
            _exit(126);
        }

        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1U);
        storage.emplace_back(executable.string());
        storage.insert(
            storage.end(),
            arguments.begin(),
            arguments.end());

        std::vector<char*> argv;
        argv.reserve(storage.size() + 1U);

        for (std::string& item : storage)
            argv.push_back(item.data());

        argv.push_back(nullptr);

        ::execv(executable.c_str(), argv.data());

        const std::string message =
            ErrnoText("execv", errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    result.started = true;
    ::close(pipeFds[1]);

    std::array<char, 4096> chunk{};

    for (;;) {
        const ssize_t bytes =
            ::read(pipeFds[0], chunk.data(), chunk.size());

        if (bytes > 0) {
            const std::size_t count =
                static_cast<std::size_t>(bytes);

            if (onOutput) {
                onOutput(std::string_view(
                    chunk.data(),
                    count));
            }

            AppendCaptured(
                result,
                chunk.data(),
                count);
            continue;
        }

        if (bytes == 0)
            break;

        if (errno == EINTR)
            continue;

        result.error = ErrnoText("read", errno);
        break;
    }

    ::close(pipeFds[0]);

    int status = 0;
    for (;;) {
        if (::waitpid(pid, &status, 0) >= 0)
            break;
        if (errno == EINTR)
            continue;
        if (result.error.empty())
            result.error = ErrnoText("waitpid", errno);
        return result;
    }

    if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status);
    else
        result.exitCode = -1;

    return result;
}
