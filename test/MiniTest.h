#pragma once

#include <iostream>
#include <string>

extern int num_tests_passed;
extern int num_tests_failed;

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    struct Register##suite##name { \
        Register##suite##name() { \
            try { \
                std::cout << "[ RUN      ] " << #suite << "." << #name << "\n"; \
                test_##suite##_##name(); \
                std::cout << "[       OK ] " << #suite << "." << #name << "\n"; \
                num_tests_passed++; \
            } catch (const std::exception& e) { \
                std::cout << "[  FAILED  ] " << #suite << "." << #name << ": " << e.what() << "\n"; \
                num_tests_failed++; \
            } \
        } \
    } register_##suite##name; \
    void test_##suite##_##name()

#define ASSERT_TRUE(condition) \
    if (!(condition)) { \
        throw std::runtime_error(std::string("ASSERT_TRUE failed: ") + #condition); \
    }

#define EXPECT_TRUE(condition) ASSERT_TRUE(condition)

#define ASSERT_FALSE(condition) \
    if (condition) { \
        throw std::runtime_error(std::string("ASSERT_FALSE failed: ") + #condition); \
    }

#define EXPECT_FALSE(condition) ASSERT_FALSE(condition)

#define ASSERT_EQ(val1, val2) \
    if ((val1) != (val2)) { \
        throw std::runtime_error(std::string("ASSERT_EQ failed: ") + #val1 + " != " + #val2); \
    }

#define EXPECT_EQ(val1, val2) ASSERT_EQ(val1, val2)
