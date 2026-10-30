#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <semaphore.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/syscall.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;
using Microseconds = std::chrono::microseconds;

constexpr int kStudentId = 28;
constexpr int kBaseOperations = 100;
constexpr std::uint32_t kBaseSeed = 1000;
constexpr int kDefaultPhilosophers = 5;
constexpr int kDefaultThinkMinMs = 1;
constexpr int kDefaultThinkMaxMs = 5;
constexpr int kDefaultEatMinMs = 5;
constexpr int kDefaultEatMaxMs = 15;
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kDefaultMaxDurationMs = 180000;

enum class Mode {
    Fifo,
    Scan
};

enum class PhilosopherState {
    Thinking,
    Waiting,
    Eating,
    Finished,
    Aborted
};

enum class RequestStatus {
    Waiting,
    Granted,
    Cancelled
};

struct Config {
    int philosophers = kDefaultPhilosophers;
    int cycles = kBaseOperations;
    int studentId = kStudentId;
    std::uint32_t seed = kBaseSeed + static_cast<std::uint32_t>(kStudentId) * 97u;
    Mode mode = Mode::Fifo;
    int thinkMinMs = kDefaultThinkMinMs;
    int thinkMaxMs = kDefaultThinkMaxMs;
    int eatMinMs = kDefaultEatMinMs;
    int eatMaxMs = kDefaultEatMaxMs;
    int timeoutMs = kDefaultTimeoutMs;
    int maxDurationMs = kDefaultMaxDurationMs;
    std::string logFile;
    std::string waitCsv;
    bool verbose = false;
    bool individual = false;
};

struct PhilosopherStats {
    int id = 0;
    long tid = 0;
    int completedCycles = 0;
    int grants = 0;
    int timeouts = 0;
    int overtakes = 0;
    long long totalWaitUs = 0;
    long long maxWaitUs = 0;
};

struct WaitRecord {
    int philosopherId = 0;
    double waitMs = 0.0;
    std::string result;
};

struct SharedState {
    Config config;
    std::mutex cs;
    std::deque<struct Request*> queue;
    std::vector<int> forkOwner;
    std::vector<PhilosopherState> states;
    std::vector<PhilosopherStats> stats;
    std::vector<WaitRecord> waits;
    int maxQueue = 0;
    int totalGrants = 0;
    int totalTimeouts = 0;
    int totalOvertakes = 0;
    long long invariantViolations = 0;
    std::atomic<bool> stop{false};
    std::atomic<bool> aborted{false};
    TimePoint started;
};

struct Request {
    int philosopherId = 0;
    int leftFork = 0;
    int rightFork = 0;
    RequestStatus status = RequestStatus::Waiting;
    TimePoint enqueuedAt;
    TimePoint grantedAt;
    sem_t event{};
    bool semaphoreReady = false;

    Request(int id, int left, int right)
        : philosopherId(id), leftFork(left), rightFork(right) {
        semaphoreReady = sem_init(&event, 0, 0) == 0;
    }

    ~Request() {
        if (semaphoreReady) {
            sem_destroy(&event);
        }
    }

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
};

std::string modeName(Mode mode) {
    return mode == Mode::Fifo ? "fifo" : "scan";
}

std::string stateName(PhilosopherState state) {
    switch (state) {
        case PhilosopherState::Thinking:
            return "THINKING";
        case PhilosopherState::Waiting:
            return "WAITING";
        case PhilosopherState::Eating:
            return "EATING";
        case PhilosopherState::Finished:
            return "FINISHED";
        case PhilosopherState::Aborted:
            return "ABORTED";
    }
    return "UNKNOWN";
}

std::string forkPair(int left, int right) {
    return std::to_string(left) + "," + std::to_string(right);
}

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Model parameters:\n"
        << "  --philosophers N       number of philosophers (default 5)\n"
        << "  --cycles C             cycles per philosopher (default 100)\n"
        << "  --student-id ID        student id (default 28)\n"
        << "  --seed S               random seed (default 1000 + ID * 97)\n"
        << "  --individual           use individual seed and cycle count\n"
        << "  --mode fifo|scan       queue service mode (default fifo)\n"
        << "  --think-min-ms N       minimum thinking time (default 1)\n"
        << "  --think-max-ms N       maximum thinking time (default 5)\n"
        << "  --eat-min-ms N         minimum eating time (default 5)\n"
        << "  --eat-max-ms N         maximum eating time (default 15)\n"
        << "  --timeout-ms N         request timeout (default 30000)\n"
        << "  --max-duration-ms N    experiment limit (default 180000)\n"
        << "\n"
        << "Output:\n"
        << "  --log-file PATH        event log file\n"
        << "  --wait-csv PATH        per-request wait CSV\n"
        << "  --verbose              print events to stdout\n"
        << "  --quiet                no verbose output\n"
        << "  --help                 show this help\n";
}

bool parseInt(const std::string& text, int& value) {
    try {
        std::size_t position = 0;
        long parsed = std::stol(text, &position);
        if (position != text.size() || parsed < INT_MIN || parsed > INT_MAX) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseSeed(const std::string& text, std::uint32_t& value) {
    try {
        std::size_t position = 0;
        unsigned long parsed = std::stoul(text, &position);
        if (position != text.size() || parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseArguments(int argc, char** argv, Config& config, std::string& error,
                    bool& helpRequested) {
    bool cyclesSet = false;
    bool seedSet = false;

    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        std::string key = argument;
        std::string value;
        bool inlineValue = false;

        const std::size_t equals = argument.find('=');
        if (equals != std::string::npos) {
            key = argument.substr(0, equals);
            value = argument.substr(equals + 1);
            inlineValue = true;
        }

        auto takeValue = [&](std::string& result) -> bool {
            if (inlineValue) {
                result = value;
                inlineValue = false;
                return true;
            }
            if (i + 1 >= argc) {
                return false;
            }
            result = argv[++i];
            return true;
        };

        if (key == "--help" || key == "-h") {
            helpRequested = true;
            continue;
        }

        if (key == "--verbose") {
            config.verbose = true;
            continue;
        }

        if (key == "--quiet") {
            config.verbose = false;
            continue;
        }

        if (key == "--individual") {
            config.individual = true;
            continue;
        }

        std::string raw;
        if (key == "--philosophers") {
            if (!takeValue(raw) || !parseInt(raw, config.philosophers)) {
                error = "invalid value for --philosophers";
                return false;
            }
        } else if (key == "--cycles") {
            if (!takeValue(raw) || !parseInt(raw, config.cycles)) {
                error = "invalid value for --cycles";
                return false;
            }
            cyclesSet = true;
        } else if (key == "--student-id") {
            if (!takeValue(raw) || !parseInt(raw, config.studentId)) {
                error = "invalid value for --student-id";
                return false;
            }
        } else if (key == "--seed") {
            if (!takeValue(raw) || !parseSeed(raw, config.seed)) {
                error = "invalid value for --seed";
                return false;
            }
            seedSet = true;
        } else if (key == "--mode") {
            if (!takeValue(raw)) {
                error = "missing value for --mode";
                return false;
            }
            if (raw == "fifo") {
                config.mode = Mode::Fifo;
            } else if (raw == "scan") {
                config.mode = Mode::Scan;
            } else {
                error = "invalid value for --mode, expected fifo or scan";
                return false;
            }
        } else if (key == "--think-min-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.thinkMinMs)) {
                error = "invalid value for --think-min-ms";
                return false;
            }
        } else if (key == "--think-max-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.thinkMaxMs)) {
                error = "invalid value for --think-max-ms";
                return false;
            }
        } else if (key == "--eat-min-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.eatMinMs)) {
                error = "invalid value for --eat-min-ms";
                return false;
            }
        } else if (key == "--eat-max-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.eatMaxMs)) {
                error = "invalid value for --eat-max-ms";
                return false;
            }
        } else if (key == "--timeout-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.timeoutMs)) {
                error = "invalid value for --timeout-ms";
                return false;
            }
        } else if (key == "--max-duration-ms") {
            if (!takeValue(raw) || !parseInt(raw, config.maxDurationMs)) {
                error = "invalid value for --max-duration-ms";
                return false;
            }
        } else if (key == "--log-file") {
            if (!takeValue(config.logFile)) {
                error = "missing value for --log-file";
                return false;
            }
        } else if (key == "--wait-csv") {
            if (!takeValue(config.waitCsv)) {
                error = "missing value for --wait-csv";
                return false;
            }
        } else {
            error = "unknown argument: " + argument;
            return false;
        }
    }

    if (!seedSet) {
        config.seed = kBaseSeed + static_cast<std::uint32_t>(config.studentId) * 97u;
    }
    if (config.individual && !cyclesSet) {
        config.cycles = kBaseOperations + config.studentId * 5;
    }

    if (config.studentId < 0) {
        error = "--student-id must be non-negative";
        return false;
    }
    if (config.philosophers < 2 || config.philosophers > 128) {
        error = "--philosophers must be between 2 and 128";
        return false;
    }
    if (config.cycles < 1) {
        error = "--cycles must be positive";
        return false;
    }
    if (config.thinkMinMs < 0 || config.thinkMaxMs < config.thinkMinMs) {
        error = "invalid thinking time range";
        return false;
    }
    if (config.eatMinMs < 0 || config.eatMaxMs < config.eatMinMs) {
        error = "invalid eating time range";
        return false;
    }
    if (config.timeoutMs <= 0) {
        error = "--timeout-ms must be positive";
        return false;
    }
    if (config.maxDurationMs <= 0) {
        error = "--max-duration-ms must be positive";
        return false;
    }

    return true;
}

class Logger {
public:
    Logger(std::string path, bool verbose, TimePoint start)
        : verbose_(verbose), start_(start) {
        if (!path.empty()) {
            file_.open(path, std::ios::out | std::ios::trunc);
            if (!file_) {
                throw std::runtime_error("cannot open log file: " + path);
            }
        }
    }

    ~Logger() {
        if (file_) {
            file_.flush();
        }
    }

    bool enabled() const {
        return verbose_ || file_.is_open();
    }

    void event(int philosopherId, const std::string& type,
               const std::string& resource = {}) {
        if (!enabled()) {
            return;
        }
        std::ostringstream line;
        line << timestamp() << " P" << philosopherId << ' ' << type;
        if (!resource.empty()) {
            line << " RESOURCE=" << resource;
        }
        write(line.str());
    }

    void raw(const std::string& text) {
        if (!enabled()) {
            return;
        }
        write(timestamp() + " " + text);
    }

private:
    std::string timestamp() const {
        const auto elapsed = std::chrono::duration_cast<Milliseconds>(
            Clock::now() - start_).count();
        std::ostringstream out;
        out << '[' << std::setw(6) << std::setfill('0') << elapsed << " ms]";
        return out.str();
    }

    void write(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) {
            file_ << line << '\n';
        }
        if (verbose_) {
            std::cout << line << '\n';
        }
    }

    std::mutex mutex_;
    std::ofstream file_;
    bool verbose_;
    TimePoint start_;
};

void addViolation(SharedState& state, Logger& logger, const std::string& message) {
    ++state.invariantViolations;
    if (state.invariantViolations <= 20) {
        logger.raw("INVARIANT_VIOLATION " + message);
    }
}

void checkInvariantsLocked(SharedState& state, Logger& logger,
                           const std::string& where, bool finalCheck,
                           bool requireCompletion) {
    const int count = state.config.philosophers;
    int ownedForks = 0;
    int eatingPhilosophers = 0;

    for (int fork = 0; fork < count; ++fork) {
        const int owner = state.forkOwner[fork];
        if (owner < -1 || owner >= count) {
            addViolation(state, logger,
                         where + ": invalid fork owner " + std::to_string(owner));
        } else if (owner >= 0) {
            ++ownedForks;
        }
    }

    for (int id = 0; id < count; ++id) {
        const int left = id;
        const int right = (id + 1) % count;

        if (state.states[id] == PhilosopherState::Eating) {
            ++eatingPhilosophers;
            if (state.forkOwner[left] != id || state.forkOwner[right] != id) {
                addViolation(state, logger,
                             where + ": eating philosopher " + std::to_string(id) +
                                 " does not own both forks");
            }
        } else if (state.forkOwner[left] == id || state.forkOwner[right] == id) {
            addViolation(state, logger,
                         where + ": non-eating philosopher " + std::to_string(id) +
                             " owns a fork");
        }
    }

    if (ownedForks != 2 * eatingPhilosophers) {
        addViolation(state, logger,
                     where + ": owned forks " + std::to_string(ownedForks) +
                         " != 2 * eating " + std::to_string(eatingPhilosophers));
    }

    if (static_cast<int>(state.queue.size()) > count) {
        addViolation(state, logger, where + ": queue is too long");
    }

    std::vector<int> seen(count, 0);
    for (const Request* request : state.queue) {
        if (request->philosopherId < 0 || request->philosopherId >= count) {
            addViolation(state, logger, where + ": invalid request id");
        } else if (++seen[request->philosopherId] > 1) {
            addViolation(state, logger,
                         where + ": duplicate request for philosopher " +
                             std::to_string(request->philosopherId));
        }
    }

    if (!finalCheck) {
        return;
    }

    if (!state.queue.empty()) {
        addViolation(state, logger,
                     where + ": queue is not empty after experiment");
    }

    for (int fork = 0; fork < count; ++fork) {
        if (state.forkOwner[fork] != -1) {
            addViolation(state, logger,
                         where + ": fork " + std::to_string(fork) +
                             " is still owned after experiment");
        }
    }

    if (state.config.mode == Mode::Fifo && state.totalOvertakes != 0) {
        addViolation(state, logger, where + ": FIFO mode has overtakes");
    }

    if (requireCompletion) {
        for (int id = 0; id < count; ++id) {
            if (state.states[id] != PhilosopherState::Finished) {
                addViolation(state, logger,
                             where + ": philosopher " + std::to_string(id) +
                                 " is not finished");
            }
            if (state.stats[id].completedCycles != state.config.cycles) {
                addViolation(state, logger,
                             where + ": philosopher " + std::to_string(id) +
                                 " completed wrong number of cycles");
            }
        }
    }
}

void grantLocked(SharedState& state, Logger& logger, Request* request) {
    const auto now = Clock::now();
    const int id = request->philosopherId;

    request->status = RequestStatus::Granted;
    request->grantedAt = now;
    state.forkOwner[request->leftFork] = id;
    state.forkOwner[request->rightFork] = id;
    state.states[id] = PhilosopherState::Eating;

    const long long waitUs = std::chrono::duration_cast<Microseconds>(
        now - request->enqueuedAt).count();
    PhilosopherStats& stats = state.stats[id];
    stats.grants += 1;
    stats.totalWaitUs += waitUs;
    stats.maxWaitUs = std::max(stats.maxWaitUs, waitUs);
    state.totalGrants += 1;
    state.waits.push_back(
        WaitRecord{id, static_cast<double>(waitUs) / 1000.0, "GRANTED"});

    if (sem_post(&request->event) != 0) {
        addViolation(state, logger, "sem_post failed");
    }

    logger.event(id, "ACQUIRED",
                 forkPair(request->leftFork, request->rightFork));
    checkInvariantsLocked(state, logger, "grant", false, false);
}

void dispatchLocked(SharedState& state, Logger& logger) {
    if (state.stop.load()) {
        return;
    }

    if (state.config.mode == Mode::Fifo) {
        while (!state.queue.empty()) {
            Request* request = state.queue.front();
            if (state.forkOwner[request->leftFork] == -1 &&
                state.forkOwner[request->rightFork] == -1) {
                state.queue.pop_front();
                grantLocked(state, logger, request);
            } else {
                break;
            }
        }
        return;
    }

    auto request = state.queue.begin();
    while (request != state.queue.end()) {
        Request* current = *request;
        if (state.forkOwner[current->leftFork] == -1 &&
            state.forkOwner[current->rightFork] == -1) {
            if (request != state.queue.begin()) {
                ++state.stats[current->philosopherId].overtakes;
                ++state.totalOvertakes;
            }
            request = state.queue.erase(request);
            grantLocked(state, logger, current);
        } else {
            ++request;
        }
    }
}

bool waitForGrant(SharedState& state, Request& request, int timeoutMs,
                  Logger& logger) {
    const auto timeout = Milliseconds(timeoutMs);
    const auto deadline = Clock::now() + timeout;

    while (true) {
        if (state.stop.load()) {
            return false;
        }

        const auto now = Clock::now();
        if (now >= deadline) {
            return false;
        }

        const auto sliceEnd = std::min(deadline, now + Milliseconds(100));
        timespec absolute{};
        if (clock_gettime(CLOCK_REALTIME, &absolute) != 0) {
            logger.raw("CLOCK_GETTIME_FAILED " + std::string(std::strerror(errno)));
            state.stop.store(true);
            state.aborted.store(true);
            return false;
        }

        const long long remainingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            sliceEnd - now).count();
        absolute.tv_sec += static_cast<time_t>(remainingNs / 1000000000LL);
        absolute.tv_nsec += static_cast<long>(remainingNs % 1000000000LL);
        if (absolute.tv_nsec >= 1000000000L) {
            absolute.tv_sec += 1;
            absolute.tv_nsec -= 1000000000L;
        }

        const int result = sem_timedwait(&request.event, &absolute);
        if (result == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ETIMEDOUT) {
            if (Clock::now() >= deadline) {
                return false;
            }
            continue;
        }

        logger.raw("SEM_TIMEDWAIT_FAILED " + std::string(std::strerror(errno)));
        state.stop.store(true);
        state.aborted.store(true);
        return false;
    }
}

void philosopherMain(SharedState& state, Logger& logger, int id) {
    const long tid = static_cast<long>(syscall(SYS_gettid));
    {
        std::lock_guard<std::mutex> lock(state.cs);
        state.stats[id].tid = tid;
    }
    logger.event(id, "START TID=" + std::to_string(tid));

    std::mt19937 generator(
        static_cast<std::uint32_t>(state.config.seed) +
        static_cast<std::uint32_t>(id) * 1000003u);

    auto randomDuration = [&](int minimum, int maximum) {
        if (minimum >= maximum) {
            return Milliseconds(minimum);
        }
        std::uniform_int_distribution<int> distribution(minimum, maximum);
        return Milliseconds(distribution(generator));
    };

    const int left = id;
    const int right = (id + 1) % state.config.philosophers;
    const std::string resources = forkPair(left, right);
    int completed = 0;

    while (completed < state.config.cycles) {
        if (state.stop.load()) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(state.cs);
            state.states[id] = PhilosopherState::Thinking;
        }
        logger.event(id, "THINKING");
        std::this_thread::sleep_for(randomDuration(state.config.thinkMinMs,
                                                    state.config.thinkMaxMs));

        if (state.stop.load()) {
            break;
        }

        Request request(id, left, right);
        if (!request.semaphoreReady) {
            logger.raw("SEM_INIT_FAILED P" + std::to_string(id));
            state.stop.store(true);
            state.aborted.store(true);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(state.cs);
            if (state.stop.load()) {
                break;
            }
            state.states[id] = PhilosopherState::Waiting;
            request.enqueuedAt = Clock::now();
            state.queue.push_back(&request);
            state.maxQueue = std::max(
                state.maxQueue, static_cast<int>(state.queue.size()));
            logger.event(id, "WAITING", resources);
            dispatchLocked(state, logger);
        }

        bool granted = waitForGrant(state, request, state.config.timeoutMs, logger);

        if (!granted) {
            std::lock_guard<std::mutex> lock(state.cs);
            if (request.status == RequestStatus::Granted) {
                granted = true;
            } else {
                const auto position = std::find(
                    state.queue.begin(), state.queue.end(), &request);
                if (position != state.queue.end()) {
                    state.queue.erase(position);
                }
                request.status = RequestStatus::Cancelled;
                state.stats[id].timeouts += 1;
                state.totalTimeouts += 1;
                state.waits.push_back(
                    WaitRecord{id, static_cast<double>(state.config.timeoutMs),
                               "TIMEOUT"});
                logger.event(id, "TIMEOUT", resources);
                dispatchLocked(state, logger);
            }
        }

        if (!granted) {
            if (state.stop.load()) {
                break;
            }
            continue;
        }

        if (state.stop.load()) {
            std::lock_guard<std::mutex> lock(state.cs);
            if (state.states[id] == PhilosopherState::Eating) {
                state.forkOwner[left] = -1;
                state.forkOwner[right] = -1;
                state.states[id] = PhilosopherState::Aborted;
                logger.event(id, "RELEASED", resources);
                checkInvariantsLocked(state, logger, "abort release", false, false);
            }
            break;
        }

        logger.event(id, "EATING");
        std::this_thread::sleep_for(randomDuration(state.config.eatMinMs,
                                                    state.config.eatMaxMs));

        {
            std::lock_guard<std::mutex> lock(state.cs);
            state.forkOwner[left] = -1;
            state.forkOwner[right] = -1;
            state.states[id] = PhilosopherState::Thinking;
            state.stats[id].completedCycles += 1;
            logger.event(id, "RELEASED", resources);
            checkInvariantsLocked(state, logger, "release", false, false);
            dispatchLocked(state, logger);
        }

        ++completed;
    }

    {
        std::lock_guard<std::mutex> lock(state.cs);
        if (state.stats[id].completedCycles == state.config.cycles) {
            state.states[id] = PhilosopherState::Finished;
            logger.event(id, "FINISHED");
        } else {
            state.states[id] = PhilosopherState::Aborted;
            logger.event(id, "ABORTED");
        }
        checkInvariantsLocked(state, logger, "thread exit", false, false);
    }
}

bool writeWaitCsv(const std::string& path, const std::vector<WaitRecord>& waits,
                  std::string& error) {
    if (path.empty()) {
        return true;
    }

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        error = "cannot open wait CSV: " + path;
        return false;
    }

    output << "philosopher,wait_ms,result\n";
    output << std::fixed << std::setprecision(3);
    for (const WaitRecord& wait : waits) {
        output << wait.philosopherId << ',' << wait.waitMs << ','
               << wait.result << '\n';
    }
    return true;
}

int run(const Config& config) {
    const auto start = Clock::now();
    Logger logger(config.logFile, config.verbose, start);
    logger.raw("START mode=" + modeName(config.mode) +
               " philosophers=" + std::to_string(config.philosophers) +
               " cycles=" + std::to_string(config.cycles) +
               " seed=" + std::to_string(config.seed) +
               " student_id=" + std::to_string(config.studentId));

    SharedState state;
    state.config = config;
    state.started = start;
    state.forkOwner.assign(config.philosophers, -1);
    state.states.assign(config.philosophers, PhilosopherState::Thinking);
    state.stats.resize(config.philosophers);
    for (int id = 0; id < config.philosophers; ++id) {
        state.stats[id].id = id;
    }

    std::vector<std::thread> threads;
    threads.reserve(config.philosophers);
    try {
        for (int id = 0; id < config.philosophers; ++id) {
            threads.emplace_back(philosopherMain, std::ref(state),
                                 std::ref(logger), id);
        }
    } catch (const std::exception& exception) {
        logger.raw("THREAD_CREATE_FAILED " + std::string(exception.what()));
        state.stop.store(true);
        state.aborted.store(true);
        for (std::thread& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        return 1;
    }

    std::atomic<bool> finished{false};
    std::thread watchdog([&]() {
        while (!finished.load()) {
            std::this_thread::sleep_for(Milliseconds(50));
            if (finished.load()) {
                break;
            }
            const auto elapsed = std::chrono::duration_cast<Milliseconds>(
                Clock::now() - start).count();
            if (elapsed > config.maxDurationMs) {
                logger.raw("MAX_DURATION_EXCEEDED");
                state.stop.store(true);
                state.aborted.store(true);
                break;
            }
        }
    });

    for (std::thread& thread : threads) {
        thread.join();
    }
    finished.store(true);
    watchdog.join();

    const auto end = Clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        end - start).count();
    const int expectedCycles = config.philosophers * config.cycles;
    int completedCycles = 0;
    {
        std::lock_guard<std::mutex> lock(state.cs);
        for (const PhilosopherStats& stats : state.stats) {
            completedCycles += stats.completedCycles;
        }
    }

    const bool allCompleted = completedCycles == expectedCycles;
    if (!allCompleted) {
        state.aborted.store(true);
    }

    {
        std::lock_guard<std::mutex> lock(state.cs);
        checkInvariantsLocked(state, logger, "final", true, allCompleted);
    }

    std::string csvError;
    const bool csvWritten = writeWaitCsv(config.waitCsv, state.waits, csvError);

    long long totalWaitUs = 0;
    long long maxWaitUs = 0;
    for (const PhilosopherStats& stats : state.stats) {
        totalWaitUs += stats.totalWaitUs;
        maxWaitUs = std::max(maxWaitUs, stats.maxWaitUs);
    }
    const double averageWaitMs = state.totalGrants > 0
        ? static_cast<double>(totalWaitUs) / 1000.0 /
              static_cast<double>(state.totalGrants)
        : 0.0;
    const double throughput = elapsedMs > 0.0
        ? static_cast<double>(completedCycles) / (elapsedMs / 1000.0)
        : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "CONFIG mode=" << modeName(config.mode)
              << " philosophers=" << config.philosophers
              << " cycles=" << config.cycles
              << " seed=" << config.seed
              << " student_id=" << config.studentId << '\n';
    std::cout << "ELAPSED_MS=" << elapsedMs << '\n';
    std::cout << "COMPLETED_CYCLES=" << completedCycles << '/'
              << expectedCycles << '\n';
    std::cout << "THROUGHPUT_OPS_S=" << throughput << '\n';
    std::cout << "TIMEOUTS=" << state.totalTimeouts << '\n';
    std::cout << "OVERTAKES=" << state.totalOvertakes << '\n';
    std::cout << "AVG_WAIT_MS=" << averageWaitMs << '\n';
    std::cout << "MAX_WAIT_MS=" << static_cast<double>(maxWaitUs) / 1000.0
              << '\n';
    std::cout << "MAX_QUEUE=" << state.maxQueue << '\n';
    std::cout << "GRANTS=" << state.totalGrants << '\n';
    std::cout << "INVARIANT_VIOLATIONS=" << state.invariantViolations << '\n';
    std::cout << "STATUS=";
    if (state.invariantViolations > 0) {
        std::cout << "INVARIANT_VIOLATION\n";
    } else if (!allCompleted || state.aborted.load()) {
        std::cout << "ABORTED\n";
    } else {
        std::cout << "OK\n";
    }

    for (int id = 0; id < config.philosophers; ++id) {
        const PhilosopherStats& stats = state.stats[id];
        const double average = stats.grants > 0
            ? static_cast<double>(stats.totalWaitUs) / 1000.0 /
                  static_cast<double>(stats.grants)
            : 0.0;
        std::cout << "P" << id
                  << " TID=" << stats.tid
                  << " STATE=" << stateName(state.states[id])
                  << " COMPLETED=" << stats.completedCycles
                  << " GRANTS=" << stats.grants
                  << " TIMEOUTS=" << stats.timeouts
                  << " OVERTAKES=" << stats.overtakes
                  << " AVG_WAIT_MS=" << average
                  << " MAX_WAIT_MS="
                  << static_cast<double>(stats.maxWaitUs) / 1000.0 << '\n';
    }

    if (!csvWritten) {
        std::cerr << "ERROR: " << csvError << '\n';
        return 1;
    }
    if (state.invariantViolations > 0) {
        return 2;
    }
    if (!allCompleted || state.aborted.load()) {
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    bool helpRequested = false;

    if (!parseArguments(argc, argv, config, error, helpRequested)) {
        std::cerr << "ERROR: " << error << '\n';
        printUsage(argv[0]);
        return 2;
    }

    if (helpRequested) {
        printUsage(argv[0]);
        return 0;
    }

    try {
        return run(config);
    } catch (const std::exception& exception) {
        std::cerr << "ERROR: " << exception.what() << '\n';
        return 1;
    }
}
