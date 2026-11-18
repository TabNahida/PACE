#include <pace/task_group.hpp>
#include <chrono>
#include <iostream>
#include <string>
using namespace std::chrono_literals;

pace::task<std::string> main_task() {
    co_return co_await pace::with_task_group([](pace::task_group& group) -> pace::task<std::string> {
        auto first = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(10ms);
            co_return "Hello";
        });
        auto second = group.spawn([]() -> pace::task<std::string> {
            co_await pace::sleep_for(5ms);
            co_return "structured PACE";
        });
        auto a = co_await first;
        auto b = co_await second;
        co_return a + ", " + b + "!";
    }, {.deadline = pace::deadline::after(2s)});
}
int main() {
    try { std::cout << pace::run(main_task) << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
