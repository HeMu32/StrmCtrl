#include "commons.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // Required on Windows
    ix::initNetSystem();

    // Create a server
    ix::WebSocketServer server(DemoCommons::PORT, "0.0.0.0");

    server.setOnConnectionCallback(
        [](std::weak_ptr<ix::WebSocket> weakWebSocket,
           std::shared_ptr<ix::ConnectionState> connectionState) {

            // lock the weak_ptr before using
            if (auto webSocket = weakWebSocket.lock())
            {
                webSocket->setOnMessageCallback(
                    [webSocket, connectionState](const ix::WebSocketMessagePtr& msg) {
                        if (msg->type == ix::WebSocketMessageType::Message) {
                            DemoCommons::log("Client says", msg->str);
                        } else if (msg->type == ix::WebSocketMessageType::Open) {
                            DemoCommons::log("System", "New connection from client.");
                        } else if (msg->type == ix::WebSocketMessageType::Close) {
                            DemoCommons::log("System", "Client disconnected.");
                        } else if (msg->type == ix::WebSocketMessageType::Error) {
                            DemoCommons::log("Error", msg->errorInfo.reason);
                        }
                    }
                );
            }
        }
    );

    auto res = server.listen();
    if (!res.first) {
        std::cerr << "Failed to start server: " << res.second << std::endl;
        return 1;
    }

    server.start();
    DemoCommons::log("Master", "Listening on port " + std::to_string(DemoCommons::PORT));
    DemoCommons::log("Master", "Type a message and press Enter to send to all clients.");

    // Input loop
    std::string text;
    while (std::getline(std::cin, text)) {
        if (text == "quit") break;
        
        // Broadcast to all connected clients
        auto clients = server.getClients();
        for (auto& client : clients) {
            client->send(text);
        }
    }

    server.stop();
    ix::uninitNetSystem();
    return 0;
}
