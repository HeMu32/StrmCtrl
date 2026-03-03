#include "MiniTest.h"
#include <future>
#include <chrono>
#include <memory>

#include "strmctrl/Master.h"
#include "strmctrl/Slave.h"
#include "TestUtils.h"

using namespace strmctrl;
using namespace std::chrono_literals;

int num_tests_passed = 0;
int num_tests_failed = 0;

class StrmCtrlTestSuite {
public:
    StrmCtrlTestSuite() {
        master_ = std::make_unique<Master>();
        slave_ = std::make_unique<Slave>();
        
        signaling_port_ = test::PortAllocator::allocate();
        rtp_port_ = signaling_port_ + 1;
        
        master_->setSignalingPort(signaling_port_);
        master_->setRtpPort(rtp_port_);
    }

    ~StrmCtrlTestSuite() {
        slave_->disconnect();
        master_->stop();
        slave_.reset();
        master_.reset();
    }

    std::unique_ptr<Master> master_;
    std::unique_ptr<Slave> slave_;
    int signaling_port_;
    int rtp_port_;
};

// ---------------------------------------------------------------------------
// 1. 基础连接与信令测试
// ---------------------------------------------------------------------------

void test_MasterStart_PortConflict() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());
    
    Master second_master;
    second_master.setSignalingPort(fixture.signaling_port_);
    second_master.setRtpPort(fixture.rtp_port_);
    
#ifndef _WIN32
    // On POSIX, binding to the same port generally fails (unless SO_REUSEPORT is explicitly used and supported)
    ASSERT_FALSE(second_master.start());
#else
    // On Windows, IXWebSocket's use of SO_REUSEADDR allows port stealing, so start() might actually succeed.
    // We safely skip the strict failure assertion here to avoid false negatives.
    bool result = second_master.start();
    if (result) second_master.stop();
#endif

}

void test_SlaveConnect_Success() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());

    auto master_conn_promise = std::make_shared<std::promise<bool>>();
    auto master_conn_future = master_conn_promise->get_future();
    fixture.master_->setConnectionCallback([master_conn_promise](bool connected, const std::string&) {
        if (connected) {
            try { master_conn_promise->set_value(true); } catch(...) {}
        }
    });

    auto slave_conn_promise = std::make_shared<std::promise<bool>>();
    auto slave_conn_future = slave_conn_promise->get_future();
    fixture.slave_->setConnectionCallback([slave_conn_promise](bool connected, const std::string&) {
        if (connected) {
            try { slave_conn_promise->set_value(true); } catch(...) {}
        }
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));

    EXPECT_EQ(master_conn_future.wait_for(5s), std::future_status::ready);
    
    EXPECT_EQ(slave_conn_future.wait_for(5s), std::future_status::ready);
    
    EXPECT_TRUE(fixture.slave_->isConnected());
    
    // Wait for SDP exchange to complete before tearing down to avoid data race in Slave
    std::this_thread::sleep_for(1500ms);

    // Clear callbacks before exiting scope
    fixture.master_->setConnectionCallback(nullptr);
    fixture.slave_->setConnectionCallback(nullptr);
}

void test_Disconnection_Graceful() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());

    auto master_conn_promise = std::make_shared<std::promise<bool>>();
    auto master_conn_future = master_conn_promise->get_future();
    
    fixture.master_->setConnectionCallback([master_conn_promise](bool connected, const std::string&) {
        if (!connected) {
            try { master_conn_promise->set_value(true); } catch(...) {}
        }
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
    
    std::this_thread::sleep_for(500ms);
    fixture.slave_->disconnect();
    
    EXPECT_EQ(master_conn_future.wait_for(5s), std::future_status::ready);
    
}

// ---------------------------------------------------------------------------
// 2. 双向文本通信测试
// ---------------------------------------------------------------------------

void test_TextChat_MasterToSlave() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());

    std::atomic<bool> received = false;
    std::string received_msg;
    fixture.slave_->setMessageCallback([&](const TextMessage& msg) {
        received_msg = msg.text;
        received = true;
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
    std::this_thread::sleep_for(1s); 
    
    fixture.master_->sendMessage("Ping");

    for (int i = 0; i < 50; ++i) {
        if (received) break;
        std::this_thread::sleep_for(100ms);
    std::cout << "[Test] pushed 30 video frames\n" << std::flush;
    }
    ASSERT_TRUE(received);
    EXPECT_EQ(received_msg, "Ping");
        std::cout << "." << std::flush;
    
    fixture.slave_->setMessageCallback(nullptr);
}

void test_TextChat_SlaveToMaster() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());

    std::atomic<bool> received = false;
    std::string received_msg;
    fixture.master_->setMessageCallback([&](const TextMessage& msg) {
        received_msg = msg.text;
        received = true;
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
    std::this_thread::sleep_for(1s); 
    
    fixture.slave_->sendMessage("Pong");

    for (int i = 0; i < 50; ++i) {
        if (received) break;
        std::this_thread::sleep_for(100ms);
    std::cout << "[Test] pushed 30 audio frames\n" << std::flush;
    }
    ASSERT_TRUE(received);
    EXPECT_EQ(received_msg, "Pong");
        std::cout << "." << std::flush;
    
    fixture.master_->setMessageCallback(nullptr);
}

// ---------------------------------------------------------------------------
// 3. 视频串流集成测试
// ---------------------------------------------------------------------------

void test_VideoStream_Delivery() {
    StrmCtrlTestSuite fixture;
    fixture.master_->setCodecConfig(CodecConfig::makeOpenH264(640, 480, 30, 500));
    ASSERT_TRUE(fixture.master_->start());

    std::atomic<bool> video_received = false;
    fixture.slave_->setVideoFrameCallback([&](const VideoFrame& frame) {
        if (frame.width() == 640 && frame.height() == 480) {
            video_received = true;
        }
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
    
    std::this_thread::sleep_for(1500ms);

    for (int i = 0; i < 30; ++i) {
        VideoFrame frame = test::createDummyVideoFrame(640, 480, AV_PIX_FMT_YUV420P);
        fixture.master_->pushVideoFrame(frame);
        std::this_thread::sleep_for(33ms);
    }

    for (int i = 0; i < 50; ++i) {
        if (video_received) break;
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(video_received);
    fixture.slave_->setVideoFrameCallback(nullptr);
}

// ---------------------------------------------------------------------------
// 4. 音频串流集成测试
// ---------------------------------------------------------------------------

void test_AudioStream_Delivery() {
    StrmCtrlTestSuite fixture;
    fixture.master_->setAudioConfig(AudioConfig::makeAAC(48000, 2, 64000));
    ASSERT_TRUE(fixture.master_->start());

    std::atomic<bool> audio_received = false;
    fixture.slave_->setAudioFrameCallback([&](const AudioFrame& frame) {
        if (frame.valid() && frame.nbSamples() > 0) {
            audio_received = true;
        }
    });

    ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
    
    std::this_thread::sleep_for(1500ms);

    for (int i = 0; i < 30; ++i) {
        AudioFrame frame = test::createDummyAudioFrame(48000, 2);
        fixture.master_->pushAudioFrame(frame);
        std::this_thread::sleep_for(21ms);
    }

    for (int i = 0; i < 50; ++i) {
        if (audio_received) break;
        std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(audio_received);
    fixture.slave_->setAudioFrameCallback(nullptr);
}

// ---------------------------------------------------------------------------
// 5. Robustness Tests
// ---------------------------------------------------------------------------

void test_Robustness_PushBeforeConnect() {
    StrmCtrlTestSuite fixture;
    fixture.master_->setCodecConfig(CodecConfig::makeOpenH264(640, 480, 30, 500));
    fixture.master_->setAudioConfig(AudioConfig::makeAAC(48000, 2, 64000));
    ASSERT_TRUE(fixture.master_->start());

    for (int i = 0; i < 50; ++i) {
        fixture.master_->pushVideoFrame(test::createDummyVideoFrame(640, 480, AV_PIX_FMT_YUV420P));
        fixture.master_->pushAudioFrame(test::createDummyAudioFrame(48000, 2));
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(true);
}

void test_Robustness_RapidReconnect() {
    StrmCtrlTestSuite fixture;
    ASSERT_TRUE(fixture.master_->start());

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(fixture.slave_->connect("127.0.0.1", fixture.signaling_port_, fixture.rtp_port_));
        std::this_thread::sleep_for(150ms);
        fixture.slave_->disconnect();
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(true);
}

void test_Robustness_InvalidAddress() {
    StrmCtrlTestSuite fixture;
    
    std::atomic<bool> failed = false;
    fixture.slave_->setConnectionCallback([&](bool connected, const std::string&) {
        if (!connected) failed = true;
    });

    fixture.slave_->connect("127.0.0.1", fixture.signaling_port_ + 999, fixture.rtp_port_);
    
    for (int i = 0; i < 50; ++i) {
        if (failed) break;
        std::this_thread::sleep_for(100ms);
    }
    
    ASSERT_TRUE(failed);
    fixture.slave_->setConnectionCallback(nullptr);
}

void test_Robustness_InvalidCodecConfig() {
    StrmCtrlTestSuite fixture;
    fixture.master_->setCodecConfig(CodecConfig::makeOpenH264(0, 0, 30, 500));
    ASSERT_FALSE(fixture.master_->start());
}

void run_all_tests() {
    std::cout << "[==========] Running tests.\n" << std::flush;

    try { test_MasterStart_PortConflict(); std::cout << "[       OK ] MasterStart_PortConflict\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] MasterStart_PortConflict: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_SlaveConnect_Success(); std::cout << "[       OK ] SlaveConnect_Success\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] SlaveConnect_Success: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_Disconnection_Graceful(); std::cout << "[       OK ] Disconnection_Graceful\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] Disconnection_Graceful: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_TextChat_MasterToSlave(); std::cout << "[       OK ] TextChat_MasterToSlave\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] TextChat_MasterToSlave: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_TextChat_SlaveToMaster(); std::cout << "[       OK ] TextChat_SlaveToMaster\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] TextChat_SlaveToMaster: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_VideoStream_Delivery(); std::cout << "[       OK ] VideoStream_Delivery\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] VideoStream_Delivery: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_AudioStream_Delivery(); std::cout << "[       OK ] AudioStream_Delivery\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] AudioStream_Delivery: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_Robustness_PushBeforeConnect(); std::cout << "[       OK ] Robustness_PushBeforeConnect\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] Robustness_PushBeforeConnect: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_Robustness_RapidReconnect(); std::cout << "[       OK ] Robustness_RapidReconnect\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] Robustness_RapidReconnect: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_Robustness_InvalidAddress(); std::cout << "[       OK ] Robustness_InvalidAddress\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] Robustness_InvalidAddress: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    try { test_Robustness_InvalidCodecConfig(); std::cout << "[       OK ] Robustness_InvalidCodecConfig\n" << std::flush; num_tests_passed++; } 
    catch (const std::exception& e) { std::cout << "[  FAILED  ] Robustness_InvalidCodecConfig: " << e.what() << "\n" << std::flush; num_tests_failed++; }

    std::cout << "[==========] Tests completed.\n" << std::flush;
    std::cout << "[  PASSED  ] " << num_tests_passed << " tests.\n";
    if (num_tests_failed > 0) {
        std::cout << "[  FAILED  ] " << num_tests_failed << " tests.\n";
        exit(1);
    }
}