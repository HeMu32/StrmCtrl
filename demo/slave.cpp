#include "commons.h"
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // Required on Windows
    ix::initNetSystem();

    ix::WebSocket client;
    client.setUrl("ws://" + DemoCommons::HOST + ":" + std::to_string(DemoCommons::PORT));

    client.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message) {
            DemoCommons::log("Master says", msg->str);
        } else if (msg->type == ix::WebSocketMessageType::Open) {
            DemoCommons::log("System", "Connected to master.");
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            DemoCommons::log("System", "Disconnected from master.");
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            DemoCommons::log("Error", msg->errorInfo.reason);
        }
    });

    client.start();

    // Give it a moment to connect (better: wait for Open message in production)
    std::this_thread::sleep_for(std::chrono::seconds(1));

    DemoCommons::log("Slave", "Type a message and press Enter to send to master.");
    
    std::string text;
    while (std::getline(std::cin, text)) {
        if (text == "quit") break;
        client.send(text);
    }

    client.stop();
    ix::uninitNetSystem();

    return 0;
}
