/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/Ai/Tensor.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

// Set to leave ONNX Runtime telemetry fully on (opt-out variable, set by main(), and API call both
// skipped), which must make this test fail: run `NATRON_TEST_ORT_KEEP_TELEMETRY=1 ctest -R AiOrtTelemetry`.
const char* const kKeepTelemetryVariable = "NATRON_TEST_ORT_KEEP_TELEMETRY";

// The only switch that stops ONNX Runtime's telemetry thread from starting; main() sets it.
const char* const kOptOutVariable = "ORT_DISABLE_TELEMETRY";

// ONNX Runtime never starts its telemetry client when one of these is set, which would make this
// test pass vacuously. SuiteProperties.cmake strips them for ctest runs.
const char* const kTelemetrySuppressingVariables[] = {
    "CI",
    "TF_BUILD",
    "GITHUB_ACTIONS",
    "GITLAB_CI",
    "CIRCLECI",
    "TRAVIS",
    "JENKINS_URL",
    "CODEBUILD_BUILD_ID",
    "BUILDKITE",
    "TEAMCITY_VERSION",
    "APPVEYOR",
    "BITBUCKET_BUILD_NUMBER",
    "SYSTEM_TEAMFOUNDATIONCOLLECTIONURI",
    "ORT_RUNNING_UNIT_TESTS",
};

// The telemetry client's first lookup was seen about 8 s after the environment was created.
const std::chrono::seconds kUploadWait(20);

struct NetworkLog {
    std::mutex mutex;
    bool recording = false;
    std::vector<std::string> lookups;
    std::vector<std::string> connects;
};

// Never destroyed: the library's threads may still call the hooks during static destruction.
NetworkLog&
networkLog()
{
    static NetworkLog* log = new NetworkLog;

    return *log;
}

void
record(std::vector<std::string> NetworkLog::* list,
       const std::string& entry)
{
    NetworkLog& log = networkLog();
    std::lock_guard<std::mutex> lock(log.mutex);

    if (log.recording) {
        (log.*list).push_back(entry);
    }
}

// An AF_INET or AF_INET6 destination other than loopback, or any DNS port, as text; empty otherwise.
std::string
outboundAddress(const struct sockaddr* addr)
{
    char text[INET6_ADDRSTRLEN] = {};

    if (!addr) {
        return std::string();
    }
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* in = reinterpret_cast<const struct sockaddr_in*>(addr);
        const int port = ntohs(in->sin_port);
        const bool loopback = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
        if (loopback && (port != 53)) {
            return std::string();
        }
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));

        return std::string(text) + ":" + std::to_string(port);
    }
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* in6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
        const int port = ntohs(in6->sin6_port);
        const bool loopback = IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr) || (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) && (in6->sin6_addr.s6_addr[12] == 127));
        if (loopback && (port != 53)) {
            return std::string();
        }
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text));

        return "[" + std::string(text) + "]:" + std::to_string(port);
    }

    return std::string();
}

bool
isTelemetryHost(const std::string& host)
{
    const std::string suffix = ".data.microsoft.com";
    std::string lower(host);

    for (char& c : lower) {
        c = (char)std::tolower((unsigned char)c);
    }

    return (lower.size() >= suffix.size()) && (lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0);
}

std::string
joined(const std::vector<std::string>& entries)
{
    std::string text;

    for (const std::string& entry : entries) {
        text += (text.empty() ? "" : ", ") + entry;
    }

    return text;
}

} // namespace

// These replace libc's for the whole test process, including the dlopen()ed ONNX Runtime, whose
// telemetry client resolves and connects through them. Tests/CMakeLists.txt exports them.
extern "C" int
getaddrinfo(const char* node,
            const char* service,
            const struct addrinfo* hints,
            struct addrinfo** res)
{
    typedef int (*Fn)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static const Fn next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "getaddrinfo"));

    if (node) {
        record(&NetworkLog::lookups, node);
    }

    return next ? next(node, service, hints, res) : EAI_SYSTEM;
}

extern "C" int
connect(int fd,
        const struct sockaddr* addr,
        socklen_t len)
{
    typedef int (*Fn)(int, const struct sockaddr*, socklen_t);
    static const Fn next = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "connect"));
    const std::string outbound = outboundAddress(addr);

    if (!outbound.empty()) {
        record(&NetworkLog::connects, outbound);
    }

    return next ? next(fd, addr, len) : -1;
}

// Restores the recording and opt-out state however the test body ends.
struct TestStateReset {
    ~TestStateReset()
    {
        {
            NetworkLog& log = networkLog();
            std::lock_guard<std::mutex> lock(log.mutex);
            log.recording = false;
        }
        OrtLoader::setTelemetryOptOutForTesting(true);
    }
};

TEST(AiOrtTelemetry, NoTelemetryLookupOrOutboundConnect)
{
    const bool keepTelemetry = std::getenv(kKeepTelemetryVariable) != nullptr;
    for (const char* name : kTelemetrySuppressingVariables) {
        ASSERT_EQ(nullptr, std::getenv(name)) << name << " is set, so ONNX Runtime would never start its telemetry client; run this suite through ctest, which clears it";
    }
    if (keepTelemetry) {
        ASSERT_EQ(nullptr, std::getenv(kOptOutVariable)) << kOptOutVariable << " is set, so the telemetry client would not start";
    } else {
        ASSERT_NE(nullptr, std::getenv(kOptOutVariable)) << "main() did not call disableThirdPartyTelemetry()";
    }

    TestStateReset reset;
    OrtLoader::setTelemetryOptOutForTesting(!keepTelemetry);
    {
        NetworkLog& log = networkLog();
        std::lock_guard<std::mutex> lock(log.mutex);
        log.lookups.clear();
        log.connects.clear();
        log.recording = true;
    }

    {
        std::shared_ptr<const OrtLoader> ort = OrtLoader::load({ OrtLoader::Candidate { AiTest::ortDir(), true } });
        ASSERT_TRUE(ort->available()) << ort->unavailableReason();
        if (!keepTelemetry) {
            EXPECT_FALSE(ort->telemetryOptOutFailed()) << "ONNX Runtime refused DisableTelemetryEvents";
        }
        {
            std::shared_ptr<InferenceSession> session = InferenceSession::open(AiTest::modelPath("identity.onnx"), AiTest::cpuOptions(1), ort);
            const Tensor in = Tensor::image(1, 3, 4, 4);
            EXPECT_EQ(in.shape(), session->run(in).shape());
        }
        std::this_thread::sleep_for(kUploadWait);
        InferenceSession::clearCache();
    }

    std::vector<std::string> telemetryLookups;
    std::vector<std::string> lookups;
    std::vector<std::string> connects;
    {
        NetworkLog& log = networkLog();
        std::lock_guard<std::mutex> lock(log.mutex);
        log.recording = false;
        lookups = log.lookups;
        connects = log.connects;
    }
    for (const std::string& host : lookups) {
        if (isTelemetryHost(host)) {
            telemetryLookups.push_back(host);
        }
    }

    if (keepTelemetry) {
        EXPECT_FALSE(telemetryLookups.empty()) << "telemetry was left on but no collector lookup was seen, so this test cannot detect it";
    }
    EXPECT_TRUE(telemetryLookups.empty()) << "ONNX Runtime resolved its telemetry collector: " << joined(telemetryLookups)
                                          << (keepTelemetry ? std::string(" (") + kKeepTelemetryVariable + " is set)" : std::string());
    EXPECT_TRUE(connects.empty()) << "outbound connections: " << joined(connects) << "; lookups: " << joined(lookups);
}
