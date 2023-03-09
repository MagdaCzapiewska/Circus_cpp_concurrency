#include <atomic>
#include <chrono>
#include <deque>
#include <thread>
#include <iostream>

#include "system.hpp"

template <typename T, typename V>
bool checkType(const V* v) {
    return dynamic_cast<const T*>(v) != nullptr;
}

class Burger : public Product
{
};

class IceCream : public Product
{
};

class Chips : public Product
{
};

class BurgerMachine : public Machine
{
    std::atomic_uint burgersMade;
    std::chrono::seconds time = std::chrono::seconds(1);
public:
    BurgerMachine() : burgersMade(0) {}

    std::unique_ptr<Product> getProduct()
    {
        if (burgersMade > 0)
        {
            burgersMade--;
            return std::unique_ptr<Product>(new Burger());
        } else {
            std::this_thread::sleep_for(time);
            return std::unique_ptr<Product>(new Burger());
        }
    }

    void returnProduct(std::unique_ptr<Product> product)
    {
        if (!checkType<Burger>(product.get())) throw BadProductException();
        burgersMade++;
    }

    void start()
    {
        burgersMade.store(10);
    }

    void stop() {}
};

class IceCreamMachine : public Machine
{
public:
    std::unique_ptr<Product> getProduct()
    {
        throw MachineFailure();
    }

    void returnProduct(std::unique_ptr<Product> product)
    {
        if (!checkType<IceCream>(product.get())) throw BadProductException();
    }

    void start() {}

    void stop() {}
};

class ChipsMachine : public Machine
{
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cond;
    std::atomic<int> wcount;
    std::deque<std::unique_ptr<Chips>> queue;
    std::atomic<bool> running;
public:
    ChipsMachine() : running(false) {}

    std::unique_ptr<Product> getProduct()
    {
        if (!running) throw MachineNotWorking();
        wcount++;
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this](){ return !queue.empty(); });
        wcount--;
        auto product = std::move(queue.front());
        queue.pop_front();
        return product;
    }

    void returnProduct(std::unique_ptr<Product> product)
    {
        if (!checkType<Chips>(product.get())) throw BadProductException();
        if (!running) throw MachineNotWorking();
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_front((std::unique_ptr<Chips>&&) (std::move(product)));
        cond.notify_one();
    }

    void start()
    {
        running = true;
        thread = std::thread([this](){
            while (running || wcount > 0)
            {
                int count = 7;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    while (count --> 0) {
                        queue.push_back(std::unique_ptr<Chips>(new Chips()));
                        cond.notify_one();
                    }
                }
            }
        });
    }

    void stop()
    {
        running = false;
        thread.join();
    }
};


int main() {
    System system{
            {
                    {"burger", std::shared_ptr<Machine>(new BurgerMachine())},
                    {"iceCream", std::shared_ptr<Machine>(new IceCreamMachine())},
                    {"chips", std::shared_ptr<Machine>(new ChipsMachine())},
            },
            10,
            1
    };

    auto client1 = std::thread([&system]() {
        system.getMenu();
        auto p = system.order({"burger", "chips"});
        std::cout << "Zamowilem 1\n";
        p->wait();
        system.collectOrder(std::move(p));
        std::cout << "OK\n";
    });

    auto client2 = std::thread([&system](){
        system.getMenu();
        system.getPendingOrders();
        try {
            auto p = system.order({"iceCream", "chips"});
            std::cout << "Zamowilem 2\n";
            p->wait();
            system.collectOrder(std::move(p));
        } catch (const FulfillmentFailure& e) {
            std::cout << "OK\n";
        }
    });


    client1.join();
    client2.join();


    std::vector<WorkerReport>res = system.shutdown();

    auto client3 = std::thread([&system](){
        system.getMenu();
        system.getPendingOrders();
        try {
            auto p = system.order({"burger", "chips"});
            p->wait();
            system.collectOrder(std::move(p));
        } catch (const RestaurantClosedException& e) {
            std::cout << "OK\n";
        }
    });
    client3.join();

    std::cout << "Raporty:\n";
    for(unsigned int i = 0; i < res.size(); i++) {
        std::cout << "worker " << i << ":\n";
        std::cout << "collectedOrders: ";
        for(unsigned int j = 0; j < res[i].collectedOrders.size(); j++) {
            std::cout << '[';
            for(auto w : res[i].collectedOrders[j]) std::cout << w << ", ";
            std::cout << "] ";
        }
        std::cout << '\n';

        std::cout << "abandonedOrders: ";
        for(unsigned int j = 0; j < res[i].abandonedOrders.size(); j++) {
            std::cout << '[';
            for(auto w : res[i].abandonedOrders[j]) std::cout << w << ", ";
            std::cout << "] ";
        }
        std::cout << '\n';

        std::cout << "failedOrders: ";
        for(unsigned int j = 0; j < res[i].failedOrders.size(); j++) {
            std::cout << '[';
            for(auto w : res[i].failedOrders[j]) std::cout << w << ", ";
            std::cout << "] ";
        }
        std::cout << '\n';

        std::cout << "failedProducts: ";
        for(auto w : res[i].failedProducts) std::cout << w << ", ";
        std::cout << "\n\n";
    }

}