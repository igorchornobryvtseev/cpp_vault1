#include <algorithm>
#include <condition_variable>
#include <fmt/format.h>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
 
constexpr size_t maxElementNumber = 1024;
 
template<class ElementData>
class Vault {
    struct Element {
        ElementData data;
        std::mutex access;
        std::atomic_bool inUse{false};
    };
 
    std::array<Element, maxElementNumber> storage;
    std::mutex access;
 
public:
    class ElementView {
        std::unique_lock<std::mutex> lock;
        ElementData &ref;
 
        explicit ElementView(Element &e) : lock{e.access}, ref{e.data} {}
        friend class Vault;
 
    public:
        ElementData &operator()() { return ref; }
    };
 
    ElementView view(size_t idx) {
        ElementView v{storage.at(idx)};
        if (!storage.at(idx).inUse)
            throw std::out_of_range{"no such element"};
        return v;
    }
 
    ElementView allocate() {
        std::unique_lock _{access};
        auto iter = std::ranges::find_if_not(storage, &Element::inUse);
        if (iter == storage.end())
            throw std::out_of_range{"no empty element found"};
 
        iter->inUse.store(true);
        return ElementView{*iter};
    }
 
    bool deallocate(size_t idx) {
        Element &e{storage.at(idx)};
        std::unique_lock _{e.access};
        return e.inUse.exchange(false);
    }
 
    bool deallocate(std::function<bool(const ElementData &)> pred) {
        std::unique_lock _1{access};
        auto iter = std::ranges::find_if(storage, [pred](const Element &e) { return e.inUse && pred(e.data); });
        if (iter == storage.cend()) {
            // throw std::out_of_range{"no such element"};
            return false;
        }
        std::unique_lock _2{iter->access};
        _1.unlock();
        return iter->inUse.exchange(false);
    }
 
    void dump()  {
        for (size_t i = 0; i < maxElementNumber; i++) {
            auto v{view(i)};
            fmt::print("{} {} {}\n", i, v().field_3, v().field_1);
        }
    }
};
 
 
struct Data {
    int field_1{0};
    std::string field_3;
};
 
int main() {
    using namespace std::chrono_literals;
 
    Vault<Data> v;
 
    constexpr size_t threadsCount = 8;
    std::array<std::jthread, threadsCount> thr;
 
    // concurrent fill in 8 threads
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, i]() {
            for (size_t n = 0; n < maxElementNumber / threadsCount; n++) {
                auto view = v.allocate();
                view().field_3.assign(fmt::format("{}_{}", i + 1, n + 1));
                view().field_1 = 0;
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
 
    v.dump();
 
    // concurrent modify in 8 threads (multi-fields modify)
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, i]() {
            std::random_device dev;
            std::mt19937 rng(dev());
            std::uniform_int_distribution<size_t> dist(0, maxElementNumber - 1);
            for (size_t k = 0; k < 200; k++) {
                size_t idx = dist(rng);
                auto view{v.view(idx)};
                view().field_1++;
                view().field_3.assign(fmt::format("{}_{}", view().field_3, i + 1));
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
 
    v.dump();
 
    size_t sum = 0;
    for (size_t i = 0; i < maxElementNumber; i++) {
        auto view{v.view(i)};
        sum += view().field_1;
    }
    fmt::print("total modifications: {} (expect {})\n", sum, threadsCount * 200);
 
    // concurrent deallocate in 8 threads (will deallocate already deallocated also to test collisions)
    std::atomic_size_t deallocationsCount;
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, i, &deallocationsCount]() {
            for (size_t idx = i; idx < maxElementNumber; idx += 2) {
                if (v.deallocate(idx))
                    deallocationsCount.fetch_add(1);
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
 
    fmt::print("total deallocations: {} (expect {})\n", deallocationsCount, maxElementNumber);
 
 
    // concurrent fill in 8 threads (again)
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, i]() {
            for (size_t n = 0; n < maxElementNumber / threadsCount; n++) {
                auto view = v.allocate();
                view().field_3.assign(fmt::format("{}_{}", i + 1, n + 1));
                view().field_1 = 0;
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
 
    // concurrent deallocate in 8 threads by same predicate (high collisions rate)
    deallocationsCount.store(0);
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, &deallocationsCount]() {
            auto prefix_pred = [](const Data &d) { return d.field_3.starts_with("2_"); };
            while (v.deallocate(prefix_pred)) {
                deallocationsCount.fetch_add(1);
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
    fmt::print("total deallocations with predicate: {} (expect {})\n", deallocationsCount, maxElementNumber / threadsCount);
 
    // concurrent fill in 8 threads into sparse storage
    for (size_t i = 0; i < threadsCount; i++) {
        thr[i] = std::jthread([&v, i, &deallocationsCount]() {
            for (size_t n = 0; n < deallocationsCount / threadsCount; n++) {
                auto view = v.allocate();
                view().field_3.assign(fmt::format("additional {}_{}", i + 1, n + 1));
                view().field_1 = 0;
                std::this_thread::sleep_for(10ns);
            }
        });
    }
    for (auto &t: thr)
        t.join();
 
    v.dump();
}
