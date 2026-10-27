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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/Ai/Tensor.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

// Values a float16 holds exactly, so float16 models round-trip them bit for bit, scaled by two too.
Tensor
halfExactImage(int64_t height,
               int64_t width)
{
    Tensor t = Tensor::image(1, 3, height, width);

    for (std::size_t i = 0; i < t.size(); ++i) {
        t.data()[i] = (float)((int)(i % 97) - 48) * 0.125f;
    }

    return t;
}

// Arbitrary float32 values, distinct per element.
Tensor
rampImage(int64_t height,
          int64_t width)
{
    Tensor t = Tensor::image(1, 3, height, width);

    for (std::size_t i = 0; i < t.size(); ++i) {
        t.data()[i] = 0.001f * (float)i - 1.7f + 1e-7f * (float)(i * i % 13);
    }

    return t;
}

std::shared_ptr<InferenceSession>
openCpu(const char* model,
        int threads = 0)
{
    return InferenceSession::open(AiTest::modelPath(model), AiTest::cpuOptions(threads), AiTest::loader());
}

class AiInferenceSession
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(AiTest::loader()->available()) << AiTest::loader()->unavailableReason();
    }
};

class AiAbortTiming
    : public AiInferenceSession {
};

} // namespace

TEST_F(AiInferenceSession, Float32IdentityIsExact)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");

    EXPECT_EQ(eAiDeviceCpu, session->device());
    EXPECT_FALSE(session->isGpu());
    ASSERT_EQ(1u, session->inputs().size());
    EXPECT_EQ("image", session->inputs()[0].name);
    EXPECT_FALSE(session->inputs()[0].float16);
    const std::vector<int64_t> dynamicShape { 1, 3, -1, -1 };
    EXPECT_EQ(dynamicShape, session->inputs()[0].shape);
    ASSERT_EQ(1u, session->outputs().size());
    EXPECT_EQ("output", session->outputs()[0].name);

    const Tensor in = rampImage(7, 5);
    const Tensor out = session->run(in);
    EXPECT_EQ(in.shape(), out.shape());
    EXPECT_EQ(in.values(), out.values());
}

TEST_F(AiInferenceSession, Float32ScaleIsExact)
{
    std::shared_ptr<InferenceSession> session = openCpu("scale2.onnx");
    const Tensor in = rampImage(4, 9);

    std::map<std::string, Tensor> inputs;
    inputs.emplace("image", in);
    const std::map<std::string, Tensor> outputs = session->run(inputs);
    ASSERT_EQ(1u, outputs.count("output"));
    const Tensor& out = outputs.at("output");
    ASSERT_EQ(in.shape(), out.shape());
    for (std::size_t i = 0; i < in.size(); ++i) {
        ASSERT_EQ(in.data()[i] * 2.f, out.data()[i]) << "element " << i;
    }
}

TEST_F(AiInferenceSession, Float16IdentityIsExactForHalfValues)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity-fp16.onnx");

    ASSERT_EQ(1u, session->inputs().size());
    EXPECT_TRUE(session->inputs()[0].float16);
    EXPECT_TRUE(session->outputs()[0].float16);

    const Tensor in = halfExactImage(6, 11);
    const Tensor out = session->run(in);
    EXPECT_EQ(in.shape(), out.shape());
    EXPECT_EQ(in.values(), out.values());
}

TEST_F(AiInferenceSession, Float16InputsRoundToNearestHalf)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity-fp16.onnx");
    const Tensor in = rampImage(3, 4);
    const Tensor out = session->run(in);

    ASSERT_EQ(in.shape(), out.shape());
    for (std::size_t i = 0; i < in.size(); ++i) {
        ASSERT_EQ(halfToFloat(floatToHalf(in.data()[i])), out.data()[i]) << "element " << i;
    }
}

TEST_F(AiInferenceSession, Float16ScaleIsExact)
{
    std::shared_ptr<InferenceSession> session = openCpu("scale2-fp16.onnx");
    const Tensor in = halfExactImage(5, 5);
    const Tensor out = session->run(in);

    ASSERT_EQ(in.shape(), out.shape());
    for (std::size_t i = 0; i < in.size(); ++i) {
        ASSERT_EQ(in.data()[i] * 2.f, out.data()[i]) << "element " << i;
    }
}

TEST_F(AiInferenceSession, OutputShapesFollowEachRun)
{
    std::shared_ptr<InferenceSession> session = openCpu("upscale2-nearest.onnx");

    for (const std::vector<int64_t>& size : { std::vector<int64_t> { 3, 4 }, std::vector<int64_t> { 8, 1 } }) {
        const Tensor in = rampImage(size[0], size[1]);
        const Tensor out = session->run(in);
        const std::vector<int64_t> expected { 1, 3, size[0] * 2, size[1] * 2 };
        ASSERT_EQ(expected, out.shape());
        for (int64_t c = 0; c < 3; ++c) {
            for (int64_t y = 0; y < expected[2]; ++y) {
                for (int64_t x = 0; x < expected[3]; ++x) {
                    ASSERT_EQ(in.at(0, c, y / 2, x / 2), out.at(0, c, y, x)) << "c " << c << " y " << y << " x " << x;
                }
            }
        }
    }
}

TEST_F(AiInferenceSession, WrongInputShapeOrNameIsAnError)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");

    EXPECT_THROW(session->run(Tensor::image(1, 4, 2, 2)), AiError);
    EXPECT_THROW(session->run(Tensor(std::vector<int64_t> { 3, 2, 2 })), AiError);

    std::map<std::string, Tensor> wrongName;
    wrongName.emplace("not_an_input", Tensor::image(1, 3, 2, 2));
    EXPECT_THROW(session->run(wrongName), AiError);
}

TEST_F(AiInferenceSession, AMissingModelIsAnError)
{
    EXPECT_THROW(openCpu("no-such-model.onnx"), AiError);
}

TEST_F(AiInferenceSession, AnUnavailableLoaderIsAnErrorWithItsReason)
{
    const std::shared_ptr<const OrtLoader> none = OrtLoader::load(std::vector<OrtLoader::Candidate>());

    try {
        InferenceSession::open(AiTest::modelPath("identity.onnx"), AiTest::cpuOptions(), none);
        FAIL() << "open() succeeded without ONNX Runtime";
    } catch (const AiError& e) {
        EXPECT_EQ(none->unavailableReason(), e.what());
    }
}

TEST_F(AiInferenceSession, TheCacheSharesOneSessionPerKey)
{
    InferenceSession::clearCache();
    EXPECT_EQ(0u, InferenceSession::cacheSize());

    std::shared_ptr<InferenceSession> a = openCpu("identity.onnx");
    std::shared_ptr<InferenceSession> b = openCpu("identity.onnx");
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(1u, InferenceSession::cacheSize());

    // The thread count is part of the key.
    std::shared_ptr<InferenceSession> c = openCpu("identity.onnx", 1);
    EXPECT_NE(a.get(), c.get());
    EXPECT_EQ(2u, InferenceSession::cacheSize());

    const char* const models[] = { "identity.onnx", "identity-fp16.onnx", "identity-64.onnx", "scale2.onnx", "scale2-fp16.onnx", "upscale2-nearest.onnx" };
    for (int threads = 1; threads <= 2; ++threads) {
        for (const char* model : models) {
            openCpu(model, threads);
        }
    }
    EXPECT_EQ(InferenceSession::kCacheCapacity, InferenceSession::cacheSize());

    InferenceSession::clearCache();
    EXPECT_EQ(0u, InferenceSession::cacheSize());
}

TEST_F(AiInferenceSession, AMissingProviderFallsBackToTheCpu)
{
    const std::shared_ptr<const OrtLoader>& ort = AiTest::loader();
    std::string missing;
    const std::vector<AiDeviceEnum> order = InferenceSession::providerOrder(eAiDeviceCuda, std::vector<std::string> { "CPUExecutionProvider" }, &missing);

    EXPECT_EQ(std::vector<AiDeviceEnum> { eAiDeviceCpu }, order);
    EXPECT_EQ("CUDAExecutionProvider", missing);

    // The CPU release fetch-assets.sh unpacks offers no CUDA, so the session lands on the CPU.
    if (!ort->hasProvider("CUDAExecutionProvider")) {
        AiSessionOptions options = AiTest::cpuOptions();
        options.device = eAiDeviceCuda;
        std::shared_ptr<InferenceSession> session = InferenceSession::open(AiTest::modelPath("identity.onnx"), options, ort);
        EXPECT_EQ(eAiDeviceCpu, session->device());
        EXPECT_EQ(eAiDeviceCuda, session->requestedOptions().device);
        const Tensor in = rampImage(2, 3);
        EXPECT_EQ(in.values(), session->run(in).values());
    }
}

TEST_F(AiInferenceSession, TwoThreadsSharingASessionGetIdenticalResults)
{
    // One intra-op thread per run, so the CPU gate lets both threads run at once.
    std::shared_ptr<InferenceSession> session = openCpu("scale2.onnx", 1);
    const Tensor in = rampImage(64, 48);
    const Tensor expected = session->run(in);
    std::atomic<bool> go(false);

    auto worker = [&]() {
        while (!go) {
            std::this_thread::yield();
        }
        std::vector<Tensor> results;
        for (int i = 0; i < 50; ++i) {
            results.push_back(session->run(in));
        }

        return results;
    };
    std::future<std::vector<Tensor>> first = std::async(std::launch::async, worker);
    std::future<std::vector<Tensor>> second = std::async(std::launch::async, worker);
    go = true;

    for (std::future<std::vector<Tensor>>* f : { &first, &second }) {
        const std::vector<Tensor> results = f->get();
        ASSERT_EQ(50u, results.size());
        for (const Tensor& r : results) {
            ASSERT_EQ(expected.shape(), r.shape());
            ASSERT_EQ(expected.values(), r.values());
        }
    }
}

TEST_F(AiInferenceSession, CpuRunsShareTheCoresByEachSessionsThreadCount)
{
    typedef std::chrono::steady_clock Clock;
    const int cores = (int)std::thread::hardware_concurrency();
    if (cores < 2) {
        std::cout << "skipped: needs two or more cores, this machine reports " << cores << std::endl;

        return;
    }
    // Runs the model once, giving up after patience if it has not finished by then.
    auto runFor = [](const std::shared_ptr<InferenceSession>& session,
                     const Tensor& in,
                     std::chrono::milliseconds patience) {
        const Clock::time_point deadline = Clock::now() + patience;
        try {
            session->run(in, [deadline]() { return Clock::now() >= deadline; });
        } catch (const AiAborted&) {
            return std::string("aborted");
        } catch (const std::exception& e) {
            return std::string("failed: ") + e.what();
        }

        return std::string("finished");
    };

    // A run on one core that never finishes on its own holds its share for the whole test.
    std::shared_ptr<InferenceSession> holder = openCpu("slow-loop.onnx", 1);
    const Tensor loopIn(std::vector<int64_t> { 4 }, std::vector<float> { 1.f, 2.f, 3.f, 4.f });
    std::atomic<bool> release(false);
    std::atomic<bool> started(false);
    std::future<std::string> held = std::async(std::launch::async, [&]() {
        started = true;
        try {
            holder->run(loopIn, [&release]() { return release.load(); });
        } catch (const AiAborted&) {
            return std::string("aborted");
        } catch (const std::exception& e) {
            return std::string("failed: ") + e.what();
        }

        return std::string("finished");
    });
    while (!started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(std::future_status::timeout, held.wait_for(std::chrono::seconds(0))) << held.get();

    const Tensor in = rampImage(4, 4);
    // Another one-thread run fits beside it.
    EXPECT_EQ("finished", runFor(openCpu("identity.onnx", 1), in, std::chrono::milliseconds(5000)));
    // A run wanting every core queues behind it.
    std::shared_ptr<InferenceSession> wide = openCpu("identity.onnx", cores);
    EXPECT_EQ("aborted", runFor(wide, in, std::chrono::milliseconds(300)));

    release = true;
    ASSERT_EQ(std::future_status::ready, held.wait_for(std::chrono::seconds(10)));
    EXPECT_EQ("aborted", held.get());
    // Released, the cores take the wide run.
    EXPECT_EQ("finished", runFor(wide, in, std::chrono::milliseconds(5000)));
}

TEST_F(AiInferenceSession, OutOfMemoryMessagesAreRecognised)
{
    EXPECT_TRUE(InferenceSession::isOutOfMemoryMessage("CUDA failure 2: out of memory ; GPU=0"));
    EXPECT_TRUE(InferenceSession::isOutOfMemoryMessage("hipErrorOutOfMemory"));
    EXPECT_TRUE(InferenceSession::isOutOfMemoryMessage("CUDNN_STATUS_ALLOC_FAILED"));
    EXPECT_TRUE(InferenceSession::isOutOfMemoryMessage("Failed to allocate memory for requested buffer of size 1073741824"));
    EXPECT_TRUE(InferenceSession::isOutOfMemoryMessage("std::bad_alloc"));
    EXPECT_FALSE(InferenceSession::isOutOfMemoryMessage("Invalid input name: image"));
    EXPECT_FALSE(InferenceSession::isOutOfMemoryMessage(""));
}

TEST_F(AiInferenceSession, AnInjectedOutOfMemoryIsAiOutOfMemory)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");
    std::vector<std::vector<int64_t>> seen;
    AiTest::InjectorGuard guard([&seen](const InferenceSession&, const std::vector<std::vector<int64_t>>& shapes) {
        seen = shapes;

        return std::string("CUDA failure 2: out of memory");
    });

    try {
        session->run(Tensor::image(1, 3, 5, 6));
        FAIL() << "the injected failure did not surface";
    } catch (const AiOutOfMemory& e) {
        EXPECT_NE(std::string::npos, std::string(e.what()).find("out of memory")) << e.what();
    }
    ASSERT_EQ(1u, seen.size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 5, 6 }), seen[0]);
}

TEST_F(AiInferenceSession, AnyOtherInjectedFailureIsAPlainAiError)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");
    AiTest::InjectorGuard guard([](const InferenceSession&, const std::vector<std::vector<int64_t>>&) {
        return std::string("Non-zero status code returned while running Conv node");
    });

    bool threwError = false;
    bool threwOutOfMemory = false;
    try {
        session->run(Tensor::image(1, 3, 2, 2));
    } catch (const AiOutOfMemory&) {
        threwOutOfMemory = true;
    } catch (const AiError& e) {
        threwError = true;
        EXPECT_NE(std::string::npos, std::string(e.what()).find("Conv node")) << e.what();
    }
    EXPECT_TRUE(threwError);
    EXPECT_FALSE(threwOutOfMemory);
}

TEST_F(AiAbortTiming, AnAbortBeforeTheRunThrowsAiAborted)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");

    EXPECT_THROW(session->run(Tensor::image(1, 3, 2, 2), []() { return true; }), AiAborted);
}

TEST_F(AiAbortTiming, AnAbortDuringAFastRunDiscardsItsOutputs)
{
    std::shared_ptr<InferenceSession> session = openCpu("identity.onnx");
    std::atomic<bool> abort(false);
    // The hook runs after the pre-run abort check, so the abort fires as the run starts, and the
    // run ends long before the abort watcher first polls.
    AiTest::InjectorGuard guard([&abort](const InferenceSession&, const std::vector<std::vector<int64_t>>&) {
        abort = true;

        return std::string();
    });

    EXPECT_THROW(session->run(Tensor::image(1, 3, 2, 2), [&abort]() { return abort.load(); }), AiAborted);
}

TEST_F(AiInferenceSession, ProvidersOnOnePhysicalDeviceShareAGateKey)
{
    EXPECT_EQ(InferenceSession::deviceGateKey(eAiDeviceMigraphx, 0), InferenceSession::deviceGateKey(eAiDeviceRocm, 0));
    EXPECT_EQ(InferenceSession::deviceGateKey(eAiDeviceMigraphx, 1), InferenceSession::deviceGateKey(eAiDeviceRocm, 1));
    EXPECT_NE(InferenceSession::deviceGateKey(eAiDeviceRocm, 0), InferenceSession::deviceGateKey(eAiDeviceRocm, 1));
    EXPECT_NE(InferenceSession::deviceGateKey(eAiDeviceCuda, 0), InferenceSession::deviceGateKey(eAiDeviceRocm, 0));
    EXPECT_NE(InferenceSession::deviceGateKey(eAiDeviceOpenvino, 0), InferenceSession::deviceGateKey(eAiDeviceRocm, 0));
    EXPECT_NE(InferenceSession::deviceGateKey(eAiDeviceOpenvino, 0), InferenceSession::deviceGateKey(eAiDeviceCuda, 0));
    EXPECT_NE(InferenceSession::deviceGateKey(eAiDeviceCpu, 0), InferenceSession::deviceGateKey(eAiDeviceCuda, 0));
    EXPECT_EQ(InferenceSession::deviceGateKey(eAiDeviceCpu, 0), InferenceSession::deviceGateKey(eAiDeviceCpu, 3));
}

TEST_F(AiAbortTiming, AnAbortStopsARunningModelWithin200Milliseconds)
{
    typedef std::chrono::steady_clock Clock;
    // slow-loop.onnx never finishes on its own, so only the abort can end the run.
    std::shared_ptr<InferenceSession> session = openCpu("slow-loop.onnx");
    const Tensor in(std::vector<int64_t> { 4 }, std::vector<float> { 1.f, 2.f, 3.f, 4.f });
    std::atomic<bool> abort(false);
    std::atomic<bool> started(false);
    Clock::time_point flippedAt;

    std::future<std::string> run = std::async(std::launch::async, [&]() {
        started = true;
        try {
            session->run(in, [&abort]() { return abort.load(); });
        } catch (const AiAborted&) {
            return std::string("aborted");
        } catch (const std::exception& e) {
            return std::string("failed: ") + e.what();
        }

        return std::string("finished");
    });
    while (!started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(std::future_status::timeout, run.wait_for(std::chrono::seconds(0))) << "the run ended before the abort";

    flippedAt = Clock::now();
    abort = true;
    const std::future_status status = run.wait_for(std::chrono::seconds(10));
    const Clock::time_point endedAt = Clock::now();
    ASSERT_EQ(std::future_status::ready, status) << "the run ignored the abort";
    EXPECT_EQ("aborted", run.get());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(endedAt - flippedAt).count(), 200);
}
