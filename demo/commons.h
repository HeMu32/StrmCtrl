#pragma once

#include <string>
#include <iostream>

namespace DemoCommons {
    const int PORT = 11451;
    const std::string HOST = "127.0.0.1";
    
    inline void log(const std::string& prefix, const std::string& msg) {
        std::cout << "[" << prefix << "] " << msg << std::endl;
    }
}
